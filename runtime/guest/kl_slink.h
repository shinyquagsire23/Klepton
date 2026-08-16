// The Steam Link target, as a thing two drivers can both open.
//
// Everything here used to live in mains/m_slink.c, which was fine while there
// was exactly one way to run this guest: a command line on macOS. There are two
// now — `build/m_slink` and the visionOS app bundle — and they agree about
// nothing except the guest, so the guest's own description is what has to be
// shared. Which libraries are in the chain, which front door a run opens, what
// the JNI surface says the app is, and what Android's NativeActivity does to
// start it: all of that is a property of *Steam Link*, not of whoever is
// driving it.
//
// What deliberately does NOT live here is anything that decides *policy*: the
// recon phases, KL_GAP_ONLY, the fork/re-exec, the viewer, the 2D->VR handoff
// handler. Those are the driver's, and the two drivers make different choices
// about every one of them (the app bundle never forks, has no argv, and cannot
// re-exec — see kl_slink_vr_pump's note on who owns the loop).
//
// The three front doors, which are not a sequence:
//
//   CLIENT  libmain.so — the streaming client. SDL3. Its picture IS a decoded
//           video stream, so it draws nothing without a Steam host.
//   SHELL   libshell_arm64-v8a.so — the 2D configuration frontend, Qt6 on
//           Valve's own `qvirtual` QPA. Has pixels of its own; this is the one
//           that pairs.
//   VR      libvrlink_scene.so — the OpenXR NativeActivity. Not SDL3 at all,
//           and the chain is one guest library against system libraries we
//           shim. This is the one that streams.
#ifndef KL_SLINK_H
#define KL_SLINK_H

#include <stddef.h>
#include <stdio.h>

typedef enum {
    KL_SLINK_CLIENT = 0,
    KL_SLINK_SHELL  = 1,
    KL_SLINK_VR     = 2,
} kl_slink_door;

// KL_SLINK_VR / KL_SLINK_SHELL, resolved once. They are mutually exclusive and
// VR wins if both are set; the caller is expected to say so, because a run whose
// log does not name its front door produces a record that cannot be merged with
// any other.
kl_slink_door kl_slink_door_from_env(void);

// Point the runtime at this guest and describe the app to the JNI surface.
//
//   door    which front door this run opens — decides the chain, the entry
//           point, the activity class and the <meta-data>.
//   libdir  "<tree>/lib/arm64-v8a", the directory every chain path is built
//           from and the string ApplicationInfo.nativeLibraryDir reports.
//   assets  "<tree>/assets" — the ASSETS directory, not the tree root. Both
//           asset doors resolve relative paths against it, and the tree root
//           kl_guest_path_map needs is derived from it.
//   apk     the apk file itself; getPackageCodePath() hands it over and the
//           guest opens it as a zip.
//   files   a writable root for getFilesDir()/internalDataPath.
//
// Also sets the panel size (KL_SLINK_SIZE) before anything can bring ANGLE up —
// doing that from eglCreateWindowSurface is too late and silently so.
//
// `out` takes warnings that are not fatal but change what the run can do (the
// shell's Qt plugin path is the one that exists today), or NULL for silence.
//
// Returns 0, or non-zero if the door cannot be opened with this tree (the VR
// door needs steamlink-vr, which is the only one carrying libvrlink_scene.so);
// kl_slink_error() then says why.
int kl_slink_configure(kl_slink_door door, const char *libdir, const char *assets,
                       const char *apk, const char *files, FILE *out);

// Why the last call that returned non-zero did. Never NULL.
const char *kl_slink_error(void);

// The flat panel this run presents, which three things have to agree about or
// SDL reads a display that contradicts itself: nativeSetScreenResolution, the
// ANativeWindow geometry SDL fetches through getNativeSurface, and ANGLE's own
// surface size. KL_SLINK_SIZE overrides it.
void kl_slink_panel_size(int *w, int *h);

// The working set for the configured door, dependencies first. Reading the
// order off SteamLink.getLibraries() would be wrong: that is System.loadLibrary
// order and Android resolves DT_NEEDED recursively behind each entry, while we
// map and relocate explicitly.
const char *const *kl_slink_chain(size_t *n);
const char *kl_slink_main_lib(void);
const char *kl_slink_main_fn(void);
const char *kl_slink_libdir(void);
const char *kl_slink_door_name(void);

