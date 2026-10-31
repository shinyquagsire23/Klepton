// The Unreal Engine 4 target. See kl_ue4.h for why it is its own file.
#include "kl_ue4.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "klepton.h"
#include "kl_jni.h"
#include "kl_ndk.h"
#include "kl_driver.h"
#include "kl_nativeactivity.h"
#include "kl_egl.h"
#include "kl_ovrp.h"
#include "kl_ovrplat.h"
#include "kl_steamapi.h"
#include "kl_openxr.h"
#include "kl_mediandk.h"
#include <sys/stat.h>
#include "kl_aaudio.h"
#include "kl_obbmap.h"
#include "kl_opensl.h"
#include "kl_env.h"
#include "kl_vulkan.h"

// The library the chain ENDS at — "libUE4.so" for every UE4 title, and
// "libUnreal.so" for UE5, which renamed the monolith and its Java package in
// one release (Wanderer: libUnreal.so, com.epicgames.unreal.GameActivity).
// Set from the target table's entry field by kl_ue4_configure, because the
// table is where "which library proves the right guest was embedded" already
// lives; the default keeps every UE4 title exactly as it was.
static char g_entry_so[64] = "libUE4.so";
#define UE4_LIB g_entry_so
// The Java naming family that goes with it — UE5 renamed the package and the
// exported-native prefix in the same release. Both set beside g_entry_so in
// kl_ue4_configure; the defaults keep every UE4 title exactly as it was.
static const char *g_activity      = "com/epicgames/ue4/GameActivity";
static const char *g_native_prefix = "Java_com_epicgames_ue4_GameActivity_";
// The on-disk staging root the engine builds its content paths from: "UE4Game"
// for UE4, "UnrealGame" for UE5 (libUnreal). Selected by the entry lib, like the
// activity and native prefix — a UE5 title reads <files>/UnrealGame/<Project>/.
static const char *g_native_dir    = "UE4Game";
// The engine's command-line file was renamed with the package: UE4 reads
// "UE4CommandLine.txt", UE5 (libUnreal) reads "UECommandLine.txt" — see the
// paths the guest itself opens in the log. Selected by the entry lib alongside
// g_native_dir. This matters beyond cosmetics: the KL_UE4_STDOUT diagnostic
// writes -stdout into this file to surface WHY a UE title ForceQuits, and a UE5
// title (OLAR, Wanderer) never reads a file named for UE4 — so the diagnostic
// was silently inert on exactly the engine generation that needs it.
static const char *g_cmdline_name  = "UE4CommandLine.txt";
static const char *ue4_meta_str(const char *key);   // defined below; used by configure

// The chain, DEPENDENCIES FIRST, read off libUE4's own DT_NEEDED rather than
// off the Java. There is no staged NativeLoader-style load here: libUE4 binds
// its imports at RELOCATION time, so anything it needs has to be mapped before
// it is. Loading libUE4 alone stops on `__cxa_guard_acquire` inside its own
// DT_INIT_ARRAY, which is the first line of libc++_shared that a static
// initializer reaches.
//
// libUE4's DT_NEEDED also names libovrplatformloader.so, and it is deliberately
// NOT here: that is one of the three libraries this project REPLACES
// (kl_ovrplat), so loading the guest's copy would map an Oculus forwarder to a
// service that does not exist. kl_shim_lookup answers those names instead.
// Same for libOVRPlugin and libvrapi, which are not in the link at all — UE4
// dlopens them, and kl_ovrp claims both.
//
// libbink2androidarm64 and libbinkpluginandroidarm64 are absent for the other
// reason: they are in nobody's DT_NEEDED. RAD's Bink video is dlopen'd by the
// engine when a movie is first played, and kl_load_auto resolves it then.
// Every member but the entry is OPTIONAL-IF-ABSENT: this list is the union of
// what the UE titles in the corpus put in their DT_NEEDED, and no single title
// ships all of it — RE4/wrath2 carry FMOD, TWD2 is Wwise and has none, Red
// Matter 2 has ovraudio but no playcore, and Wanderer (UE5) ships not even
// libc++_shared (statically linked). A member whose FILE is not in the libdir
// is skipped with a note; only the entry library failing is fatal.
static const char *const UE4_CHAIN[] = {
    "libc++_shared.so",
    // Older UE4 (pre-4.20 NDK toolchains) links GNU STL instead of libc++:
    // libUE4 NEEDS libgnustl_shared.so and its static initializers call
    // std::ios_base::Init (_ZNSt8ios_base4InitC1Ev, no __1 namespace), which
    // only gnustl exports. Absent from the chain, Tales from the Galaxy's Edge
    // (x3484270071659289) died on that unresolved import before JNI_OnLoad.
    "libgnustl_shared.so",
    "libEOSSDK.so",         // Epic Online Services — Tetris Effect links it,
                            // RE4 and Deadpool VR do not ship it
    // FMOD, dependencies first (libfmodstudio is built on the libfmod core).
    // libUE4 has these as DT_NEEDED and binds FMOD_* / FMOD::* against them, but
    // the translated libUE4 carries no LC_LOAD_DYLIB, so a symbol only resolves
    // if its library is already in the image pool when libUE4 binds. Absent from
    // the chain, every FMOD import fell through to kl_unresolved_named and the
    // first FMOD_Debug_Initialize aborted. Loaded here, before libUE4, they bind.
    "libfmod.so",
    "libfmodstudio.so",
    "libovraudio64.so",     // the Oculus audio spatializer — RE4/Deadpool VR
                            // ship it, Tetris Effect does not
    "libplaycore.so",       // Google Play core — the OBB downloader's half
    // Vivox voice chat, the same case as FMOD: libUE4 binds its vx_* imports
    // against it, and without it in the pool the first call — Medal of Honor:
    // Above and Beyond's vx_jni_set_java_vm at start — aborted as unresolved.
    "libvivox-sdk.so",
    // Virtuix Omni treadmill support, the same case again: libUE4 NEEDS it and
    // binds OmniConnectShared_* against it. Absent from the pool, Zero Caliber 2
    // (x6072194326150034) aborted on OmniConnectShared_Create right after Vivox
    // came up. Self-contained (libc only); skipped where a guest does not ship it.
    "libOmniConnectShared.so",
    // Heroic Labs' Nakama client (its C API: createDefaultNakamaClient,
    // NClient_*, NSession_*), the same case once more: libUE4 binds against it,
    // and Contractors (x2436897736439055) aborted on createDefaultNakamaClient
    // as unresolved right after the platform check (2026-10-28). Built on
    // libc++_shared, so after it; skipped where a guest does not ship it.
    "libnakama-cpp.so",
    UE4_LIB,
};

// UE4's own Java front door. `com.epicgames.ue4.GameActivity` is a
// NativeActivity subclass, which is why the guest reaches
// ANativeActivity_onCreate at all — and the guest asks for its own class by
// name (GameActivity has a large native surface of its own), so answering
// `android/app/NativeActivity` the way Steam Link's VR door does would be
// wrong here in a way that only shows up as a FindClass several layers in.


static char g_libdir[1024];
static char g_err[512] = "no error";
static kl_image *g_ue4;

// ---- the 4.x / 5.x dialect ------------------------------------------------
//
// Between UE4.27 and UE5, Epic renamed its Java package and its entry library:
// `com.epicgames.ue4.*` became `com.epicgames.unreal.*`, `libUE4.so` became
// `libUnreal.so`, and with them went the exported native symbols
// (`Java_com_epicgames_ue4_GameActivity_native*` ->
// `Java_com_epicgames_unreal_…`) and every `<meta-data>` key the manifest
// carries. NOTHING ELSE about the door changed: the NativeActivity entry, the
// onCreate/onResume transcription below, the argument shapes and the
// AndroidThunkJava_* surface are the same calls under new names.
//
// So it is a spelling, not a second door. The entry library, activity class and
// native prefix are set together from the target row (kl_ue4_configure);
// manifest keys fall back across the two spellings in ue4_meta_raw; the JNI
// surface folds `com/epicgames/unreal/` onto `com/epicgames/ue4/` at its
// class-intern funnel (kl_jni.c, klj_fold_class_name); and guest natives are
// looked up in both spellings (ue4_native_symbol, below). This prints which
// spelling the guest is in, read off the MANIFEST — the guest's own statement
// about itself — beside the entry library, so a mismatch between the two is
// visible in the first lines of the boot log.
static void ue4_dialect(FILE *out) {
    int unreal = kl_jni_manifest_meta("com.epicgames.unreal.GameActivity.ProjectName") != NULL
              || strcmp(g_entry_so, "libUnreal.so") == 0;
    if (!out) return;
    const char *branch = kl_jni_manifest_meta(
        unreal ? "com.epicgames.unreal.GameActivity.EngineBranch"
               : "com.epicgames.ue4.GameActivity.EngineBranch");
    const char *ver = kl_jni_manifest_meta(
        unreal ? "com.epicgames.unreal.GameActivity.EngineVersion"
               : "com.epicgames.ue4.GameActivity.EngineVersion");
    fprintf(out, "  [ue4] dialect:  com.epicgames.%s (%s, engine %s %s)\n",
            unreal ? "unreal" : "ue4", g_entry_so, branch ? branch : "?", ver ? ver : "?");
    fflush(out);
}

// A native of the guest's, by exported symbol. Registered with the JNI surface
// so `AndroidThunkJava_*` handlers can make the call GameActivity.java would
// have made; see kl_jni.h's kl_jni_set_guest_native_resolver.
//
// The JNI surface asks in 4.27's spelling, because that is the one spelling its
// tables keep — the same choice kl_jni.c's klj_intern_class_locked makes when
// it folds `com/epicgames/unreal/` onto `com/epicgames/ue4/`. A 5.x guest
// exports `Java_com_epicgames_unreal_…`, so the same fold happens HERE, at the
// one funnel every such lookup passes through, rather than at each call site.
//
// What the miss costs is worth naming, because it is silent: on Pinball FX
// `AndroidThunkJava_InitHMDs` resolved NULL and that is an error nowhere —
// nativeInitHMDs is simply never called, no HMD module ever gets its PreInit,
// and the guest runs on with no XR path at all.
static void *ue4_native_symbol(const char *symbol) {
    if (!g_ue4 || !symbol) return NULL;
    void *p = kl_sym(g_ue4, symbol);
    if (p) return p;
    static const char ue4_pfx[] = "Java_com_epicgames_ue4_";
    if (strncmp(symbol, ue4_pfx, sizeof ue4_pfx - 1)) return NULL;
    char alt[256];
    snprintf(alt, sizeof alt, "Java_com_epicgames_unreal_%s",
             symbol + sizeof ue4_pfx - 1);
    return kl_sym(g_ue4, alt);
}

