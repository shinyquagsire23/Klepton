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

// Construct the jobjects the host passes *into* guest natives. Every jobject the
// guest holds must come from here — they carry a type tag, which is what lets
// GetObjectClass answer truthfully instead of guessing.
void *kl_jni_new_object(const char *class_name);
void *kl_jni_new_string(const char *utf8);

// Root for Context.getAssets()/AssetManager.open(). Defaults to "beatsaber/assets".
// With no AAssetManager_* import, this JNI path is how assets reach Unity.
void kl_jni_set_assets_dir(const char *dir);
// Writable root behind getExternalFilesDir/getFilesDir/getCacheDir — where
// Application.persistentDataPath lands. Created on demand. Default "build/android-files".
void kl_jni_set_files_dir(const char *dir);
// The APK, which Unity opens as a zip via getPackageCodePath(). Default "beatsaber.apk".
void kl_jni_set_apk_path(const char *path);

// Everything the guest asked us for: classes found, natives registered, and
// every method/field id it wanted. This is the M4 work list.
void kl_jni_report(FILE *out);

// Runnables queued by Activity.runOnUiThread that nothing has run yet. Non-zero
// means the guest is waiting on a UI-thread callback we do not yet deliver.
unsigned kl_jni_pending_ui_tasks(void);

#endif
