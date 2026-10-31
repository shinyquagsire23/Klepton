// kl_sdl2 — see kl_sdl2.h. Modelled on kl_slink's SDL3 door (the sequence is
// SDL's contract with Android and a property of the guest, not ours to invent),
// with the SDL soname at 2 and the entry library/function taken from the target.
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "klepton.h"
#include "kl_sdl2.h"
#include "kl_native.h"
#include "kl_jni.h"
#include "kl_egl.h"
#include "kl_env.h"
extern const char *kl_driver_target_name(void);
#include "kl_openxr.h"
#include "kl_env.h"

#define SDLA "org/libsdl/app/SDLActivity"

static char  g_libdir[1024];
static char  g_entry[256];        // "libxash.so" / "liblauncher.so"
static char  g_mainfn[64];        // "SDL_main" / "LauncherMainAndroid"
static char  g_error[256];
static FILE *g_out;

static int sdl2_fail(const char *msg) {
    snprintf(g_error, sizeof g_error, "%s", msg);
    return -1;
}
const char *kl_sdl2_error(void) { return g_error[0] ? g_error : "(none)"; }

// Set when the guest's main has returned: for an engine that leaves through
// SDL_QUIT that is the end of its shutdown.
static int g_main_returned;

int kl_sdl2_configure(const char *libdir, const char *entry_lib, FILE *out) {
    if (!libdir || !entry_lib || !*entry_lib) return sdl2_fail("no library directory / entry");
    // Absolute, whatever was handed in. On a device this is a path inside the
    // bundle already; a host run names the tree relative to where it was started
    // ("cs1/lib/arm64-v8a"), and Xash changes directory to its game folder before
    // it loads its renderer and game libraries by that same name — after which
    // none of them is found ("Can't initialize any renderer").
    static char abs_libdir[1024];
    if (libdir[0] != '/' && realpath(libdir, abs_libdir)) libdir = abs_libdir;
    snprintf(g_libdir, sizeof g_libdir, "%s", libdir);
    snprintf(g_entry,  sizeof g_entry,  "%s.so", entry_lib);
    // The SDL natives register on org.libsdl.app.SDLActivity regardless of which
    // subclass the app declares, so that is the class the JNI surface answers.
    kl_jni_set_activity_class(SDLA);
    // ApplicationInfo.nativeLibraryDir and dlopen-by-name resolve here — Source's
    // launcher builds "<nativeLibraryDir>/filesystem_stdio.so" and dies on a NULL
    // dir, so point it at the guest's real lib directory.
    kl_jni_set_native_lib_dir(libdir);
    // SDL 2.0.x's Android HIDAPI (SDL_hid_init -> PLATFORM_hid_init, run from
    // SDL_JoystickInit and gated only on SDK>=18) dereferences a Java-side
    // HIDDeviceManager Klepton never sets up, so it segfaults the instant the
    // joystick subsystem inits. Report SDK 17 THROUGH THE SYSPROP ONLY (JNI
    // Build.VERSION.SDK_INT stays 29) so that gate skips HID entirely; VR input
    // comes through OpenXR, not SDL HID. setenv overwrite=0 lets a real env win.
    setenv("KL_SDK_INT", "17", 0);
    kl_engine_setenv(libdir, entry_lib, out);   // XASH3D_* / EXECUTABLE_PATH the guest getenv's
    return kl_native_configure(libdir, entry_lib, out) == 0
             ? 0 : sdl2_fail(kl_native_error());
}

// libSDL2's JNI_OnLoad registers the SDLActivity/Audio/Controller natives. The
// shared loader runs the ENTRY library's JNI_OnLoad (if any); SDL's lives in a
// dependency, so run it explicitly here — exactly as the SDL3 door does.
static int sdl2_onload(FILE *out) {
    char path[1024];
    snprintf(path, sizeof path, "%s/libSDL2.so", g_libdir);
    kl_image *sdl = kl_find_image(path);
    if (!sdl) return sdl2_fail("libSDL2.so is not in the image registry");
    int (*onload)(void *, void *) = (int (*)(void *, void *))kl_sym(sdl, "JNI_OnLoad");
    if (!onload) return sdl2_fail("libSDL2.so exports no JNI_OnLoad");
    kl_jni_local_frame_push();
    int version = onload(kl_jni_vm(), NULL);
    kl_jni_local_frame_pop();
    if (out) { fprintf(out, "  libSDL2 JNI_OnLoad returned 0x%08x\n", version); fflush(out); }
    if (version != 0x00010004 && version != 0x00010006)
        return sdl2_fail("libSDL2.so JNI_OnLoad did not return a JNI version we serve");
    return 0;
}