static int ue4_fail(const char *why) {
    snprintf(g_err, sizeof g_err, "%s", why);
    return 1;
}

const char *kl_ue4_error(void) { return g_err; }

// The engine's command-line OVERRIDE file, named and printed.
//
// UE4's `InitCommandLine` reads `<external>/UE4Game/<project>/UE4CommandLine.txt`
// before anything else, and whatever is in it configures the whole engine —
// threading model, RHI, log verbosity. It is a file in the guest's USERDATA
// directory, which outlives `make clean`, outlives a rebuild, and is not part
// of the tree, so a line left in it by one debugging session silently
// configures every run after it.
//
// That is not hypothetical: `-onethread`, added to test whether a stall was
// threading (it was not — it was the condvar table), stayed behind and made
// every later host run single-threaded. Under it this title's main menu renders
// an all-zero eye, so the run reads as "the guest goes black after the title
// screen" — a rendering bug with no rendering cause, and the file that caused
// it appears nowhere in any log.
//
// Both places are looked at because the base the engine builds this path from
// is not ours to restate; naming what is THERE is honest where reimplementing
// the search would be a second, drifting copy of it.
static void ue4_report_command_line(FILE *out) {
    if (!out) return;
    const char *files = kl_jni_files_dir();
    if (!files || !*files) return;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/%s", files, g_native_dir);

    const char *names[8];
    char        held[8][1024];
    size_t      n = 0;
    snprintf(held[n], sizeof held[n], "%s/%s", dir, g_cmdline_name);
    names[n] = held[n]; n++;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < sizeof names / sizeof *names) {
            if (e->d_name[0] == '.') continue;
            snprintf(held[n], sizeof held[n], "%s/%s/%s",
                     dir, e->d_name, g_cmdline_name);
            names[n] = held[n]; n++;
        }
        closedir(d);
    }

    for (size_t i = 0; i < n; i++) {
        FILE *f = fopen(names[i], "rb");
        if (!f) continue;
        char buf[512];
        size_t got = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[got] = 0;
        for (char *p = buf; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        fprintf(out, "  [ue4] command line: %s\n", buf);
        fprintf(out, "  [ue4]   from %s — the ENGINE reads this, and it is in "
                     "userdata rather than in the tree\n", names[i]);
    }
    fflush(out);
}

// KL_UE4_STDOUT=1: force UE4's engine log to stdout so it reaches our log.
// UE4 logs to a FILE (Saved/Logs/<Project>.log) and makes no __android_log
// calls, so a fatal that ends in ForceQuit leaves nothing in our capture to
// explain it (TWD2/Vampire quit early with no visible reason). UE4 reads a
// writable command-line override at <files>/UE4Game/UE4CommandLine.txt before
// the packaged one, so this writes that override with the packaged args plus
// -stdout. Diagnostic only; the file persists, so it is written ONLY under the
// knob and names itself in the log.
// Default KL_UE4_STDOUT ON for titles stuck in the silent-ForceQuit mode, where
// the ONLY way to learn the fatal reason is the engine's own stdout log — and the
// user runs on device, where an env var cannot be set. olar (UE5) quits during
// PreInit with nothing in our capture; forcing its engine log out is the only
// diagnostic. Remove from this list once its fatal is understood and fixed.
// x5190288877703616 is twd2 as a store install (The Walking Dead: Saints &
// Sinners - Chapter 2: Retribution, 32342700): it is listed so that the
// override file below is written for it too and carries twd2's loading fix.
static int ue4_stdout_default(void) {
    static const char *on[] = { "olar", "wrath2", "twd2", "x5190288877703616" };
    const char *t = kl_driver_target_name();
    if (t) for (unsigned i = 0; i < sizeof on / sizeof on[0]; i++)
        if (strcmp(t, on[i]) == 0) return 1;
    return 0;
}

static void ue4_maybe_stdout_cmdline(const char *proj) {
    if (!kl_env_on("KL_UE4_STDOUT", ue4_stdout_default())) return;
    // Start from the packaged command line if we can read it, else synthesize
    // the one thing that must be present — the .uproject path.
    char args[512];
    snprintf(args, sizeof args, "../../../%s/%s.uproject", proj, proj);
    char apath[1024];
    snprintf(apath, sizeof apath, "%s/%s", kl_jni_assets_dir(), g_cmdline_name);
    FILE *pf = fopen(apath, "r");
    if (pf) {
        char buf[512];
        if (fgets(buf, sizeof buf, pf)) {
            buf[strcspn(buf, "\r\n")] = 0;
            if (*buf) snprintf(args, sizeof args, "%s", buf);
        }
        fclose(pf);
    }
    // Extra command-line args that MUST reach the engine on device. An env var
    // cannot: the user runs on a headset. The packaged UECommandLine.txt is the
    // intended knob, but staging is keyed on the APK's mtime (see the Makefile),
    // so a loose edit to it never re-stages — this file, which the engine really
    // reads (files_dir/<native>/UECommandLine.txt), is the one place an override
    // is guaranteed to land. KL_UE4_EXTRA_CVARS works off device; on device the
    // per-target block below is the lever, self-naming in the log and removable.
    const char *extra = getenv("KL_UE4_EXTRA_CVARS");
    char extra_buf[512]; extra_buf[0] = 0;
    if (!extra || !*extra) {
        const char *t = kl_driver_target_name();
        // olar has TWO independent visual bugs; its device-profile cvars are built
        // additively so a test for one does not disturb the other.
        //
        // (1) "missing assets" — some world textures never appear while the menu is
        //     fine. olar is a UE5 IoStore title whose world textures stream on
        //     demand out of .ucas (windowed OBB reads); the menu's UI textures are
        //     always resident. A broken async mip-stream reads exactly as "menu
        //     fine, world shows UE's default gray, some never appear". TEST: force
        //     full residency (KL_OLAR_NOSTREAM, default on). KL_OLAR_NOVT=1 re-runs
        //     the ruled-out virtual-texturing A/B instead.
        //
        // (2) grayscale WORLD with a correctly COLOURED menu. The world passes
        //     through UE's mobile tonemapper + colour-grading LUT; the menu is a
        //     separate OVR overlay layer that does not. UE builds the colour-grade
        //     LUT as a Texture3D — seen in this run's log as vkCreateImage type 2
        //     (3D) with the 2D_ARRAY_COMPATIBLE flag — and fills its Z-slices with
        //     layered rendering, which MoltenVK handles poorly: if only slice 0 is
        //     written the LUT is ~neutral and every tonemapped pixel desaturates,
        //     while the un-tonemapped UI keeps colour. That is the exact symptom.
        //     KL_OLAR_COLORTEST=1 forces the 2D-unwrapped LUT (r.UseVolumeTextureLUT
        //     =0), which MoltenVK renders correctly and which preserves the intended
        //     look. If colour returns, the 3D-LUT layered write is the bug.
        //     KL_OLAR_NOFILM=1 is the second bisection arm: drop the ACES film curve
        //     (r.TonemapperFilm=0) in case the desaturation is in the film-tonemap
        //     shader itself rather than the LUT — this DOES flatten the look, so it
        //     is a diagnostic, not the intended fix.
        if (t && strcmp(t, "olar") == 0) {
            char cv[400]; int nc = 0;
            #define OLAR_CV(fmt) nc += snprintf(cv + nc, sizeof cv - nc, \
                                                "%s" fmt, nc ? "," : "")
            if (kl_env_on("KL_OLAR_NOVT", 0))
                OLAR_CV("r.VirtualTextures=0,r.VT.EnableFeedback=0,"
                        "r.VirtualTexturedLightmaps=0");
            else if (kl_env_on("KL_OLAR_NOSTREAM", 1))
                OLAR_CV("r.TextureStreaming=0,r.Streaming.FullyLoadUsedTextures=1");
            // COLORTEST (r.UseVolumeTextureLUT=0) proven INERT 2026-08-31: the
            // command-line echo showed it took, world stayed gray. LUT-storage
            // theory dead. Default OFF; knob kept for A/B.
            if (kl_env_on("KL_OLAR_COLORTEST", 0))
                OLAR_CV("r.UseVolumeTextureLUT=0");
            // NOMSAA (default ON, decisive + shippable). Fresh rpass2 census: olar's
            // scene pass is multiview (viewMask 0x3) + 2x MSAA + a fused framebuffer-
            // fetch tonemap subpass (atts 3 subpasses 2 inputAtts 1; att0 fmt43
            // samples2 RESOLVES into att1 samples1) — the exact MoltenVK-thorny
            // multiview-MSAA-resolve shape wanderer's black is already blamed on
            // (olar=gray, wanderer=black, same pass shape, one bug two faces).
            // NOMSAA (r.MobileMSAA=1) DISPROVEN 2026-08-31: on device it did NOT drop
            // the scene-pass MSAA (rpass2 still att0 samples2 RESOLVES into att1 — UE5
            // mobile ignores it here) AND made the world BLACK (only the non-tonemapped
            // rain overlay showed, then went black on turn). So the MSAA-resolve is NOT
            // the desaturation cause. Default OFF.
            if (kl_env_on("KL_OLAR_NOMSAA", 0))
                OLAR_CV("r.MobileMSAA=1");
            // NOFILM DISPROVEN 2026-08-31: r.TonemapperFilm=0 made olar BLACK, not
            // colour — so the tonemapper was NOT the desaturation; it was BRIGHTENING
            // a near-black base scene into the gray we saw. => the base 3D scene render
            // is itself dark/black (the real root), and the gray is the tonemapper
            // lifting it. Default OFF (restore the gray baseline, better than black).
            // Next: why the base pass is dark — lighting/exposure/ambient under
            // MoltenVK, not the grade. See [[olar-wanderer-multiview-msaa-resolve]].
            if (kl_env_on("KL_OLAR_NOFILM", 0))
                OLAR_CV("r.TonemapperFilm=0,r.Tonemapper.Quality=0");
            #undef OLAR_CV
            if (nc > 0)
                snprintf(extra_buf, sizeof extra_buf, " -dpcvars=%s", cv);
        }
        // wanderer (UE5): stuck at a huge MoltenVK PSO-precompile wall (342s+ and
        // still climbing on device) and the eye render is black. r.MobileMSAA=1
        // removes the MSAA pipeline VARIANTS — fewer PSOs to compile (shorter wall)
        // and, if UE5 honours it here (olar did NOT — the rpass2 census still showed
        // att0 samples2 there, so verify), it also drops the multiview MSAA-resolve
        // that may be the black ([[olar-wanderer-multiview-msaa-resolve]]). The
        // durable wall fix is a disk PSO cache. KL_WANDERER_NOMSAA=0 disables.
        else if (t && strcmp(t, "wanderer") == 0) {
            if (kl_env_on("KL_WANDERER_NOMSAA", 1))
                snprintf(extra_buf, sizeof extra_buf, " -dpcvars=r.MobileMSAA=1");
        }
        // wrath2: FAndroidMisc::IsVulkanAvailable() (verified by disassembly of
        // libUE4.so) returns false -> the "does not support Vulkan" fatal box,
        // even now that the device reports api 1.1. Its gate is a config read:
        // bSupportsVulkan / bSupportsVulkanSM5 from the cooked
        // [/Script/AndroidRuntimeSettings.AndroidRuntimeSettings] section, BOTH
        // preset false and only true if that config is read. It isn't reaching the
        // engine (IoStore title; config not mounted when the check runs), so both
        // stay false and Vulkan is refused. Force the flag with UE4's own -ini
        // command-line override — a no-op if the config already said true, and the
        // deterministic fix if it didn't. KL_WRATH2_FORCEVK=0 disables.
        else if (t && strcmp(t, "wrath2") == 0 && kl_env_on("KL_WRATH2_FORCEVK", 1))
            // (1) Force the Vulkan RHI gate open (see above).
            // (2) Disable the async loading THREAD (-noasyncloadingthread +
            //     s.AsyncLoadingThreadEnabled=0). Once past the Vulkan gate wrath2
            //     boots and compiles 5000+ shaders, then hangs on the first map
            //     load: the FAsyncLoading thread spins post-I/O (files all read,
            //     never finalises) while the game thread blocks on the synchronous
            //     load behind the OVR splash — a game-thread<->async-thread
            //     deadlock. With the ALT off, all loading runs on the game thread,
            //     so there is no cross-thread finalisation wait to deadlock on.
            //     KL_WRATH2_ALT=1 restores the async thread to A/B this.
            // (3) The stuck "COMPILING SHADERS" screen is the Sanzaru title
            //     SEGMENT AAW_CompileShadersTitleScreenSegment, which waits on
            //     FShaderPipelineCache::NumPrecompilesRemaining() reaching 0. The
            //     precompile stalls (~529 PSOs then never drains), so the segment
            //     never advances to Calibration. DISABLING the cache made it worse
            //     (the segment then never sees the cache open/complete). A guest
            //     patch that forced NumPrecompilesRemaining()->0 stood in for it
            //     and is gone — see kl_guestpatch.c: the precompile drains now,
            //     and the segment is left to the game.
            snprintf(extra_buf, sizeof extra_buf,
                     " -ini:Engine:[/Script/AndroidRuntimeSettings.AndroidRuntimeSettings]:"
                     "bSupportsVulkan=True%s",
                     kl_env_on("KL_WRATH2_ALT", 0) ? "" :
                     " -noasyncloadingthread"
                     " -ini:Engine:[ConsoleVariables]:s.AsyncLoadingThreadEnabled=0");
        // twd2 (UE4 GLES) boots but stays black: the sampler shows FAsyncLoading
        // spinning (15/16 running) while RenderThread and the game thread sit idle
        // (0/16) with file I/O stopped — the same game-thread<->async-thread
        // finalisation deadlock wrath2 hit, so the render thread never draws (0
        // glDraw* all run) and the eyes composite black. Same lever: run all
        // loading on the game thread. No Vulkan gate here — twd2 is GLES.
        // KL_TWD2_ALT=1 restores the async thread to A/B. The store install
        // (x5190288877703616) is the same build and stayed black the same way
        // on device: fence set, 0 layers, 0 draws, only shader compiles.
        else if (t && (strcmp(t, "twd2") == 0 || strcmp(t, "x5190288877703616") == 0) &&
                 !kl_env_on("KL_TWD2_ALT", 0))
            snprintf(extra_buf, sizeof extra_buf,
                     " -noasyncloadingthread"
                     " -ini:Engine:[ConsoleVariables]:s.AsyncLoadingThreadEnabled=0");
        extra = extra_buf;
    } else {
        snprintf(extra_buf, sizeof extra_buf, " %s", extra);
        extra = extra_buf;
    }

    char ov[1024];
    snprintf(ov, sizeof ov, "%s/%s/%s", kl_jni_files_dir(), g_native_dir, g_cmdline_name);
    char dir[1024]; snprintf(dir, sizeof dir, "%s/%s", kl_jni_files_dir(), g_native_dir);
    mkdir(dir, 0777);
    FILE *f = fopen(ov, "w");
    if (!f) { fprintf(stderr, "  [ue4] KL_UE4_STDOUT: cannot write %s\n", ov); return; }
    fprintf(f, "%s%s -stdout -FullStdOutLogOutput\n", args, extra);
    fclose(f);
    fprintf(stderr, "  [ue4] KL_UE4_STDOUT: wrote %s = \"%s%s -stdout -FullStdOutLogOutput\" "
                    "(engine log will follow on stdout)\n", ov, args, extra);
}

