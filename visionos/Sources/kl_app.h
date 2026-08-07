// The bundle-shaped entry into the Klepton runtime.
//
// This is t_boot's sequence with the harness removed. What goes away is
// everything that assumed a command line and a Unix process tree: the forked
// DRM-guard self-test, the re-exec'd recon child (PLANNING §12.2 — an app
// bundle never forks, and the Metal-refuses-forked-children reason for it is
// moot once it does not), argv path handling, and the SDL viewer.
//
// What stays is the part that is actually under test: load libmain, run its
// JNI_OnLoad against the synthetic JavaVM, drive NativeLoader.load to pull in
// libunity, and call UnityPlayer.initJni. That is the P4 gate.
//
// Deliberately C, not Swift. Everything here is guest-facing — opaque jobjects,
// function pointers cast to JNI signatures, a JNIEnv we synthesise — and none
// of it is expressible in Swift without fighting the type system for nothing.
// PLANNING §12.6 puts the language boundary at the platform layer: Swift owns
// the App, the ImmersiveSpace, Metal and ARKit; C owns the guest.
#ifndef KL_APP_H
#define KL_APP_H

// Point the runtime at the four paths it needs. `resources` is the app
// bundle's resource dir (guest libraries, translated dylibs); `container` is
// the app's Documents dir, where the 2.3 GB of APK assets are staged and where
// the guest's writable roots live.
//
// Both must be absolute — trap 6c: Unity mounts the APK into its VFS under
// whatever getPackageCodePath() returned and then resolves entries by
// concatenation, so a relative mount point silently stops matching and the
// failure surfaces three layers away as "not enough storage space".
//
// Returns 0 on success, or non-zero if a required path is missing, in which
// case kl_app_status() explains which one. Checking here is the point: a
// missing asset tree otherwise presents as a shim bug deep inside Unity.
int kl_app_configure(const char *resources, const char *container);

// Run the boot sequence. Returns 0 if initJni completed with no unimplemented
// JNI call. Everything printed goes to the log file below as well as stdout,
// because an unimplemented JNI slot aborts the process by design and the
// report has to survive that — on device there is no shell to have caught it.
int kl_app_boot(void);

// The Android lifecycle, after kl_app_boot: nativeRecreateGfxState, nativeResume,
// nativeRender, then `frames` more pumped frames with the Choreographer ticked
// and the posted-task queue drained between them.
//
// Separate from kl_app_boot on purpose. Boot is the P4 gate and refuses a second
// entry, so keeping them apart lets a run take the gate's numbers first and only
// then go further — and it keeps "initJni completed" reportable even when the
// lifecycle is what fails. This is P5.4: it is where libil2cpp (66 MB, 3,083 x18
// veneers) first loads under AMFI, and where the synthetic /proc is first read on
// device (trap 6d — it prints that before anything else, because a silent zero
// there is what made Unity refuse to start on the host).
//
// Returns 0 if the sequence completed. Once per process.
int kl_app_lifecycle(unsigned frames);

// The same lifecycle, taken apart so something else can be the frame clock.
// P5b needs this: on device the deadline belongs to Compositor Services
// (cp_frame_predict_timing), so the compositor calls kl_app_frame() once per
// drawable rather than the guest owning a pump loop of its own. This is also
// why CADisplayLink was not added as an interim pacer — it would be a third
// mechanism to then remove.
//
// _begin runs everything up to the first frame (the synthetic /proc report,
// nativeRecreateGfxState, nativeResume, nativeRender) and is once per process,
// exactly as kl_app_lifecycle is. _frame runs one frame and returns what
// nativeRender returned, or -1 if _begin has not run. _report prints the P5.4
// numbers. kl_app_lifecycle is implemented on top of these three, so its
// measurement is unchanged.
int  kl_app_lifecycle_begin(void);
int  kl_app_frame(void);
void kl_app_lifecycle_report(void);

// Absolute path of the log kl_app_boot writes, valid after kl_app_configure.
// The Swift side displays it and offers it for export.
const char *kl_app_log_path(void);

// Human-readable status of the last call, for the UI. Never NULL.
const char *kl_app_status(void);

#endif