int kl_sdl2_load(FILE *out) {
    g_out = out;
    if (kl_native_load(out) != 0) return sdl2_fail(kl_native_error());
    return sdl2_onload(out);
}

unsigned kl_sdl2_gap(FILE *out) { return kl_native_gap(out); }

static void panel_size(int *w, int *h) {
    *w = 2064; *h = 2208;                        // a per-eye-ish default
    const char *e = getenv("KL_SDL2_SIZE");
    if (e) sscanf(e, "%dx%d", w, h);
}

// SDL's contract: nativeInitMainThread, then nativeRunMain(lib, fn, args) on a
// background thread, then nativeCleanupMainThread. The three are resolved on the
// UI thread and handed over (see kl_slink for why).
static void *g_slot[3];

static void *sdl2_thread(void *arg) {
    (void)arg;
    kl_thread_init();                            // seed the stack-guard canary
    FILE *out = g_out;
    void *env = kl_jni_env(), *cls = kl_jni_class(SDLA);

    if (g_slot[0]) {
        if (out) { fprintf(out, "  [sdl2] nativeInitMainThread()\n"); fflush(out); }
        ((void (*)(void *, void *))g_slot[0])(env, cls);
    }

    char libpath[1024];
    snprintf(libpath, sizeof libpath, "%s/%s", g_libdir, g_entry);

    char        argbuf[1024];
    const char *argv[32];
    int         argc = 0;
    // Per-target default argv, prepended so KL_SDL2_ARGS still wins.
    const char *t = kl_driver_target_name();
    const char *defarg = "";
    // cs1 is a COUNTER-STRIKE port but Xash defaults to the valve (Half-Life) base
    // game because nothing selects the mod — the port passes no args, so the engine
    // never gets "-game cstrike", runs in valve, and CS content (scope_arc sprites,
    // maps) is invisible -> "Cannot load Sniper Scope arcs" on LAN create. cstrike
    // has a real liblist.gam; -game selects it. KL_XASH_GAME overrides the dir.
    if (t && strcmp(t, "cs1") == 0) {
        static char g[192];
        const char *gm = getenv("KL_XASH_GAME"); if (!gm || !*gm) gm = "cstrike";
        // KL_CS_NAME sets the player name up front: the visionOS software keyboard
        // attaches to the 2D boot window, not the immersive menu the person is
        // looking at, so raising it to type a name is unreliable. "+name <n>" runs
        // the name cvar at startup so a LAN server can be created without typing.
        // (No spaces — argv is split on whitespace.) Unset => leave the saved name.
        const char *nm = getenv("KL_CS_NAME");
        if (nm && *nm) snprintf(g, sizeof g, "-game %s +name %s ", gm, nm);
        else           snprintf(g, sizeof g, "-game %s ", gm);
        defarg = g;
    }
    // hl2 (HL2Q3VR): the flat Java LauncherActivity builds the engine's command line
    // and hands it to SDLActivity as intent args -> getArguments() -> nativeRunMain(args).
    // Under Klepton the native LauncherMainAndroid runs with NULL args, so that command
    // line was NEVER built and the engine got none of the -vr parms. CVARS still can't ride
    // argv (they go via the synthesized autoexec/launcher cfg, kl_libc.c), but the -vr
    // PARMS do: the mod's VR render-target creation is sized/gated by -vr-combine-capture-size
    // and -vr-water-reflection-size (Quest boot argv: "-game hl2 -vr -vr-foveation 1
    // -vr-combine-capture-size 512 -vr-water-reflection-size 768 -language english"). Without
    // them CreateRenderTargets never runs -> "Eye copy skipped: render target unavailable" ->
    // black (see hl2-quest-reference-trace). Supply the real Quest command line.
    else if (t && strcmp(t, "hl2") == 0) {
        defarg = "-game hl2 -vr -vr-foveation 1 -vr-combine-capture-size 512 "
                 "-vr-water-reflection-size 768 -language english ";
    }
    const char *a = getenv("KL_SDL2_ARGS");
    if (*defarg || (a && *a)) {
        snprintf(argbuf, sizeof argbuf, "%s%s", defarg, (a && *a) ? a : "");
        for (char *tok = strtok(argbuf, " \t");
             tok && argc < (int)(sizeof argv / sizeof argv[0]);
             tok = strtok(NULL, " \t"))
            argv[argc++] = tok;
    }
    void *jargs = argc ? kl_jni_new_string_array(argv, argc) : NULL;

    if (out) { fprintf(out, "  [sdl2] nativeRunMain(\"%s\", \"%s\", %s)\n",
                       libpath, g_mainfn, argc ? "[args]" : "NULL"); fflush(out); }
    kl_jni_local_frame_push();
    int rc = ((int (*)(void *, void *, void *, void *, void *))g_slot[1])(
        env, cls, kl_jni_new_string(libpath), kl_jni_new_string(g_mainfn), jargs);
    kl_jni_local_frame_pop();
    if (out) { fprintf(out, "  [sdl2] nativeRunMain returned %d\n", rc); fflush(out); }
    __atomic_store_n(&g_main_returned, 1, __ATOMIC_RELEASE);

    if (g_slot[2]) {
        if (out) { fprintf(out, "  [sdl2] nativeCleanupMainThread()\n"); fflush(out); }
        ((void (*)(void *, void *))g_slot[2])(kl_jni_env(), kl_jni_class(SDLA));
    }
    return NULL;
}

