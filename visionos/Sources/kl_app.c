// See kl_app.h.
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>          // task_info(TASK_VM_INFO) — the heartbeat's footprint
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "klepton.h"
#include "kl_jni.h"
#include "kl_fault.h"
#include "kl_egl.h"
#include "kl_opensl.h"
#include "kl_ovrp.h"      // kl_ovrp_frame_latch — one pose per guest frame
#include "kl_openxr.h"    // kl_openxr_set_pacer — the OpenXR guest's frame clock
#include "kl_slink.h"
#include "kl_x18.h"       // trap 11 — claim TSD slot 300 before anything else can
#include "kl_env.h"

// --- Which guest ------------------------------------------------------------
//
// Everything below used to say "beatsaber" in five places, which was honest
// while there was one guest and became a lie the moment a second app was built
// from this tree. A target is the small set of facts that differ: what the tree
// is called, which library the chain starts at, and which boot sequence runs.
//
// The default is baked in at BUILD time (KL_TARGET_DEFAULT, set by
// gen_xcodeproj.py) rather than read from the environment, because the app has
// to know which guest it is when it is launched by hand from the Home View with
// no environment at all. KL_TARGET still overrides it, which is what makes an
// A/B possible from `run.sh` without regenerating the project.
#ifndef KL_TARGET_DEFAULT
#define KL_TARGET_DEFAULT "beatsaber"
#endif

typedef struct {
    const char *name;      // KL_TARGET / KL_TARGET_DEFAULT
    const char *tree;      // the unpacked APK's directory, under the container
    const char *apk;       // ...and the APK itself, which is load-bearing
    const char *entry_lib; // the library whose presence proves the guest was embedded
    int         steamlink; // which boot sequence: Unity's, or kl_slink's VR door
} kl_target;

static const kl_target TARGETS[] = {
    { "beatsaber",    "beatsaber",    "beatsaber.apk",    "libmain",           0 },
    // The VR front door only. The 2D shell pairs and hands off, and on visionOS
    // that handoff has nowhere to go yet — see kl_app_boot's note.
    { "steamlink-vr", "steamlink-vr", "steamlink-vr.apk", "libvrlink_scene",   1 },
};

static const kl_target *g_target;

static const kl_target *target_lookup(const char *name) {
    for (unsigned i = 0; i < sizeof TARGETS / sizeof *TARGETS; i++)
        if (!strcmp(TARGETS[i].name, name)) return &TARGETS[i];
    return NULL;
}

const char *kl_app_target_name(void) { return g_target ? g_target->name : "(unconfigured)"; }
int kl_app_target_is_steamlink(void) { return g_target && g_target->steamlink; }

static char g_libdir[1024];
static char g_assets[1024];
static char g_files[1024];
static char g_apk[1024];
static char g_dylibs[1024];
static char g_log[1024];
static char g_status[512] = "not configured";

const char *kl_app_log_path(void) { return g_log; }
const char *kl_app_status(void)   { return g_status; }

static int have(const char *p) { struct stat st; return p && *p && stat(p, &st) == 0; }

// Frameworks/ always exists (the Swift runtime lives there), so its presence
// says nothing. What matters is whether *this target's* translations were
// embedded — a bundle carrying the other app's guest would pass a bare
// directory test and then fail to find a single library.
static int have_translations(void) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s.framework/%s", g_dylibs,
             g_target->entry_lib, g_target->entry_lib);
    return have(p);
}

static int missing(const char *what, const char *path) {
    snprintf(g_status, sizeof g_status, "missing %s: %s", what, path);
    return 1;
}

