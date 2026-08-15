// Synthetic JavaVM / JNIEnv (M3 bootstrap, M4 surface measurement).
//
// There is no JVM here and there will never be one. Beat Saber is a
// com.unity3d.player.UnityPlayerActivity app, not a NativeActivity: libmain.so
// exports JNI_OnLoad and nothing else, so *every* entry into guest code starts
// with a JNI call. We answer those calls ourselves.
//
// The tables are full — all 233 JNIEnv slots and all 8 JavaVM slots are
// populated, and every slot we have not implemented is a *named* abort rather
// than a NULL. That is the point: it turns "the guest wandered off" into
// "the guest called GetMethodID(android/os/Build, getFingerprint)". The JNI
// surface is measured this way, not guessed (PLANNING §6 M4).
#ifndef KL_JNI_H
#define KL_JNI_H
#include <stdint.h>
#include <stdio.h>

#define KL_JNI_VERSION_1_6 0x00010006

// Guest-visible types. Layout only — we never interpret a jobject.
typedef int32_t kl_jint;
typedef void   *kl_jobject;

// JNINativeMethod, as passed to RegisterNatives. 24 bytes, three pointers.
typedef struct {
    const char *name;
    const char *signature;
    void       *fnPtr;
} kl_jni_method;

// Both handles are "pointer to a pointer to a function table" — the guest does
// `ldr x8,[x0]; ldr x8,[x8,#slot*8]; blr x8` and nothing else.
void *kl_jni_vm(void);        // JavaVM*  — hand this to JNI_OnLoad
void *kl_jni_env(void);       // JNIEnv*  — for the calling thread

// Calling a Java method with no host implementation aborts by default, so a
// bring-up run stops exactly where the surface ends. Method/field *lookups*
// never abort — the guest resolves ids it may never call, so only a call proves
// something is genuinely needed. Permissive mode downgrades the failed call to a
// zero return, which collects a whole batch of gaps in one pass instead of one
// abort-fix-rerun cycle per method.
void kl_jni_set_permissive(int on);

// Natives the guest registered, by (class, name, signature). NULL if absent.
void *kl_jni_native(const char *cls, const char *name, const char *sig);

// Deliver Android's low-memory notification — UnityPlayer.nativeLowMemory,
// which is where a title runs Resources.UnloadUnusedAssets(). The declaring
// class comes from the natives table rather than being assumed. Returns 0 if
// the guest never registered it. Driven from kl_mem_pressure_* (klepton.h).
int kl_jni_low_memory(void);

// Construct the jobjects the host passes *into* guest natives. Every jobject the
// guest holds must come from here — they carry a type tag, which is what lets
// GetObjectClass answer truthfully instead of guessing.
void *kl_jni_new_object(const char *class_name);

// The rest of UnityPlayer's constructor, for a driver that has just run initJni.
//
// initJni is not the whole of `new UnityPlayer(context, events)` — the
// constructor goes on to build a handful of helper objects, and each of those
// hands ITSELF to libunity through a private native in its own constructor.
// Where the guest keeps one of those handles and later calls a method on it, a
// driver that stopped at initJni has left it NULL, and libunity does not check:
// VRChat dies in GetObjectClass(NULL) resolving `HFPStatus.clearHFPStat`.
//
// Both drivers must call this, which is why it lives here rather than in either
// one (see runtime/kl_slink.c for the same argument). Idempotent.
void kl_jni_unity_construct_helpers(void);

// The interned jclass for a name — the host side of FindClass. A native method
// is called with its declaring class as the second argument, and a guest may
// keep it: SDL3's nativeSetupJNI stashes it in a global ref and makes every
// static call through it. Passing NULL there is not a missing log line, it is a
// class handle the guest can never resolve.
void *kl_jni_class(const char *name);

// The one activity instance. A singleton because the guest reaches it two ways
// — as the Context handed to a native, and through statics like
// UnityPlayer.currentActivity — and compares them for identity. The class
// defaults to UnityPlayerActivity; the SDL3 target overrides it.
void *kl_jni_activity(void);
void kl_jni_set_activity_class(const char *cls);
void *kl_jni_new_string(const char *utf8);
// A java/lang/String[] — what SDL3's nativeRunMain takes as argv, and what the
// real SteamLink activity fills from the launching intent's "sArgs" extra.
void *kl_jni_new_string_array(const char *const *items, int n);

// The 2D->VR handoff: SteamLink.startVRLink(String) reached, with the session
// the host just authorized (PLANNING §11.9).
//
// A callback rather than something this file does itself, because honouring the
// handoff means starting a different FRONT DOOR — a different guest library,
// chain and lifecycle — and which front doors exist is the driver's knowledge,
// not the JNI layer's. Nothing installs one by default, and with none installed
// the call aborts by name exactly as it always has. A handler that RETURNS is
// one that could not do the handoff, and the abort still happens: this is the
// only path by which the guest can be told its activity started, so telling it
// that falsely would end the run silently (the shell finishes itself the
// instant this returns).
void kl_jni_set_vrlink_handoff(void (*fn)(const char *sargs));