static void *sdl2_want(FILE *out, const char *name) {
    void *p = kl_jni_native(SDLA, name, NULL);
    if (out) fprintf(out, "  %-28s %s\n", name, p ? "ok" : "NOT REGISTERED");
    return p;
}

int kl_sdl2_begin(FILE *out) {
    g_out = out;

    // The main function: SDL_main for a plain SDL app (cs1), or the name the
    // app's SDLActivity.getMainFunction() overrides to (Source: LauncherMainAndroid).
    // Read it off the entry library rather than trusting a guess.
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_libdir, g_entry);
    kl_image *entry = kl_find_image(path);
    if (!entry) return sdl2_fail("the entry library is not in the image registry");
    const char *cands[] = { "SDL_main", "LauncherMainAndroid" };
    g_mainfn[0] = 0;
    for (size_t i = 0; i < sizeof cands / sizeof *cands; i++)
        if (kl_sym(entry, cands[i])) { snprintf(g_mainfn, sizeof g_mainfn, "%s", cands[i]); break; }
    if (!g_mainfn[0]) return sdl2_fail("the entry library exports neither SDL_main nor LauncherMainAndroid");
    if (out) { fprintf(out, "  [sdl2] entry %s %s\n", g_entry, g_mainfn); fflush(out); }

    // nativeSetupJNI on each SDL manager class — each caches its own jclass and
    // method ids, and skipping one fails much later and elsewhere (see kl_slink).
    static const char *const SETUP[] = {
        "org/libsdl/app/SDLActivity",
        "org/libsdl/app/SDLAudioManager",
        "org/libsdl/app/SDLControllerManager",
    };
    for (size_t i = 0; i < sizeof SETUP / sizeof *SETUP; i++) {
        void *fn = kl_jni_native(SETUP[i], "nativeSetupJNI", NULL);
        if (out) { fprintf(out, "  %s.nativeSetupJNI %s\n", SETUP[i], fn ? "" : "NOT REGISTERED"); fflush(out); }
        if (!fn) continue;
        kl_jni_local_frame_push();
        ((void (*)(void *, void *))fn)(kl_jni_env(), kl_jni_class(SETUP[i]));
        kl_jni_local_frame_pop();
    }

    void *setres = sdl2_want(out, "nativeSetScreenResolution");
    void *surfcr = sdl2_want(out, "onNativeSurfaceCreated");
    void *resize = sdl2_want(out, "onNativeResize");
    g_slot[0] = sdl2_want(out, "nativeInitMainThread");
    g_slot[1] = sdl2_want(out, "nativeRunMain");
    g_slot[2] = sdl2_want(out, "nativeCleanupMainThread");
    if (!g_slot[1]) return sdl2_fail("no nativeRunMain — cannot reach the guest's main");

    void *env = kl_jni_env(), *cls = kl_jni_class(SDLA);
    int w, h; panel_size(&w, &h);
    if (setres) {
        // SDL2's signature is (surfaceW, surfaceH, deviceW, deviceH, rate) —
        // FIVE args, one float (SDL3 added the density float; do not pass it here).
        if (out) { fprintf(out, "  nativeSetScreenResolution(%d,%d, %d,%d, 90.0)\n", w, h, w, h); fflush(out); }
        kl_jni_local_frame_push();
        ((void (*)(void *, void *, int, int, int, int, float))setres)(env, cls, w, h, w, h, 90.0f);
        kl_jni_local_frame_pop();
    }
    if (resize) { kl_jni_local_frame_push(); ((void (*)(void *, void *))resize)(env, cls); kl_jni_local_frame_pop(); }
    if (surfcr) { kl_jni_local_frame_push(); ((void (*)(void *, void *))surfcr)(env, cls); kl_jni_local_frame_pop(); }

    // sdl2_thread IS the guest's main thread — nativeRunMain never returns and
    // the whole engine runs on it. The Source engine expects a desktop/Android-
    // main-sized stack there: its VGUI (libGameUI) and vstdlib build strings and
    // panel layout on large stack buffers, and hl2 overflowed Apple's 512 KB
    // default pthread stack while loading the menu — a KERN_PROTECTION_FAILURE on
    // the guard page only ~20 frames deep, i.e. exhaustion, not runaway recursion.
    // Android's main thread is ~8 MB; 16 MB gives comfortable headroom and costs
    // only address space until touched.
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)16 * 1024 * 1024);
    int rc = pthread_create(&th, &attr, sdl2_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return sdl2_fail("pthread_create for the SDL thread failed");
    pthread_detach(th);
    return 0;
}