int kl_app_configure(const char *resources, const char *container) {
    if (!resources || !container) return missing("path", "(null)");

    const char *want = kl_env_str("KL_TARGET", KL_TARGET_DEFAULT);
    g_target = target_lookup(want);
    if (!g_target) return missing("guest target (KL_TARGET)", want);

    // Trap 11, claimed HERE rather than wherever the first guest library happens
    // to load. TSD slot 300 is a constant baked into every veneer, and Darwin
    // hands external pthread keys out upward and never reissues a held one — so
    // whether it is still free is a race against everything else in the process
    // that creates keys. Beat Saber won that race by accident: kl_app_boot loads
    // libmain before ANGLE exists. The Steam Link chain does not — something on
    // its load path brings ANGLE up first, ANGLE takes the process past 300, and
    // the guest then fails to load with "TSD slot 300 is unavailable", which
    // reads as a platform limit and is a scheduling accident.
    //
    // This is the earliest point both targets share. It is not fatal on its own:
    // an ELF-tree run with no veneered library is unaffected, and the load path
    // still refuses by name for one that needs them.
    if (kl_x18_init() != 0)
        fprintf(stderr, "  [klepton] x18: TSD slot %d was already taken at "
                        "configure time — a veneered guest will refuse to load\n",
                KLX_TSD_SLOT);

    // The guest libraries ride in the bundle: AMFI is content about a dylib
    // inside a bundle we signed (P3/P12), and nothing has established that it
    // is content about one pushed into Documents afterwards. The 2.3 GB of
    // assets go the other way — into the container — because they carry no
    // code and re-uploading them on every install would make the M4 loop
    // unusable. See visionos/README.md.
    snprintf(g_libdir, sizeof g_libdir, "%s/guest/lib/arm64-v8a", resources);
    // Frameworks/, not a directory of our own: that is where Xcode code-signs
    // what it embeds, and a loose Mach-O elsewhere in the bundle is only
    // sealed. kl_load_auto knows both layouts.
    snprintf(g_dylibs, sizeof g_dylibs, "%s/Frameworks", resources);
    snprintf(g_assets, sizeof g_assets, "%s/%s/assets", container, g_target->tree);
    snprintf(g_apk,    sizeof g_apk,    "%s/%s", container, g_target->apk);
    snprintf(g_files,  sizeof g_files,  "%s/android-files", container);
    snprintf(g_log,    sizeof g_log,    "%s/klepton-boot.log", container);

    // Check before running, not after failing. A missing asset tree otherwise
    // surfaces three layers up inside Unity as something that reads like a shim
    // bug — trap 6c is exactly that failure wearing a different hat.
    // With translations embedded, g_libdir never has to exist: it is only the
    // string NativeLoader.load() is handed and the prefix the DT_NEEDED walk
    // builds paths from, and kl_load_auto turns "<libdir>/libunity.so" into
    // "Frameworks/libunity.framework/libunity" before touching the disk. So the
    // ELF tree is not in the bundle — 80 MB that would buy an A/B against the
    // mmap loader, which is the thing M1b exists to replace.
    if (!have_translations() && !have(g_libdir))
        return missing("guest libraries (neither translations nor an ELF tree)", g_libdir);
    if (!have(g_assets)) return missing("staged assets (run stage_assets.sh)", g_assets);
    if (!have(g_apk))    return missing("staged APK (run stage_assets.sh)", g_apk);
    mkdir(g_files, 0755);

    if (g_target->steamlink) {
        // Everything this guest is told about itself — the activity class, the
        // four paths, the <meta-data>, the panel size — is kl_slink's, and
        // shared verbatim with `build/m_slink`. Two drivers describing one guest
        // differently is a class of bug with no error surface at all: the run
        // works and answers the guest's questions wrongly.
        if (kl_slink_configure(KL_SLINK_VR, g_libdir, g_assets, g_apk, g_files,
                               NULL) != 0)
            return missing("Steam Link front door", kl_slink_error());
    } else {
        kl_set_library_path(g_libdir);
        // Explicitly, because the default is a *relative* path that gets
        // absolutised against the working directory — which is the repo root
        // under t_boot and `/` inside an app bundle. Left unset on device it
        // became "//beatsaber/lib/arm64-v8a", and that is the string Unity reads
        // back as ApplicationInfo.nativeLibraryDir and hands to
        // ClassLoader.findLibrary. It survived only because kl_can_load matches
        // a translation on the basename; anything that actually used the
        // directory would have been quietly wrong.
        kl_jni_set_native_lib_dir(g_libdir);
        kl_jni_set_assets_dir(g_assets);
        kl_jni_set_apk_path(g_apk);
        kl_jni_set_files_dir(g_files);
        // Raw "<apk>/assets/..." opens: Unity mounts the APK into its VFS and
        // then resolves entries by concatenating onto the mount point (trap 6c).
        kl_guest_path_map(g_apk, g_assets);
    }

    // Prefer klepton-ld translations when the bundle carries them. On device
    // this is not a preference but the whole point — the mmap loader maps guest
    // text RWX from a file the bundle does not own, which is the shape AMFI
    // exists to refuse. A translation that is present and fails is an error,
    // not a fallback (kl_load_auto).
    if (have_translations()) setenv("KL_DYLIB_DIR", g_dylibs, 1);

    // ANGLE rides in the same Frameworks/ directory, so the app can answer
    // "where is ANGLE" itself rather than have the launcher guess at a path
    // that only exists on the device. Not forced: an explicit KL_ANGLE_DIR
    // still wins, which is how a run points at a different build.
    setenv("KL_ANGLE_DIR", g_dylibs, 0);

    // ...and the app uses it by default. KL_GLFB stays opt-IN for the host,
    // where the null driver is a legitimate answer — `make check` and every
    // lifecycle loop run on it, and the reference renderer is a deliberate
    // extra. In a shipped app it is not an answer at all: the null driver
    // records GL calls and draws nothing, so a hand-launched Klepton would
    // show a black immersive space and look broken.
    //
    // overwrite=0, so an explicit KL_GLFB=0 from run.sh or the launcher still
    // selects the null driver — which is why kl_glfb_enabled() had to start
    // reading the value rather than the presence.
    setenv("KL_GLFB", "1", 0);

    snprintf(g_status, sizeof g_status, "configured");
    return 0;
}

// Everything the run prints goes to a file, line-buffered.
//
// Line-buffered specifically: an unimplemented JNI slot aborts by design, and
// a fully-buffered stream loses the whole report when the process dies on a
// signal — which on the host reads as a much earlier failure than actually
// happened, and on device would be the only evidence there is.
static int open_log(void) {
    if (!freopen(g_log, "w", stdout)) return 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    dup2(fileno(stdout), STDERR_FILENO);   // the guest's own writes(2) land here too
    setvbuf(stderr, NULL, _IOLBF, 0);
    return 0;
}

