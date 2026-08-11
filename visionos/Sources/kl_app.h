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

// Redirect stdout/stderr into the run's log file without booting a guest.
// kl_app_boot() already does this; this is for callers that want the log and
// deliberately do not want a guest — the KL_TEMPLATE floor test. Returns 0 on
// success. Without it such a run leaves the PREVIOUS run's log in place, which
// reads as "the new run produced nothing" rather than "the new run could not
// write anything".
int kl_app_open_log(void);

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

// The guest on its own thread — PLANNING §12.12.
//
// Calling kl_app_frame() straight from the compositor's render loop, which is
// what P5b did, means the guest's nativeRender runs *inside* Compositor
// Services' submission window: the loop cannot present until the guest has
// finished, so a late guest frame is a missed display frame rather than a
// reprojected one. That defeats the reprojection pass, whose entire purpose is
// to show the previous picture against a newer pose — it can never do that
// while the thing it would reproject is the thing blocking the loop.
//
// So: _start spawns a thread that runs _begin and then one _frame per
// *published* pose, and the compositor calls _publish where it used to call
// _frame and carries straight on to its drawable.
//
// **The pacing is the design decision, and it is "skip", not "queue".**
// Publishes coalesce: the guest runs at most one frame per publish and, when it
// is behind, the publishes it missed are dropped rather than owed. A guest that
// keeps up therefore behaves exactly as it does today (one frame per drawable,
// against the freshest pose), and a guest that does not falls behind by dropped
// frames instead of accumulating a backlog it can never work off.
//
// _start is once per process, as _begin is. It returns 0 if the thread was
// spawned — not if the guest booted, which happens on the thread and is
// reported through kl_app_guest_state(). _stop asks the thread to finish the
// frame it is in, joins it, and only then returns, so the P5.4 report (which
// belongs to the guest's loop end, not the render loop's) has been written by
// the time the caller continues.
int  kl_app_guest_start(void);
void kl_app_guest_publish(void);
void kl_app_guest_stop(void);

// Where the guest thread has got to. 0 = still in _begin (nothing to present
// yet), 1 = running frames, -1 = _begin failed or the loop has ended; in the
// last case kl_app_status() says why.
int  kl_app_guest_state(void);

// Which guest this app was built for, and whether it is the Steam Link one.
//
// Valid after kl_app_configure; before that the name is "(unconfigured)" and the
// predicate is false. The Swift side needs both — what the boot window says, and
// whether the compositor is driving a guest that owns its own frame loop.
//
// The default is compiled in (KL_TARGET_DEFAULT, from gen_xcodeproj.py) rather
// than read from the environment, because an app launched by hand from the Home
// View has no environment. KL_TARGET overrides it.
const char *kl_app_target_name(void);
int         kl_app_target_is_steamlink(void);

// The 2D -> VR handoff (PLANNING §11.9), from the Swift side's point of view.
//
// Steam Link's 2D shell pairs with a host, and when the host authorizes it
// calls SteamLink.startVRLink(sArgs) and finishes itself. On the host that is a
// re-exec into the OpenXR front door (SL-15); an app bundle cannot re-exec, so
// here the window's job is to notice this has happened and open the
// ImmersiveSpace. The guest thread the compositor then starts opens the other
// front door in the same process.
//
// _pending is 0 until the shell hands off and 1 after, and never goes back —
// the shell's main thread is parked inside startVRLink by then and there is
// nothing to return to. _sargs is the authorized session, for the log: the
// runtime already has it (it is in the environment as KL_SLINK_SARGS, which is
// the same path a hand-carried one takes), so nothing has to pass it along.
int         kl_app_vrlink_pending(void);
const char *kl_app_vrlink_sargs(void);

// Absolute path of the log kl_app_boot writes, valid after kl_app_configure.
// The Swift side displays it and offers it for export.
const char *kl_app_log_path(void);

// Human-readable status of the last call, for the UI. Never NULL.
const char *kl_app_status(void);

#endif
