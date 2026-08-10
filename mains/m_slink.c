// SL-1 gate: the second target (Steam Link, SDL3) enters the guest.
//
// The shape differs from t_boot in exactly the ways PLANNING §11 predicted, and
// it is worth being explicit about them because they are what this target buys:
//
//   - The chain is SEVEN libraries, not one that dlopens another. libmain.so
//     DT_NEEDEDs libSDL3, libSDL3_ttf, libSDL3_image, the two bitstream helpers
//     and libc++_shared, and it binds 250 imports against them at RELOCATION
//     time. So there is no NativeLoader-style staged load here — the whole set
//     has to be mapped, dependencies first, before libmain's own relocations
//     can resolve. kl_load_recursive is exactly that, and it already existed.
//   - JNI_OnLoad lives in libSDL3.so, not libmain.so. libmain exports SDL_main
//     and seven Java_com_valvesoftware_* natives and nothing else JNI-shaped.
//   - The natives are a MIX: SDL3 registers its own dynamically from
//     JNI_OnLoad (like Unity), while Steam Link's seven are static Java_*
//     exports resolved by name (unlike Unity). Both halves are checked below.
//
// Phase 1 is the assertion: the chain maps, relocates and runs its init arrays.
// Phase 2 is the JNI gate: libSDL3's JNI_OnLoad runs against our synthetic
// JavaVM and registers the SDLActivity surface. Phase 3 is reconnaissance —
// it drives what SDLActivity.onCreate drives, and stops by name wherever the
// shim ends. As in t_boot, that stop IS the measurement.
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../runtime/klepton.h"
#include "../runtime/kl_jni.h"
#include "../runtime/kl_ndk.h"
#include "../runtime/kl_fault.h"
#include "../runtime/kl_glfb.h"
#include "../runtime/kl_view.h"
#include "../runtime/kl_mediandk.h"
#include "../runtime/kl_aaudio.h"
#include "../runtime/kl_openxr.h"
#include "../runtime/kl_egl.h"
#include "../runtime/kl_slink.h"

static const char *LIBDIR = "steamlink-android/lib/arm64-v8a";

typedef int (*jni_onload_fn)(void *vm, void *reserved);

// Set when the viewer's window closes; the phase-4 wait loop watches it so the
// guest is not torn down while a window is still showing its output.
static volatile int g_view_quit;

static int fail(const char *msg) { fprintf(stderr, "FAIL: %s\n", msg); return 1; }

// Which front door, resolved once in slink_run() before anything reads it. The
// chains, the entry points and everything the JNI surface has to say about this
// app moved to runtime/kl_slink.c when the visionOS app became the second
// driver — see that file's header comment for why the split falls where it
// does. What stays here is policy: the recon phases, KL_GAP_ONLY, the fork, the
// viewer and the 2D->VR handoff.
static kl_slink_door g_door;
#define g_shell (g_door == KL_SLINK_SHELL)
#define g_vr    (g_door == KL_SLINK_VR)

// ---------------------------------------------------------------- phase 4 --
// SDLActivity.onCreate's sequence, read out of the APK's own bytecode rather
// than from SDL's upstream source, because Steam Link overrides three of the
// hooks that decide it (see SL_MAIN_LIB below).
//
//   SDLSurface.surfaceChanged -> nativeSetScreenResolution(IIIIFF)
//                             -> onNativeResize()
//   SDLSurface.surfaceCreated -> onNativeSurfaceCreated()
//   SDLMain.run [on mSDLThread] -> nativeInitMainThread()
//                               -> SDLActivity.main() -> nativeRunMain(lib, fn, args)
//                               -> nativeCleanupMainThread()
//
// The thread split is not incidental and folding it away would hang: SDL runs
// the guest's main on mSDLThread and pumps events on the UI thread, and
// SDL_main blocks on the event queue. Same shape as the HandlerThread lesson
// in M4 — the guest blocks waiting for a thread we declined to create.
#define SL_SDLA "org/libsdl/app/SDLActivity"

typedef void (*v_env_cls)(void *, void *);