// A heartbeat on its own thread, because three different failures look identical
// in a log that simply stops growing — and on device there is no shell, no
// sampler and no way to attach:
//
//   * the process is SUSPENDED  -> the timestamps show a gap and then resume
//   * the process was KILLED    -> the log just ends (jetsam/watchdog send SIGKILL,
//                                  which no handler can report)
//   * the GUEST THREAD is stuck -> the heartbeat KEEPS TICKING while the guest's
//                                  output stops, which is the case nothing else
//                                  distinguishes
//
// Wall clock rather than monotonic on purpose: a suspension is exactly the thing
// a monotonic clock is designed to hide.
//
// The line also carries memory, because "killed" above has two causes that the
// log cannot otherwise tell apart — jetsam and the watchdog both send SIGKILL,
// and neither leaves anything behind. `foot` is our physical footprint, which is
// the number jetsam actually meters; `avail` is what the OS says is left before
// it acts. A run that ends with avail falling toward zero was killed for memory;
// one that ends with avail still comfortable was killed for time.
static const char *volatile g_phase = "start";

// Bytes of headroom before this app is jetsammed, or -1 where the OS will not
// say. os_proc_available_memory() is the app-process-only API and returns 0 in
// contexts it does not apply to, which is not distinguishable from "none left" —
// hence the explicit -1 rather than passing a bare 0 through to the log.
// TARGET_OS_IPHONE, not __has_include: the header exists on macOS too, where the
// function is marked unavailable — so an include test compiles and then fails at
// the call.
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
static long long kl_avail_memory(void) {
#if TARGET_OS_IPHONE
    size_t a = os_proc_available_memory();
    return a ? (long long)a : -1;
#else
    return -1;
#endif
}

static long long kl_phys_footprint(void) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
        return -1;
    return (long long)info.phys_footprint;
}

static void *heartbeat(void *arg) {
    (void)arg;
    // Polled faster than it prints, so a phase change is reported when it
    // happens rather than up to two seconds later. That matters here: the first
    // device run to reach graphics died inside one 2 s interval, so the whole
    // lifecycle was described by a single "phase=start" line.
    const char *last_phase = NULL;
    int ticks = 0;
    for (;;) {
        const char *phase = g_phase;
        if (phase != last_phase || ticks % 8 == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            struct tm tm;
            localtime_r(&ts.tv_sec, &tm);
            fprintf(stderr, "[hb] %02d:%02d:%02d  phase=%s  foot=%lldM avail=%lldM\n",
                    tm.tm_hour, tm.tm_min, tm.tm_sec, phase,
                    kl_phys_footprint() >> 20, kl_avail_memory() >> 20);
            fflush(stderr);
            last_phase = phase;
        }
        ticks++;
        struct timespec iv = { 0, 250000000 };  // nanosleep: usleep is EINVAL at >= 1e6
        nanosleep(&iv, NULL);
    }
    return NULL;
}
static void heartbeat_start(void) {
    static pthread_t th;
    static int started;
    if (started) return;
    started = 1;
    pthread_create(&th, NULL, heartbeat, NULL);
    pthread_detach(th);
}

typedef int    (*jni_onload_fn)(void *vm, void *reserved);
typedef int8_t (*nativeloader_load_fn)(void *env, void *clazz, void *path);

static int fail(const char *msg) {
    snprintf(g_status, sizeof g_status, "%s", msg);
    fprintf(stderr, "FAIL: %s\n", msg);
    fflush(NULL);
    return 1;
}

// Open the log WITHOUT booting. kl_app_boot() does this itself, and for the
// whole life of this file that was the only way to get one — which is fine
// until something wants a log and deliberately does not want a guest. The
// KL_TEMPLATE floor test is exactly that, and its absence was not a quiet
// inconvenience: it left the container holding the PREVIOUS run's log, so the
// floor test looked like it had produced no output when it had simply never
// been able to write any, and the stale file read as a current result.
int kl_app_open_log(void) {
    if (!*g_log) return 1;                 // kl_app_configure was not called
    return open_log();
}