// KL_UE4_PSO_LIST=1 — Moss from Steam compiles Unreal's own pipeline list at
// start, as the Meta build does.
//
// Both builds ship the list Polyarc recorded (Content/PipelineCaches/Android/
// Moss_SF_VULKAN_ES31_ANDROID.stable.upipelinecache). The Meta build has
// r.ShaderPipelineCache.Enabled=1 and compiles about 350 pipelines before its
// first frames; the Steam build sets it to 0 (Config/Android/Extensions/
// SteamAndroidEngine.ini — on Steam Frame, Steam pre-caches shaders itself), so
// here every one of them waits for its first draw, a third of a second each on
// the headset. -dpcvars on the command line does not reach this cvar (a Mac run
// still compiled 50); a [ConsoleVariables] line in the game's user Engine.ini
// does (357), and the game keeps the line when it saves its settings. So the
// line is written there before the engine starts, and taken out again when the
// switch is off — the file outlives every build. Every other line of the file
// is left as it is.
// One `key=value` line in one [section] of a game's user Engine.ini
// (<files>/<native>/<Project>/<Project>/Saved/Config/Android/Engine.ini), put
// there before the engine starts when `on`, and taken out again when not — the
// file outlives every build. Every other line of the file is left as it is.
// Returns 0 when the file already said what it should, 1 when it was written,
// -1 when it could not be.
static int ue4_user_engine_ini(const char *project, const char *section,
                               const char *key, const char *value, int on,
                               char *path_out, size_t path_cap) {
    char dir[1024], path[1100], tmp[1110];
    snprintf(dir, sizeof dir, "%s/%s/%s/%s/Saved/Config/Android",
             kl_jni_files_dir(), g_native_dir, project, project);
    snprintf(path, sizeof path, "%s/Engine.ini", dir);
    snprintf(tmp, sizeof tmp, "%s.kltmp", path);
    if (path_out) snprintf(path_out, path_cap, "%s", path);
    size_t sl = strlen(section), kl = strlen(key), vl = strlen(value);

    char *old = NULL; size_t n = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        if (len > 0 && len < (8 << 20) && (old = malloc((size_t)len + 1)))
            n = fread(old, 1, (size_t)len, f);
        fclose(f);
        if (old) old[n] = 0;
    }
    if (!old && !on) return 0;

    // Copy the file line by line, without any line of ours: a key line in the
    // section is dropped, and the section header remembered so the line goes
    // right under it when the switch is on.
    size_t cap = n + sl + kl + vl + 16;
    char *nw = malloc(cap);
    if (!nw) { free(old); return -1; }
    size_t w = 0;
    int in_sec = 0, had_sec = 0, had_key = 0, was_on = 0;
    const char *p = old ? old : "";
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t ll = e ? (size_t)(e - p + 1) : strlen(p);
        const char *s = p;
        while (s < p + ll && (*s == ' ' || *s == '\t')) s++;
        if (*s == '[') in_sec = !strncmp(s, section, sl);
        int keyline = in_sec && !strncmp(s, key, kl) && (s[kl] == '=' || s[kl] == ' ');
        if (keyline) {
            had_key = 1;
            const char *v = strchr(s, '=');
            was_on |= v && !strncmp(v + 1, value, vl);
        } else {
            memcpy(nw + w, p, ll); w += ll;
            if (in_sec && *s == '[' && on && !had_sec) {
                if (nw[w - 1] != '\n') nw[w++] = '\n';
                w += (size_t)snprintf(nw + w, cap - w, "%s=%s\n", key, value);
                had_sec = 1;
            }
        }
        p += ll;
    }
    if (on && !had_sec) {
        if (w && nw[w - 1] != '\n') nw[w++] = '\n';
        w += (size_t)snprintf(nw + w, cap - w, "\n%s\n%s=%s\n", section, key, value);
    }
    free(old);
    if (on ? (had_key && was_on) : !had_key) {   // already as it should be
        free(nw);
        return 0;
    }
    char mk[1024];
    snprintf(mk, sizeof mk, "%s/%s", kl_jni_files_dir(), g_native_dir);
    const char *const parts[] = { project, project, "Saved", "Config", "Android" };
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
        mkdir(mk, 0777);
        strncat(mk, "/", sizeof mk - strlen(mk) - 1);
        strncat(mk, parts[i], sizeof mk - strlen(mk) - 1);
    }
    mkdir(mk, 0777);
    f = fopen(tmp, "wb");
    int ok = f && fwrite(nw, 1, w, f) == w;
    if (f && fclose(f) != 0) ok = 0;
    if (ok) ok = rename(tmp, path) == 0;
    else remove(tmp);
    free(nw);
    return ok ? 1 : -1;
}

static void ue4_moss_steam_pso_list(FILE *out) {
    const char *t = kl_driver_target_name();
    if (!t || strcmp(t, "moss") || !kl_steamapi_shipped()) return;
    int on = kl_env_on("KL_UE4_PSO_LIST", 0);
    char path[1100];
    int r = ue4_user_engine_ini("MossGame", "[ConsoleVariables]",
                                "r.ShaderPipelineCache.Enabled", "1", on, path, sizeof path);
    if (!out) return;
    if (r == 0)
        fprintf(out, "  [ue4] Moss from Steam: Unreal's own pipeline list %s (KL_UE4_PSO_LIST)\n",
                on ? "on, as before" : "off, as the game ships");
    else
        fprintf(out, "  [ue4] Moss from Steam: Unreal's own pipeline list %s (KL_UE4_PSO_LIST, %s)%s\n",
                on ? "switched on" : "back off, as the game ships", path,
                r > 0 ? "" : " — could not write the file");
}