// Map and relocate the whole chain, and register each image under the path the
// next library's imports will resolve against.
//
// Mapping is separated from DT_INIT_ARRAY on purpose: an unresolved import
// aborts by name when it is CALLED, and the first init array calls one — so
// running inits as we go would stop the run before the gap could be printed,
// and the shim work list would arrive one symbol per rebuild.
//
// `out` takes the per-image report line, or NULL for silence.
int  kl_slink_load_chain(FILE *out);
// Everything still unresolved once the guest libraries have satisfied each
// other. This is the number that matters: t_load on one library reports SDL_*
// as missing because it loads that library alone.
void kl_slink_report_gap(FILE *out);
void kl_slink_run_inits(FILE *out);

// --- the SDL3 front doors (CLIENT and SHELL) --------------------------------
//
// Both of these are SDLActivity apps, so what starts them is SDL3's contract
// with Android and not ours: libSDL3's JNI_OnLoad, SDL.setupJNI()'s three
// natives, and then the surface/resolution calls SDLSurface would have made
// before SDLMain.run spawns mSDLThread. It lives here for the same reason the
// VR door's onCreate does — it is a property of *this guest*, and the visionOS
// app must not describe it differently from `build/m_slink`.
//
// _onload runs libSDL3's JNI_OnLoad and checks the two natives SDLActivity's
// own onCreate calls first, so a chain that bound but registered nothing fails
// here rather than three phases later.
int  kl_slink_sdl_onload(FILE *out);
// SDL.setupJNI() — and it is THREE natives, not one (SDLActivity,
// SDLAudioManager, SDLControllerManager). Each caches its own jclass and method
// ids in file-static C globals, and skipping one surfaces much later as a call
// through a {NULL, NULL} pair with nothing pointing back here.
void kl_slink_sdl_setup(FILE *out);
// The rest of onCreate: the panel geometry, the surface, and mSDLThread — which
// is where the guest's main() runs. Returns 0 if the thread was spawned.
//
// The thread split is not incidental and folding it away would hang: SDL runs
// the guest's main on mSDLThread and pumps events on the UI thread, and
// SDL_main blocks on the event queue. What the caller owes it afterwards is a
// UI thread that keeps draining posted tasks — see kl_slink_sdl_pump.
int  kl_slink_sdl_start_main(FILE *out);
// The UI thread's job while main() runs: drain the posted-task queue on a
// cadence. `seconds` is a DEADLINE in wall time; negative means "until *quit",
// which is the app-bundle shape. `quit` may be NULL. Returns seconds spent.
double kl_slink_sdl_pump(double seconds, const volatile int *quit);

// --- the VR front door ------------------------------------------------------
//
// libvrlink_scene.so is a real NativeActivity: no JNI_OnLoad, no natives to
// register, no SDL. Its whole entry is the one function Android's
// NativeActivity dlsyms, and everything after that is the activity lifecycle.
//
// _create runs ANativeActivity_onCreate and reports how many of the sixteen
// callbacks the guest's glue installed — a glue that returned early leaves the
// table empty, and that reads identically to "it worked" without the count.
// _start is onStart/onResume/onNativeWindowCreated/onWindowFocusChanged.
int  kl_slink_vr_create(FILE *out);
void kl_slink_vr_start(FILE *out);

// Pump this thread's looper.
//
// **The UI thread's job here is not to sleep.** This guest is not
// native_app_glue: it does not run android_main on a thread of its own, it
// hangs a UIThreadCallbackHandler off *this* thread's looper and expects
// Looper.loop() to be turning. Sleeping instead leaves it with one live thread
// blocked in read() and no sign of a problem anywhere, because nothing is wrong
// except that nobody is pumping.
//
// `seconds` is a DEADLINE in wall time, not a pump count — kl_ndk_pump_looper
// returns as soon as it has work, so counting iterations measures the guest's
// busyness rather than time. A negative
// deadline means "until *quit becomes non-zero", which is the app-bundle shape:
// there the run ends when the immersive space does, not on a timer.
// `quit` may be NULL. Returns the seconds actually spent.
double kl_slink_vr_pump(double seconds, const volatile int *quit);

// Media, audio, XR and GL, in that order, then the JNI surface.
//
// Every one of these otherwise reports only through kl_fault.c, and a VR run
// that works never takes that path — so the run that most needs these numbers
// was the one printing none of them. They go BEFORE the JNI surface, which is
// thousands of lines: at the bottom of that they are unfindable, and worse,
// indistinguishable from not having printed at all.
void kl_slink_report(FILE *out);

#endif