// The Steam Link half of kl_app_boot: map the chain, print the shim gap, run
// the init arrays. It deliberately stops BEFORE ANativeActivity_onCreate, for
// the same reason the Unity path stops at initJni — that is the part which is
// supposed to be clean, and separating it means "the guest loaded" and "the
// activity started" fail as two different reports rather than one.
//
// The chain here is the VR front door's, which is one library. The 2D shell is
// deliberately not reachable from the app yet: on the host the shell pairs and
// then re-execs into the VR door carrying the session (SL-15), and an app bundle
// cannot re-exec — so the shell needs a window of its own and a handoff that
// opens the ImmersiveSpace instead. Until that exists, the session arrives the
// way the host's did before SL-15: KL_SLINK_SARGS, from a pairing run, which
// visionos/run.sh already forwards.
static int boot_steamlink(void) {
    g_phase = "steamlink chain";
    printf("=== Steam Link: %s front door (%s -> %s) ===\n",
           kl_slink_door_name(), kl_slink_main_lib(), kl_slink_main_fn());
    fflush(NULL);

    // Mapping is separated from DT_INIT_ARRAY on purpose — see kl_slink.h. The
    // gap between them is the shim work list, and printing it is the whole
    // reason the two are not one call.
    if (kl_slink_load_chain(stdout) != 0) return fail(kl_slink_error());
    kl_slink_report_gap(stdout);

    printf("\n=== DT_INIT_ARRAY, dependencies first ===\n");
    fflush(NULL);
    g_phase = "steamlink inits";
    kl_slink_run_inits(stdout);

    // Not "no sArgs, therefore stop": the guest decides that for itself and
    // prints "No sArgs and release build panic" on its own way out, which is
    // more informative than anything we would say here. But it exits BEFORE its
    // first frame, and that reads exactly like a compositor failure from the
    // outside, so it is worth naming before it happens.
    if (!getenv("KL_SLINK_SARGS"))
        printf("\n  NOTE: KL_SLINK_SARGS is unset. This guest reads its session out of\n"
               "  the launching Intent and exits before its first frame without one —\n"
               "  see notes/STEAMLINK.md for the pairing -> handoff loop.\n");

    printf("\n=== EXIT CRITERION MET: the Steam Link chain is bound and "
           "initialised on visionOS ===\n");
    g_phase = "boot report";
    kl_jni_report(stdout);
    fflush(NULL);
    g_phase = "boot done";
    snprintf(g_status, sizeof g_status, "Steam Link chain initialised");
    return 0;
}

int kl_app_boot(void) {
    // Once, and never concurrently. The Boot button invites a second press,
    // and the second entry is not merely redundant: the runtime's JNI tables
    // are process-global, so another JNI_OnLoad re-registers every native onto
    // the same table, while open_log() truncates the file the first run is
    // still writing. Five presses on device produced "natives registered: 61"
    // instead of 45 with a chunk of the log missing — which reads as a
    // platform difference and is not one. Refusing is better than allowing a
    // run whose numbers cannot be compared to anything.
    static pthread_mutex_t once_mu = PTHREAD_MUTEX_INITIALIZER;
    static int entered;
    pthread_mutex_lock(&once_mu);
    int already = entered;
    entered = 1;
    pthread_mutex_unlock(&once_mu);
    if (already) return fail("kl_app_boot was already run in this process");

    if (!*g_libdir) return fail("kl_app_configure was not called");
    if (open_log()) return fail("could not open the log file");

    heartbeat_start();
    kl_fault_install();
    // Strict: an unimplemented *call* is fatal, so the run stops exactly where
    // the surface genuinely ends. Lookups are not — the guest resolves plenty
    // it never calls.
    kl_jni_set_permissive(kl_env_on("KL_PERMISSIVE", 0));
    kl_egl_dump_textures(kl_env_str("KL_DUMP_TEXTURES", NULL));

    printf("=== Klepton on visionOS — P4 ===\n");
    printf("  target    : %s\n", g_target->name);
    printf("  libraries : %s\n", g_libdir);
    printf("  dylibs    : %s%s\n", g_dylibs,
           have_translations() ? "" : "  (none embedded — falling back to the mmap ELF loader)");
    printf("  assets    : %s\n", g_assets);
    printf("  apk       : %s\n", g_apk);
    printf("  files     : %s\n\n", g_files);
    fflush(NULL);

    if (g_target->steamlink) return boot_steamlink();

    char path[1200];
    snprintf(path, sizeof path, "%s/libmain.so", g_libdir);

    printf("=== libmain.so entry ===\n");
    kl_image *main_img = kl_load_auto(path);
    if (!main_img) return fail(kl_error());
    kl_register_image("libmain.so", main_img);
    kl_run_init(main_img);

    jni_onload_fn onload = (jni_onload_fn)kl_sym(main_img, "JNI_OnLoad");
    if (!onload) return fail("libmain.so exports no JNI_OnLoad");

    kl_jni_local_frame_push();          // the JVM would pop each native's local
    int version = onload(kl_jni_vm(), NULL);  // frame on return; the host plays
    kl_jni_local_frame_pop();           // that half (see kl_jni.h)
    printf("  JNI_OnLoad returned 0x%08x\n", version);
    if (version != KL_JNI_VERSION_1_6)
        return fail("JNI_OnLoad did not return JNI_VERSION_1_6");

    const char *CLS = "com/unity3d/player/NativeLoader";
    void *load = kl_jni_native(CLS, "load", NULL);
    if (!load || !kl_jni_native(CLS, "unload", NULL))
        return fail("NativeLoader natives were not registered");
    printf("  registered %s.load=%p\n", CLS, load);
    printf("\n=== M3 EXIT CRITERION MET: guest JNI_OnLoad ran, natives registered ===\n");
    fflush(NULL);

    // load() takes the *directory* — it appends "/libunity.so" itself.
    g_phase = "NativeLoader.load";
    printf("\n=== NativeLoader.load(\"%s\") ===\n", g_libdir);
    fflush(NULL);
    kl_jni_local_frame_push();
    int8_t ok = ((nativeloader_load_fn)load)(kl_jni_env(), NULL,
                                             kl_jni_new_string(g_libdir));
    kl_jni_local_frame_pop();
    printf("  NativeLoader.load returned %d\n", ok);
    if (!ok) return fail("NativeLoader.load could not bring up libunity.so");

    // UnityPlayer's constructor calls initJni(Context) first. It is
    // `private final native`, so the guest sees (JNIEnv*, jobject thiz,
    // jobject context). The Context must be the Activity — the manifest
    // declares UnityPlayerActivity and Unity checks with IsInstanceOf — and the
    // shared singleton, because Unity reads it back through the static
    // UnityPlayer.currentActivity and compares.
    void *initJni = kl_jni_native("com/unity3d/player/UnityPlayer", "initJni", NULL);
    if (!initJni) return fail("UnityPlayer.initJni was never registered");
    g_phase = "initJni";
    printf("\n=== UnityPlayer.initJni(Context) ===\n");
    fflush(NULL);
    void *thiz = kl_jni_new_object("com/unity3d/player/UnityPlayer");
    kl_jni_local_frame_push();
    ((void (*)(void *, void *, void *))initJni)(kl_jni_env(), thiz, kl_jni_activity());
    kl_jni_local_frame_pop();
    printf("  initJni returned\n");

    printf("\n=== P4 EXIT CRITERION MET: initJni completed on visionOS, "
           "no unimplemented JNI calls ===\n");
    g_phase = "boot report";
    kl_jni_report(stdout);
    fflush(NULL);
    g_phase = "boot done";
    snprintf(g_status, sizeof g_status, "initJni completed");
    return 0;
}