// KL_UE4_MOBILE_MULTIVIEW_OFF=1 — Star Wars Pinball VR (x3180994211970184,
// UE 4.25 on GLES) is drawn one eye after the other instead of with mobile
// multiview.
//
// The project ships vr.MobileMultiView=True, and in this engine's
// FSceneView::FSceneView that cvar alone turns bIsMobileMultiViewEnabled on —
// whether or not the RHI has multiview (GSupportsMobileMultiView stays 0 here:
// the GL gateway does not name the extensions to it, see ue4_no_multiview).
// The eye layer then is DoubleWide, so the renderer goes the non-direct way and
// reads SceneContext.MobileMultiViewSceneColor in GetMultiViewSceneColor —
// which was never made: a NULL read at its first scene frame (Mac, 2026-10-19).
// Told multiview exists (KL_OVRP_GL_ARRAY, the Array eye layout Maestro and
// Tetris Effect get) it runs, but every eye picture stayed empty, colour and
// alpha 0. With the cvar off the same cooked shaders run single-view (UE4's own
// `#define gl_ViewID_OVR 0`) and the room and its "Choose Your Side" screen are
// in both halves of the eye picture.
//
// The cvar is read-only: -dpcvars and the -ini: override on the command line
// (dropped in a Shipping build) do not reach it; the game's user Engine.ini
// does. Written there before the engine starts, and taken out again with
// KL_UE4_MOBILE_MULTIVIEW_OFF=0.
static void ue4_mobile_multiview_off(FILE *out) {
    const char *t = kl_driver_target_name();
    if (!t || strcmp(t, "x3180994211970184")) return;
    int on = kl_env_on("KL_UE4_MOBILE_MULTIVIEW_OFF", 1);
    char path[1100];
    int r = ue4_user_engine_ini("SWP_VR", "[/Script/Engine.RendererSettings]",
                                "vr.MobileMultiView", "False", on, path, sizeof path);
    if (out)
        fprintf(out, "  [ue4] mobile multiview %s in the game's Engine.ini "
                     "(KL_UE4_MOBILE_MULTIVIEW_OFF%s%s)%s\n",
                on ? "off" : "as the project ships it", r > 0 ? ", " : "",
                r > 0 ? path : "", r < 0 ? " — could not write the file" : "");
}

// KL_UE4_NO_OCCLUSION=0|1 — Roboquest VR, Meta Store build (x9269393463181825,
// UE 5.6 mobile on Vulkan): the engine's occlusion culling off, so it draws
// what is in view without first asking the depth buffer whether it is hidden.
//
// With foveated rendering on (kl_fove.m) at ANY level — Light, Medium, Strong —
// objects blinked in and out; with it off they did not (on device, 2026-10-23).
// Unreal decides visibility from the depth the scene pass leaves: occlusion
// queries count the pixels of an object's bounds that pass the depth test, and
// the scene pass also resolves its depth into an image of its own that it
// stores for later. Under a rasterization rate map both are foveated: a small
// box in the coarse part covers few or no pixels, and the stored depth is in
// the physical (compressed) layout, which kl_fove expands for the eye colour
// only ("nothing reads the eye depth after the eye pass" — this game does).
// Either way a visible object is judged hidden for a frame and comes back the
// next. Drawing everything in view costs this GPU little.
//
// Written under [ConsoleVariables] in the game's user Engine.ini, which a
// device profile does not override; =0 takes the lines out again.
static void ue4_roboquest_no_occlusion(FILE *out) {
    const char *t = kl_driver_target_name();
    if (!t || strcmp(t, "x9269393463181825")) return;
    int on = kl_env_on("KL_UE4_NO_OCCLUSION", 1);
    char path[1100];
    static const char *const keys[] = { "r.AllowOcclusionQueries", "r.HZBOcclusion",
                                        "r.Mobile.HZBOcclusion" };
    int r = 0;
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        int ri = ue4_user_engine_ini("RoboquestVR", "[ConsoleVariables]", keys[i], "0",
                                     on, path, sizeof path);
        if (ri < 0) r = -1; else if (ri > 0 && r == 0) r = 1;
    }
    if (out)
        fprintf(out, "  [ue4] occlusion culling %s in the game's Engine.ini "
                     "(KL_UE4_NO_OCCLUSION%s%s)%s\n",
                on ? "off" : "as the project ships it", r > 0 ? ", " : "",
                r > 0 ? path : "", r < 0 ? " — could not write the file" : "");
}

int kl_ue4_configure(const char *libdir, const char *entry_lib, FILE *out) {
    if (!libdir || !*libdir) return ue4_fail("no library directory");
    snprintf(g_libdir, sizeof g_libdir, "%s", libdir);
    // The entry from the target row decides the whole naming family: libUE4 is
    // com.epicgames.ue4, libUnreal (UE5) is com.epicgames.unreal, and the
    // native prefix follows. Defaults stand when the row says libUE4 or is
    // silent, so every existing UE4 target boots exactly as before.
    if (entry_lib && strcmp(entry_lib, "libUnreal") == 0) {
        snprintf(g_entry_so, sizeof g_entry_so, "libUnreal.so");
        g_cmdline_name  = "UECommandLine.txt";
        g_activity      = "com/epicgames/unreal/GameActivity";
        g_native_prefix = "Java_com_epicgames_unreal_GameActivity_";
        g_native_dir    = "UnrealGame";
    }
    // Everything else — assets, apk, files, native lib dir — has already been
    // set from the target row by kl_target_apply_host(). Restating any of it
    // here is how two descriptions of one guest start to drift.
    ue4_dialect(out);
    kl_jni_set_activity_class(g_activity);
    // ...and the way back. UE4's Java front end calls natives on itself, so a
    // driver standing in for that Java has to be able to make those calls; the
    // JNI surface does not know which image is the guest, and this file does.
    kl_jni_set_guest_native_resolver(ue4_native_symbol);
    // Bridge the OBB paks to the loose paths this engine opens. The project
    // name is UE4Game/<Project>, the same ProjectName the manifest gives
    // nativeSetObbInfo, and the entries inside the OBBs are "<Project>/Content/
    // ...", so the loose base is <files>/UE4Game/<Project>. See kl_obbmap.c.
    {
        const char *proj = ue4_meta_str("com.epicgames.ue4.GameActivity.ProjectName");
        if (proj && *proj) {
            char base[1024];
            snprintf(base, sizeof base, "%s/%s/%s", kl_jni_files_dir(), g_native_dir, proj);
            kl_obbmap_init(kl_jni_obb_dir(), base);
            ue4_maybe_stdout_cmdline(proj);
        }
    }
    ue4_moss_steam_pso_list(out);
    ue4_mobile_multiview_off(out);
    ue4_roboquest_no_occlusion(out);
    if (out) {
        fprintf(out, "  [ue4] activity: %s\n", g_activity);
        fprintf(out, "  [ue4] userdata: %s\n", kl_jni_files_dir());
        // Named and CENSUSED here, because nothing else on this door will.
        // A Unity guest asks Java where its OBB is and the census rides along
        // on the answer; UE4 builds Android's own path itself and asks nobody,
        // so a completely unstaged 8.5 GB produced no line anywhere — and the
        // engine reports a missing OBB the way it reports a missing .uproject,
        // by carrying on.
        fprintf(out, "  [ue4] obb:      %s\n", kl_jni_obb_dir());
        fflush(out);
        ue4_report_command_line(out);
    }
    return 0;
}

// libc++_shared's own calls to operator new/delete, bound to libUE4's — which is
// where bionic binds them, libUE4 being the root of the group that loads
// libc++_shared. Our chain binds dependencies first, so libc++_shared's slots
// for these named its own copies (plain malloc/free) instead. Zero Caliber 2
// (x6072194326150034) is where that mattered: the AWS SDK's log formatting
// (Aws::InitAPI -> UnrealLogSystem::LogStream) grows an ostringstream inside
// libc++_shared, so on its own operator new, then libUE4 frees the string inline
// through FMallocBinned2, which does not know the block and raises a Fatal —
// AndroidThunkJava_ForceQuit one second into the boot, and a black screen.
// KL_UE4_CXX_ALLOC=1 for any UE4 title, =0 for the old binding.
static void ue4_cxx_alloc_from_ue4(FILE *out) {
    static const char *const ops[] = { "_Znwm", "_Znam", "_ZdlPv", "_ZdaPv" };
    const char *t = kl_driver_target_name();
    if (!kl_env_on("KL_UE4_CXX_ALLOC", t && strcmp(t, "x6072194326150034") == 0)) return;
    kl_image *cxx = kl_find_image("libc++_shared.so");
    if (!cxx || !g_ue4) return;
    int moved = 0;
    for (size_t k = 0; k < sizeof ops / sizeof *ops; k++) {
        void *own = kl_sym(cxx, ops[k]), *ue4 = kl_sym(g_ue4, ops[k]);
        if (!own || !ue4) continue;
        for (size_t i = 0; i < sizeof UE4_CHAIN / sizeof *UE4_CHAIN; i++) {
            kl_image *img = kl_find_image(UE4_CHAIN[i]);
            if (!img || img == g_ue4) continue;
            int n = kl_rebind(img, ops[k], own, ue4);
            if (n > 0) moved += n;
        }
    }
    if (out) {
        fprintf(out, "  [ue4] operator new/delete: %d slot(s) that bound libc++_shared's "
                     "own copies now take %s's, as bionic binds them (KL_UE4_CXX_ALLOC)\n",
                moved, UE4_LIB);
        fflush(out);
    }
}

