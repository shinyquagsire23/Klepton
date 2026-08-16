// Which guest a run is for — the small set of facts that differ between them.
//
// There are three consumers and they used to be three tables. `mains/m_boot.c`
// said "beatsaber" in one place and let kl_jni.c's defaults say it in four
// more; `visionos/Sources/kl_app.c` carried its own array of literals; and
// `visionos/targets.py` carried the build's copy. A table in three places is a
// table that disagrees with itself, and the way THAT failure presents is the
// worst kind: the build succeeds, the app installs, and the guest is described
// wrongly to itself — Beat Saber's APK opened as SUPERHOT's zip, or a title
// writing its saves into another's userdata directory.
//
// So targets.py stays the authority (it also carries the bundle id, product and
// display name, which only the build needs) and `make targets` generates
// kl_target_table.h from it. This header is the runtime's view of that table.
//
// What a target does NOT decide is where the files are. The host reads them out
// of the repo, the app reads them out of its container, and each one builds its
// own absolute paths from the relative ones here — kl_target_apply_host() is
// the host's half, and kl_app_configure() is the app's.
#ifndef KL_TARGET_H
#define KL_TARGET_H

// Which boot sequence the guest wants. Not a property of the file layout, which
// is why it lives here rather than being sniffed: libmain's JNI_OnLoad and
// kl_slink's front doors are different entry points into different lifecycles,
// and a guest that is asked the wrong question answers by aborting somewhere
// unrelated.
typedef enum {
    KL_GUEST_UNITY = 0,   // libmain -> NativeLoader.load -> libunity -> lifecycle
    KL_GUEST_STEAMLINK,   // kl_slink's shell / OpenXR doors
    // Unreal Engine 4 — kl_ue4's NativeActivity door. A different ENTRY, not a
    // different lifecycle: UE4's GameActivity is a NativeActivity, so the guest
    // is started through ANativeActivity_onCreate and then driven by the
    // callbacks it fills in, where a Unity guest is started through libmain's
    // JNI_OnLoad and driven by calling natives it registered. Everything after
    // the door — the looper, the Choreographer, EGL, the XR seam — is shared.
    KL_GUEST_UE4,
} kl_guest_kind;

typedef struct {
    const char *name;       // KLEPTON_TARGET / KL_TARGET / m_boot's argv[1]
    const char *tree;       // the unpacked APK's directory
    const char *apk;        // ...and the APK itself, which is load-bearing
    const char *assets;     // <tree>/assets, the AssetManager root
    const char *libdir;     // <tree>/lib/arm64-v8a, relative to the repo root
    const char *entry_lib;  // the library the chain starts at ("libmain")
    const char *userdata;   // the userdata KEY — see kl_target_resolve()
    // Where this guest looks for its OBB, RELATIVE to the external-storage
    // root — <userdata>/<obb> on the host, <container>/android-files/<obb> in
    // the app. It is in the table because it is not one layout: a Unity guest
    // asks Java (Context.getObbDirs() -> <files>/obb) and an Unreal guest
    // builds Android's own path itself (Android/obb/<package>/), so staging a
    // UE4 target the Unity way puts gigabytes somewhere the engine never looks
    // — and both engines report a missing OBB by carrying on.
    const char *obb;
    kl_guest_kind kind;
} kl_target;

// NULL if the name is not in the table. `kl_target_names()` is for the error
// message that follows.
const kl_target *kl_target_lookup(const char *name);
const kl_target *kl_target_default(void);
const char      *kl_target_names(void);

// What a driver actually calls: turn one argument into a target.
//
//   NULL / ""                     KL_TARGET, else the default (beatsaber)
//   "superhot"                    that target
//   "superhot/lib/arm64-v8a"      ...the same one, named by its libdir
//   "beatsaber-128/lib/arm64-v8a" a tree with no table entry: SYNTHESIZED
//
// The third form is what every documented run command and every Makefile gate
// passes, so it has to keep working exactly as it does. The fourth is the
// version-swap trees (beatsaber-113, -128, -16): a path is taken at its word,
// because refusing it would break the one workflow those directories exist for,
// and a synthesized target reads its assets and APK from BESIDE the libdir
// rather than from whatever the default target happens to be — which is what
// the old hardcoded defaults did, silently pairing 1.28's libraries with
// 1.40's assets.
//
// A synthesized target's `userdata` key drops a trailing "-<digits>", so
// beatsaber-128 and beatsaber share one profile. That is not tidiness: it is
// the documented rule that swapping Beat Saber versions
// must not repeat first-run setup.
//
// Never returns NULL for a path; returns NULL only for an unknown NAME, which
// is a typo the caller should report with kl_target_names().
const kl_target *kl_target_resolve(const char *arg);

// The host's paths, applied: library path, native lib dir, assets, APK and the
// userdata directory. The visionOS app does NOT call this — its files live in
// the app container and kl_app_configure builds those paths itself.
//
// `libdir_override` is for a run that was handed a path outside the tree
// (nothing in-tree does this today); NULL uses the target's own.
void kl_target_apply_host(const kl_target *t, const char *libdir_override);

#endif
