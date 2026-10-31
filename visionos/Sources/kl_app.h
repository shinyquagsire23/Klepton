// The bundle-shaped entry into the Klepton runtime.
//
// The guest's own sequences — the four doors, the lifecycle, one frame, the
// reports — are runtime/guest/kl_driver.c, shared verbatim with `build/m_boot`,
// because a guest described two ways is a bug with no error surface. What is
// here is the bundle: where the files are, the log file, the heartbeat, the
// guest thread the compositor paces, and the 2D->VR handoff. What is
// deliberately absent is everything that assumes a command line and a Unix
// process tree — the forked DRM-guard self-test, the re-exec'd recon child, argv
// path handling and the SDL viewer.
//
// Deliberately C, not Swift. Everything here is guest-facing — opaque jobjects,
// function pointers cast to JNI signatures, a JNIEnv we synthesise — and none
// of it is expressible in Swift without fighting the type system for nothing.
// The language boundary is the platform layer: Swift owns the App, the
// ImmersiveSpace, Metal and ARKit; C owns the guest.
#ifndef KL_APP_H
#define KL_APP_H

// Point the runtime at the four paths it needs. `resources` is the app
// bundle's resource dir (guest libraries, translated dylibs); `container` is
// the app's Documents dir, where the 2.3 GB of APK assets are staged and where
// the guest's writable roots live.
//
// Both must be absolute: Unity mounts the APK into its VFS under
// whatever getPackageCodePath() returned and then resolves entries by
// concatenation, so a relative mount point silently stops matching and the
// failure surfaces three layers away as "not enough storage space".
//
// Returns 0 on success, or non-zero if a required path is missing, in which
// case kl_app_status() explains which one. Checking here is the point: a
// missing asset tree otherwise presents as a shim bug deep inside Unity.
int kl_app_configure(const char *resources, const char *container);
// A test of a cold shader start, asked for by a file in Documents; kl_app.c
// has the story. From the App's init, before Metal is used.
void kl_app_shader_cache_test(void);

// Run the boot sequence. Returns 0 if initJni completed with no unimplemented
// JNI call. Everything printed goes to the log file below as well as stdout,
// because an unimplemented JNI slot aborts the process by design and the
// report has to survive that — on device there is no shell to have caught it.
int kl_app_boot(void);

// The Android lifecycle, after kl_app_boot: nativeRecreateGfxState, nativeResume,
// nativeRender, then `frames` more pumped frames with the posted-task queue
// drained between them. A guest that owns its own frame loop takes a pump in
// SECONDS instead — a frame count means nothing to it.
//
// Separate from kl_app_boot on purpose. Boot is the gate and refuses a second
// entry, so keeping them apart lets a run take the gate's numbers first and only
// then go further — and it keeps "initJni completed" reportable even when the
// lifecycle is what fails. It is where libil2cpp (66 MB, 3,083 x18 veneers)
// first loads under AMFI, and where the synthetic /proc is first read on device.
// The /proc dump prints before anything else, because a silent zero there is
// what makes Unity refuse to start.
//
// Returns 0 if the sequence completed. Once per process.
int kl_app_lifecycle(unsigned frames);

// The same lifecycle, taken apart so something else can be the frame clock.
// The compositor needs this: on device the deadline belongs to Compositor
// Services
// (cp_frame_predict_timing), so the compositor calls kl_app_frame() once per
// drawable rather than the guest owning a pump loop of its own. This is also
// why CADisplayLink was not added as an interim pacer — it would be a third
// mechanism to then remove.
//
// _begin runs everything up to the first frame (the synthetic /proc report,
// nativeRecreateGfxState, nativeResume, nativeRender) and is once per process,
// exactly as kl_app_lifecycle is. _frame runs one frame and returns what
// nativeRender returned, or -1 if _begin has not run. _report prints the
// lifecycle numbers. kl_app_lifecycle is implemented on top of these three, so its
// measurement is unchanged.
int  kl_app_lifecycle_begin(void);
int  kl_app_frame(void);
void kl_app_lifecycle_report(void);

// The guest on its own thread.
//
// Calling kl_app_frame() straight from the compositor's render loop, which is
// the compositor once did, means the guest's nativeRender runs INSIDE
// Compositor
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
// frame it is in, joins it, and only then returns, so the lifecycle report
// (which
// belongs to the guest's loop end, not the render loop's) has been written by
// the time the caller continues.
int  kl_app_guest_start(void);
void kl_app_guest_publish(void);