int kl_ue4_load(FILE *out) {
    if (out) {
        fprintf(out, "=== the chain (%zu libraries, dependencies first) ===\n",
                sizeof UE4_CHAIN / sizeof *UE4_CHAIN);
        fflush(out);
    }
    // Mapped and relocated in order, then the initializers are run in the same
    // order — NOT interleaved. libUE4's static initializers reach straight into
    // libc++_shared, so a chain that ran each library's DT_INIT_ARRAY as it
    // loaded would be running libUE4's against a libc++ whose own had not
    // happened yet.
    for (size_t i = 0; i < sizeof UE4_CHAIN / sizeof *UE4_CHAIN; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", g_libdir, UE4_CHAIN[i]);
        // A dependency this guest does not ship is skipped by name; only the
        // entry library is load-or-fail. See the note above UE4_CHAIN.
        // kl_can_load, NOT access(): on device the guest libraries ship as
        // translated frameworks (libfoo.framework/libfoo) and the ELF tree is
        // deliberately not in the bundle, so access() on the ELF path says
        // "absent" for every one of them. The first build of this skip did
        // exactly that — all five deps skipped on a guest that ships all five,
        // libUE4 bound without libc++_shared, and its first static initializer
        // died on an unresolved __cxa_guard_acquire.
        if (strcmp(UE4_CHAIN[i], UE4_LIB) != 0 && !kl_can_load(path)) {
            if (out) fprintf(out, "  --     %-22s not in this guest — skipped\n",
                             UE4_CHAIN[i]);
            continue;
        }
        kl_image *img = kl_load_auto(path);
        if (!img) {
            snprintf(g_err, sizeof g_err, "%s: %s", UE4_CHAIN[i], kl_error());
            return 1;
        }
        kl_register_image(UE4_CHAIN[i], img);
        if (!strcmp(UE4_CHAIN[i], UE4_LIB)) g_ue4 = img;
        // The LOAD ADDRESS is printed, in the shape tools/symbolize_sample.py
        // parses (`<soname> @0x... N.NN MB`). `sample <pid>` is the diagnostic
        // that keeps paying off here — a UE4 guest owns its own frame loop, so
        // "what is the game thread doing" is a question no return value answers
        // — and it reports every guest frame as `??? (in <unknown binary>)`
        // until something joins the two. Without this line the sampler is
        // useless on the one target that needs it most.
        if (out) {
            fprintf(out, "  mapped %-22s @%p %7.2f MB\n", UE4_CHAIN[i],
                    kl_base(img), kl_span(img) / 1048576.0);
            fflush(out);
        }
    }
    if (!g_ue4) {
        snprintf(g_err, sizeof g_err, "%s is not in the chain", g_entry_so);
        return 1;
    }
    ue4_cxx_alloc_from_ue4(out);     // before any initializer allocates

    for (size_t i = 0; i < sizeof UE4_CHAIN / sizeof *UE4_CHAIN; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", g_libdir, UE4_CHAIN[i]);
        kl_image *img = kl_find_image(path);
        if (!img) continue;
        if (out) { fprintf(out, "  init %s\n", UE4_CHAIN[i]); fflush(out); }
        kl_run_init(img);
    }

    // JNI_OnLoad — for EVERY library in the chain that exports one, not just
    // libUE4.
    //
    // UE4 exports all three doors — JNI_OnLoad, ANativeActivity_onCreate and
    // android_main — and Android runs them in that order: System.loadLibrary
    // first (which is what calls JNI_OnLoad), then the activity, then the
    // engine's own thread inside android_main.
    //
    // The part that was missing is that GameActivity.java calls
    // System.loadLibrary for each packaged library, so Android's linker calls
    // JNI_OnLoad on each of them — that is the ONLY moment a library gets
    // handed a JavaVM*, and a library loaded as somebody's DT_NEEDED never
    // gets one. Running it for libUE4 alone left every other library in the
    // chain without a VM, and the cost of that was invisible until it was
    // enormous: libEOSSDK.so caches the VM in its JNI_OnLoad, and without it
    // `EOS_Initialize` returns **17000, EOS_Android_JavaVMNotStored**. Epic
    // Online Services then never starts, the Redpoint subsystem factory
    // produces nothing, and Tetris Effect dereferences the resulting null
    // `GetOnlineSubsystem(FName("RedpointEOS"))` at level BeginPlay — a
    // SIGSEGV an hour of rendering work away from the missing call.
    //
    // Chain order, dependencies first, with libUE4 last exactly as it is in
    // UE4_CHAIN: a library's JNI_OnLoad may call into what it links against.
    //
    // NOT fatal if a library has no JNI_OnLoad, and not fatal if libUE4's is
    // absent either, unlike the Unity path where JNI_OnLoad IS the entry
    // point. Here it is the engine registering its GameActivity natives, and a
    // build that registers them statically instead would be a different shape
    // rather than a broken one — so this reports and carries on, and the
    // NativeActivity door is what the run actually turns on.
    typedef int (*jni_onload_fn)(void *vm, void *reserved);
    int ue4_saw_onload = 0;
    for (size_t i = 0; i < sizeof UE4_CHAIN / sizeof *UE4_CHAIN; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", g_libdir, UE4_CHAIN[i]);
        kl_image *img = kl_find_image(path);
        if (!img) continue;
        jni_onload_fn onload = (jni_onload_fn)kl_sym(img, "JNI_OnLoad");
        if (!onload) continue;
        kl_jni_local_frame_push();
        int version = onload(kl_jni_vm(), NULL);
        kl_jni_local_frame_pop();
        if (img == g_ue4) ue4_saw_onload = 1;
        if (out) {
            fprintf(out, "  JNI_OnLoad %-22s returned 0x%08x\n",
                    UE4_CHAIN[i], version);
            fflush(out);
        }
    }
    if (!ue4_saw_onload && out)
        fprintf(out, "  no JNI_OnLoad in %s — the activity door is the entry\n",
                UE4_LIB);
    return 0;
}

unsigned kl_ue4_gap(FILE *out) {
    if (!g_ue4) return 0;
    unsigned n = 0;
    const char *const *miss = kl_missing_imports(g_ue4, &n);
    if (!out) return n;
    // Uncategorised, deliberately: t_load already sorts a library's gap into
    // families and this is the RUNTIME's view of the same question after the
    // whole chain is up, where the interesting property is which names survived
    // rather than what kind they are. Four to a line, as t_load prints them, so
    // the two lists can be diffed by eye.
    fprintf(out, "  %s: %u unique unresolved import%s\n",
            UE4_LIB, n, n == 1 ? "" : "s");
    for (unsigned i = 0; i < n; i++) {
        if (i % 4 == 0) fprintf(out, "      ");
        fprintf(out, "%-28s", miss[i]);
        if (i % 4 == 3 || i + 1 == n) fprintf(out, "\n");
    }
    fflush(out);
    return n;
}

// ---- GameActivity.java's own half of the boot ----
//
// The NativeActivity door is only half of how a UE4 guest starts, and the half
// that is missing is invisible: `ANativeActivity_onCreate` spawns the game
// thread through the NDK's app glue, and that thread's FIRST act inside
// `android_main` is to spin waiting for `GResumeMainInit`, which nothing native
// ever sets. It is set by `nativeResumeMainInit`, one of twenty-eight natives
// GameActivity.java calls on itself — so on Android the engine is configured
// from JAVA, by the activity, and released by the activity.
//
// That is a genuinely different shape from every guest here so far. A Unity
// guest's Java front end hands libunity a few objects and then libunity drives
// itself; UE4's hands the engine its file paths, its device strings, its window
// geometry and its OBB identity as ARGUMENTS, and until they arrive the engine
// has not started. Nothing about it is optional and nothing about it fails by
// name: the guest simply sits, forever, with every counter healthy — which is
// exactly what the first runs of this target looked like.
//
// So this is a transcription of `GameActivity.onCreate` and `onResume` from
// THIS APK's smali, in their order, calling the natives libUE4 exports by
// symbol. The values are read from the same places the Java reads them — the
// manifest <meta-data> block, the files directory, the package name and version
// — rather than being restated here, because a constant transcribed twice is a
// constant that goes stale on a guest swap.
//
// The natives are static exports (`Java_com_epicgames_ue4_GameActivity_native*`)
// rather than `RegisterNatives` bindings, which is why the JNI surface reports
// zero natives registered on this target and is not a gap.
typedef unsigned char ue4_jboolean;

// One typedef per shape, named after the Java signature it is a transcription
// of. A function-pointer type cannot be a macro argument (its commas are the
// macro's), and naming them after the descriptor keeps the call sites checkable
// against the smali line above each one.
typedef void (*ue4_fn_v)(void *, void *);
typedef void (*ue4_fn_ZI)(void *, void *, ue4_jboolean, int);
typedef void (*ue4_fn_Z)(void *, void *, ue4_jboolean);
typedef void (*ue4_fn_ZZssZsZ)(void *, void *, ue4_jboolean, ue4_jboolean,
                               void *, void *, ue4_jboolean, void *, ue4_jboolean);
// UE 4.21 and older: nativeSetGlobalActivity without bPublicLogFiles —
// (bUseExternalFilesDir, internalFilePath, externalFilePath, bOBBinAPK, APKFilename).
typedef void (*ue4_fn_ZssZs)(void *, void *, ue4_jboolean, void *, void *, ue4_jboolean, void *);
typedef void (*ue4_fn_sssss)(void *, void *, void *, void *, void *, void *, void *);
typedef void (*ue4_fn_ssIIs)(void *, void *, void *, void *, int, int, void *);
// UE 4.26+ grew a TargetSDKVersion int as the 2nd arg of
// nativeSetAndroidVersionInformation: (String, int, String, String, String, String).
typedef void (*ue4_fn_sIssss)(void *, void *, void *, int, void *, void *, void *, void *);
// (String, String): the Redpoint EOS plugin's nativeSetEOSCacheDirectories.
typedef void (*ue4_fn_ss)(void *, void *, void *, void *);
// (String[]): nativeSetConfigRulesVariables.
typedef void (*ue4_fn_as)(void *, void *, void *);

static void *ue4_native(const char *name) {
    if (!g_ue4) return NULL;
    char sym[256];
    snprintf(sym, sizeof sym, "%s%s", g_native_prefix, name);
    return kl_sym(g_ue4, sym);
}

