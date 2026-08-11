// See kl_slink.h. Moved out of mains/m_slink.c, which had been the only driver
// until the visionOS app became the second one.
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "klepton.h"
#include "kl_slink.h"
#include "kl_jni.h"
#include "kl_ndk.h"
#include "kl_glfb.h"
#include "kl_mono.h"
#include "kl_egl.h"
#include "kl_mediandk.h"
#include "kl_aaudio.h"
#include "kl_openxr.h"
#include "kl_env.h"

// ---------------------------------------------------------------- the chains --
//
// libmain.so DT_NEEDEDs libSDL3, libSDL3_ttf, libSDL3_image, the two bitstream
// helpers and libc++_shared, and binds 250 imports against them at RELOCATION
// time. So there is no NativeLoader-style staged load here — the whole set has
// to be mapped, dependencies first, before libmain's own relocations resolve.
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
// order. This one is DEPENDENCIES FIRST, read off libshell's own DT_NEEDED
// (SDL3 x4, Qt6 Widgets/Svg/Gui/Network/Core, the two bitstream helpers,
// libc++_shared) rather than off the Java.
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

// §11.9 measured libvrlink_scene's DT_NEEDED as libopenxr_loader/libaaudio/
// libmediandk/libandroid/liblog/libEGL/libGLESv3/libm/libdl/libc: no libmain,
// no libSDL3, no libc++_shared, no Qt. So the "chain" is ONE guest library
// against system libraries we shim, which is why this array has a single entry
// and is not a mistake.
//
// libopenxr_loader.so is deliberately ABSENT. It is the Khronos loader and it
// finds a runtime through an Android `org.khronos.openxr.runtime_broker`
// service that does not exist here — exactly libOVRPlugin.so's situation in
// §3.1, and settled the same way: REPLACED, not translated. The xr* names bind
// against kl_openxr.c through kl_shim_lookup instead. Putting the real loader
// in this list would map 300 KB of code that can only fail.
static const char *const CHAIN_VR[] = {
    "libvrlink_scene.so",
};

// The entry point, per front door. Both SDL ones are what the APK's own
// bytecode says: SteamLink.getMainSharedObject()/getMainFunction() return
// "libshell_<abi>.so" and "main" (verified in the smali, not assumed), and
// SDL's inherited defaults are "libmain.so" / "SDL_main".
#define SL_CLIENT_LIB "libmain.so"
#define SL_CLIENT_FN  "SDL_main"
#define SL_SHELL_LIB  "libshell_arm64-v8a.so"
#define SL_SHELL_FN   "main"
// The VR half has no SDL_main-shaped entry. It is a real NativeActivity and the
// manifest says so (`android.app.lib_name = vrlink_scene`), so the entry point
// is the one Android's own NativeActivity.onCreate would dlsym. Checked against
// the library's exports: ANativeActivity_onCreate is there and `android_main`
// is NOT, so whatever glue it uses keeps that symbol internal.
#define SL_VR_LIB     "libvrlink_scene.so"
#define SL_VR_FN      "ANativeActivity_onCreate"

// The activity Steam Link actually declares. SDL3 caches this class in
// nativeSetupJNI and every callback it makes goes through it, so getting it
// wrong is not cosmetic — it is the class every GetStaticMethodID resolves in.
#define SL_ACTIVITY   "com/valvesoftware/steamlink/SteamLink"

static kl_slink_door g_door;
static char g_libdir[1024];
static char g_error[512];

const char *kl_slink_error(void)  { return g_error; }
const char *kl_slink_libdir(void) { return g_libdir; }

kl_slink_door kl_slink_door_from_env(void) {
    if (kl_env_on("KL_SLINK_VR", 0))    return KL_SLINK_VR;
    if (kl_env_on("KL_SLINK_SHELL", 0)) return KL_SLINK_SHELL;
    return KL_SLINK_CLIENT;
}

const char *const *kl_slink_chain(size_t *n) {
    switch (g_door) {
    case KL_SLINK_VR:    *n = sizeof CHAIN_VR    / sizeof *CHAIN_VR;    return CHAIN_VR;
    case KL_SLINK_SHELL: *n = sizeof CHAIN_SHELL / sizeof *CHAIN_SHELL; return CHAIN_SHELL;
    default:             *n = sizeof CHAIN_CLIENT/ sizeof *CHAIN_CLIENT;return CHAIN_CLIENT;
    }
}