static void *sdl_thread_body(void *arg) {
    void **slot = (void **)arg;
    // Trap 1: any thread that runs guest code seeds bionic's stack-guard canary
    // into TSD slot 5 first. This thread runs the whole of SDL_main.
    kl_thread_init();

    void *env = kl_jni_env(), *cls = kl_jni_class(SL_SDLA);

    if (slot[0]) {
        printf("  [sdl] nativeInitMainThread()\n");
        fflush(NULL);
        ((v_env_cls)slot[0])(env, cls);
    }

    const char *lib = getenv("KL_SLINK_LIB"); if (!lib) lib = kl_slink_main_lib();
    const char *fn  = getenv("KL_SLINK_FN");  if (!fn)  fn  = kl_slink_main_fn();

    // SDL dlopens this by path and dlsyms the function out of it. The path has
    // to be the one the image is REGISTERED under or kl_dl's dlopen will try to
    // map a second copy: kl_jni_set_native_lib_dir() and the chain loader above
    // both use LIBDIR, so build it the same way. (Trap 6c: absolute would be
    // more Android-like, but the registry key is what must match.)
    char libpath[1024];
    snprintf(libpath, sizeof libpath, "%s/%s", LIBDIR, lib);

    // argv. SDL3's nativeRunMain takes a String[] and prepends argv[0] itself,
    // so this array is argv[1..]. The real activity fills it from the launching
    // intent's "sArgs" extra (§11.9) and libmain PARSES IT — --server, --appid,
    // --steamid, --transport and ~40 more. Passing NULL is not "no options
    // chosen", it is the streaming client being asked to stream nothing, which
    // is why it reaches OnStreamError before opening a single socket.
    char  argbuf[2048];
    const char *argv[64];
    int   argc = 0;
    const char *a = getenv("KL_SLINK_ARGS");
    if (a && *a) {
        snprintf(argbuf, sizeof argbuf, "%s", a);
        for (char *tok = strtok(argbuf, " \t");
             tok && argc < (int)(sizeof argv / sizeof argv[0]);
             tok = strtok(NULL, " \t"))
            argv[argc++] = tok;
    }
    void *jargs = argc ? kl_jni_new_string_array(argv, argc) : NULL;

    printf("  [sdl] nativeRunMain(\"%s\", \"%s\", %s", libpath, fn,
           argc ? "[" : "NULL");
    for (int i = 0; i < argc; i++) printf("%s\"%s\"", i ? ", " : "", argv[i]);
    printf("%s)\n", argc ? "])" : ")");
    fflush(NULL);

    kl_jni_local_frame_push();
    int rc = ((int (*)(void *, void *, void *, void *, void *))slot[1])(
        env, cls, kl_jni_new_string(libpath), kl_jni_new_string(fn), jargs);
    kl_jni_local_frame_pop();

    printf("  [sdl] nativeRunMain returned %d\n", rc);
    fflush(NULL);

    if (slot[2]) {
        printf("  [sdl] nativeCleanupMainThread()\n");
        fflush(NULL);
        ((v_env_cls)slot[2])(kl_jni_env(), kl_jni_class(SL_SDLA));
    }
    return NULL;
}

// Look one up and say so. A native SDL3 did not register is a different failure
// from one that aborts when called, and conflating them wastes a run.
static void *want(const char *name) {
    void *p = kl_jni_native(SL_SDLA, name, NULL);
    printf("  %-28s %s\n", name, p ? "ok" : "NOT REGISTERED");
    return p;
}