// The synthetic /proc that trap 6d built, read back on the platform where it has
// never been read. PLANNING §12.7 asks for this BEFORE chasing anything else in
// a device lifecycle run, and the reason is specific: proc_build() gets the
// free-page count from host_statistics64(), which links on visionOS but may be
// restricted inside the sandbox — and a silent zero there is not read by Unity
// as "unknown", it is read as a machine with no memory. That is the exact
// failure trap 6d records: `Cores = 0, Memory = 0mb`, and Unity refuses to
// start, with nothing anywhere mentioning memory.
static void report_proc(void) {
    static const char *files[] = { "/proc/cpuinfo", "/proc/meminfo",
                                   "/sys/devices/system/cpu/possible" };
    printf("\n=== the synthetic /proc, on device (trap 6d) ===\n");
    for (unsigned i = 0; i < sizeof files / sizeof *files; i++) {
        // Through the same rewrite the guest's own open() takes, so this reads
        // what the guest reads and not something adjacent to it.
        char buf[1024];
        const char *real = kl_guest_path(files[i], buf, sizeof buf);
        FILE *f = real ? fopen(real, "r") : NULL;
        if (!f) { printf("  %-34s NOT SERVED (%s)\n", files[i], real ? real : "no path");
                  continue; }
        char line[256];
        int shown = 0;
        while (shown < 4 && fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            // MemTotal/MemFree are the two that matter; for cpuinfo the first
            // lines are enough to see it is not empty.
            if (strstr(files[i], "meminfo") && !strstr(line, "Mem")) continue;
            printf("  %-34s %s\n", shown ? "" : files[i], line);
            shown++;
        }
        if (!shown) printf("  %-34s (served but EMPTY — see trap 6d)\n", files[i]);
        fclose(f);
    }
    fflush(NULL);
}

// The guest handle and the resolved nativeRender, kept between _begin and each
// _frame. Set by kl_app_lifecycle_begin and read by kl_app_frame; a NULL
// g_render is how kl_app_frame knows _begin has not run.
static void *g_thiz, *g_render;
static unsigned g_alarm_secs = 120, g_frames_pumped;