const char *kl_slink_main_lib(void) {
    return g_door == KL_SLINK_VR ? SL_VR_LIB
         : g_door == KL_SLINK_SHELL ? SL_SHELL_LIB : SL_CLIENT_LIB;
}
const char *kl_slink_main_fn(void) {
    return g_door == KL_SLINK_VR ? SL_VR_FN
         : g_door == KL_SLINK_SHELL ? SL_SHELL_FN : SL_CLIENT_FN;
}
const char *kl_slink_door_name(void) {
    return g_door == KL_SLINK_VR ? "OpenXR VR"
         : g_door == KL_SLINK_SHELL ? "2D SHELL" : "streaming CLIENT";
}

// The panel we present, in one place. Three things must agree about it or SDL
// reads a display that contradicts itself (the "group answer" rule): the
// resolution given to nativeSetScreenResolution, the ANativeWindow geometry SDL
// fetches through getNativeSurface, and ANGLE's own surface size.
void kl_slink_panel_size(int *w, int *h) {
    *w = 1280; *h = 800;
    const char *e = getenv("KL_SLINK_SIZE");
    if (e) sscanf(e, "%dx%d", w, h);
}

static int slink_fail(const char *msg) {
    snprintf(g_error, sizeof g_error, "%s", msg);
    return 1;
}