// The panel's own refresh rate, as the compositor measured it: what
// KL_GUEST_PACE_DIV divides. Not the rate the guest is told (KL_DISPLAY_HZ can
// force that), and not the rate the app is being shown at, which the system
// may halve.
void kl_app_set_panel_hz(float hz);
// The system's thermal state (ProcessInfo.ThermalState's raw value, 0 nominal
// .. 3 critical), for the [perf] line.
void kl_app_note_thermal(int state);
// The least memory left before the app's limit while the game drew, in MB; -1
// where that is not known.
long long kl_app_min_avail_mb(void);
void kl_app_guest_stop(void);

// Where the guest thread has got to. 0 = still in _begin (nothing to present
// yet), 1 = running frames, -1 = _begin failed or the loop has ended; in the
// last case kl_app_status() says why.
int  kl_app_guest_state(void);
// Before the app ends itself on being backgrounded: ask a guest that has a quit
// of its own to take it. 1 means a request went out and the guest should be
// given a moment — it ends the process itself when it is done.
int  kl_app_guest_request_quit(void);
// ...and before that, for a game that is being watched for a stop with every
// thread waiting: who waits on what, written to the log. A player ends such a
// run by leaving it, so this is the only moment that sees it.
void kl_app_exit_report(void);
// ...and whether it has: its main has returned.
int  kl_app_guest_quit_done(void);
// Did the guest's loop end because the game itself asked to quit — a Unity
// guest's nativeRender returning false, where that is taken for the end of the
// run (KL_UNITY_QUIT_EXITS; see guest_thread)? Then there is nothing left to
// show and the app is to leave, as Android finishes the activity.
int  kl_app_guest_asked_quit(void);

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
// The target name even BEFORE kl_app_configure runs (g_target is nil until then):
// the build's compile-time default, honouring a KL_TARGET override. Lets the
// launcher UI identify itself on the first render, not only after a boot.
const char *kl_app_target_name_or_default(void);
int         kl_app_target_is_steamlink(void);
// Whether this target should open the immersive space FULLY immersive by default
// (no passthrough) rather than .mixed. True for the native OpenXR VR kind
// (GTA Vice City), which is an in-world game meant to fill the view — .mixed
// leaves it a window in passthrough. KL_FULL in the environment overrides.
int         kl_app_target_wants_full(void);

// ...and the question the COMPOSITOR is really asking, which is not the same
// one. Three of the four doors here hand the frame loop to the guest: Steam
// Link spawns a thread inside onCreate and runs OpenXR on it, a UE4 guest is a
// NativeActivity that spawns its own game thread inside android_main, and an
// OpenJK guest spawns its render thread inside its own onCreate. For all of
// those `kl_app_frame()` returns -1 by design and what our thread owes the
// guest is a turning looper. Only a Unity guest hands us a nativeRender to
// call.
//
// It was spelled `is_steamlink` at all three of its call sites, which is a
// predicate that answers the right thing for the wrong reason and stops being
// right the moment a second such guest exists — RE4 is that guest, and the
// symptom is a black immersive space and a pacing hypothesis.
int         kl_app_target_owns_frame_loop(void);

// The 2D -> VR handoff, from the Swift side's point of view.
//
// Steam Link's 2D shell pairs with a host, and when the host authorizes it
// calls SteamLink.startVRLink(sArgs) and finishes itself. On the host that is a
// re-exec into the OpenXR front door; an app bundle cannot re-exec, so
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

// Absolute path of the staged asset tree (<container>/<tree>/assets), valid
// after kl_app_configure. The compositor reads the loading videos from here.
const char *kl_app_assets_dir(void);

// Absolute path of the log kl_app_boot writes, valid after kl_app_configure.
// The Swift side displays it and offers it for export.
const char *kl_app_log_path(void);

// Human-readable status of the last call, for the UI. Never NULL.
const char *kl_app_status(void);

// While kl_app_boot is compiling a pipeline list ahead of the game ("preparing
// graphics"): returns 1 and how many pipelines are done of how many. 0 at any
// other time.
int kl_app_prepare_progress(unsigned *done, unsigned *total);

#endif