int kl_app_lifecycle_begin(void) {
    // Separate from kl_app_boot rather than folded into it, because boot is the
    // P4 gate and refuses a second entry: keeping them apart lets the UI run the
    // gate, read its numbers, and only then go further.
    static pthread_mutex_t once_mu = PTHREAD_MUTEX_INITIALIZER;
    static int entered;
    pthread_mutex_lock(&once_mu);
    int already = entered;
    entered = 1;
    pthread_mutex_unlock(&once_mu);
    if (already) return fail("kl_app_lifecycle was already run in this process");

    // The Steam Link guest's whole lifecycle is Android's NativeActivity one,
    // and it MUST run on the thread that will go on to pump: onCreate takes
    // ALooper_forThread() and the guest hangs its UIThreadCallbackHandler off
    // exactly that looper. Splitting the two across threads leaves the guest
    // with callbacks nobody will ever run and no error anywhere.
    if (g_target->steamlink) {
        g_phase = "proc";
        report_proc();
        g_alarm_secs = kl_env_int("KL_ALARM", 120);
        g_phase = "ANativeActivity_onCreate";
        printf("\n=== ANativeActivity_onCreate (the VR front door) ===\n");
        fflush(NULL);
        if (kl_slink_vr_create(stdout) != 0) return fail(kl_slink_error());
        g_phase = "activity lifecycle";
        printf("\n=== the activity lifecycle ===\n");
        fflush(NULL);
        kl_slink_vr_start(stdout);
        g_phase = "looper pump";
        return 0;
    }

    void *thiz = kl_jni_new_object("com/unity3d/player/UnityPlayer");
    if (!thiz) return fail("kl_app_boot must run first");
    g_thiz = thiz;

    // First, before anything can be misdiagnosed on top of it.
    g_phase = "proc";
    report_proc();

    unsigned alarm_secs = kl_env_int("KL_ALARM", 120);
    g_alarm_secs = alarm_secs;

    // The order UnityPlayerActivity drives, exactly as t_boot's recon does:
    // attach a surface, resume, then one frame. nativeRecreateGfxState is what
    // reaches for EGL, and on device it is also what pulls in libil2cpp — the
    // 66 MB image with 3,083 x18 veneers that P4 never loaded here (§12.7).
    void *surface = kl_jni_new_object("android/view/Surface");
    struct { const char *name; int kind; } seq[] = {
        { "nativeRecreateGfxState", 2 }, { "nativeResume", 0 }, { "nativeRender", 1 },
    };
    for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        void *fn = kl_jni_native("com/unity3d/player/UnityPlayer", seq[i].name, NULL);
        if (!fn) { printf("  %s: not registered\n", seq[i].name); continue; }
        g_phase = seq[i].name;
        printf("\n=== UnityPlayer.%s ===\n", seq[i].name);
        fflush(NULL);
        // Arm the watchdog, exactly as t_boot does. kl_fault_install already
        // handles SIGALRM and reports it as "still alive and blocked" rather than
        // as a crash, which is the whole point: on device there is no shell to
        // notice a hang and no way to attach a sampler, so a run that simply
        // stops growing its log says nothing about *where* it stopped. KL_ALARM
        // widens it when the question is what the guest is waiting on.
        alarm(alarm_secs);
        kl_jni_local_frame_push();
        if (seq[i].kind == 2)
            ((void (*)(void *, void *, int, void *))fn)(kl_jni_env(), thiz, 0, surface);
        else if (seq[i].kind == 1)
            printf("  -> %d\n", ((int8_t (*)(void *, void *))fn)(kl_jni_env(), thiz));
        else
            ((void (*)(void *, void *))fn)(kl_jni_env(), thiz);
        kl_jni_local_frame_pop();
        alarm(0);
        printf("  %s returned\n", seq[i].name);
        // Android's UI thread runs its looper between callbacks; here nothing
        // else will, so the queue would only grow.
        alarm(alarm_secs);
        unsigned ran = kl_jni_drain_ui_tasks();
        alarm(0);
        if (ran) printf("  drained %u posted task%s\n", ran, ran == 1 ? "" : "s");
        fflush(NULL);
    }

    g_render = kl_jni_native("com/unity3d/player/UnityPlayer", "nativeRender", NULL);
    g_phase = "frame pump";
    return 0;
}

// One guest frame. Split out of the pump loop so that Compositor Services can
// be the clock (P5b): on device the frame deadline belongs to
// cp_frame_predict_timing, and a pump that owns its own loop cannot be paced by
// something else. kl_app_lifecycle keeps calling this in a plain loop, so the
// P5.4 measurement is the same code it always was.
//
// Returns what nativeRender returned, or -1 if kl_app_lifecycle_begin has not
// run (or nativeRender was never registered).
int kl_app_frame(void) {
    // Not a frame the caller can drive on this target, and saying so is the
    // point. The Steam Link guest runs its own OpenXR frame loop on a thread it
    // created inside onCreate; what our thread owes it is a turning looper, not
    // a call per display frame. Pacing happens where OpenXR puts it — xrWaitFrame
    // blocks on the compositor's published pose (kl_openxr_set_pacer) — so a
    // caller that pumped here as well would be a second, disagreeing clock.
    if (g_target && g_target->steamlink) return -1;
    if (!g_render || !g_thiz) return -1;
    alarm(g_alarm_secs);
    // Pin this frame's poses before anything in the frame can ask. The
    // compositor publishes a new pose every display frame from its own thread,
    // and a guest frame is longer than a display frame whenever performance is
    // short — so without this the head moves *inside* the frame and the pose
    // recorded for timewarp is not the one the picture was drawn from. See
    // kl_ovrp_frame_latch: the residual is a whole guest frame of rotation,
    // corrected backwards, which is the doubling seen on device.
    kl_ovrp_frame_latch();
    // The frame clock next: on Android the Choreographer's doFrame is what
    // wakes the engine, and nativeRender then draws what it decided. Ticking
    // after would hand every frame the previous one's time.
    kl_jni_tick_choreographer();
    kl_jni_local_frame_push();
    int r = ((int8_t (*)(void *, void *))g_render)(kl_jni_env(), g_thiz);
    kl_jni_local_frame_pop();
    kl_jni_drain_ui_tasks();
    alarm(0);
    g_frames_pumped++;
    return r;
}

void kl_app_lifecycle_report(void) {
    if (g_target && g_target->steamlink) {
        // Media, audio, XR and GL — which between them ARE the VR half of this
        // app. Every one of them otherwise reports only on kl_fault.c's abort
        // path, which a working run never takes, so the run that most needs
        // these numbers was the one printing none of them.
        printf("\n=== the Steam Link VR run ===\n");
        kl_slink_report(stdout);
        snprintf(g_status, sizeof g_status, "Steam Link VR run ended");
        return;
    }
    printf("  pumped %u frames\n", g_frames_pumped);
    printf("\n=== P5.4: the lifecycle ran on device ===\n");
    kl_jni_report(stdout);
    kl_egl_report(stdout);
    kl_opensl_report(stdout);
    // The OVRPlugin surface, which t_boot has always printed and this never
    // did — so the eye swapchain's own numbers, including whether the pose and
    // the picture stayed associated, have never been readable from a device
    // run. That is the one report the graphics work most needs.
    kl_ovrp_report(stdout);
    fflush(NULL);
    snprintf(g_status, sizeof g_status, "lifecycle ran, %u frames", g_frames_pumped);
}