int kl_slink_configure(kl_slink_door door, const char *libdir, const char *assets,
                       const char *apk, const char *files, FILE *out) {
    g_door = door;
    g_error[0] = 0;
    snprintf(g_libdir, sizeof g_libdir, "%s", libdir);

    // Which of the two APKs this is, decided from the APK's own name rather than
    // from the library directory. The libdir test that used to be here was a
    // host-path heuristic — `steamlink-vr/lib/arm64-v8a` — and it does not
    // survive the app bundle, where the libdir is a synthetic path under
    // Resources/ that names no APK at all. It failed CLOSED, which was worse
    // than failing loudly: kl_app_configure gave up before it had opened a log,
    // so the run produced no output whatsoever and read as a launch that never
    // reached our code.
    //
    // It matters because the two APKs are genuinely different: the shell loads
    // assets/config/{default,ui,hmd,controller}_config.json at startup, and
    // those exist only in the VR tree.
    int vr_apk = strstr(apk, "steamlink-vr") != NULL;
    if (door == KL_SLINK_VR && !vr_apk)
        return slink_fail("the VR front door needs steamlink-vr.apk — "
                          "libvrlink_scene.so exists in no other build");

    kl_set_library_path(g_libdir);

    // The JNI surface has to describe THIS app, not Beat Saber. The activity
    // class is the load-bearing one (see above); the paths matter because
    // trap 6c applies to any guest — Android hands out absolute paths and SDL3's
    // SDL_GetBasePath/SDL_GetPrefPath propagate whatever we say.
    //
    // ...and the VR front door is a DIFFERENT activity in the same package. The
    // manifest declares the two as peers (§11.9), and this is the one with
    // `category.VR` — Android's stock NativeActivity, whose only app-specific
    // part is the `android.app.lib_name` meta-data.
    kl_jni_set_activity_class(door == KL_SLINK_VR ? "android/app/NativeActivity"
                                                  : SL_ACTIVITY);
    kl_jni_set_apk_path(apk);
    kl_jni_set_native_lib_dir(g_libdir);
    kl_jni_set_files_dir(files);
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
    //
    // Not on the VR door: these are the SDLActivity's <meta-data>, and the VR
    // front door is a different activity that declares only
    // `android.app.lib_name`. Handing them over would be inventing manifest
    // entries the app does not have — and STEAM_LINK_VR in particular is
    // libmain's build discriminator, so a wrong answer there is a wrong branch
    // rather than a dead variable.
    if (door == KL_SLINK_VR) {
        kl_jni_add_manifest_env("android.app.lib_name", "vrlink_scene");
    } else {
        kl_jni_add_manifest_env("SDL_JOYSTICK_HIDAPI", "1");
        kl_jni_add_manifest_env("SDL_TV_REMOTE_AS_JOYSTICK", "0");
        kl_jni_add_manifest_env("SDL_ANDROID_TRAP_BACK_BUTTON", "1");
        kl_jni_add_manifest_env("SDL_ANDROID_ALLOW_RECREATE_ACTIVITY", "1");
        if (vr_apk) kl_jni_add_manifest_env("STEAM_LINK_VR", "1");
    }

    // Qt has to be told where its plugins live, and on Android it always is —
    // Qt's own Android bootstrap sets the library paths from the APK layout
    // before main() runs. We author that side, so this is the faithful
    // equivalent rather than a workaround: without it libshell aborts with
    //   Could not find the Qt platform plugin "virtual" in ""
    // (libshell sets QT_QPA_PLATFORM=virtual itself; only the path is missing).
    //
    // The value is the flat native-library directory, NOT a plugins/ tree:
    // Qt-for-Android globs each search path for `libplugins_<subdir>_*.so` (the
    // format string is in libQt6Core), which is exactly how these are named.
    // Absolute, per trap 6c — the guest chdir()s and a relative path would
    // resolve somewhere else later.
    if (door == KL_SLINK_SHELL) {
        char abs[PATH_MAX];
        if (realpath(g_libdir, abs)) setenv("QT_PLUGIN_PATH", abs, 1);
        else if (out) fprintf(out, "  [slink] realpath(%s) failed; Qt will not "
                                   "find its platform plugin\n", g_libdir);

        // **libQt6Core carries PCRE2 with its JIT, and a JIT cannot run here.**
        //
        // This is the shell's device crash (SL-19), and it is trap 12 in its
        // strongest form: AMFI kills any pc not backed by a signed file, so the
        // moment PCRE2 branches into the ARM64 it just generated the process
        // dies — `EXC_BAD_ACCESS / KERN_PROTECTION_FAILURE`, termination
        // namespace CODESIGNING, "Invalid Page", into a 64 KB anonymous rw-
        // region. No signal a handler can catch, which is why kl_fault.c had
        // nothing to say about it and why the log simply stopped mid-sentence.
        //
        // Trap 26's `mrs CTR_EL0` was this same JIT: compiler-rt's
        // `__clear_cache` is linked into libQt6Core precisely because something
        // writes instructions and flushes them. Veneering it did not fix the
        // crash, it advanced it — from the cache flush to the branch.
        //
        // Qt gives us the switch by name and PCRE2's interpreter is a
        // first-class fallback (`pcre2_jit_compile` failing is an ordinary,
        // handled outcome), so the whole class costs pattern-matching speed and
        // nothing else. Set rather than defaulted, because leaving it to the
        // guest means the crash is back the first time the environment differs.
        setenv("QT_ENABLE_REGEXP_JIT", "0", 1);
        if (out) fprintf(out, "  [slink] QT_ENABLE_REGEXP_JIT=0 — PCRE2's JIT "
                              "writes code into anonymous memory and AMFI will "
                              "not execute it (trap 12); the interpreter is the "
                              "supported fallback\n");
    }

    // The panel, published BEFORE anything can bring ANGLE up. Doing this from
    // eglCreateWindowSurface was too late and silently so: the guest touches EGL
    // early enough that kl_glfb_init has already run by then, kl_glfb_set_size
    // is a no-op once it has, and the guest ended up drawing a 1280x800 viewport
    // into a 4000x3200 surface — which the capture then read in full, mostly
    // empty. Nothing errored; the picture was just wrong.
    int pw, ph;
    kl_slink_panel_size(&pw, &ph);
    kl_ndk_set_window(pw, ph, 1 /* RGBA_8888 */);
    kl_glfb_set_size(pw, ph);
    return 0;
}

// --------------------------------------------------------------- the chain --