static void run_main_sequence(kl_image *sdl_img, kl_image *entry_img) {
    (void)sdl_img; (void)entry_img;
    printf("\n=== phase 4: onCreate -> SDL_main (KL_SLINK_MAIN) ===\n");
    fflush(NULL);

    void *setres  = want("nativeSetScreenResolution");
    void *surfcr  = want("onNativeSurfaceCreated");
    void *resize  = want("onNativeResize");
    void *initmt  = want("nativeInitMainThread");
    void *runmain = want("nativeRunMain");
    void *cleanmt = want("nativeCleanupMainThread");
    if (!runmain) { printf("  no nativeRunMain — cannot reach SDL_main\n"); return; }

    void *env = kl_jni_env(), *cls = kl_jni_class(SL_SDLA);

    // A plausible flat window. These are the numbers SDLSurface would have read
    // off the Display; nothing downstream has been shown to care yet, so they
    // are deliberately ordinary rather than a Quest's panel.
    int w, h;
    kl_slink_panel_size(&w, &h);

    if (setres) {
        printf("  nativeSetScreenResolution(%d,%d, %d,%d, 2.0, 60.0)\n", w, h, w, h);
        fflush(NULL);
        kl_jni_local_frame_push();
        ((void (*)(void *, void *, int, int, int, int, float, float))setres)(
            env, cls, w, h, w, h, 2.0f, 60.0f);
        kl_jni_local_frame_pop();
    }
    if (resize) { printf("  onNativeResize()\n"); fflush(NULL);
                  kl_jni_local_frame_push(); ((v_env_cls)resize)(env, cls); kl_jni_local_frame_pop(); }
    if (surfcr) { printf("  onNativeSurfaceCreated()\n"); fflush(NULL);
                  kl_jni_local_frame_push(); ((v_env_cls)surfcr)(env, cls); kl_jni_local_frame_pop(); }

    // ...and now the SDL thread, which is where SDL_main lives.
    void *slot[3] = { initmt, runmain, cleanmt };
    pthread_t th;
    if (pthread_create(&th, NULL, sdl_thread_body, slot) != 0) {
        printf("  pthread_create for the SDL thread failed\n");
        return;
    }

    // The UI thread's job while SDL_main runs. There is no looper to pump yet —
    // this is the recon shape, and what it measures is how far SDL_main gets
    // before it blocks on something we have not built. KL_ALARM is the backstop.
    //
    // Under the viewer there is no deadline: SDL_main runs for as long as the
    // window is open, and the loop ends when the user closes it. A fixed wait
    // there would tear the guest down mid-frame a few seconds after it started.
    unsigned secs = 0;
    const char *lim = getenv("KL_SLINK_WAIT");
    unsigned maxs = lim ? (unsigned)atoi(lim) : 10;
    int windowed = getenv("KL_VIEW") != NULL;
    struct timespec ts = { 0, 100 * 1000 * 1000 };
    for (unsigned t = 0; windowed ? !g_view_quit : t < maxs * 10; t++) {
        nanosleep(&ts, NULL);
        if (++secs % 10 == 0) { kl_jni_drain_ui_tasks(); }
    }
    if (windowed) { printf("  (window closed)\n"); fflush(NULL); return; }
    printf("  (UI thread waited %us; SDL thread not joined — see above for where it got to)\n", maxs);
    fflush(NULL);
}

// ------------------------------------------------- the 2D -> VR handoff, run --
//
// SteamLink.startVRLink(String) is the end of the shell's job: it has paired,
// the host has authorized, and it now starts the VR activity with the session
// as `sArgs` and calls finishAndRemoveTask() on itself. Both halves exist here
// now (SL-9 onward), so there is no reason for a person to carry the string
// between two runs by hand — but they are two front doors with different
// chains, different libraries and different lifecycles, and the shell's `main`
// is on the stack of the thread making this call.
//
// So: re-EXEC, which is what Android's own model already is — a new activity in
// a fresh task, the old one finishing. It replaces the process image, so every
// question about tearing the shell down (Qt, SDL, ANGLE's contexts, the audio
// unit, the shell's threads) simply does not arise, and it is the mechanism
// this project already uses for the recon child. The exec'd process reads the
// session out of the environment through the same KL_SLINK_SARGS path a pasted
// one uses, so nothing downstream can tell the difference.
//
// Only knobs that are UNSET are filled in, and every one is announced. Two of
// them are not cosmetic:
//
//   KL_OVRP_IPD   — SL-12: on a host run nothing measures the eye offsets, so
//                   they are 0, and this client reads an IPD of zero as
//                   "unchanged" and never sends the host its projection. With
//                   no picture at all as the alternative, the documented host
//                   stopgap is the right default here — but it IS a stopgap,
//                   and saying so out loud is the difference between a default
//                   and a fabrication.
//   KL_SLINK_WAIT — the VR path's deadline, 10 s by default, which is not worth
//                   the pairing that just paid for it.
//
// KL_SLINK_HANDOFF=0 restores the old behaviour (print the session, abort by
// name), which is what the `make slink-shell` gate uses: that gate measures the
// shell, and a run that re-execs into a different front door measures something
// else.
static const char *g_argv0 = "./build/m_slink";