int kl_sdl2_main_returned(void) { return __atomic_load_n(&g_main_returned, __ATOMIC_ACQUIRE); }

static void *sdl2_quit_thread(void *arg) {
    (void)arg;
    extern void kl_thread_init(void);
    kl_thread_init();                 // guest code runs on this thread
    void *fn = kl_jni_native(SDLA, "nativeSendQuit", NULL);
    if (fn) {
        kl_jni_local_frame_push();
        ((void (*)(void *, void *))fn)(kl_jni_env(), kl_jni_class(SDLA));
        kl_jni_local_frame_pop();
    }
    return NULL;
}
int kl_sdl2_send_quit(void) {
    if (!kl_jni_native(SDLA, "nativeSendQuit", NULL)) return 0;
    pthread_t th;
    if (pthread_create(&th, NULL, sdl2_quit_thread, NULL) != 0) return 0;
    pthread_detach(th);
    return 1;
}

double kl_sdl2_pump(double seconds, const volatile int *quit) {
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    double elapsed = 0;
    struct timespec iv = { 0, 100 * 1000 * 1000 };
    for (unsigned t = 0; (quit ? !*quit : 1) && (seconds < 0 || elapsed < seconds); t++) {
        nanosleep(&iv, NULL);
        if ((t + 1) % 10 == 0) kl_jni_drain_ui_tasks();
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - t0.tv_sec) + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
    }
    return elapsed;
}

void kl_sdl2_report(FILE *out) {
    if (!out) return;
    kl_openxr_report(out);
    kl_egl_report(out);
    fprintf(out, "\n=== JNI surface ===\n");
    kl_jni_report(out);
    fflush(out);
}