// A missing native is reported and skipped rather than fatal: this table is a
// transcription of one build's Java, and a build that dropped a setter is a
// different shape rather than a broken one. What must not happen silently is
// the opposite — a setter present and never called — so every one of these is
// named either way.
#define UE4_CALL(out, name, type, ...)                                          \
    do {                                                                         \
        type fn__ = (type)ue4_native(name);                                       \
        if (!fn__) {                                                              \
            if (out) fprintf(out, "  [ue4] %s — not exported, skipped\n", name);   \
        } else {                                                                  \
            if (out) { fprintf(out, "  [ue4] %s\n", name); fflush(out); }          \
            fn__(env, thiz, ##__VA_ARGS__);                                        \
        }                                                                          \
    } while (0)

// `<meta-data android:value="true"/>` as the Java reads it: Bundle.getBoolean
// answers false for anything that is not the literal "true", including an
// absent key.
// The <meta-data> keys are spelled with the engine generation's package segment
// — "com.epicgames.ue4.*" for UE4, "com.epicgames.unreal.*" for UE5. This file's
// call sites all name the ue4 spelling; a UE5 manifest carries the unreal one.
// Resolving either spelling from one lookup is what keeps a UE5 target (Wanderer,
// OLAR) from reading empty ProjectName — which left the OBB read-through
// uninitialised and the guest crashing on content it never mounted.
static const char *ue4_meta_raw(const char *key) {
    if (!key) return NULL;
    const char *v = kl_jni_manifest_meta(key);
    if (v && *v) return v;
    char alt[256];
    const char *u4 = strstr(key, ".ue4.");
    const char *un = strstr(key, ".unreal.");
    if (u4) snprintf(alt, sizeof alt, "%.*s.unreal.%s", (int)(u4 - key), key, u4 + 5);
    else if (un) snprintf(alt, sizeof alt, "%.*s.ue4.%s", (int)(un - key), key, un + 8);
    else return v;   // no package segment to swap
    const char *w = kl_jni_manifest_meta(alt);
    return (w && *w) ? w : v;
}

static ue4_jboolean ue4_meta_bool(const char *key) {
    const char *v = ue4_meta_raw(key);
    return (ue4_jboolean)(v && strcmp(v, "true") == 0);
}

static int ue4_meta_int(const char *key, int dflt) {
    const char *v = ue4_meta_raw(key);
    return v ? (int)strtol(v, NULL, 10) : dflt;
}

static const char *ue4_meta_str(const char *key) {
    const char *v = ue4_meta_raw(key);
    return v ? v : "";
}

// Portrait or landscape, as `Configuration.orientation == ORIENTATION_PORTRAIT`.
// A headset is landscape, and it is the same answer the display panel gives
// everywhere else in this project.
#define UE4_PORTRAIT 0

static void kl_ue4_java_create(FILE *out) {
    void *env  = kl_jni_env();
    void *thiz = kl_jni_activity();
    if (!env || !thiz) return;

    if (out) { fprintf(out, "=== GameActivity.onCreate (the Java half) ===\n"); fflush(out); }

    // nativeSetGlobalActivity(bUseExternalFilesDir, bPublicLogFiles,
    //                         internalFilePath, externalFilePath,
    //                         bOBBinAPK, APKPath, bIsNotDebuggable)
    //
    // Both file paths are the guest's userdata directory here. On Android they
    // are `getFilesDir()` and `getExternalFilesDir(null)`, two directories the
    // app owns; here there is one, and handing over two names for it is what
    // makes `bUseExternalFilesDir` a choice with no consequence rather than a
    // fork into a tree that does not exist: a resource with two doors has to
    // resolve to one place by construction.
    const char *files = kl_jni_files_dir();
    void *jinternal = kl_jni_new_string(files);
    void *jexternal = kl_jni_new_string(files);
    void *japk      = kl_jni_new_string(kl_jni_apk_path());
    // The shape of this native is the engine's, like nativeSetAndroidVersion-
    // Information's below, and the manifest's EngineVersion says which. UE 4.21
    // declares it WITHOUT bPublicLogFiles: (bUseExternalFilesDir, internalFilePath,
    // externalFilePath, bOBBinAPK, APKFilename) — read off the dex of Robo
    // Recall: Unplugged (x1906871599408213, 4.21.2, 2026-10-14). Called in the
    // later shape, that engine reads bPublicLogFiles' register as its internal
    // files path — a null jstring, an empty string — and in a Shipping build
    // with bUseExternalFilesDir that empty string becomes GFilePathBase: the
    // game then probes "/UE4Game/RoboRecall/..." at the root of the filesystem
    // (KL_TRACE_FS), finds neither its paks nor a descriptor, and stops at
    // "Failed to open descriptor file ../../../RoboRecall/RoboRecall.uproject"
    // with a black screen. The extra arguments shift bOBBinAPK and APKFilename
    // the same way. 4.22 and later carry the flag: every UE title in this tree
    // whose manifest names its engine is 4.24 or newer (4.24.3, 4.25.3, 4.26.2,
    // 4.27.x, 5.2.0), so none of them takes the old form, and a manifest that
    // names no engine keeps the call it always had.
    int global_major = 0, global_minor = 0;
    {
        const char *ev = ue4_meta_str("com.epicgames.ue4.GameActivity.EngineVersion");
        if (sscanf(ev, "%d.%d", &global_major, &global_minor) != 2)
            global_major = global_minor = 0;
    }
    if (global_major == 4 && global_minor <= 21) {
        if (out) fprintf(out, "  engine %d.%d per the manifest: nativeSetGlobalActivity in the five-argument (4.21) form\n",
                         global_major, global_minor);
        UE4_CALL(out, "nativeSetGlobalActivity", ue4_fn_ZssZs,
                 ue4_meta_bool("com.epicgames.ue4.GameActivity.bUseExternalFilesDir"),
                 jinternal, jexternal,
                 ue4_meta_bool("com.epicgames.ue4.GameActivity.bPackageDataInsideApk"),
                 japk);
    } else {
        UE4_CALL(out, "nativeSetGlobalActivity", ue4_fn_ZZssZsZ,
                 ue4_meta_bool("com.epicgames.ue4.GameActivity.bUseExternalFilesDir"),
                 ue4_meta_bool("com.epicgames.ue4.GameActivity.bPublicLogFiles"),
                 jinternal, jexternal,
                 ue4_meta_bool("com.epicgames.ue4.GameActivity.bPackageDataInsideApk"),
                 japk,
                 // The Java's last argument is `(ApplicationInfo.flags &
                 // FLAG_DEBUGGABLE) == 0`. A release APK unpacked from a device is
                 // not debuggable.
                 (ue4_jboolean)1);
    }

    // nativeSetWindowInfo(bIsPortrait, DepthBufferPreference)
    UE4_CALL(out, "nativeSetWindowInfo", ue4_fn_ZI,
             (ue4_jboolean)UE4_PORTRAIT,
             ue4_meta_int("com.epicgames.ue4.GameActivity.DepthBufferPreference", 0));

    // nativeSetAndroidStartupState(bDebuggerAttached)
    UE4_CALL(out, "nativeSetAndroidStartupState", ue4_fn_Z,
             (ue4_jboolean)0);

    // nativeSetAndroidVersionInformation(Build.VERSION.RELEASE, Build.MANUFACTURER,
    //                                    Build.MODEL, Build.DISPLAY, locale)
    // Read through kl_jni's Build table, so this guest is told the same device
    // the JNI surface describes to every other one.
    char lang[16] = "en", country[16] = "US";
    kl_jni_locale_parts(lang, sizeof lang, country, sizeof country);
    char locale[40];
    snprintf(locale, sizeof locale, "%s_%s", lang, country);
    // "RELEASE", not "VERSION.RELEASE": kl_jni_build_string matches the FIELD
    // name within Build / Build$VERSION, and the dotted spelling matched nothing
    // — so every UE4 guest was told its Android version was the empty string.
    // Most never look. Moss does: its device-profile rules only enable Vulkan
    // for `SRC_AndroidVersion` matching "([0-9]+).*" with a major >= 9, and ""
    // matches nothing, leaving r.Android.DisableVulkanSupport at the base
    // profile's 1 and the engine at "This device does not support Vulkan".
    void *jrel   = kl_jni_new_string(kl_jni_build_string("RELEASE"));
    void *jmake  = kl_jni_new_string(kl_jni_build_string("MANUFACTURER"));
    void *jmodel = kl_jni_new_string(kl_jni_build_string("MODEL"));
    void *jbuild = kl_jni_new_string(kl_jni_build_string("DISPLAY"));
    void *jloc   = kl_jni_new_string(locale);
    // The signature is UE4-version-dependent. UE 4.25 takes five strings
    // (AndroidVersion, PhoneMake, PhoneModel, PhoneBuildNumber, OSLanguage);
    // UE 4.26+ inserts an int TargetSDKVersion as the 2nd argument:
    // (AndroidVersion, TargetSDKVersion, PhoneMake, PhoneModel, PhoneBuildNumber,
    // OSLanguage). Passing the wrong shape shifts every following jstring by one
    // register, so the engine reads garbage as a String and faults the moment it
    // calls GetStringUTFChars on it — which is exactly how TWD2 died, in klj_str,
    // right after this call.
    //
    // 4.26+ is the DEFAULT and 4.25 is the exception, not the other way round:
    // every Quest UE4 title in this tree from 2022 on (Wrath2 4.27, TWD2, Red
    // Matter 2, Vampire) is 4.26+, and UE5 (Wanderer, libUnreal) inherited the
    // int-inserted form too. Resident Evil 4 VR (2021, built on 4.25) is the
    // only known five-string guest; it and any future 4.25 title are named here.
    //
    // Which form a guest takes is its engine's version, and the manifest says
    // it: com.epicgames.ue4.GameActivity.EngineVersion ("4.27.2" for Wrath 2).
    // 4.25 and older is the five-string form. Where the manifest does not say,
    // the names decide as before — "re4", and the same game under its
    // store-built target, which is how it was sent in as failing: the int 32
    // read as a jstring, a fault at 0x20 before the first frame (fix 18,
    // 2026-10-11). A wrong guess does not corrupt, it faults loudly in klj_str
    // exactly as TWD2 did, which names the fix.
    const char *ue4_tgt = kl_driver_target_name();
    // Tales from the Galaxy's Edge (x3484270071659289, fix 16) is the third:
    // an engine old enough to link GNU STL.
    int is_425 = ue4_tgt && (strcmp(ue4_tgt, "re4") == 0 || strcmp(ue4_tgt, "x2637179839719680") == 0 ||
                             strcmp(ue4_tgt, "x3484270071659289") == 0);
    {
        const char *ev = ue4_meta_str("com.epicgames.ue4.GameActivity.EngineVersion");
        int major = 0, minor = 0;
        if (sscanf(ev, "%d.%d", &major, &minor) == 2 && major >= 4) {
            is_425 = major == 4 && minor <= 25;
            fprintf(out, "  engine %s per the manifest: nativeSetAndroidVersionInformation in the %s form\n",
              ev, is_425 ? "five-string (4.25)" : "4.26+");
        }
    }
    if (is_425) {
        UE4_CALL(out, "nativeSetAndroidVersionInformation", ue4_fn_sssss,
                 jrel, jmake, jmodel, jbuild, jloc);
    } else {
        UE4_CALL(out, "nativeSetAndroidVersionInformation", ue4_fn_sIssss,
                 jrel, 32 /* TargetSDKVersion — informational to the engine */,
                 jmake, jmodel, jbuild, jloc);
    }

    // nativeSetObbInfo(ProjectName, PackageName, Version, PatchVersion, AppType)
    // The Java passes `PackageInfo.versionCode` for Version and a literal 0 for
    // PatchVersion — this is what the engine builds `main.<version>.<package>.obb`
    // out of, so it is the same fact klj_obb_census checks the staged files
    // against and it comes from the same place (apktool.yml, via kl_jni).
    long version = 0;
    kl_jni_guest_version(&version, NULL);
    void *jproj = kl_jni_new_string(ue4_meta_str("com.epicgames.ue4.GameActivity.ProjectName"));
    void *jpkg  = kl_jni_new_string(kl_jni_guest_package());
    void *jtype = kl_jni_new_string(ue4_meta_str("com.epicgames.ue4.GameActivity.AppType"));
    UE4_CALL(out, "nativeSetObbInfo", ue4_fn_ssIIs,
             jproj, jpkg, (int)version, 0, jtype);

    // nativeSetEOSCacheDirectories(InternalFilesDir, ExternalFilesDir)
    // Not the engine's: Redpoint's EOS Online Subsystem adds it to onCreate
    // through its UPL, right after nativeSetObbInfo, with the same two paths
    // nativeSetGlobalActivity got. FEOSRuntimePlatformAndroid keeps them as
    // InternalPath/ExternalPath and check()s both when the EOS platform is
    // initialised; never set, that check() raised SIGTRAP on the game thread
    // seven seconds into Ghostbusters: Rise of the Ghost Lord
    // (x4746232908818706, headset log 2026-10-18, just after an
    // AndroidThunkJava_ForceQuit from the same failure path). Only a build
    // with that plugin exports the native, so the others are not told it is
    // missing.
    if (ue4_native("nativeSetEOSCacheDirectories")) {
        UE4_CALL(out, "nativeSetEOSCacheDirectories", ue4_fn_ss,
                 kl_jni_new_string(files), kl_jni_new_string(files));
    }

    // nativeSetConfigRulesVariables(String[] keyValuePairs)
    //
    // GameActivity.onCreate hands the engine the variables its config rules
    // set — key, value, key, value — evaluated in Java against the device.
    // Nothing here evaluates config rules, so the answer is the one a device
    // that no rule matched gets: an empty array. Unreal 5.6 in Meta's fork
    // (Roboquest VR: x3151760, and the Meta Store build x9269393463181825)
    // WAITS for the call: its EventWorker's first config-rule lookup parks until
    // this native has run, so the APP_CMD_START that onStart queues is never
    // taken off the pipe and the game sits on a black screen with every counter
    // healthy (KL_TRACE_CONDDUMP, 2026-10-22). Only for this title, so no other
    // UE5 game is called with anything it was not called with before.
    {
        const char *t = kl_driver_target_name();
        if (t && strcmp(t, "x9269393463181825") == 0 &&
            ue4_native("nativeSetConfigRulesVariables")) {
            UE4_CALL(out, "nativeSetConfigRulesVariables", ue4_fn_as,
                     kl_jni_new_string_array(NULL, 0));
        }
    }
}

// ...and onResume's half, which is the one that starts the engine: the game
// thread has been spinning on GResumeMainInit since ANativeActivity_onCreate
// returned. GameActivity.onResume re-states the window geometry first (the
// orientation can have changed while the activity was away) and then releases
// it, in that order, so the engine's first look at the window is at a value
// somebody set.
static void kl_ue4_java_resume(FILE *out) {
    void *env  = kl_jni_env();
    void *thiz = kl_jni_activity();
    if (!env || !thiz) return;

    if (out) { fprintf(out, "=== GameActivity.onResume (the Java half) ===\n"); fflush(out); }

    UE4_CALL(out, "nativeSetWindowInfo", ue4_fn_ZI,
             (ue4_jboolean)UE4_PORTRAIT,
             ue4_meta_int("com.epicgames.ue4.GameActivity.DepthBufferPreference", 0));

    // The other arm of the Java's `if` is nativeOnInitialDownloadStarted, i.e.
    // "the OBB is not here, go and fetch it". There is no downloader here and
    // the OBB is staged, so this is the arm a device with its data present
    // takes.
    UE4_CALL(out, "nativeResumeMainInit", ue4_fn_v);
}

int kl_ue4_create(FILE *out) {
    if (!g_ue4) {
        snprintf(g_err, sizeof g_err, "%s was never loaded", g_entry_so);
        return 1;
    }
    if (kl_na_create(g_ue4, "ANativeActivity_onCreate", NULL, out) != 0)
    {
        snprintf(g_err, sizeof g_err, "%s exports no ANativeActivity_onCreate",
                 g_entry_so);
        return 1;
    }
    // After the super call, exactly as GameActivity.onCreate does it: the
    // activity's own body runs on the UI thread while the game thread it just
    // spawned is already spinning.
    kl_ue4_java_create(out);
    return 0;
}

void kl_ue4_start(FILE *out) {
    kl_na_start(out);
    kl_ue4_java_resume(out);
}
void kl_ue4_stop(FILE *out)  { kl_na_stop(out); }

// ------------------------------------------------------- mobile multiview
//
// UE4's OpenGL RHI decides mobile multiview ONCE, into two exported globals,
// and every later decision reads them:
//
//   FOpenGLES::bSupportsMobileMultiView   set in FOpenGLES::ProcessExtensions,
//                                         true only when the GL_EXTENSIONS
//                                         string names GL_OVR_multiview,
//                                         GL_OVR_multiview2 AND
//                                         GL_OVR_multiview_multisampled_-
//                                         render_to_texture, and both
//                                         glFramebufferTexture*MultiviewOVR
//                                         resolve through eglGetProcAddress
//   GSupportsMobileMultiView              copied from it during RHI init
//
// GSupportsMobileMultiView is what GLSLToDeviceCompatibleGLSL branches on when
// it rewrites every cooked shader (read at libUE4+0x9f0a620 in Tetris Effect):
//
//   true  -> it PREPENDS "#extension GL_OVR_multiview2 : enable" and
//            "layout(num_views = 2) in;" to the shader
//   false -> it prepends "#define gl_ViewID_OVR 0" instead
//
// That false branch is UE4's OWN fallback for a device without the extension,
// and it turns the identical cooked shader into ordinary single-view ES 3.0.
// So the multiview wall this gateway hits — ANGLE's Metal backend implements
// no multiview at all, so every such shader fails to compile, the link fails,
// and UE4 turns the Fatal into a ForceQuit — is not a wall that has to be
// climbed. It only has to be told the truth.
//
// This gateway does NOT name any of the three extensions and does NOT serve
// the two entry points, so the honest value of both flags is 0. Writing them
// is belt-and-braces against a path that sets them from something other than
// the extension string; when they are already 0 this costs one byte read per
// pump tick and says nothing.
//
// EVERY WORD ABOVE IS ABOUT OPENGL ES, so the clear is gated on the guest
// actually using the GL gateway (kl_vulkan_guest_active(), inside the
// function). A Vulkan RHI rewrites no cooked GLSL and never reads an ANGLE
// extension string; clearing there only forces UE4's two-sequential-view
// DoubleWide mobile stereo path: on Into the Radius (Vulkan) that path makes
// static meshes vanish and reappear as the head moves.
//
// KL_UE4_MULTIVIEW is a TRI-STATE override of that decision: unset = decide
// from the RHI, 1 = never clear (the A/B, and the way back if a future ANGLE
// grows GL_OVR_multiview2), 0 = always clear (also on Vulkan, the way back if a
// Vulkan guest is ever found that needs the clear).
static void ue4_no_multiview(void) {
    static uint8_t *gsupports, *bsupports;
    static int looked, said, said_vk, ever_vk;
    static uint8_t saved_g, saved_b;
    if (!g_ue4) return;
    // In this fork ANGLE DOES have multiview, and a guest served the Array eye
    // layout on GL (kl_ovrp_gl_array — Maestro) is named all three extensions
    // and renders both eyes in one pass through them. Its flags are true
    // because that is the truth here; clearing them would compile every one of
    // its shaders single-view against a two-layer target. The clear below is
    // only for a guest the extensions are not named to, where it is the
    // belt-and-braces it was written as.
    if (kl_gl_cap_names_multiview()) return;
    // TRI-STATE, and the unset state is the interesting one:
    //   unset -> decide from the RHI the guest actually brought up (below);
    //   1     -> always leave the guest's answer alone;
    //   0     -> always clear, even on Vulkan — the way back if a Vulkan guest
    //            is ever found that needs the clear.
    const char *knob = kl_env_str("KL_UE4_MULTIVIEW", NULL);
    int force_keep  = knob && kl_env_on("KL_UE4_MULTIVIEW", 0);
    int force_clear = knob && !force_keep;
    if (force_keep) return;
    if (!looked) {
        looked = 1;
        gsupports = ue4_native_symbol("GSupportsMobileMultiView");
        bsupports = ue4_native_symbol("_ZN9FOpenGLES24bSupportsMobileMultiViewE");
    }
    // THE CLEAR IS A STATEMENT ABOUT THE GL GATEWAY, so it may only be made
    // about a guest that is using it. ANGLE names no GL_OVR_multiview
    // extension, UE4 prepends `layout(num_views = 2) in;` to every cooked
    // shader when the flag is true, and the compile failure becomes a
    // ForceQuit — that is a real wall for a GL guest and the reason this
    // function exists. A VULKAN RHI rewrites no cooked GLSL and ANGLE is not in
    // the picture: clearing there buys nothing and forces UE4 onto the
    // two-sequential-view DoubleWide mobile stereo path the title never shipped
    // with — on Into the Radius, static meshes that vanish as the head moves.
    //
    // kl_vulkan_guest_active() is the honest test rather than a renderer enum:
    // it is a measurement of what the guest DID (resolved vk* symbols AND
    // created a VkDevice through us, i.e. it is inside FVulkanDynamicRHI::Init)
    // rather than a guess. It goes true before the first shader compile reads
    // the flag, but AFTER some number of pump ticks — so an early tick can
    // clear a flag the Vulkan RHI set moments earlier. Hence save-and-restore
    // rather than merely declining future clears.
    // Sticky: once a Vulkan device has been seen, the guest is a Vulkan guest
    // for good — at quit vkDestroyDevice leaves none, and a tick after that
    // would otherwise clear (and announce clearing) a flag nothing reads any
    // more.
    if (kl_vulkan_guest_active()) ever_vk = 1;
    if (!force_clear && ever_vk) {
        if (saved_g && gsupports) *gsupports = saved_g;
        if (saved_b && bsupports) *bsupports = saved_b;
        saved_g = saved_b = 0;
        if (!said_vk) {
            said_vk = 1;
            fprintf(stderr, "  [ue4] mobile multiview left alone — this guest "
                            "brought up a VULKAN device, so no cooked GLSL is "
                            "rewritten and the GL gateway's extension string "
                            "decides nothing. KL_UE4_MULTIVIEW=0 forces the "
                            "clear back on.\n");
        }
        return;
    }
    int was = (gsupports && *gsupports) || (bsupports && *bsupports);
    if (gsupports && *gsupports) saved_g = *gsupports;
    if (bsupports && *bsupports) saved_b = *bsupports;
    if (gsupports) *gsupports = 0;
    if (bsupports) *bsupports = 0;
    if (was && !said) {
        said = 1;
        fprintf(stderr, "  [ue4] mobile multiview was ON — cleared "
                        "(GSupportsMobileMultiView / FOpenGLES::"
                        "bSupportsMobileMultiView). The GL gateway names no "
                        "GL_OVR_multiview extension, so UE4's own non-multiview "
                        "shader path is the correct one; KL_UE4_MULTIVIEW=1 "
                        "leaves the guest's answer alone.\n");
    }
}

static int g_force_quit;
void kl_ue4_note_force_quit(void) { __atomic_store_n(&g_force_quit, 1, __ATOMIC_RELEASE); }
int kl_ue4_force_quit_obeyed(void) { return __atomic_load_n(&g_force_quit, __ATOMIC_ACQUIRE); }

double kl_ue4_pump(double seconds, const volatile int *quit) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double elapsed = 0;
    for (unsigned t = 0; (quit ? !*quit : 1) && (seconds < 0 || elapsed < seconds)
                         && !kl_ue4_force_quit_obeyed(); t++) {
        kl_ndk_pump_looper(100);
        // Cheap and idempotent: the RHI decides multiview during its own init,
        // which is somewhere inside this pump, and the answer is read much
        // later at the first shader compile.
        ue4_no_multiview();
        // The UI thread's task queue and the frame clock, on the same thread
        // that turns the looper — which is what makes it the UI thread by the
        // only definition that matters: "am I the UI thread" is answered by
        // WHO DRAINS THE QUEUE.
        if ((t + 1) % 10 == 0) kl_jni_drain_ui_tasks();
        kl_jni_tick_choreographer();
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - t0.tv_sec)
                + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
    }
    return elapsed;
}