// How the JNI surface calls BACK into the guest, by exported symbol name.
//
// The Unity guests never need this: their Java front end hands objects to
// libunity and libunity drives itself. UE4's does the opposite in several
// places — `AndroidThunkJava_InitHMDs` posts a Runnable whose whole body is
// `nativeInitHMDs()`, and the virtual-keyboard and sensor surfaces are the same
// shape — so standing in for that Java means being able to make the call the
// Java would have made. The natives are static exports, so a name is enough.
//
// Installed by whoever knows which image is the guest (kl_ue4.c); with none
// installed every one of those thunks reports that it could not call back,
// which is a stated refusal rather than a silent no-op.
void kl_jni_set_guest_native_resolver(void *(*resolve)(const char *symbol));

// Root for Context.getAssets()/AssetManager.open(). Defaults to "beatsaber/assets".
// With no AAssetManager_* import, this JNI path is how assets reach Unity.
void kl_jni_set_assets_dir(const char *dir);
// Writable root behind getExternalFilesDir/getFilesDir/getCacheDir — where
// Application.persistentDataPath lands. Created on demand. Defaults to
// kl_userdata_dir("beatsaber") if nothing sets it.
void kl_jni_set_files_dir(const char *dir);
// The per-guest userdata root: ~/Library/Application Support/Klepton/userdata/<guest>,
// or KL_FILES_DIR if set. Keyed on the guest rather than the APK so that swapping
// The guest's own versionCode / versionName, as read from the unpacked tree
// (apktool.yml). Exported because kl_ovrplat answers ovr_Application_GetVersion
// from it — one number, not two that can disagree.
void kl_jni_guest_version(long *code, const char **name);

// ...and the guest's package name, read from the same tree's
// AndroidManifest.xml. It is half an OBB's filename
// (main.<versionCode>.<package>.obb) and how the guest looks itself up through
// the PackageManager, so it is a property of the APK rather than a constant —
// see klj_guest_package.
const char *kl_jni_guest_package(void);

// Beat Saber versions keeps one save/pairing profile instead of repeating first
// setup — and deliberately OUTSIDE build/, which `make clean` deletes. Drivers
// pass the result to kl_jni_set_files_dir; visionOS overrides it with the app
// container. Returns an absolute path.
const char *kl_userdata_dir(const char *guest);
// ...read back, absolute. ANativeActivity carries internalDataPath/obbPath as
// plain char*, so a NativeActivity guest gets them here rather than over JNI.
const char *kl_jni_files_dir(void);

// The host's locale, as the two parts Android reports separately ("en", "US").
// Here rather than private to kl_jni.c because there are TWO doors onto it —
// java.util.Locale and the NDK's AConfiguration — and a guest that reads one
// for its language and the other for its resource selection must not be told
// two different things. Unreal Engine 4 reads both.
void kl_jni_locale_parts(char *lang, size_t lang_sz, char *country, size_t country_sz);
// The APK, which Unity opens as a zip via getPackageCodePath(). Default "beatsaber.apk".
void kl_jni_set_apk_path(const char *path);
// ...and what it resolved to, absolute. This is `getPackageResourcePath()` — the
// same string, under the other of Android's two names for it — and a UE4 guest
// is handed it directly rather than asking for it, so a driver acting out
// GameActivity's own Java needs to read it back.
const char *kl_jni_apk_path(void);

// Where this guest's OBB is, built from the target row's own layout, created if
// absent, and CENSUSED the first time it is asked for — which is the only thing
// in the tree that says whether the game data is actually there.
//
// A Unity guest reaches it through Context.getObbDirs() and never needs this.
// UE4 builds Android's path itself and asks nobody, so on that target the
// census had no caller at all and a completely unstaged 8.5 GB was silent.
const char *kl_jni_obb_dir(void);

// One `<meta-data>` value from the guest's own AndroidManifest.xml, or NULL if
// the key is not declared. This is the same table `ApplicationInfo.metaData`
// serves the guest over JNI, read through the front door rather than a second
// copy: a UE4 guest never asks for these itself — GameActivity.java reads them
// in Java and passes the ANSWERS to native setters — so a driver standing in
// for that Java has to read them the same way the guest would have.
const char *kl_jni_manifest_meta(const char *key);
// The native library directory: what ApplicationInfo.nativeLibraryDir reports and
// what ClassLoader.findLibrary() builds paths in. Defaults to the *relative*
// "beatsaber/lib/arm64-v8a" resolved against the working directory, which is the
// repo root under the tests and `/` inside an app bundle — so a bundle must set
// it explicitly or Unity reads back a path like "//beatsaber/lib/arm64-v8a".
void kl_jni_set_native_lib_dir(const char *dir);