int kl_app_lifecycle(unsigned frames) {
    int rc = kl_app_lifecycle_begin();
    if (rc) return rc;

    // The Steam Link guest counts no frames here, because it does not take any
    // from us: what a bounded run means for it is a bounded PUMP. That is the
    // window-and-report shape (KL_IMMERSIVE=0) — the recon run that says how far
    // the guest got with no compositor in the picture, which is exactly the
    // measurement P4 is for Beat Saber and has to stay takeable for this guest
    // too. `frames` would be a number with nothing behind it, so the deadline is
    // KL_SLINK_WAIT's, in seconds, as it is on the command line.
    if (g_target->steamlink) {
        unsigned secs = kl_env_uint("KL_SLINK_WAIT", 30);
        printf("\n=== pumping the looper for %us ===\n", secs);
        fflush(NULL);
        double spent = kl_slink_vr_pump((double)secs, NULL);
        printf("  pumped for %.1fs\n", spent);
        kl_app_lifecycle_report();
        return 0;
    }

    printf("\n=== pumping %u frames ===\n", frames);
    fflush(NULL);
    while (g_frames_pumped < frames && kl_app_frame() >= 0) { }
    kl_app_lifecycle_report();
    return 0;
}

// --- The guest on its own thread — PLANNING §12.12 --------------------------
// See kl_app.h for what this is for. What is here is the handoff itself, and
// it is deliberately in C rather than Swift for one reason above the language
// boundary in §12.6: trap 1. Every thread that runs guest code must call
// kl_thread_init() before it does — the TPIDR slot the veneered guest reads is
// per-thread and this is where it is established. A Swift `Thread {}` that
// called kl_app_frame() without it would fault in a way that looks exactly
// like an x18 bug and is not, so the thread is created at the same place the
// init happens and there is no way to get one without the other.
static struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    uint64_t        published;    // poses the compositor has offered
    uint64_t        consumed;     // poses the guest has started a frame for
    unsigned        limit;        // KL_FRAMES, or 0 for "until stopped"
    // volatile because the Steam Link pump reads it WITHOUT the lock: that loop
    // is inside kl_slink and is handed a plain flag, which is the honest shape
    // for "somebody else will set this once". Every other reader takes the mutex.
    volatile int    stop;
    int             state;        // kl_app_guest_state()
    int             running;      // has the thread been spawned and not joined?
    int             finished;     // has it left its loop and written the report?
    pthread_t       thread;
} g_guest = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

int kl_app_guest_state(void) {
    pthread_mutex_lock(&g_guest.mu);
    int s = g_guest.state;
    pthread_mutex_unlock(&g_guest.mu);
    return s;
}

static void guest_set_state(int s) {
    pthread_mutex_lock(&g_guest.mu);
    g_guest.state = s;
    pthread_mutex_unlock(&g_guest.mu);
}

// Both exit paths run through here, so kl_app_guest_stop() can never be left
// waiting on a thread that has already gone.
static void guest_finished(void) {
    pthread_mutex_lock(&g_guest.mu);
    g_guest.state = -1;
    g_guest.finished = 1;
    pthread_cond_broadcast(&g_guest.cv);
    pthread_mutex_unlock(&g_guest.mu);
}

// The OpenXR guest's xrWaitFrame, waiting on the same publish the Beat Saber
// loop above consumes. Called from a thread libvrlink_scene created, not from
// the one guest_thread runs on, which is why it takes the lock and touches
// nothing else.
//
// Timed, not indefinite: a compositor that stops publishing must make the guest
// render against the last pose it had, not wedge it. A wedged guest is
// indistinguishable from a crashed one in a device log, and this is exactly the
// path a backgrounded app takes.
static void guest_pace_wait(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    pthread_mutex_lock(&g_guest.mu);
    while (!g_guest.stop && g_guest.published == g_guest.consumed)
        if (pthread_cond_timedwait(&g_guest.cv, &g_guest.mu, &ts) == ETIMEDOUT) break;
    // Coalesce, do not queue: everything published since the last frame is one
    // turn. A guest that is behind then skips frames — which reprojection covers
    // — rather than being owed a backlog it can never work off.
    g_guest.consumed = g_guest.published;
    g_frames_pumped++;
    pthread_mutex_unlock(&g_guest.mu);
}