static void slink_setenv_default(const char *k, const char *v, const char *why) {
    if (getenv(k)) return;
    setenv(k, v, 1);
    printf("    %s=%s   (%s)\n", k, v, why);
}

static void slink_vrlink_handoff(const char *sargs) {
    if (!kl_env_on("KL_SLINK_HANDOFF", 1)) {
        printf("  (KL_SLINK_HANDOFF=0 — not entering the VR front door)\n");
        return;                      // the caller aborts by name, as before
    }

    printf("\n=== 2D -> VR handoff: re-exec into the OpenXR front door ===\n");
    printf("    %s %s\n", g_argv0, LIBDIR);
    setenv("KL_SLINK_SARGS", sargs, 1);
    setenv("KL_SLINK_VR", "1", 1);
    unsetenv("KL_SLINK_SHELL");      // g_shell would otherwise win the tie
    unsetenv("KL_VIEW_POKE");        // a click script for the SHELL's UI, and
                                     // replaying it into the VR view would be
                                     // synthetic input nobody asked for
    slink_setenv_default("KL_SLINK_MAIN", "1",
                         "drive the lifecycle on into android_main");
    slink_setenv_default("KL_GLFB", "1",
                         "the null GL driver cannot present a stream");
    slink_setenv_default("KL_OVRP_IPD", "0.063",
                         "HOST STOPGAP: an IPD of 0 stops the host sending video");
    if (!getenv("KL_VIEW"))
        slink_setenv_default("KL_SLINK_WAIT", "45",
                             "the VR deadline; 10 s would waste the pairing");
    fflush(NULL);

    execl(g_argv0, g_argv0, LIBDIR, (char *)NULL);

    // Only reachable if exec failed. Returning is the contract for "could not
    // do the handoff" and the caller then aborts by name with the session
    // printed above, so the run is no worse off than before.
    printf("  !! exec(\"%s\") failed: %s\n", g_argv0, strerror(errno));
    fflush(NULL);
}

// ------------------------------------------------------------- VR front door --
// The ANativeActivity ABI, the onCreate call and the looper pump all live in
// runtime/kl_slink.c now — they are Android's contract with this guest, not
// this driver's. What is left here is what a *command-line* run does with them:
// where it stops (KL_SLINK_MAIN), how long it waits (KL_SLINK_WAIT) and that a
// windowed run waits on the window instead.
static void run_vr_sequence(void) {
    printf("\n=== phase 2: ANativeActivity_onCreate (the VR front door) ===\n");
    fflush(NULL);
    if (kl_slink_vr_create(stdout) != 0) {
        printf("  %s\n", kl_slink_error());
        return;
    }

    if (!getenv("KL_SLINK_MAIN")) {
        printf("  (KL_SLINK_MAIN unset: stopping after onCreate)\n");
        return;
    }

    printf("\n=== phase 3: the activity lifecycle ===\n");
    kl_slink_vr_start(stdout);

    // KL_SLINK_WAIT is the deadline; what it measures is how far the guest gets
    // once its callbacks can actually run. Under the viewer there is no
    // deadline — the window is what ends the run.
    unsigned maxs = getenv("KL_SLINK_WAIT") ? (unsigned)atoi(getenv("KL_SLINK_WAIT")) : 10;
    int windowed = getenv("KL_VIEW") != NULL;
    double elapsed = kl_slink_vr_pump(windowed ? -1.0 : (double)maxs,
                                      windowed ? &g_view_quit : NULL);
    printf("  (UI thread waited %.1fs of %us — see above for where the guest got to)\n",
           elapsed, maxs);
    fflush(NULL);
}

