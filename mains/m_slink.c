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

static const char *LIBDIR = "steamlink-android/lib/arm64-v8a";

// The activity Steam Link actually declares. SDL3 caches this class in
// nativeSetupJNI and every callback it makes goes through it, so getting it
// wrong is not cosmetic — it is the class every GetStaticMethodID resolves in.
static const char *ACTIVITY = "com/valvesoftware/steamlink/SteamLink";

typedef int (*jni_onload_fn)(void *vm, void *reserved);

// Set when the viewer's window closes; the phase-4 wait loop watches it so the
// guest is not torn down while a window is still showing its output.
static volatile int g_view_quit;

static int fail(const char *msg) { fprintf(stderr, "FAIL: %s\n", msg); return 1; }

// The working set, dependencies first.
//
// There are TWO of these, because the app has two front doors and they are not
// a sequence (§11.9). Which one is loaded decides what the run can possibly
// show, so it is selected once, here, and everything downstream reads
// slink_chain()/slink_main_lib().
//
//   CHAIN_CLIENT  libmain.so — the streaming client. Its picture IS a decoded
//                 video stream, so it draws nothing at all until a Steam host
//                 is streaming to it (§11.11, measured: zero swaps ever).
//   CHAIN_SHELL   libshell_arm64-v8a.so — the 2D configuration frontend, and
//                 the app's own `getMainSharedObject()` answer. This one has
//                 pixels of its OWN: Qt Widgets, drawn locally, no host needed.
//
// §11.2 ruled the shell out of scope on cost, and that was right while the
// question was "which is the cheapest first window". It is the wrong answer to
// a different question — "what can draw without a Steam host on the LAN" — and
// that is the question now, because the client answered it with "nothing".
static const char *const CHAIN_CLIENT[] = {
    "libc++_shared.so",
    "libSDL3.so",
    "libSDL3_ttf.so",
    "libSDL3_image.so",
    "libh264bitstream.so",
    "libhevcbitstream.so",
    "libmain.so",
};

// SteamLink.getLibraries() lists fourteen, but that list is System.loadLibrary
// order and Android resolves DT_NEEDED recursively behind each entry. We map
// and relocate explicitly, so the order here is DEPENDENCIES FIRST — read off
// libshell's own DT_NEEDED (SDL3 ×4, Qt6 Widgets/Svg/Gui/Network/Core, the two
// bitstream helpers, libc++_shared) rather than off the Java.
//
// libSDL3_mixer and libsteamwebrtc are in getLibraries() and NOT in libshell's
// DT_NEEDED; they are loaded anyway because the Java side loads them and the
// shell may dlopen them later. libmain.so is deliberately absent — it is not a
// dependency of the shell, it is what the shell launches once you pick a host.
static const char *const CHAIN_SHELL[] = {
    "libc++_shared.so",
    "libSDL3.so",
    "libSDL3_image.so",
    "libSDL3_mixer.so",
    "libSDL3_ttf.so",
    "libQt6Core_arm64-v8a.so",
    "libQt6Network_arm64-v8a.so",
    "libQt6Gui_arm64-v8a.so",
    "libQt6Widgets_arm64-v8a.so",
    "libQt6Svg_arm64-v8a.so",
    "libh264bitstream.so",
    "libhevcbitstream.so",
    "libsteamwebrtc.so",
    "libshell_arm64-v8a.so",
};

// KL_SLINK_SHELL=1 picks the frontend. Set once in slink_run() before anything
// reads it, so the two accessors below cannot disagree.
static int g_shell;

static const char *const *slink_chain(size_t *n) {
    if (g_shell) { *n = sizeof CHAIN_SHELL / sizeof *CHAIN_SHELL; return CHAIN_SHELL; }
    *n = sizeof CHAIN_CLIENT / sizeof *CHAIN_CLIENT;
    return CHAIN_CLIENT;
}