static void report_image(FILE *out, const char *soname, kl_image *img) {
    if (!out) return;
    const kl_stats *st = kl_get_stats(img);
    unsigned total = st->relative + st->abs64 + st->glob_dat + st->jump_slot;
    // The load address is printed because `sample <pid>` is the diagnostic that
    // keeps paying off on this target and it reports guest frames as
    // "??? (in <unknown binary>)". With the base here, one subtraction plus
    // llvm-nm turns a wall of hex into CShellApplication::BInit. Fourteen
    // libraries makes guessing which image an address belongs to impractical.
    fprintf(out, "  %-26s @%p %7.2f MB  reloc %-7u tls %-5u x18 %u/%u",
            soname, kl_base(img), kl_span(img) / 1048576.0, total,
            st->tls_rewrites, st->x18_patched, st->x18_sites);
    if (st->x18_refused) fprintf(out, " (refused %u)", st->x18_refused);
    // Trap 0b: words that decode as x18 sites but sit in data. Named here
    // because the number is how the second detector is watched — silence would
    // make a change of mind about what is code invisible.
    if (st->x18_data_words) fprintf(out, " (data %u)", st->x18_data_words);
    // ...and the same test refusing a TLS site, which is the opposite of free:
    // that thread pointer is garbage on every thread (trap 1).
    if (st->tls_refused) fprintf(out, " (TLS REFUSED %u)", st->tls_refused);
    fprintf(out, "  imports %u bound / %u unresolved\n",
            st->imports_bound, st->imports_missing);
}