static int slink_run(void) {
    // Trap 1, and it bit immediately: every thread that runs guest code must
    // seed bionic's stack-guard canary into TSD slot 5 first. Without it the
    // slot holds whatever Darwin last left there, libSDL3's JNI_OnLoad copies
    // that at entry, something writes the slot during the 68 RegisterNatives
    // calls, and the epilogue reports "stack smashing detected" — a real
    // canary mismatch with no buffer overflow anywhere near it.
    kl_thread_init();
    kl_fault_install();
    kl_jni_set_permissive(getenv("KL_PERMISSIVE") != NULL);
    g_door = kl_slink_door_from_env();
    if (kl_env_on("KL_SLINK_VR", 0) && kl_env_on("KL_SLINK_SHELL", 0))
        printf("(KL_SLINK_VR and KL_SLINK_SHELL are different front doors; "
               "taking VR)\n");
    // Only the shell can reach startVRLink, and only it should be able to hand
    // off — installing this unconditionally would let a client or VR run take a
    // path neither of them has any business on.
    if (g_shell) kl_jni_set_vrlink_handoff(slink_vrlink_handoff);

    // Which APK this LIBDIR came out of. It was hardcoded to the old one, which
    // was harmless while nothing read an asset and wrong the moment something
    // did: the shell loads assets/config/{default,ui,hmd,controller}_config.json
    // at startup, and those exist only in the VR tree.
    int vr_apk = strstr(LIBDIR, "steamlink-vr") != NULL;
    const char *apk = vr_apk ? "steamlink-vr.apk" : "steamlink-android.apk";
    // The ASSETS directory, not the unpacked tree root — kl_jni_set_assets_dir
    // wants "<tree>/assets" (that is what the Beat Saber default is), and both
    // asset doors resolve relative paths against it. The tree root that
    // kl_guest_path_map needs is derived from it by stripping the last
    // component, so this stays right for both.
    const char *assets = vr_apk ? "steamlink-vr/assets" : "steamlink-android/assets";

    // Everything the guest is told about itself: the activity class, the four
    // paths, the <meta-data>, Qt's plugin path and the panel size. Shared with
    // the visionOS app, which describes the same guest and must not describe it
    // differently — runtime/kl_slink.c.
    if (kl_slink_configure(g_door, LIBDIR, assets, apk,
                           "build/steamlink-files", stdout) != 0) {
        fprintf(stderr, "%s\n", kl_slink_error());
        return 1;
    }

    // Say which front door this run opened, before anything can go wrong. The
    // two chains fail in different places for different reasons, and a log that
    // does not name the mode makes the two records unmergeable.
    printf("=== Steam Link: %s front door (%s -> %s), %s ===\n",
           kl_slink_door_name(), kl_slink_main_lib(), kl_slink_main_fn(), LIBDIR);

    // ---- phase 1: the chain ----
    // Mapping and relocation are separated from DT_INIT_ARRAY on purpose, which
    // is why this does not use kl_load_recursive (that does all three per
    // library). An unresolved import aborts by name when it is CALLED, and the
    // first init array calls one — so running inits as we go would stop the run
    // before the gap could be printed, and the whole shim work list would arrive
    // one symbol per rebuild. Relocating everything first makes the list a
    // single measurement. Same reasoning as KL_PERMISSIVE on the JNI side.
    printf("=== phase 1: mapping the working set ===\n");
    if (kl_slink_load_chain(stdout) != 0) return fail("chain load");
    kl_slink_report_gap(stdout);

    if (getenv("KL_GAP_ONLY")) { printf("\n(KL_GAP_ONLY: stopping before init)\n"); return 0; }

    printf("\n=== phase 1b: DT_INIT_ARRAY, dependencies first ===\n");
    fflush(NULL);
    kl_slink_run_inits(stdout);

    // The entry library, whichever front door this run opened. In shell mode
    // libmain.so is not in the chain at all — it is not a dependency of the
    // shell, it is what the shell launches once a host has been picked.
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", LIBDIR, kl_slink_main_lib());
    kl_image *entry_img = kl_find_image(path);

    // The VR front door diverges here and never rejoins. Everything below is
    // SDL3's — JNI_OnLoad, RegisterNatives, SDLActivity.onCreate — and
    // libvrlink_scene has none of it: no libSDL3 in its DT_NEEDED, no
    // JNI_OnLoad export, no natives to register. Its whole entry is the one
    // function Android's NativeActivity dlsyms.
    if (g_vr) {
        if (!entry_img) return fail("libvrlink_scene.so not in the registry");
        run_vr_sequence();
        // The VR path has its own exit and had never reported the video, audio,
        // XR or GL surfaces — which between them ARE the VR half of the app, so
        // the run that most needs those numbers was the one that printed none
        // of them. Every one of these is reported on the abort path through
        // kl_fault.c, and a VR run that works does not take it. An empty report
        // reads as "nothing happened"; no report at all reads the same way and
        // takes longer to disbelieve (SL-13 lost a run to exactly that).
        kl_slink_report(stdout);
        return 0;
    }

    snprintf(path, sizeof path, "%s/libSDL3.so", LIBDIR);
    kl_image *sdl_img = kl_find_image(path);
    if (!entry_img || !sdl_img) return fail("the entry library / libSDL3.so not in the registry");

    // ---- phase 2: the JNI gate ----
    // JNI_OnLoad is libSDL3's, and it is a versioned symbol (JNI_OnLoad@@SDL3_0.0.0).
    // The version lives in a separate table; the .dynstr name is plain, so an
    // ordinary lookup finds it.
    printf("\n=== phase 2: libSDL3.so JNI_OnLoad ===\n");
    fflush(NULL);
    jni_onload_fn onload = (jni_onload_fn)kl_sym(sdl_img, "JNI_OnLoad");
    if (!onload) return fail("libSDL3.so exports no JNI_OnLoad");

    kl_jni_local_frame_push();
    int version = onload(kl_jni_vm(), NULL);
    kl_jni_local_frame_pop();
    printf("  JNI_OnLoad returned 0x%08x\n", version);
    // SDL3 asks for JNI_VERSION_1_4, not Unity's 1_6 — read off its own
    // epilogue (`mov w0,#4; movk w0,#1,lsl#16`), not assumed. Both are versions
    // we satisfy; what matters is that it did not return a negative error.
    if (version != 0x00010004 && version != KL_JNI_VERSION_1_6)
        return fail("JNI_OnLoad did not return a JNI version we serve");

    // SDL3 registers its natives from JNI_OnLoad the way Unity does. These
    // three are the ones SDLActivity.onCreate calls first, through
    // SDL.setupJNI(), so their absence would stop phase 3 immediately.
    const char *SDLA = "org/libsdl/app/SDLActivity";
    void *setup = kl_jni_native(SDLA, "nativeSetupJNI", NULL);
    void *runmain = kl_jni_native(SDLA, "nativeRunMain", NULL);
    printf("  %s.nativeSetupJNI=%p nativeRunMain=%p\n", SDLA, setup, runmain);

    // ...and Steam Link's own seven are STATIC exports, so they resolve by
    // name off libmain rather than through RegisterNatives. Checking both
    // halves here is what makes "the natives are a mix" an assertion instead
    // of a note. They live in the streaming client only; the shell has no
    // video surface to hand anyone, so this half of the assertion is the
    // client chain's.
    static const char *const STATIC_NATIVES[] = {
        "Java_com_valvesoftware_steamlink_SteamLink_useVideoSurface",
        "Java_com_valvesoftware_steamlink_SteamLink_videoSurfaceCreated",
        "Java_com_valvesoftware_steamlink_SteamLink_videoSurfaceDestroyed",
        "Java_com_valvesoftware_steamlink_SteamLink_overlaySurfaceCreated",
        "Java_com_valvesoftware_steamlink_SteamLink_overlaySurfaceDestroyed",
        "Java_com_valvesoftware_steamlink_SteamLink_freezeRendering",
        "Java_com_valvesoftware_steamlink_SteamLink_thawRendering",
    };
    if (!g_shell) {
        unsigned nstatic = 0;
        for (size_t i = 0; i < sizeof STATIC_NATIVES / sizeof *STATIC_NATIVES; i++)
            if (kl_sym(entry_img, STATIC_NATIVES[i])) nstatic++;
        printf("  libmain static Java_* natives resolved: %u/%zu\n",
               nstatic, sizeof STATIC_NATIVES / sizeof *STATIC_NATIVES);
    }

    // The entry point itself, looked up the way SDL's nativeRunMain will look it
    // up. Failing here rather than three phases later is the difference between
    // "the guest exports no such symbol" and "something went wrong in main".
    void *entry = kl_sym(entry_img, kl_slink_main_fn());
    printf("  %s %s=%p\n", kl_slink_main_lib(), kl_slink_main_fn(), entry);
    if (!entry) return fail("the entry library does not export its entry point");

    if (!setup || !runmain)
        return fail("SDL3 did not register the SDLActivity natives");

    printf("\n=== SL-1 EXIT CRITERION MET: chain bound, SDL3 JNI_OnLoad ran ===\n");

    // ---- phase 3: reconnaissance ----
    // SDLActivity.onCreate calls SDL.setupJNI() first, which caches the
    // activity class and every method id SDL3 will ever call back through.
    // It is the densest single JNI call in the app and therefore the right
    // place for the surface to start failing by name.
    printf("\n=== recon: SDL.setupJNI() ===\n");
    fflush(NULL);
    // SDL.setupJNI() calls THREE natives, not one — SDLActivity's, then
    // SDLAudioManager's, then SDLControllerManager's — and each caches its own
    // jclass and its own method ids in file-static globals on the C side.
    // Calling only the first leaves the other two sets NULL, and the failure
    // surfaces much later and somewhere else: the audio backend calls a cached
    // (class, methodID) pair that is still {NULL, NULL} and it presents as
    // "Call*Method with a jmethodID we never issued", with no hint that a setup
    // call was skipped. Cost one debugging round; do not trim this list.
    static const char *const SETUP_CLASSES[] = {
        "org/libsdl/app/SDLActivity",
        "org/libsdl/app/SDLAudioManager",
        "org/libsdl/app/SDLControllerManager",
    };
    for (size_t i = 0; i < sizeof SETUP_CLASSES / sizeof *SETUP_CLASSES; i++) {
        void *fn = kl_jni_native(SETUP_CLASSES[i], "nativeSetupJNI", NULL);
        printf("  %s.nativeSetupJNI %s\n", SETUP_CLASSES[i], fn ? "" : "NOT REGISTERED");
        if (!fn) continue;
        fflush(NULL);
        kl_jni_local_frame_push();
        // The jclass is the second argument, and this one is load-bearing: SDL3
        // takes a global ref to it and resolves every static method it will ever
        // call against it. NULL here reads as "GetStaticMethodID on
        // <unknown-jclass>" in the report and would be a dangling class later.
        ((void (*)(void *, void *))fn)(kl_jni_env(), kl_jni_class(SETUP_CLASSES[i]));
        kl_jni_local_frame_pop();
    }
    printf("  setupJNI returned\n");

    // ---- phase 4: the rest of onCreate, to SDL_main ----
    // Behind a knob for the same reason KL_LIFECYCLE gates t_boot: SL-1 is a
    // gate that must stay green, and everything below here is expected to stop
    // by name until the shim catches up. `make slink` keeps measuring phases
    // 1-3; KL_SLINK_MAIN=1 is the working loop.
    // KL_VIEW implies it: opening a window for a guest whose main never runs
    // would show a permanently black rectangle and read as a rendering bug.
    if (getenv("KL_SLINK_MAIN") || getenv("KL_VIEW"))
        run_main_sequence(sdl_img, entry_img);

    // The video and audio paths report on the abort path through kl_fault.c,
    // which a clean exit never takes — and a clean exit is exactly the run
    // where "did anything actually decode?" is the question (SL-11).
    //
    // BEFORE the JNI surface, which is thousands of lines: these few are the
    // answer the run was for, and at the bottom of that they are unfindable —
    // and worse, indistinguishable from not having printed at all (SL-13).
    kl_mediandk_report(stdout);
    kl_aaudio_report(stdout);
    printf("\n=== JNI surface ===\n");
    kl_jni_report(stdout);
    return 0;
}