static void report_image(const char *soname, kl_image *img) {
    const kl_stats *st = kl_get_stats(img);
    unsigned total = st->relative + st->abs64 + st->glob_dat + st->jump_slot;
    // The load address is printed because `sample <pid>` is the diagnostic that
    // keeps paying off on this target and it reports guest frames as
    // "??? (in <unknown binary>)". With the base here, one subtraction plus
    // llvm-nm turns a wall of hex into CShellApplication::BInit. Fourteen
    // libraries makes guessing which image an address belongs to impractical.
    printf("  %-26s @%p %7.2f MB  reloc %-7u tls %-5u x18 %u/%u",
           soname, kl_base(img), kl_span(img) / 1048576.0, total,
           st->tls_rewrites, st->x18_patched, st->x18_sites);
    if (st->x18_refused) printf(" (refused %u)", st->x18_refused);
    // Trap 0b: words that decode as x18 sites but sit in data. Named here
    // because the number is how the second detector is watched — silence would
    // make a change of mind about what is code invisible.
    if (st->x18_data_words) printf(" (data %u)", st->x18_data_words);
    printf("  imports %u bound / %u unresolved\n",
           st->imports_bound, st->imports_missing);
}

// Everything still unresolved once the guest libraries have satisfied each
// other. This is the number that matters: t_load run on one library reports
// SDL_* as missing because it loads that library alone, and reading those
// lists as shim gaps would send the whole session chasing symbols the guest
// already provides.
static void report_gap(void) {
    printf("\n=== shim gap: unresolved after cross-binding ===\n");
    unsigned grand = 0;
    size_t nchain;
    const char *const *chain = slink_chain(&nchain);
    for (size_t i = 0; i < nchain; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", LIBDIR, chain[i]);
        kl_image *img = kl_find_image(path);
        if (!img) continue;
        unsigned nm = 0;
        const char *const *miss = kl_missing_imports(img, &nm);
        if (!nm) continue;
        grand += nm;
        printf("  %s: %u\n    ", chain[i], nm);
        // Three per line at 36 columns, with a guaranteed space: these names run
        // to 35 characters (AMediaCodec_releaseOutputBufferAtTime) and a %-28s
        // silently joins two of them into one unsearchable token.
        for (unsigned k = 0, shown = 0; k < nm; k++, shown++) {
            if (shown && shown % 3 == 0) printf("\n    ");
            printf("%-36s ", miss[k]);
        }
        printf("\n");
    }
    if (!grand) printf("  (none — every import bound)\n");
    else printf("\n  %u unique unresolved names across the chain\n", grand);
}

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

// The entry point, per front door. Both are what the APK's own bytecode says:
// SteamLink.getMainSharedObject()/getMainFunction() return "libshell_<abi>.so"
// and "main" (verified in the smali, not assumed), and SDL's inherited defaults
// are "libmain.so" / "SDL_main".
//
// "a whole QtAndroid JNI surface" was the reason §11.2 priced the shell out, and
// on the OLD apk that was true — steamlink-android/ ships Qt5 with
// libplugins_platforms_qtforandroid, the stock Android QPA, whose whole job is
// to talk to QtNative/QtSurface over JNI. The VR apk does NOT: it ships Qt6 with
// **libplugins_platforms_qvirtual_arm64-v8a.so**, a QPA plugin of Valve's own
// (`QVirtualIntegrationPlugin`), and its DT_NEEDED is Qt6Gui, Qt6Core, GLESv2,
// EGL, log, z, m, c++_shared, dl, c — **no libandroid and no JNI at all**.
// libshell holds the other half of it (`QVirtualPlatformDelegate`,
// `QVirtualWindowDelegate`) and sets QT_QPA_PLATFORM itself. So Qt here renders
// into a backing store Valve owns, and the expensive part of §11.2's estimate
// does not exist on this build. That is what makes the shell reachable.
//
// KL_SLINK_LIB / _FN still override either.
#define SL_CLIENT_LIB "libmain.so"
#define SL_CLIENT_FN  "SDL_main"
#define SL_SHELL_LIB  "libshell_arm64-v8a.so"
#define SL_SHELL_FN   "main"

