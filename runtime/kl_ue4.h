// The Unreal Engine 4 target, as a thing two drivers can both open.
//
// Same rationale as kl_slink.h: the host driver (`build/m_boot`) and the
// visionOS app will both need to start this guest, and a guest described
// differently by two drivers is a bug with no error surface. So which library
// the chain starts at, what the JNI surface says the app is, and what Android's
// NativeActivity does to start it live here; the policy — recon phases, the
// viewer, the frame pump's shape — stays with the driver.
//
// **This is the first non-Unity ENGINE in the tree** (RE4 is UE 4.25.3), and
// the door is the reason it needs a file at all. A Unity guest is entered
// through libmain's JNI_OnLoad and then driven by CALLING natives it registered
// — `UnityPlayer.nativeRender` is the frame. A UE4 guest is a NativeActivity:
// it is handed an `ANativeActivity`, fills in a callback table, spawns its own
// game thread inside `android_main`, and from then on the driver's job is to
// deliver lifecycle and pump the looper rather than to call a render function.
// kl_nativeactivity.c is that door, shared with Steam Link's VR front end.
//
// What is NOT different, and is the whole reason this target is worth having:
// everything below the door. The looper, the Choreographer, EGL/ANGLE, the
// audio sink, the OVRPlugin seam and the compositor are the same code a Unity
// guest reaches, and until now they had only ever been asked for by Unity.
#ifndef KL_UE4_H
#define KL_UE4_H

#include <stdio.h>

// Point the runtime at this guest and describe the app to the JNI surface.
// Paths come from the target row (kl_target_apply_host has already run); what
// this adds is the part that is UE4's rather than the tree's — the activity
// class the guest will ask about by name.
//
// `out` takes warnings that change what the run can do, or NULL for silence.
// Returns 0, or non-zero with kl_ue4_error() set.
int kl_ue4_configure(const char *libdir, FILE *out);

// Why the last call that returned non-zero did. Never NULL.
const char *kl_ue4_error(void);

// Load the chain and run the guest's `JNI_OnLoad`.
//
// There is no libmain here and no loader shim to drive: UE4 links its engine,
// its game and its plugins into ONE object (172 MB for RE4) and the manifest
// names it with `android.app.lib_name`. Its DT_NEEDED pulls libc++_shared, the
// Oculus audio spatializer, the platform loader and libplaycore; anything else
// (Bink's two libraries) is dlopen'd by the guest when it wants it.
//
// Returns 0 on success. `out` gets the load report.
int kl_ue4_load(FILE *out);

// What the entry library still has unbound after the chain is up — the M4 work
// list, and the most valuable thing a first run of a new target produces.
// Returns the number of unique names.
unsigned kl_ue4_gap(FILE *out);

// ...and then the NativeActivity sequence: onCreate, onStart, onResume, the
// window, focus. Split from the load so a driver can report the gap in between,
// which is the first thing worth knowing about a new target.
int kl_ue4_create(FILE *out);
void kl_ue4_start(FILE *out);

// Deliver lifecycle and turn the looper for `seconds` (negative = forever),
// stopping early if `*quit` goes non-zero. Returns the seconds actually spent.
//
// This is the shape a NativeActivity guest wants: it runs its own game thread,
// so the driver's loop is a pump rather than a render call.
double kl_ue4_pump(double seconds, const volatile int *quit);

// The way back out, for a clean shutdown rather than an exit.
void kl_ue4_stop(FILE *out);

// Everything this target's subsystems have to say, in one place.
void kl_ue4_report(FILE *out);

#endif
