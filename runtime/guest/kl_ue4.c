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
#include "kl_mediandk.h"
#include <sys/stat.h>
#include "kl_aaudio.h"
#include "kl_obbmap.h"
#include "kl_opensl.h"

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
    // FMOD, dependencies first (libfmodstudio is built on the libfmod core).
    // libUE4 has these as DT_NEEDED and binds FMOD_* / FMOD::* against them, but
    // the translated libUE4 carries no LC_LOAD_DYLIB, so a symbol only resolves
    // if its library is already in the image pool when libUE4 binds. Absent from
    // the chain, every FMOD import fell through to kl_unresolved_named and the
    // first FMOD_Debug_Initialize aborted. Loaded here, before libUE4, they bind.
    "libfmod.so",
    "libfmodstudio.so",
    "libovraudio64.so",     // the Oculus audio spatializer
    "libplaycore.so",       // Google Play core — the OBB downloader's half
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

// A native of the guest's, by exported symbol. Registered with the JNI surface
// so `AndroidThunkJava_*` handlers can make the call GameActivity.java would
// have made; see kl_jni.h's kl_jni_set_guest_native_resolver.
static void *ue4_native_symbol(const char *symbol) {
    return (g_ue4 && symbol) ? kl_sym(g_ue4, symbol) : NULL;
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
static int ue4_stdout_default(void) {
    static const char *on[] = { "olar", "wrath2", "twd2" };
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
            //     (the segment then never sees the cache open/complete). Instead a
            //     guest patch forces NumPrecompilesRemaining()->0 (see
            //     kl_guestpatch.c "wrath2-pso-done") so the segment completes while
            //     the cache still opens normally; PSOs compile on demand.
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
        // KL_TWD2_ALT=1 restores the async thread to A/B.
        else if (t && strcmp(t, "twd2") == 0 && !kl_env_on("KL_TWD2_ALT", 0))
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

    for (size_t i = 0; i < sizeof UE4_CHAIN / sizeof *UE4_CHAIN; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", g_libdir, UE4_CHAIN[i]);
        kl_image *img = kl_find_image(path);
        if (!img) continue;
        if (out) { fprintf(out, "  init %s\n", UE4_CHAIN[i]); fflush(out); }
        kl_run_init(img);
    }

    // FMOD's Android platform layer — the AudioTrack/OpenSL output and the JavaVM
    // it needs for AudioManager queries — is set up in libfmod's JNI_OnLoad, which
    // on Android is called by System.loadLibrary("fmod"). Here libfmod is mapped by
    // the chain above (kl_load_auto), which only runs DT_INIT, never JNI_OnLoad. So
    // call it explicitly, with the synthetic JavaVM, before libUE4's own JNI_OnLoad.
    // Without it FMOD::getGlobals returns FMOD_ERR_INTERNAL (28) -> System::create
    // fails -> Studio never comes up -> no banks, no events (vampire: silent). The
    // DT_NEEDED path has its own hook (kl_maybe_jni_onload); this covers the UE4
    // chain, which is how every FMOD title here actually loads it.
    {
        char fpath[1200];
        snprintf(fpath, sizeof fpath, "%s/libfmod.so", g_libdir);
        kl_image *fmod = kl_find_image(fpath);
        if (fmod) {
            int (*fonload)(void *, void *) = (int (*)(void *, void *))
                kl_sym(fmod, "JNI_OnLoad");
            if (fonload) {
                kl_jni_local_frame_push();
                int fr = fonload(kl_jni_vm(), NULL);
                kl_jni_local_frame_pop();
                if (out) { fprintf(out, "  libfmod JNI_OnLoad -> 0x%08x "
                                        "(FMOD Android platform init)\n", fr); fflush(out); }
            }
        }
    }

    // JNI_OnLoad. UE4 exports all three doors — JNI_OnLoad,
    // ANativeActivity_onCreate and android_main — and Android runs them in that
    // order: System.loadLibrary first (which is what calls JNI_OnLoad), then
    // the activity, then the engine's own thread inside android_main.
    //
    // NOT fatal if it is absent, unlike the Unity path where JNI_OnLoad IS the
    // entry point. Here it is the engine registering its GameActivity natives,
    // and a build that registers them statically instead would be a different
    // shape rather than a broken one — so this reports and carries on, and the
    // NativeActivity door is what the run actually turns on.
    typedef int (*jni_onload_fn)(void *vm, void *reserved);
    jni_onload_fn onload = (jni_onload_fn)kl_sym(g_ue4, "JNI_OnLoad");
    if (!onload) {
        if (out) fprintf(out, "  no JNI_OnLoad — the activity door is the entry\n");
        return 0;
    }
    kl_jni_local_frame_push();
    int version = onload(kl_jni_vm(), NULL);
    kl_jni_local_frame_pop();
    if (out) {
        fprintf(out, "  JNI_OnLoad returned 0x%08x\n", version);
        fflush(out);
    }
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
typedef void (*ue4_fn_sssss)(void *, void *, void *, void *, void *, void *, void *);
typedef void (*ue4_fn_ssIIs)(void *, void *, void *, void *, int, int, void *);
// UE 4.26+ grew a TargetSDKVersion int as the 2nd arg of
// nativeSetAndroidVersionInformation: (String, int, String, String, String, String).
typedef void (*ue4_fn_sIssss)(void *, void *, void *, int, void *, void *, void *, void *);

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
    void *jrel   = kl_jni_new_string(kl_jni_build_string("VERSION.RELEASE"));
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
    // The engine version is not cheaply readable from the stripped binary, so
    // this is a name list rather than a probe — a wrong guess does not corrupt,
    // it faults loudly in klj_str exactly as TWD2 did, which names the fix.
    //
    // ...plus one probe, for a 4.25 guest under a name that is not in the list:
    // Resident Evil 4 adopted from the user's own APK runs as a generic target
    // (AVP Play names it x<store id>), took the 4.26+ shape, and died in klj_str
    // exactly as described. The exported GameActivity natives tell the two
    // apart: 4.26 added nativeOnTrimMemory, nativeOnThermalStatusChangedListener
    // and nativeSetMemoryAdvisorState (measured: Wrath 2's 4.27 libUE4 exports
    // all three, RE4's 4.25 none), so a guest exporting NONE of them is 4.25.
    // Any one present keeps the 4.26+ default, so a 4.26+ build that dropped
    // one of the three is still read correctly.
    const char *ue4_tgt = kl_driver_target_name();
    int is_425 = ue4_tgt && strcmp(ue4_tgt, "re4") == 0;
    if (!is_425 && !ue4_native("nativeOnTrimMemory") &&
        !ue4_native("nativeOnThermalStatusChangedListener") &&
        !ue4_native("nativeSetMemoryAdvisorState")) {
        is_425 = 1;
        if (out) fprintf(out, "  [ue4] no 4.26+ natives exported — taking UE 4.25's "
                              "five-string nativeSetAndroidVersionInformation\n");
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

double kl_ue4_pump(double seconds, const volatile int *quit) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double elapsed = 0;
    for (unsigned t = 0; (quit ? !*quit : 1) && (seconds < 0 || elapsed < seconds); t++) {
        kl_ndk_pump_looper(100);
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
    fprintf(out, "\n=== JNI surface ===\n");
    kl_jni_report(out);
    fflush(out);
}