// ---------------------------------------------------------------- KL_VIEW --
// The viewer is not a debugging aid for this target the way it is for Beat
// Saber — Steam Link is a FLAT app, so an SDL window is its actual output
// device. The guest runs on a spawned thread and SDL owns the main thread,
// because macOS requires windowing there.
//
// No fork and no re-exec: a windowed app never forks, and Metal's XPC shader
// compiler refuses forked children anyway (the AGX story in CLAUDE.md).
//
// The frame path is the READBACK sink, deliberately. The hardware compositor
// (kl_view_mtl.m) samples eye MTLTextures keyed by (eye, stage) out of kl_ovrp,
// and a flat guest has none of that: no ovrp, no eye textures, no stages. Its
// picture is the default framebuffer of an EGL window surface. Reading that
// back needs no Metal interop at all, which is why it works today — giving the
// mono path a zero-copy route means backing the window surface with an
// EGLImage-bound MTLTexture, the same interop the eye textures use (§12.9).
static void *slink_view_guest(void *arg) {
    (void)arg;
    kl_thread_init();
    slink_run();
    return NULL;
}

static int slink_view(void) {
    if (!kl_env_on("KL_GLFB", 0)) {
        fprintf(stderr, "KL_VIEW=1 requires KL_GLFB=1 — the null GL driver "
                        "draws nothing, so the window would be black\n");
        return 1;
    }
    if (!kl_view_available()) {
        fprintf(stderr, "KL_VIEW=1 but m_slink was built without SDL3\n");
        return 1;
    }
    // The sink has to be registered BEFORE the guest reaches its first swap,
    // and kl_glfb only captures at all when a sink or a dump directory is set.
    kl_glfb_set_frame_sink(kl_view_frame_sink, NULL);

    pthread_t guest;
    if (pthread_create(&guest, NULL, slink_view_guest, NULL)) {
        fprintf(stderr, "view: pthread_create failed\n");
        return 1;
    }
    int rc = kl_view_main(LIBDIR, 0 /* readback path — see above */);
    g_view_quit = 1;
    // The guest is inside SDL_main and does not return on its own; the process
    // exiting is what ends it. Detach rather than join so closing the window
    // closes the app, which is what a window close is supposed to mean.
    pthread_detach(guest);
    return rc;
}

int main(int argc, char **argv) {
    if (argc > 1) LIBDIR = argv[1];
    if (argc > 0 && argv[0] && *argv[0]) g_argv0 = argv[0];
    kl_set_library_path(LIBDIR);

    if (getenv("KL_VIEW")) return slink_view();

    if (getenv("KL_RECON_CHILD")) return slink_run();

    // Re-exec'd, not forked, for the reason in t_boot: Metal's shader compiler
    // is an XPC service that refuses forked children. Nothing here touches
    // Metal yet, but this target is heading straight for video decode, and
    // VideoToolbox is service-backed the same way.
    if (getenv("KL_NOFORK")) return slink_run();

    printf("=== recon: spawning child ===\n");
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        setenv("KL_RECON_CHILD", "1", 1);
        execl(argv[0], argv[0], LIBDIR, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st) || WEXITSTATUS(st) != 0) {
        if (WIFSIGNALED(st))
            printf("\n  (recon stopped on signal %d — %s)\n", WTERMSIG(st),
                   strsignal(WTERMSIG(st)));
        else
            printf("\n  (recon child exited %d)\n", WEXITSTATUS(st));
        return 1;
    }
    return 0;
}
