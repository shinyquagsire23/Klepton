// See kl_app.h.
#include <errno.h>
#include <fcntl.h>
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
// says nothing. What matters is whether *our* translations were embedded.
static int have_translations(void) {
    char p[1200];
    snprintf(p, sizeof p, "%s/libmain.framework/libmain", g_dylibs);
    return have(p);
}

static int missing(const char *what, const char *path) {
    snprintf(g_status, sizeof g_status, "missing %s: %s", what, path);
    return 1;
}

int kl_app_configure(const char *resources, const char *container) {
    if (!resources || !container) return missing("path", "(null)");

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
    snprintf(g_assets, sizeof g_assets, "%s/beatsaber/assets", container);
    snprintf(g_apk,    sizeof g_apk,    "%s/beatsaber.apk", container);
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

    kl_set_library_path(g_libdir);
    // Explicitly, because the default is a *relative* path that gets absolutised
    // against the working directory — which is the repo root under t_boot and `/`
    // inside an app bundle. Left unset on device it became
    // "//beatsaber/lib/arm64-v8a", and that is the string Unity reads back as
    // ApplicationInfo.nativeLibraryDir and hands to ClassLoader.findLibrary. It
    // survived only because kl_can_load matches a translation on the basename;
    // anything that actually used the directory would have been quietly wrong.
    kl_jni_set_native_lib_dir(g_libdir);
    kl_jni_set_assets_dir(g_assets);
    kl_jni_set_apk_path(g_apk);
    kl_jni_set_files_dir(g_files);
    // Raw "<apk>/assets/..." opens: Unity mounts the APK into its VFS and then
    // resolves entries by concatenating onto the mount point (trap 6c).
    kl_guest_path_map(g_apk, g_assets);

    // Prefer klepton-ld translations when the bundle carries them. On device
    // this is not a preference but the whole point — the mmap loader maps guest
    // text RWX from a file the bundle does not own, which is the shape AMFI
    // exists to refuse. A translation that is present and fails is an error,
    // not a fallback (kl_load_auto).
    if (have_translations()) setenv("KL_DYLIB_DIR", g_dylibs, 1);

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
static const char *volatile g_phase = "start";
static void *heartbeat(void *arg) {
    (void)arg;
    for (;;) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        localtime_r(&ts.tv_sec, &tm);
        fprintf(stderr, "[hb] %02d:%02d:%02d  phase=%s\n",
                tm.tm_hour, tm.tm_min, tm.tm_sec, g_phase);
        fflush(stderr);
        struct timespec iv = { 2, 0 };   // nanosleep: usleep is EINVAL at >= 1e6
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
    kl_jni_set_permissive(getenv("KL_PERMISSIVE") != NULL);
    kl_egl_dump_textures(getenv("KL_DUMP_TEXTURES"));

    printf("=== Klepton on visionOS — P4 ===\n");
    printf("  libraries : %s\n", g_libdir);
    printf("  dylibs    : %s%s\n", g_dylibs,
           have_translations() ? "" : "  (none embedded — falling back to the mmap ELF loader)");
    printf("  assets    : %s\n", g_assets);
    printf("  apk       : %s\n", g_apk);
    printf("  files     : %s\n\n", g_files);
    fflush(NULL);

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

int kl_app_lifecycle(unsigned frames) {
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

    void *thiz = kl_jni_new_object("com/unity3d/player/UnityPlayer");
    if (!thiz) return fail("kl_app_boot must run first");

    // First, before anything can be misdiagnosed on top of it.
    g_phase = "proc";
    report_proc();

    const char *aenv = getenv("KL_ALARM");
    unsigned alarm_secs = aenv ? (unsigned)strtoul(aenv, NULL, 10) : 120;

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

    void *render = kl_jni_native("com/unity3d/player/UnityPlayer", "nativeRender", NULL);
    printf("\n=== pumping %u frames ===\n", frames);
    fflush(NULL);
    unsigned i = 0;
    g_phase = "frame pump";
    alarm(alarm_secs);
    for (; render && i < frames; i++) {
        // The frame clock first: on Android the Choreographer's doFrame is what
        // wakes the engine, and nativeRender then draws what it decided.
        // Ticking after would hand every frame the previous one's time.
        kl_jni_tick_choreographer();
        kl_jni_local_frame_push();
        ((int8_t (*)(void *, void *))render)(kl_jni_env(), thiz);
        kl_jni_local_frame_pop();
        kl_jni_drain_ui_tasks();
    }
    alarm(0);
    printf("  pumped %u frames\n", i);
    printf("\n=== P5.4: the lifecycle ran on device ===\n");
    kl_jni_report(stdout);
    kl_egl_report(stdout);
    fflush(NULL);
    snprintf(g_status, sizeof g_status, "lifecycle ran, %u frames", i);
    return 0;
}