int kl_slink_load_chain(FILE *out) {
    size_t nchain;
    const char *const *chain = kl_slink_chain(&nchain);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t i = 0; i < nchain; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", g_libdir, chain[i]);
        kl_image *img = kl_load_auto(path);
        if (!img) {
            snprintf(g_error, sizeof g_error, "%s: %s", chain[i], kl_error());
            if (out) fprintf(out, "  %s\n", g_error);
            return 1;
        }
        kl_register_image(path, img);      // dependencies first, so the next
        report_image(out, chain[i], img);  // library's imports bind against it
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (out) fprintf(out, "  chain mapped and relocated in %.1f ms\n",
                     (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    return 0;
}

void kl_slink_report_gap(FILE *out) {
    if (!out) return;
    fprintf(out, "\n=== shim gap: unresolved after cross-binding ===\n");
    unsigned grand = 0;
    size_t nchain;
    const char *const *chain = kl_slink_chain(&nchain);
    for (size_t i = 0; i < nchain; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", g_libdir, chain[i]);
        kl_image *img = kl_find_image(path);
        if (!img) continue;
        unsigned nm = 0;
        const char *const *miss = kl_missing_imports(img, &nm);
        if (!nm) continue;
        grand += nm;
        fprintf(out, "  %s: %u\n    ", chain[i], nm);
        // Three per line at 36 columns, with a guaranteed space: these names run
        // to 35 characters (AMediaCodec_releaseOutputBufferAtTime) and a %-28s
        // silently joins two of them into one unsearchable token.
        for (unsigned k = 0, shown = 0; k < nm; k++, shown++) {
            if (shown && shown % 3 == 0) fprintf(out, "\n    ");
            fprintf(out, "%-36s ", miss[k]);
        }
        fprintf(out, "\n");
    }
    if (!grand) fprintf(out, "  (none — every import bound)\n");
    else fprintf(out, "\n  %u unique unresolved names across the chain\n", grand);
}

void kl_slink_run_inits(FILE *out) {
    size_t nchain;
    const char *const *chain = kl_slink_chain(&nchain);
    for (size_t i = 0; i < nchain; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", g_libdir, chain[i]);
        if (out) { fprintf(out, "  %s\n", chain[i]); fflush(out); }
        kl_run_init(kl_find_image(path));
    }
    if (out) fprintf(out, "  all init arrays returned\n");
}

// ----------------------------------------------------------- SDL front doors --
// SDLActivity.onCreate's sequence, read out of the APK's own bytecode rather
// than from SDL's upstream source, because Steam Link overrides three of the
// hooks that decide it:
//
//   SDLSurface.surfaceChanged -> nativeSetScreenResolution(IIIIFF)
//                             -> onNativeResize()
//   SDLSurface.surfaceCreated -> onNativeSurfaceCreated()
//   SDLMain.run [on mSDLThread] -> nativeInitMainThread()
//                               -> SDLActivity.main() -> nativeRunMain(lib, fn, args)
//                               -> nativeCleanupMainThread()
//
// This moved out of mains/m_slink.c when the visionOS app became the second
// driver: it is SDL3's contract with Android and a property of this guest, so
// two drivers must not be able to describe it differently. What stayed with the
// driver is when to run it and how long to wait afterwards.
#define SL_SDLA "org/libsdl/app/SDLActivity"

typedef void (*v_env_cls)(void *, void *);
typedef int (*jni_onload_fn)(void *vm, void *reserved);

int kl_slink_sdl_onload(FILE *out) {
    char path[1024];
    snprintf(path, sizeof path, "%s/libSDL3.so", g_libdir);
    kl_image *sdl_img = kl_find_image(path);
    if (!sdl_img) return slink_fail("libSDL3.so is not in the image registry");

    // JNI_OnLoad is libSDL3's, and it is a versioned symbol
    // (JNI_OnLoad@@SDL3_0.0.0). The version lives in a separate table; the
    // .dynstr name is plain, so an ordinary lookup finds it.
    jni_onload_fn onload = (jni_onload_fn)kl_sym(sdl_img, "JNI_OnLoad");
    if (!onload) return slink_fail("libSDL3.so exports no JNI_OnLoad");

    kl_jni_local_frame_push();
    int version = onload(kl_jni_vm(), NULL);
    kl_jni_local_frame_pop();
    if (out) fprintf(out, "  JNI_OnLoad returned 0x%08x\n", version);
    // SDL3 asks for JNI_VERSION_1_4, not Unity's 1_6 — read off its own
    // epilogue (`mov w0,#4; movk w0,#1,lsl#16`), not assumed. Both are versions
    // we serve; what matters is that it did not return a negative error.
    if (version != 0x00010004 && version != 0x00010006)
        return slink_fail("libSDL3.so JNI_OnLoad did not return a JNI version we serve");

    // The two SDLActivity.onCreate calls first, through SDL.setupJNI(). Their
    // absence stops everything below and says so here instead.
    void *setup   = kl_jni_native(SL_SDLA, "nativeSetupJNI", NULL);
    void *runmain = kl_jni_native(SL_SDLA, "nativeRunMain", NULL);
    if (out) fprintf(out, "  %s.nativeSetupJNI=%p nativeRunMain=%p\n",
                     SL_SDLA, setup, runmain);
    if (!setup || !runmain)
        return slink_fail("libSDL3.so registered no SDLActivity natives");

    // ...and the entry point itself, looked up the way nativeRunMain will look
    // it up. Failing here rather than on the SDL thread is the difference
    // between "the guest exports no such symbol" and "something went wrong in
    // main".
    snprintf(path, sizeof path, "%s/%s", g_libdir, kl_slink_main_lib());
    kl_image *entry_img = kl_find_image(path);
    if (!entry_img) return slink_fail("the entry library is not in the image registry");
    void *entry = kl_sym(entry_img, kl_slink_main_fn());
    if (out) fprintf(out, "  %s %s=%p\n", kl_slink_main_lib(), kl_slink_main_fn(), entry);
    if (!entry) return slink_fail("the entry library does not export its entry point");
    return 0;
}

void kl_slink_sdl_setup(FILE *out) {
    // THREE natives, not one — SDLActivity's, then SDLAudioManager's, then
    // SDLControllerManager's — and each caches its own jclass and its own
    // method ids in file-static globals on the C side. Calling only the first
    // leaves the other two sets NULL, and the failure surfaces much later and
    // somewhere else: the audio backend calls a cached (class, methodID) pair
    // that is still {NULL, NULL}, and it presents as "Call*Method with a
    // jmethodID we never issued" with no hint that a setup call was skipped.
    // Cost one debugging round; do not trim this list.
    static const char *const SETUP_CLASSES[] = {
        "org/libsdl/app/SDLActivity",
        "org/libsdl/app/SDLAudioManager",
        "org/libsdl/app/SDLControllerManager",
    };
    for (size_t i = 0; i < sizeof SETUP_CLASSES / sizeof *SETUP_CLASSES; i++) {
        void *fn = kl_jni_native(SETUP_CLASSES[i], "nativeSetupJNI", NULL);
        if (out) { fprintf(out, "  %s.nativeSetupJNI %s\n", SETUP_CLASSES[i],
                           fn ? "" : "NOT REGISTERED"); fflush(out); }
        if (!fn) continue;
        kl_jni_local_frame_push();
        // The jclass is the second argument, and this one is load-bearing: SDL3
        // takes a global ref to it and resolves every static method it will
        // ever call against it. NULL here reads as "GetStaticMethodID on
        // <unknown-jclass>" in the report and would be a dangling class later.
        ((void (*)(void *, void *))fn)(kl_jni_env(), kl_jni_class(SETUP_CLASSES[i]));
        kl_jni_local_frame_pop();
    }
    if (out) fprintf(out, "  setupJNI returned\n");
}

// The three natives mSDLThread calls, resolved on the UI thread and handed
// over. Resolving them on the thread itself would work; passing them keeps the
// "not registered" report in one place, with the others.
static void *g_sdl_slot[3];
static FILE *g_sdl_out;

static void *sdl_thread_body(void *arg) {
    (void)arg;
    // Trap 1: any thread that runs guest code seeds bionic's stack-guard canary
    // into TSD slot 5 first. This thread runs the whole of the guest's main().
    kl_thread_init();
    FILE *out = g_sdl_out;

    void *env = kl_jni_env(), *cls = kl_jni_class(SL_SDLA);

    if (g_sdl_slot[0]) {
        if (out) { fprintf(out, "  [sdl] nativeInitMainThread()\n"); fflush(out); }
        ((v_env_cls)g_sdl_slot[0])(env, cls);
    }

    const char *lib = getenv("KL_SLINK_LIB"); if (!lib) lib = kl_slink_main_lib();
    const char *fn  = getenv("KL_SLINK_FN");  if (!fn)  fn  = kl_slink_main_fn();

    // SDL dlopens this by path and dlsyms the function out of it. The path has
    // to be the one the image is REGISTERED under or kl_dl's dlopen will try to
    // map a second copy: kl_jni_set_native_lib_dir() and the chain loader both
    // use g_libdir, so build it the same way. (Trap 6c: absolute would be more
    // Android-like, but the registry key is what must match.)
    char libpath[1024];
    snprintf(libpath, sizeof libpath, "%s/%s", g_libdir, lib);

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

    if (out) {
        fprintf(out, "  [sdl] nativeRunMain(\"%s\", \"%s\", %s", libpath, fn,
                argc ? "[" : "NULL");
        for (int i = 0; i < argc; i++) fprintf(out, "%s\"%s\"", i ? ", " : "", argv[i]);
        fprintf(out, "%s)\n", argc ? "])" : ")");
        fflush(out);
    }

    kl_jni_local_frame_push();
    int rc = ((int (*)(void *, void *, void *, void *, void *))g_sdl_slot[1])(
        env, cls, kl_jni_new_string(libpath), kl_jni_new_string(fn), jargs);
    kl_jni_local_frame_pop();

    if (out) { fprintf(out, "  [sdl] nativeRunMain returned %d\n", rc); fflush(out); }

    if (g_sdl_slot[2]) {
        if (out) { fprintf(out, "  [sdl] nativeCleanupMainThread()\n"); fflush(out); }
        ((v_env_cls)g_sdl_slot[2])(kl_jni_env(), kl_jni_class(SL_SDLA));
    }
    return NULL;
}

// Look one up and say so. A native SDL3 did not register is a different failure
// from one that aborts when called, and conflating them wastes a run.
static void *sdl_want(FILE *out, const char *name) {
    void *p = kl_jni_native(SL_SDLA, name, NULL);
    if (out) fprintf(out, "  %-28s %s\n", name, p ? "ok" : "NOT REGISTERED");
    return p;
}

int kl_slink_sdl_start_main(FILE *out) {
    void *setres  = sdl_want(out, "nativeSetScreenResolution");
    void *surfcr  = sdl_want(out, "onNativeSurfaceCreated");
    void *resize  = sdl_want(out, "onNativeResize");
    g_sdl_slot[0] = sdl_want(out, "nativeInitMainThread");
    g_sdl_slot[1] = sdl_want(out, "nativeRunMain");
    g_sdl_slot[2] = sdl_want(out, "nativeCleanupMainThread");
    if (!g_sdl_slot[1]) return slink_fail("no nativeRunMain — cannot reach the guest's main");

    void *env = kl_jni_env(), *cls = kl_jni_class(SL_SDLA);

    // The panel, from the one place that decides it — the same numbers
    // kl_slink_configure already published to kl_ndk and kl_glfb, so the
    // display SDL reads does not contradict the surface it gets.
    int w, h;
    kl_slink_panel_size(&w, &h);

    if (setres) {
        if (out) { fprintf(out, "  nativeSetScreenResolution(%d,%d, %d,%d, 2.0, 60.0)\n",
                           w, h, w, h); fflush(out); }
        kl_jni_local_frame_push();
        ((void (*)(void *, void *, int, int, int, int, float, float))setres)(
            env, cls, w, h, w, h, 2.0f, 60.0f);
        kl_jni_local_frame_pop();
    }
    if (resize) { if (out) { fprintf(out, "  onNativeResize()\n"); fflush(out); }
                  kl_jni_local_frame_push(); ((v_env_cls)resize)(env, cls);
                  kl_jni_local_frame_pop(); }
    if (surfcr) { if (out) { fprintf(out, "  onNativeSurfaceCreated()\n"); fflush(out); }
                  kl_jni_local_frame_push(); ((v_env_cls)surfcr)(env, cls);
                  kl_jni_local_frame_pop(); }

    g_sdl_out = out;
    pthread_t th;
    if (pthread_create(&th, NULL, sdl_thread_body, NULL) != 0)
        return slink_fail("pthread_create for the SDL thread failed");
    pthread_detach(th);
    return 0;
}

double kl_slink_sdl_pump(double seconds, const volatile int *quit) {
    // There is no looper on this path — the SDL door's UI thread has no
    // ALooper of its own the way the NativeActivity's does, and what the guest
    // needs from it is the posted-task queue being drained. Sleeping in
    // between is correct here for the same reason pumping is correct there:
    // this is what Android's main thread would be doing.
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double elapsed = 0;
    struct timespec iv = { 0, 100 * 1000 * 1000 };
    for (unsigned t = 0; (quit ? !*quit : 1) && (seconds < 0 || elapsed < seconds); t++) {
        nanosleep(&iv, NULL);
        if ((t + 1) % 10 == 0) kl_jni_drain_ui_tasks();
        // The scripted click sequence, from HERE and not from a frontend. This
        // is the Android UI thread — the thread a real MotionEvent would be
        // delivered on — so it is where a synthetic one belongs, and it is the
        // one thread that is guaranteed to keep turning. Driving it from the
        // frontend's frame callback instead made it stop the moment the window
        // stopped drawing, which is the state a click is most wanted in.
        kl_mono_poke_tick();
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - t0.tv_sec)
                + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
    }
    return elapsed;
}

// ------------------------------------------------------------- VR front door --
// ANativeActivity, transcribed from <android/native_activity.h>. It is ABI, not
// an interface we get to design: the guest's glue reads these fields by offset
// out of the pointer we hand ANativeActivity_onCreate, and it writes the
// callbacks table back through the first one.
//
// `clazz` is the NDK's own misnomer — it is the activity INSTANCE object, not a
// jclass, and the guest calls getIntent()/getPackageName() on it. Ours is the
// jobject kl_jni hands out for the activity, so those land on g_bindings and
// fail by name like every other M4 gap.
typedef struct kl_ANativeActivity kl_ANativeActivity;
typedef struct {
    void (*onStart)(kl_ANativeActivity *);
    void (*onResume)(kl_ANativeActivity *);
    void *(*onSaveInstanceState)(kl_ANativeActivity *, size_t *);
    void (*onPause)(kl_ANativeActivity *);
    void (*onStop)(kl_ANativeActivity *);
    void (*onDestroy)(kl_ANativeActivity *);
    void (*onWindowFocusChanged)(kl_ANativeActivity *, int);
    void (*onNativeWindowCreated)(kl_ANativeActivity *, void *);
    void (*onNativeWindowResized)(kl_ANativeActivity *, void *);
    void (*onNativeWindowRedrawNeeded)(kl_ANativeActivity *, void *);
    void (*onNativeWindowDestroyed)(kl_ANativeActivity *, void *);
    void (*onInputQueueCreated)(kl_ANativeActivity *, void *);
    void (*onInputQueueDestroyed)(kl_ANativeActivity *, void *);
    void (*onContentRectChanged)(kl_ANativeActivity *, const void *);
    void (*onConfigurationChanged)(kl_ANativeActivity *);
    void (*onLowMemory)(kl_ANativeActivity *);
} kl_ANativeActivityCallbacks;
struct kl_ANativeActivity {
    kl_ANativeActivityCallbacks *callbacks;
    void       *vm;
    void       *env;
    void       *clazz;
    const char *internalDataPath;
    const char *externalDataPath;
    int32_t     sdkVersion;
    void       *instance;
    void       *assetManager;
    const char *obbPath;
};

typedef void (*anativeactivity_oncreate_fn)(kl_ANativeActivity *, void *, size_t);

static kl_ANativeActivityCallbacks g_cbs;
static kl_ANativeActivity g_act;

// One call per lifecycle hook, named, so a NULL callback is distinguishable
// from one that ran. Android calls these from the UI thread; so do we.
#define VR_CB(out, name, ...)                                                  \
    do {                                                                       \
        if (g_cbs.name) {                                                      \
            if (out) { fprintf(out, "  [vr] %s\n", #name); fflush(out); }      \
            kl_jni_local_frame_push();                                         \
            g_cbs.name(&g_act, ##__VA_ARGS__);                                 \
            kl_jni_local_frame_pop();                                          \
        } else if (out) fprintf(out, "  [vr] %s — not registered\n", #name);   \
    } while (0)

int kl_slink_vr_create(FILE *out) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_libdir, SL_VR_LIB);
    kl_image *scene = kl_find_image(path);
    if (!scene) return slink_fail("libvrlink_scene.so is not in the registry");

    anativeactivity_oncreate_fn onCreate =
        (anativeactivity_oncreate_fn)kl_sym(scene, SL_VR_FN);
    if (!onCreate) return slink_fail("libvrlink_scene.so exports no "
                                     SL_VR_FN);

    // This thread is the app's UI thread, and on Android that means it has a
    // looper before any activity is created. The guest takes it with
    // ALooper_forThread() inside onCreate and does not check for NULL.
    kl_ndk_prepare_looper();

    g_act.callbacks        = &g_cbs;
    g_act.vm               = kl_jni_vm();
    g_act.env              = kl_jni_env();
    g_act.clazz            = kl_jni_activity();
    g_act.internalDataPath = kl_jni_files_dir();
    g_act.externalDataPath = kl_jni_files_dir();
    // 29, the same Quest-2 answer Build.SDK_INT gives. Two numbers describing
    // one device have to agree — this is the display-panel group answer again.
    g_act.sdkVersion       = 29;
    g_act.assetManager     = kl_ndk_asset_manager();
    g_act.obbPath          = kl_jni_files_dir();

    if (out) {
        fprintf(out, "  activity: clazz=%p env=%p assets=%p sdk=%d dataPath=%s\n",
                g_act.clazz, g_act.env, g_act.assetManager, g_act.sdkVersion,
                g_act.internalDataPath ? g_act.internalDataPath : "(null)");
        fflush(out);
    }

    kl_jni_local_frame_push();
    onCreate(&g_act, NULL, 0);
    kl_jni_local_frame_pop();
    // How many hooks it installed is the cheapest confirmation that onCreate
    // did its job: a glue that returned early leaves the table empty, and that
    // reads identically to "it worked" without this line.
    int nhooks = 0;
    void **slot = (void **)&g_cbs;
    for (size_t i = 0; i < sizeof g_cbs / sizeof(void *); i++) nhooks += slot[i] != NULL;
    if (out) {
        fprintf(out, "  onCreate returned; %d of %zu callbacks registered\n",
                nhooks, sizeof g_cbs / sizeof(void *));
        fflush(out);
    }
    return 0;
}

void kl_slink_vr_start(FILE *out) {
    // The rest of what Android's NativeActivity does, in its order. onCreate
    // itself only spawns the guest's thread and returns — nothing renders until
    // the window arrives, and the glue's own loop blocks until it does.
    VR_CB(out, onStart);
    VR_CB(out, onResume);
    VR_CB(out, onNativeWindowCreated, kl_ndk_window());
    VR_CB(out, onWindowFocusChanged, 1);
}

double kl_slink_vr_pump(double seconds, const volatile int *quit) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double elapsed = 0;
    for (unsigned t = 0; (quit ? !*quit : 1) && (seconds < 0 || elapsed < seconds); t++) {
        kl_ndk_pump_looper(100);
        if ((t + 1) % 10 == 0) kl_jni_drain_ui_tasks();
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - t0.tv_sec)
                + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
    }
    return elapsed;
}

void kl_slink_report(FILE *out) {
    if (!out) return;
    kl_mediandk_report(out);
    kl_aaudio_report(out);
    kl_openxr_report(out);
    kl_egl_report(out);
    fprintf(out, "\n=== JNI surface ===\n");
    kl_jni_report(out);
    fflush(out);
}
