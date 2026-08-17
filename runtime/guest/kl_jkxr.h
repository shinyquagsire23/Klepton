// JKXR — Team Beef's OpenJK-based VR port of Jedi Outcast and Jedi Academy, as
// a thing two drivers can both open.
//
// Same rationale as kl_ue4.h and kl_slink.h: the host driver (`build/m_boot`)
// and the visionOS app both need to start this guest, and a guest described
// differently by two drivers is a bug with no error surface. What the guest IS
// lives here — the chain, the front door, where its data goes; the policy —
// recon phases, the viewer, the frame pump's shape — stays with the driver.
//
// **A THIRD SHAPE OF DOOR.** A Unity guest is entered through libmain's
// JNI_OnLoad and then driven by CALLING natives it registered. A UE4 guest is a
// NativeActivity: it is handed an `ANativeActivity`, fills in a callback table
// and spawns its own thread. This one is neither. `com.drbeef.jkxr.
// GLES3JNIActivity` is a plain Activity implementing SurfaceHolder.Callback,
// and the nine natives it calls are declared on a separate class
// (`GLES3JNILib`) and STATICALLY EXPORTED by the engine as
// `Java_com_drbeef_jkxr_GLES3JNILib_*`. So there is nothing to register and
// nothing to enumerate: the driver stands in for the Java by calling those
// exports in the Activity's own order, and the `jlong` the first one returns is
// the argument to every one after it.
//
// Everything BELOW the door is shared, which is the reason the target is cheap:
// the looper, the Choreographer, EGL/ANGLE, the OpenSL sink and the OpenXR
// runtime are the same code Steam Link and the Unity guests reach.
//
// ONE APK, TWO GAMES. The APK ships both engines — libopenjk_ja (Academy) and
// libopenjk_jo (Outcast) — with identical exports, and on Android the Java
// picks between them by reading `/sdcard/JKXR/commandline.txt` and appending
// the result to "openjk_". Here the choice is the TARGET ROW's, and everything
// that follows from it (the renderer, the game module, the data directory, the
// command line the engine parses) is derived from the one token in the entry
// library's name so that the row and the file the guest reads cannot disagree.
#ifndef KL_JKXR_H
#define KL_JKXR_H

#include <stdio.h>

// Point the runtime at this guest, stage its data and describe the app to the
// JNI surface. Paths come from the target row (kl_target_apply_host has already
// run); `entry_lib` is that row's entry ("libopenjk_ja"), and the token after
// the underscore decides which game this run is.
//
// `out` takes the staging census and any warning that changes what the run can
// do — chiefly whether the retail game data is present, which is the one thing
// here that this project cannot supply. Returns 0, or non-zero with
// kl_jkxr_error() set.
int kl_jkxr_configure(const char *libdir, const char *entry_lib, FILE *out);

// Why the last call that returned non-zero did. Never NULL.
const char *kl_jkxr_error(void);

// Load the chain (libgl4es, then the engine) and run the engine's JNI_OnLoad.
int kl_jkxr_load(FILE *out);

// The runtime's view of the shim gap once the whole chain is up: which of the
// engine's imports are still unresolved. Returns the count; `out` may be NULL.
unsigned kl_jkxr_gap(FILE *out);

// GLES3JNIActivity.onCreate's half — the native call that builds the engine and
// hands back the handle everything else is keyed on.
int kl_jkxr_create(FILE *out);

// onStart / onResume / surfaceCreated / surfaceChanged, in the order the
// Activity calls them. The surface is where the engine stops waiting: it is
// what ANativeWindow_fromSurface answers on, and the render thread blocks until
// one arrives.
void kl_jkxr_start(FILE *out);

// onPause / onSurfaceDestroyed / onStop, the same sequence unwound.
void kl_jkxr_stop(FILE *out);

// Turn the looper, the UI task queue and the frame clock for `seconds`
// (negative = until `quit`). The engine owns its own render thread, so this is
// the Android side of the process rather than the frame loop. Returns the
// elapsed seconds. `quit` may be NULL.
double kl_jkxr_pump(double seconds, const volatile int *quit);

void kl_jkxr_report(FILE *out);

// The accessory-haptics census, owned by the JNI family rather than by this
// file (runtime/jni/kl_jni_jkxr.c). Declared here because kl_jkxr_report is
// what calls it and kl_jni_int.h does not leave the jni/ directory.
void klj_jkxr_report(FILE *out);

#endif