// One `SDL_ENV.<key>` <meta-data> entry from the app's AndroidManifest.xml, with
// the prefix already stripped. SDLActivity.getManifestEnvironmentVariables()
// walks these and calls nativeSetenv on each, so they are real behaviour and not
// decoration — SDL_ANDROID_TRAP_BACK_BUTTON changes event routing, and
// STEAM_LINK_VR is how Steam Link's own code knows which build it is.
//
// The caller supplies them because the two Steam Link APKs genuinely differ here
// (the VR build adds STEAM_LINK_VR), so a table baked into the runtime would be
// wrong for one of them. Same reasoning as kl_jni_set_activity_class().
void kl_jni_add_manifest_env(const char *key, const char *value);

// Everything the guest asked us for: classes found, natives registered, and
// every method/field id it wanted. This is the M4 work list.
void kl_jni_report(FILE *out);

// Runnables queued by Activity.runOnUiThread that nothing has run yet. Non-zero
// means the guest is waiting on a UI-thread callback we do not yet deliver.
unsigned kl_jni_pending_ui_tasks(void);

// Run them. This is the host->guest direction: each queued Runnable is a
// bitter/jnibridge proxy, and running it means calling the guest's own
// JNIBridge.invoke native with a synthetic java.lang.reflect.Method. On Android
// this is Looper.loop() on the UI thread; there is no looper thread here, so the
// host pumps it. Returns how many ran.
unsigned kl_jni_drain_ui_tasks(void);

// --- the on-screen keyboard, frontend side ----------------------------------
//
// A guest that wants text calls UnityPlayer.showSoftInput and then waits: on
// Android the platform puts up a keyboard and reports what was typed. There is
// no platform keyboard here, so a FRONTEND is that keyboard — the viewer's SDL
// window, the visionOS app — and these four calls are the whole of it. The
// reports run on the pump thread (kl_jni_drain_ui_tasks), so a frontend may
// call them from its own thread.
//
// A frontend that implements none of this is not broken, it is a guest whose
// text field never fills in; the login screen it is holding is the measure of
// what that costs.
int  kl_jni_soft_input_active(void);

// The text the field holds now, and whether it is a PASSWORD — a frontend must
// not echo or log a secure field. Returns the same thing as _active().
int  kl_jni_soft_input_get(char *buf, size_t cap, int *secure);

// Replace the text. The frontend owns the editing (it has the real keyboard);
// this reports the result, clamped to the character limit the guest asked for.
void kl_jni_soft_input_set(const char *utf8);

// Done, or dismissed. `canceled` is the difference between the guest reading
// the text back and the guest being told the user walked away — it is a
// different managed status, and committing a cancelled field is worse than
// dropping a committed one.
void kl_jni_soft_input_close(int canceled);

// The offline gate for all of the above — `make softinput`, in `make check`.
// Needs no guest, no window and no headset: it registers its own natives
// through the real RegisterNatives and drives the real bindings, and asserts
// the sequence the guest's own Java would have produced. Returns non-zero on
// pass. See the implementation's comment for why a runtime failure here is
// invisible from everywhere else.
int  kl_jni_soft_input_selftest(FILE *out);

// Fire the pending Choreographer frame callback, if the guest posted one. There is
// no vsync source here, so the host's frame pump defines what a frame is. One-shot,
// matching Android: doFrame re-posts if it wants the next one.
void kl_jni_tick_choreographer(void);

// Local-ref hygiene at the host->guest boundary. On Android the JVM pops the
// local frame when a native method returns; our guest entries return to the
// host instead, so without this every local the guest allocates inside
// nativeRender & co. leaks (~3 objects/frame — observed exhausting the
// 131072-slot pool at swap 43582). The host wraps each guest entry in a
// push/pop pair, playing the JVM's half.
void kl_jni_local_frame_push(void);
void kl_jni_local_frame_pop(void);

// The String constant `android/os/Build`.<field> reports, or NULL if this build
// does not describe that field. Exists so the OTHER way of asking the same
// question — __system_property_get("ro.product.model") over libc — can be
// answered from the same table instead of from a second copy of the answer.
// Two sources of one fact is the "group answers" hazard: Steam Link asks BOTH
// ways and compares (CShellSystem::BIsVRHeadset), so a drift between them is
// not cosmetic, it is a contradiction the guest can see.
const char *kl_jni_build_string(const char *field);
// ...and the int-valued ones (SDK_INT). See the implementation comment: this is
// what lets __system_property_get("ro.build.version.sdk") answer from the same
// place Build.VERSION.SDK_INT does.
int kl_jni_build_int(const char *field, int dflt);

#endif