static void *guest_thread(void *unused) {
    (void)unused;
    // Trap 1, before a single guest instruction runs on this thread.
    kl_thread_init();
    pthread_setname_np("Klepton Guest");

    if (kl_app_lifecycle_begin() != 0) {
        printf("[guest] lifecycle_begin failed: %s\n", g_status);
        fflush(NULL);
        guest_finished();
        return NULL;
    }
    guest_set_state(1);

    // Two shapes, because the two guests differ in who owns the frame loop.
    //
    // Beat Saber has none: nativeRender is a call, so this thread makes one per
    // published pose. Steam Link brought its own — libvrlink_scene spawns a
    // thread inside onCreate and runs OpenXR on it — so what this thread owes it
    // is a turning looper and nothing else. Pacing is not lost by that: it moves
    // to where OpenXR puts it, xrWaitFrame, which blocks on the same published
    // pose through kl_openxr_set_pacer. One clock either way.
    if (g_target->steamlink) {
        printf("\n=== guest thread pumping the activity's looper ===\n");
        fflush(NULL);
        double secs = kl_slink_vr_pump(-1.0, &g_guest.stop);
        printf("[guest] pumped for %.1fs\n", secs);
        fflush(NULL);
        kl_app_lifecycle_report();
        guest_finished();
        return NULL;
    }

    printf("\n=== guest thread running, one frame per published pose ===\n");
    fflush(NULL);

    for (;;) {
        pthread_mutex_lock(&g_guest.mu);
        while (!g_guest.stop && g_guest.published == g_guest.consumed)
            pthread_cond_wait(&g_guest.cv, &g_guest.mu);
        int stop = g_guest.stop;
        // Coalesce, do not queue: take everything published since the last
        // frame as a single turn. This is what makes "the guest is late" mean
        // "frames were skipped" — which reprojection covers — rather than
        // "frames are owed", which is a backlog that only grows.
        g_guest.consumed = g_guest.published;
        unsigned limit = g_guest.limit;
        pthread_mutex_unlock(&g_guest.mu);
        if (stop) break;

        if (kl_app_frame() < 0) {
            printf("[guest] nativeRender is gone — the frame loop ends here\n");
            fflush(NULL);
            break;
        }
        if (limit && g_frames_pumped >= limit) {
            printf("[guest] reached KL_FRAMES=%u\n", limit);
            fflush(NULL);
            break;
        }
    }

    // The report belongs to the guest's loop end, not the render loop's: with
    // the two decoupled, "the run ended" has two meanings and these are the
    // guest's numbers.
    kl_app_lifecycle_report();
    guest_finished();
    return NULL;
}

int kl_app_guest_start(void) {
    pthread_mutex_lock(&g_guest.mu);
    if (g_guest.running) {
        pthread_mutex_unlock(&g_guest.mu);
        return fail("kl_app_guest_start was already run in this process");
    }
    g_guest.limit = kl_env_uint("KL_FRAMES", 0);
    g_guest.running = 1;
    g_guest.state = 0;
    pthread_mutex_unlock(&g_guest.mu);

    // Only on this path, and before the thread exists. kl_app_lifecycle() has no
    // publisher at all — it is the command line's shape, a plain loop — so a
    // pacer installed there would block the guest on a pose nobody will ever
    // publish. Installing it here also closes the window in which the guest could
    // reach its first xrWaitFrame unpaced, because the guest does not exist yet.
    if (g_target && g_target->steamlink) kl_openxr_set_frame_pacer(guest_pace_wait);

    // User-interactive, because this thread is now the one producing frames.
    // A plain pthread gets an unspecified QoS, and a guest demoted below the
    // compositor is a guest that misses deadlines for scheduling reasons that
    // would then be blamed on the shim.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    int rc = pthread_create(&g_guest.thread, &attr, guest_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_lock(&g_guest.mu);
        g_guest.running = 0;
        g_guest.state = -1;
        pthread_mutex_unlock(&g_guest.mu);
        return fail("could not create the guest frame thread");
    }
    return 0;
}

void kl_app_guest_publish(void) {
    pthread_mutex_lock(&g_guest.mu);
    g_guest.published++;
    pthread_cond_signal(&g_guest.cv);
    pthread_mutex_unlock(&g_guest.mu);
}

void kl_app_guest_stop(void) {
    pthread_mutex_lock(&g_guest.mu);
    if (!g_guest.running) { pthread_mutex_unlock(&g_guest.mu); return; }
    g_guest.stop = 1;
    g_guest.running = 0;
    pthread_t t = g_guest.thread;
    pthread_cond_broadcast(&g_guest.cv);

    // Bounded, not a plain join. The guest can wedge — a Baselib futex nobody
    // will post, the GC failing to stop the world — and that is a documented
    // failure mode here, not a hypothetical. A render loop being torn down
    // must not be the thing that hangs waiting for it: past the deadline the
    // thread is left to whatever it is doing and the caller carries on without
    // the report, which is an honest missing report rather than a silent hang
    // that would read as a compositor bug.
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10;
    while (!g_guest.finished)
        if (pthread_cond_timedwait(&g_guest.cv, &g_guest.mu, &ts) == ETIMEDOUT) break;
    int finished = g_guest.finished;
    pthread_mutex_unlock(&g_guest.mu);

    if (finished) {
        // Joined, not detached, in the normal case: the report is written on
        // the way out and the caller's next act may be to hand the log to the UI.
        pthread_join(t, NULL);
    } else {
        pthread_detach(t);
        printf("[guest] did not stop within 10 s — carrying on without its report\n");
        fflush(NULL);
    }
}