void kl_ue4_report(FILE *out) {
    if (!out) return;
    // Who is blocked on what, first — this guest owns its own threads and its
    // own frame loop, so unlike the Unity path there is no return value that
    // says "the engine stopped". A UE4 run that produced no frames looks
    // exactly like one that produced frames until the reports are read, and
    // the mutex owner map is the one instrument that separates "still working"
    // from "two of its threads are waiting on each other".
    kl_pthread_report(out);
    kl_egl_report(out);
    kl_opensl_report(out);
    kl_aaudio_report(out);
    kl_mediandk_report(out);
    kl_ovrp_report(out);
    kl_ovrplat_report(out);
    // An Unreal guest from Steam (Moss, depot 846472): Steam instead of the
    // Oculus platform, OpenXR instead of OVRPlugin. Both say nothing for a
    // guest that used neither.
    kl_steamapi_report(out);
    if (kl_openxr_in_use()) kl_openxr_report(out);
    fprintf(out, "\n=== JNI surface ===\n");
    kl_jni_report(out);
    fflush(out);
}

// --- Medal of Honor: the desk's book and receivers answer a nearby hand ------
//
// The game's menu book and telephone receivers in Whitehall are BP_GrabButton
// actors whose hover reaction is switched off in this build's own data: the
// class default has TurnHoverAnimsOff = true and UseLabel = false (nothing in
// the game sets UseLabel, and BFL_Global.SetHighlightForGrabObject only calls
// HighlightOn behind it), and the level gives them no OnHoverTransform, so the
// construction script copies their resting place into it. Only the hand opens
// to say "you can grab this", which players do not read as the main menu.
//
// KL_MOH_HOVER (default on for this title): every 30 guest frames, on the game
// thread, for every BP_GrabButton of the level 001_DesignLevel: UseLabel = 1,
// TurnHoverAnimsOff = 0 (the game sets it again while its menu is open, and
// back only when the player leaves the desk area), and — once, while it still
// equals the resting place — OnHoverTransform = the resting place raised by
// KL_MOH_HOVER_CM (default 4) cm. The game's own HighlightOn/HighlightOff then
// play its ButtonLerp timeline there and back. Objects are found through the
// guest's exported GUObjectArray, matched by FName index (memory reads, no
// strings) against the UE 4.25 layout: FUObjectItem 24 bytes, 64K a chunk;
// UObjectBase flags +0x8, index +0xC, class +0x10, name +0x18, outer +0x20;
// UStruct super +0x40, ChildProperties +0x50; FField next +0x20, name +0x28;
// FProperty offset +0x4C.
#include <mach/mach.h>
typedef struct { uint16_t *data; int32_t num, max; } klmoh_fstring;
// A read that answers 0 instead of faulting, for a pointer not yet known good.
static int klmoh_peek(const void *addr, void *out, size_t n) {
    vm_size_t got = 0;
    if ((uintptr_t)addr < 0x100000000ull) return 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)(uintptr_t)addr, n,
                             (vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == n;
}
// An FName (at `fname`) as ASCII, through the guest's own FName::ToString.
static void klmoh_name(const void *fname, char *buf, size_t cap) {
    static void (*tostring)(const void *, klmoh_fstring *);
    static void (*mfree)(void *);
    if (!tostring) tostring = kl_guest_sym_global("_ZNK5FName8ToStringER7FString");
    if (!mfree) mfree = kl_guest_sym_global("_ZN7FMemory4FreeEPv");
    buf[0] = 0;
    if (!tostring) return;
    klmoh_fstring s = {0};
    tostring(fname, &s);
    size_t n = 0;
    for (int32_t i = 0; s.data && i < s.num && s.data[i] && n + 1 < cap; i++)
        buf[n++] = s.data[i] < 128 ? (char)s.data[i] : '?';
    buf[n] = 0;
    if (s.data && mfree) mfree(s.data);   // the guest allocated it; give it back
}
static void klmoh_hover(void) {
    static int on = -1;
    static unsigned tick;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_MOH_HOVER", t && strcmp(t, "x3647259232032222") == 0);
    }
    if (!on || tick++ % 30) return;
    static uint8_t *arr; static void (*mkname)(uint64_t *, const char *, int);
    static uint64_t n_cls, n_lvl; static const uint8_t *cls; static float lift;
    static int32_t off_label = -1, off_anims = -1, off_hover = -1, off_unhover = -1;
    if (!arr) {
        arr = kl_guest_sym_global("GUObjectArray");
        mkname = kl_guest_sym_global("_ZN5FNameC1EPKc9EFindName");
        if (!arr || !mkname) { on = 0; return; }
        lift = kl_env_float("KL_MOH_HOVER_CM", 4.0f);
    }
    // FNAME_Find: NAME_None until the shell's packages are loaded.
    if (!n_cls) { mkname(&n_cls, "BP_GrabButton_C", 0); mkname(&n_lvl, "001_DesignLevel", 0); }
    if (!n_cls || !n_lvl) { n_cls = 0; return; }
    uint8_t **chunks = *(uint8_t ***)(arr + 16);
    int32_t num = *(int32_t *)(arr + 16 + 20);
    for (int32_t i = 0; i < num; i++) {
        uint8_t *chunk = chunks[i / 65536];
        if (!chunk) continue;
        uint8_t *o = *(uint8_t **)(chunk + (size_t)(i % 65536) * 24);
        if (!o) continue;
        const uint8_t *c = *(const uint8_t **)(o + 0x10);
        if (!c || (cls && c != cls)) continue;
        if (!cls) {
            uint64_t cn;
            if (!klmoh_peek(c + 0x18, &cn, 8) || cn != n_cls) continue;
            cls = c;
            for (const uint8_t *st = cls; st; st = *(const uint8_t **)(st + 0x40))
                for (const uint8_t *f = *(const uint8_t **)(st + 0x50); f; f = *(const uint8_t **)(f + 0x20)) {
                    char nm[64]; klmoh_name(f + 0x28, nm, sizeof nm);
                    int32_t off = *(const int32_t *)(f + 0x4C);
                    if (!strcmp(nm, "UseLabel")) off_label = off;
                    else if (!strcmp(nm, "TurnHoverAnimsOff")) off_anims = off;
                    else if (!strcmp(nm, "OnHoverTransform")) off_hover = off;
                    else if (!strcmp(nm, "OnUnHoverTransform")) off_unhover = off;
                }
            fprintf(stderr, "  [ue4] KL_MOH_HOVER: BP_GrabButton found; UseLabel @0x%x TurnHoverAnimsOff @0x%x "
                            "OnHoverTransform @0x%x OnUnHoverTransform @0x%x, lift %.1f cm\n",
                    off_label, off_anims, off_hover, off_unhover, (double)lift);
            if (off_label < 0 || off_anims < 0 || off_hover < 0 || off_unhover < 0) { on = 0; return; }
        }
        if (*(int32_t *)(o + 0xC) != i) continue;
        if (*(const int32_t *)(o + 8) & 0x10) continue;          // RF_ClassDefaultObject
        // only the desk in Whitehall: actor -> PersistentLevel -> world "001_DesignLevel"
        const uint8_t *lvl = *(const uint8_t **)(o + 0x20);
        const uint8_t *world = lvl ? *(const uint8_t **)(lvl + 0x20) : NULL;
        uint64_t wn = 0;
        if (!world || !klmoh_peek(world + 0x18, &wn, 8) || wn != n_lvl) continue;
        o[off_label] = 1;
        o[off_anims] = 0;
        float *hov = (float *)(o + off_hover), *un = (float *)(o + off_unhover);
        if (lift > 0 && !memcmp(hov, un, 48)) {                  // FTransform: quat, translation at +16
            hov[6] = un[6] + lift;
            char nm[96]; klmoh_name(o + 0x18, nm, sizeof nm);
            fprintf(stderr, "  [ue4] KL_MOH_HOVER: %s lifts %.1f cm on hover\n", nm, (double)lift);
        }
    }
}

void kl_ue4_frame_tick(void) { klmoh_hover(); }
