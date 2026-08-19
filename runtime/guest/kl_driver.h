// The guest run, as the thing both frontends drive.
//
// A target's boot sequence, its lifecycle and its reports are properties of the
// GUEST, not of who is running it: `build/m_boot` on macOS and the visionOS app
// bundle have to describe one guest identically or the two records cannot be
// compared, and the failure has no error surface — the run works and answers the
// guest's questions differently. So the four doors live here and each frontend
// keeps only what is genuinely its own.
//
// What stays with the frontend: process shape (fork, re-exec, the log file, the
// heartbeat), how far to go (KL_LIFECYCLE, KL_SLINK_MAIN, KL_GAP_ONLY), who owns
// the frame clock, the MTLTexture provider, the viewer, the host-only
// instruments (sampler, managed probe, metadata dump) and the 2D->VR handoff
// handler.
#ifndef KL_DRIVER_H
#define KL_DRIVER_H

#include <stdio.h>
#include "kl_slink.h"
#include "kl_target.h"

// Bind the driver to a target before anything else. `libdir` is where the guest
// libraries are, which is not t->libdir on every frontend: the app's is an
// absolute path inside its bundle. `door` is read only by a Steam Link target.
//
// The paths the guest is told about itself (assets, APK, files) are the
// frontend's to apply first — kl_target_apply_host() on the host,
// kl_app_configure() in the bundle — because only it knows where they are.
void kl_driver_init(const kl_target *t, const char *libdir, kl_slink_door door);

// Why the last call that returned non-zero did. Never NULL.
const char *kl_driver_error(void);

// Whether the guest brought its own render thread, and therefore whether there
// is any such thing as an inline frame. Three of the four doors do: Steam Link
// spawns one inside onCreate, an Unreal guest inside android_main, an OpenJK
// guest inside its own onCreate. Only a Unity guest hands over a nativeRender to
// call, and for the others kl_driver_frame() returns -1 by design — what the
// caller owes them is a turning looper (kl_driver_pump).
int kl_driver_owns_frame_loop(void);

// A phase name for a frontend that reports progress out of band (the visionOS
// heartbeat). NULL clears it; the driver never reads it back.
void kl_driver_set_phase_hook(void (*fn)(const char *phase));

// The watchdog window armed around each lifecycle call and each frame, in
// seconds; 0 leaves the alarm alone entirely, which is what a frontend that
// arms one of its own around a whole pump needs. The default is 20. KL_ALARM is
// the frontend's to read.
void kl_driver_set_alarm(unsigned seconds);

// ---- the run ----------------------------------------------------------------

// Load the guest and prove its entry point. What that means per door:
//
//   unity      libmain -> JNI_OnLoad -> NativeLoader.load(libunity) ->
//              UnityPlayer.initJni(Context) and the constructor's helper objects
//   steamlink  the door's chain mapped and relocated, the shim gap, DT_INIT_ARRAY
//              dependencies first, then libSDL3's JNI_OnLoad and SDL.setupJNI()
//              on the two SDL doors
//   ue4/jkxr   the chain, then the shim gap
//
// Stops after the gap report when KL_GAP_ONLY is set — the work list is the most
// valuable thing a new target's run produces and it is worth having when what
// follows dies immediately. kl_driver_gap_only() then says so to the caller,
// which is what keeps it from going on to the lifecycle.
//
// Returns 0, or non-zero with kl_driver_error() set. `out` may be NULL.
int kl_driver_boot(FILE *out);
int kl_driver_gap_only(void);

// Start the guest running: the Unity lifecycle natives (nativeRecreateGfxState,
// nativeResume, nativeRender), ANativeActivity_onCreate plus the activity
// lifecycle for Unreal, GLES3JNILib.onCreate plus the surface calls for OpenJK,
// or the front door's own start for Steam Link.
//
// Once per process, and it MUST run on the thread that will go on to pump: the
// three doors that own their frame loop take ALooper_forThread() inside onCreate
// and hang their callbacks off exactly that looper, so create and pump on
// different threads leaves the guest with callbacks nobody will run and no error
// anywhere.
int kl_driver_lifecycle_begin(FILE *out);

// One Unity frame: the pose latch, nativeRender, the posted-task drain and the
// memory-pressure poll. Returns what nativeRender returned, or -1 if _begin has
// not run or the guest owns its own frame loop.
int kl_driver_frame(void);

// Turn the activity's looper for a guest that owns its frame loop. `seconds` is
// a DEADLINE in wall time; negative means "until *quit", which is the shape a
// frontend with no timer uses. `quit` may be NULL. Returns seconds spent, and 0
// for a Unity guest, which has no looper to turn.
double kl_driver_pump(double seconds, const volatile int *quit);

// The seconds budget this door reads from the environment, and its default:
// KL_SLINK_WAIT / KL_UE4_WAIT / KL_JKXR_WAIT. A frame count means nothing to a
// guest that owns its own loop.
double kl_driver_pump_default(void);

// Frames handed to the guest. The pacing path counts its own (the guest calls
// xrWaitFrame rather than being called), which is what _note_frame is for.
unsigned kl_driver_frames(void);
void     kl_driver_note_frame(void);

// Everything the run has to say: the door's own report, then the media, audio,
// XR and GL surfaces and the JNI census. Each of those otherwise prints only on
// the fatal path, which a working run never takes — so the run that most needs
// the numbers is the one that prints none of them.
void kl_driver_report(FILE *out);

#endif