static const char *slink_main_lib(void) { return g_shell ? SL_SHELL_LIB : SL_CLIENT_LIB; }
static const char *slink_main_fn(void)  { return g_shell ? SL_SHELL_FN  : SL_CLIENT_FN;  }

typedef void (*v_env_cls)(void *, void *);

// The panel we present, in one place. Three things must agree about it or SDL
// reads a display that contradicts itself (the "group answer" rule): the
// resolution given to nativeSetScreenResolution, the ANativeWindow geometry
// SDL fetches through getNativeSurface, and ANGLE's own surface size.
static void slink_panel_size(int *w, int *h) {
    *w = 1280; *h = 800;
    const char *e = getenv("KL_SLINK_SIZE");
    if (e) sscanf(e, "%dx%d", w, h);
}

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

    const char *lib = getenv("KL_SLINK_LIB"); if (!lib) lib = slink_main_lib();
    const char *fn  = getenv("KL_SLINK_FN");  if (!fn)  fn  = slink_main_fn();

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
    slink_panel_size(&w, &h);

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
    g_shell = kl_env_on("KL_SLINK_SHELL", 0);

    // Which APK this LIBDIR came out of. It was hardcoded to the old one, which
    // was harmless while nothing read an asset and wrong the moment something
    // did: the shell loads assets/config/{default,ui,hmd,controller}_config.json
    // at startup, and those exist only in the VR tree.
    int vr = strstr(LIBDIR, "steamlink-vr") != NULL;
    const char *apk    = vr ? "steamlink-vr.apk" : "steamlink-android.apk";
    const char *assets = vr ? "steamlink-vr"     : "steamlink-android";

    // The JNI surface has to describe THIS app, not Beat Saber. The activity
    // class is the load-bearing one (see above); the paths matter because
    // trap 6c applies to any guest — Android hands out absolute paths and
    // SDL3's SDL_GetBasePath/SDL_GetPrefPath propagate whatever we say.
    kl_jni_set_activity_class(ACTIVITY);
    kl_jni_set_apk_path(apk);
    kl_jni_set_native_lib_dir(LIBDIR);
    kl_jni_set_files_dir("build/steamlink-files");
    // The old APK has no assets/ directory at all — checked, not assumed — so
    // for it this only keeps AAssetManager_open from being a crash, and an
    // absent file is the honest answer. The VR APK does have one, and the shell
    // reads it.
    kl_jni_set_assets_dir(assets);

    // The SDL_ENV.* <meta-data>, transcribed from the APK's own manifest — real
    // behaviour, not decoration (SDL_ANDROID_TRAP_BACK_BUTTON routes the back
    // button; STEAM_LINK_VR is the app's own build discriminator). The two APKs
    // differ by exactly that last entry, which is why the runtime does not carry
    // a baked-in table: it would be wrong for one of them.
    kl_jni_add_manifest_env("SDL_JOYSTICK_HIDAPI", "1");
    kl_jni_add_manifest_env("SDL_TV_REMOTE_AS_JOYSTICK", "0");
    kl_jni_add_manifest_env("SDL_ANDROID_TRAP_BACK_BUTTON", "1");
    kl_jni_add_manifest_env("SDL_ANDROID_ALLOW_RECREATE_ACTIVITY", "1");
    if (vr) kl_jni_add_manifest_env("STEAM_LINK_VR", "1");

    // Qt has to be told where its plugins live, and on Android it always is —
    // Qt's own Android bootstrap sets the library paths from the APK layout
    // before main() runs. We author that side, so this is the faithful
    // equivalent rather than a workaround: without it libshell aborts with
    //   Could not find the Qt platform plugin "virtual" in ""
    // (libshell sets QT_QPA_PLATFORM=virtual itself; only the path is missing).
    //
    // The value is the flat native-library directory, NOT a plugins/ tree:
    // Qt-for-Android globs each search path for `libplugins_<subdir>_*.so`
    // (the format string is in libQt6Core), which is exactly how these are
    // named. Absolute, per trap 6c — the guest chdir()s and a relative path
    // would resolve somewhere else later.
    if (g_shell) {
        char abs[PATH_MAX];
        if (realpath(LIBDIR, abs)) setenv("QT_PLUGIN_PATH", abs, 1);
        else fprintf(stderr, "  [slink] realpath(%s) failed; Qt will not find "
                             "its platform plugin\n", LIBDIR);
    }

    // Say which front door this run opened, before anything can go wrong. The
    // two chains fail in different places for different reasons, and a log that
    // does not name the mode makes the two records unmergeable.
    printf("=== Steam Link: %s front door (%s -> %s), %s ===\n",
           g_shell ? "2D SHELL" : "streaming CLIENT",
           slink_main_lib(), slink_main_fn(), LIBDIR);

    // The panel, published BEFORE anything can bring ANGLE up. Doing this from
    // eglCreateWindowSurface was too late and silently so: the guest touches EGL
    // early enough that kl_glfb_init has already run by then, kl_glfb_set_size
    // is a no-op once it has, and the guest ended up drawing a 1280x800 viewport
    // into a 4000x3200 surface — which the capture then read in full, mostly
    // empty. Nothing errored; the picture was just wrong.
    {
        int pw, ph;
        slink_panel_size(&pw, &ph);
        kl_ndk_set_window(pw, ph, 1 /* RGBA_8888 */);
        kl_glfb_set_size(pw, ph);
    }

    // ---- phase 1: the chain ----
    // Mapping and relocation are separated from DT_INIT_ARRAY on purpose, which
    // is why this does not use kl_load_recursive (that does all three per
    // library). An unresolved import aborts by name when it is CALLED, and the
    // first init array calls one — so running inits as we go would stop the run
    // before the gap could be printed, and the whole shim work list would arrive
    // one symbol per rebuild. Relocating everything first makes the list a
    // single measurement. Same reasoning as KL_PERMISSIVE on the JNI side.
    printf("=== phase 1: mapping the working set ===\n");
    struct timespec t0, t1;
    size_t nchain;
    const char *const *chain = slink_chain(&nchain);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t i = 0; i < nchain; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", LIBDIR, chain[i]);
        kl_image *img = kl_load_auto(path);
        if (!img) { fprintf(stderr, "  %s: %s\n", chain[i], kl_error()); return fail("chain load"); }
        kl_register_image(path, img);      // dependencies first, so the next
        report_image(chain[i], img);       // library's imports bind against it
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("  chain mapped and relocated in %.1f ms\n",
           (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    report_gap();

    if (getenv("KL_GAP_ONLY")) { printf("\n(KL_GAP_ONLY: stopping before init)\n"); return 0; }

    printf("\n=== phase 1b: DT_INIT_ARRAY, dependencies first ===\n");
    fflush(NULL);
    for (size_t i = 0; i < nchain; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", LIBDIR, chain[i]);
        printf("  %s\n", chain[i]);
        fflush(NULL);
        kl_run_init(kl_find_image(path));
    }
    printf("  all init arrays returned\n");

    // The entry library, whichever front door this run opened. In shell mode
    // libmain.so is not in the chain at all — it is not a dependency of the
    // shell, it is what the shell launches once a host has been picked.
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", LIBDIR, slink_main_lib());
    kl_image *entry_img = kl_find_image(path);
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
    void *entry = kl_sym(entry_img, slink_main_fn());
    printf("  %s %s=%p\n", slink_main_lib(), slink_main_fn(), entry);
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
