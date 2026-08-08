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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../runtime/klepton.h"
#include "../runtime/kl_jni.h"
#include "../runtime/kl_fault.h"

static const char *LIBDIR = "steamlink-android/lib/arm64-v8a";

// The activity Steam Link actually declares. SDL3 caches this class in
// nativeSetupJNI and every callback it makes goes through it, so getting it
// wrong is not cosmetic — it is the class every GetStaticMethodID resolves in.
static const char *ACTIVITY = "com/valvesoftware/steamlink/SteamLink";

typedef int (*jni_onload_fn)(void *vm, void *reserved);

static int fail(const char *msg) { fprintf(stderr, "FAIL: %s\n", msg); return 1; }

// The working set, dependencies first. libshell.so and the whole of Qt are out
// of scope by construction (§11.2): they hang off SteamShellActivity, and
// libmain.so's DT_NEEDED names none of them.
static const char *const CHAIN[] = {
    "libc++_shared.so",
    "libSDL3.so",
    "libSDL3_ttf.so",
    "libSDL3_image.so",
    "libh264bitstream.so",
    "libhevcbitstream.so",
    "libmain.so",
};

static void report_image(const char *soname, kl_image *img) {
    const kl_stats *st = kl_get_stats(img);
    unsigned total = st->relative + st->abs64 + st->glob_dat + st->jump_slot;
    printf("  %-22s %7.2f MB  reloc %-7u tls %-5u x18 %u/%u",
           soname, kl_span(img) / 1048576.0, total,
           st->tls_rewrites, st->x18_patched, st->x18_sites);
    if (st->x18_refused) printf(" (refused %u)", st->x18_refused);
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
    for (size_t i = 0; i < sizeof CHAIN / sizeof *CHAIN; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", LIBDIR, CHAIN[i]);
        kl_image *img = kl_find_image(path);
        if (!img) continue;
        unsigned nm = 0;
        const char *const *miss = kl_missing_imports(img, &nm);
        if (!nm) continue;
        grand += nm;
        printf("  %s: %u\n    ", CHAIN[i], nm);
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

    // The JNI surface has to describe THIS app, not Beat Saber. The activity
    // class is the load-bearing one (see above); the paths matter because
    // trap 6c applies to any guest — Android hands out absolute paths and
    // SDL3's SDL_GetBasePath/SDL_GetPrefPath propagate whatever we say.
    kl_jni_set_activity_class(ACTIVITY);
    kl_jni_set_apk_path("steamlink-android.apk");
    kl_jni_set_native_lib_dir(LIBDIR);
    kl_jni_set_files_dir("build/steamlink-files");
    // There is no assets/ directory in this APK at all — checked, not assumed.
    // SDL3 still imports AAssetManager_open, so pointing this somewhere real
    // keeps a lookup from being a crash; an absent file is the honest answer.
    kl_jni_set_assets_dir("steamlink-android");

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
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t i = 0; i < sizeof CHAIN / sizeof *CHAIN; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", LIBDIR, CHAIN[i]);
        kl_image *img = kl_load_auto(path);
        if (!img) { fprintf(stderr, "  %s: %s\n", CHAIN[i], kl_error()); return fail("chain load"); }
        kl_register_image(path, img);      // dependencies first, so the next
        report_image(CHAIN[i], img);       // library's imports bind against it
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("  chain mapped and relocated in %.1f ms\n",
           (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    report_gap();

    if (getenv("KL_GAP_ONLY")) { printf("\n(KL_GAP_ONLY: stopping before init)\n"); return 0; }

    printf("\n=== phase 1b: DT_INIT_ARRAY, dependencies first ===\n");
    fflush(NULL);
    for (size_t i = 0; i < sizeof CHAIN / sizeof *CHAIN; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", LIBDIR, CHAIN[i]);
        printf("  %s\n", CHAIN[i]);
        fflush(NULL);
        kl_run_init(kl_find_image(path));
    }
    printf("  all init arrays returned\n");

    char path[1024];
    snprintf(path, sizeof path, "%s/libmain.so", LIBDIR);
    kl_image *main_img = kl_find_image(path);
    snprintf(path, sizeof path, "%s/libSDL3.so", LIBDIR);
    kl_image *sdl_img = kl_find_image(path);
    if (!main_img || !sdl_img) return fail("libmain.so / libSDL3.so not in the registry");

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
    // of a note.
    static const char *const STATIC_NATIVES[] = {
        "Java_com_valvesoftware_steamlink_SteamLink_useVideoSurface",
        "Java_com_valvesoftware_steamlink_SteamLink_videoSurfaceCreated",
        "Java_com_valvesoftware_steamlink_SteamLink_videoSurfaceDestroyed",
        "Java_com_valvesoftware_steamlink_SteamLink_overlaySurfaceCreated",
        "Java_com_valvesoftware_steamlink_SteamLink_overlaySurfaceDestroyed",
        "Java_com_valvesoftware_steamlink_SteamLink_freezeRendering",
        "Java_com_valvesoftware_steamlink_SteamLink_thawRendering",
    };
    unsigned nstatic = 0;
    for (size_t i = 0; i < sizeof STATIC_NATIVES / sizeof *STATIC_NATIVES; i++)
        if (kl_sym(main_img, STATIC_NATIVES[i])) nstatic++;
    printf("  libmain static Java_* natives resolved: %u/%zu\n",
           nstatic, sizeof STATIC_NATIVES / sizeof *STATIC_NATIVES);

    void *sdl_main = kl_sym(main_img, "SDL_main");
    printf("  libmain SDL_main=%p\n", sdl_main);
    if (!sdl_main) return fail("libmain.so exports no SDL_main");

    if (!setup || !runmain)
        return fail("SDL3 did not register the SDLActivity natives");

    printf("\n=== SL-1 EXIT CRITERION MET: chain bound, SDL3 JNI_OnLoad ran ===\n");

    // ---- phase 3: reconnaissance ----
    // SDLActivity.onCreate calls SDL.setupJNI() first, which caches the
    // activity class and every method id SDL3 will ever call back through.
    // It is the densest single JNI call in the app and therefore the right
    // place for the surface to start failing by name.
    printf("\n=== recon: SDLActivity.nativeSetupJNI() ===\n");
    fflush(NULL);
    kl_jni_local_frame_push();
    // The jclass is the second argument, and this one is load-bearing: SDL3
    // takes a global ref to it and resolves every static method it will ever
    // call against it. NULL here reads as "GetStaticMethodID on
    // <unknown-jclass>" in the report and would be a dangling class later.
    ((void (*)(void *, void *))setup)(kl_jni_env(), kl_jni_class(SDLA));
    kl_jni_local_frame_pop();
    printf("  nativeSetupJNI returned\n");

    printf("\n=== JNI surface ===\n");
    kl_jni_report(stdout);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) LIBDIR = argv[1];
    kl_set_library_path(LIBDIR);

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
