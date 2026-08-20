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
#include "kl_driver.h"    // the guest's own sequences, shared with build/m_boot
#include "kl_audio.h"     // kl_audio_set_latency_ms — streamed audio needs a deeper buffer
#include "kl_ovrp.h"      // kl_ovrp_set_frame_pacer — the OVRPlugin frame clock
#include "kl_openxr.h"    // kl_openxr_set_frame_pacer — the OpenXR one
#include "kl_slink.h"
#include "kl_mono.h"      // the flat guest's window seam — frame out, pointer in
#include "kl_glfb.h"      // kl_glfb_release_current — the handoff hands the context on
#include "kl_x18.h"       // claim the veneers' TSD slot before anything else can
#include "kl_env.h"

// --- Which guest ------------------------------------------------------------
//
// The table is runtime/kl_target.h — one row per guest, generated from
// visionos/targets.py and shared with `build/m_boot`, so the host driver and the
// app cannot describe one guest differently.
//
// The default is baked in at BUILD time (KL_TARGET_DEFAULT, set by
// gen_xcodeproj.py) rather than read from the environment, because the app has
// to know which guest it is when it is launched by hand from the Home View with
// no environment at all. KL_TARGET still overrides it, which is what makes an
// A/B possible from `run.sh` without regenerating the project.
#include "kl_target.h"

#ifndef KL_TARGET_DEFAULT
#define KL_TARGET_DEFAULT KL_TARGET_DEFAULT_NAME
#endif

static const kl_target *g_target;

// Which of Steam Link's front doors this run opened, and whether the VR one's
// chain has been mapped yet.
//
// Both doors can be opened by one process here, which is the difference between
// this driver and `build/m_boot`. On the host the shell pairs and then re-EXECs
// into the VR door carrying the session, which makes every question about
// tearing the shell down go away. An app bundle cannot
// re-exec, so the two chains coexist: the shell's fourteen libraries stay
// mapped and its main thread parks inside startVRLink, exactly as an Android
// activity that called finishAndRemoveTask() would stop running.
static kl_slink_door g_door;
static int           g_vr_loaded;

// "Stop pumping." Set by kl_app_guest_stop (the immersive space going away) and
// by the 2D->VR handoff, which is the shell's own end. Plain and volatile
// because it is written once by somebody else and read by a loop inside
// kl_slink, which is handed a flag rather than a lock — the honest shape for
// exactly that.
static volatile int  g_guest_quit;

const char *kl_app_target_name(void) { return g_target ? g_target->name : "(unconfigured)"; }
int kl_app_target_is_steamlink(void) {
    return g_target && g_target->kind == KL_GUEST_STEAMLINK;
}
static int target_is_jkxr(void) {
    return g_target && g_target->kind == KL_GUEST_JKXR;
}
int kl_app_target_owns_frame_loop(void) {
    return g_target ? kl_driver_owns_frame_loop() : 0;
}

static char g_libdir[1024];
static char g_assets[1024];
static char g_files[1024];
static char g_apk[1024];
static char g_dylibs[1024];
static char g_log[1024];
static char g_status[512] = "not configured";

// Where the run has got to, for the heartbeat below; the driver reports its own
// phases through it.
static void app_phase(const char *p);

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
    g_target = kl_target_lookup(want);
    if (!g_target) return missing("guest target (KL_TARGET)", want);

    // The veneers' TSD slot is claimed HERE rather than wherever the first guest
    // library happens to load. The slot number is a constant baked into every
    // veneer, and Darwin hands external pthread keys out upward and never
    // reissues a held one — so whether it is still free is a race against
    // everything else in the process that creates keys. Beat Saber wins it by
    // accident: kl_app_boot loads libmain before ANGLE exists. The Steam Link
    // chain does not — something on its load path brings ANGLE up first, ANGLE
    // takes the process past the slot, and the guest fails to load with "TSD
    // slot N is unavailable", which reads as a platform limit and is a
    // scheduling accident.
    //
    // This is the earliest point both targets share. It is not fatal on its own:
    // an ELF-tree run with no veneered library is unaffected, and the load path
    // still refuses by name for one that needs them.
    if (kl_x18_init() != 0)
        fprintf(stderr, "  [klepton] x18: TSD slot %d was already taken at "
                        "configure time — a veneered guest will refuse to load\n",
                KLX_TSD_SLOT);

    // The guest libraries ride in the bundle: AMFI accepts a dylib inside a
    // bundle we signed, and nothing has established that it accepts one pushed
    // into Documents afterwards. The 2.3 GB of assets go the other way — into
    // the container — because they carry no code and re-uploading them on every
    // install would make the edit/run loop unusable. See visionos/README.md.
    snprintf(g_libdir, sizeof g_libdir, "%s/guest/lib/arm64-v8a", resources);
    // Frameworks/, not a directory of our own: that is where Xcode code-signs
    // what it embeds, and a loose Mach-O elsewhere in the bundle is only
    // sealed. kl_load_auto knows both layouts.
    snprintf(g_dylibs, sizeof g_dylibs, "%s/Frameworks", resources);
    snprintf(g_assets, sizeof g_assets, "%s/%s/assets", container, g_target->tree);
    snprintf(g_apk,    sizeof g_apk,    "%s/%s", container, g_target->apk);
    snprintf(g_files,  sizeof g_files,  "%s/android-files", container);
    snprintf(g_log,    sizeof g_log,    "%s/klepton-boot.log", container);

    // A crash report of its own, beside the log. The boot log is stdout with
    // stderr duped onto it, and neither survives a fault reliably — a guest may
    // take both fds, and stdio can hold the last writes behind a lock the dying
    // thread never releases. This path is opened at fault time and fsync'd, so
    // "it crashed and the log stops just before" stops being the whole story.
    static char crash[1024];
    snprintf(crash, sizeof crash, "%s/klepton-crash.log", container);
    kl_fault_set_crash_path(crash);

    // Check before running, not after failing. A missing asset tree otherwise
    // surfaces three layers up inside Unity as something that reads like a shim
    // bug.
    // With translations embedded, g_libdir never has to exist: it is only the
    // string NativeLoader.load() is handed and the prefix the DT_NEEDED walk
    // builds paths from, and kl_load_auto turns "<libdir>/libunity.so" into
    // "Frameworks/libunity.framework/libunity" before touching the disk. The ELF
    // tree is therefore not in the bundle — 80 MB that would buy only an A/B
    // against the mmap loader the translations replace.
    if (!have_translations() && !have(g_libdir))
        return missing("guest libraries (neither translations nor an ELF tree)", g_libdir);
    if (!have(g_assets)) return missing("staged assets (run stage_assets.sh)", g_assets);
    if (!have(g_apk))    return missing("staged APK (run stage_assets.sh)", g_apk);
    mkdir(g_files, 0755);

    if (kl_app_target_is_steamlink()) {
        // Which front door this launch opens. The SHELL is the default because
        // it is the only one that can produce a session: the VR half reads its
        // own out of the launching Intent and leaves before its first frame
        // without one. A run handed a session by hand — KL_SLINK_SARGS from a
        // host pairing — wants the VR door directly, and says so by carrying
        // one.
        g_door = KL_SLINK_SHELL;
        if (kl_env_on("KL_SLINK_VR", 0) || kl_env_str("KL_SLINK_SARGS", NULL))
            g_door = KL_SLINK_VR;
        if (kl_env_on("KL_SLINK_SHELL", 0)) g_door = KL_SLINK_SHELL;

        // Everything this guest is told about itself — the activity class, the
        // four paths, the <meta-data>, the panel size — is kl_slink's, and
        // shared verbatim with `build/m_boot`. Two drivers describing one guest
        // differently is a class of bug with no error surface at all: the run
        // works and answers the guest's questions wrongly.
        if (kl_slink_configure(g_door, g_libdir, g_assets, g_apk, g_files,
                               NULL) != 0)
            return missing("Steam Link front door", kl_slink_error());

        // ...and then Qt's plugin path, overriding what kl_slink_configure
        // derived, because on this platform the two are not the same directory.
        //
        // **Qt reads a plugin as a FILE before it will load it.** libQt6Core's
        // search is a glob (`libplugins_%1_*.so`), so it lists the path and
        // then parses each candidate's ELF metadata for the IID and the Qt
        // version. Everywhere else in this project a guest library is a NAME
        // the loader resolves — on device the ELF tree is deliberately not in
        // the bundle at all — and that is not enough here: with no real files
        // nothing is ever a candidate, and libshell aborts with `Could not find
        // the Qt platform plugin "virtual"`.
        //
        // So the six plugin .so files are staged into the container as data
        // (visionos/targets.py, `qtplugins`), and the dlopen Qt then does is
        // unaffected: kl_load_auto matches a translation on the BASENAME
        // whatever the path says, so it still gets the signed framework out of
        // Frameworks/. A missing directory is left alone rather than pointed at
        // — an empty QT_PLUGIN_PATH and a wrong one fail the same way, and the
        // staging error belongs in stage_assets.sh's report.
        char qtdir[1200];
        snprintf(qtdir, sizeof qtdir, "%s/%s/qtplugins", container, g_target->tree);
        if (have(qtdir)) setenv("QT_PLUGIN_PATH", qtdir, 1);
        else if (g_door == KL_SLINK_SHELL)
            fprintf(stderr, "  [klepton] %s is not staged — Qt will not find its "
                            "platform plugin (run visionos/stage_assets.sh)\n", qtdir);
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
        // ...and HOME, for a guest that is a UNIX PORT wearing an Android
        // manifest rather than an Android app. OpenJK derives fs_homepath from
        // $HOME and then walks it with mkdir -p (FS_CreatePath), which is fatal
        // on the first component it cannot make. The OS sets HOME to the app's
        // data container ROOT, and that directory is not ours to extend: the
        // system provisions Documents/, Library/ and tmp/ inside it and refuses
        // anything else. So the engine walked seven directories that already
        // existed and died on mkdir("<container>/.local") with EPERM.
        //
        // The guest's external storage is the writable tree it keeps everything
        // else in, so point HOME there and fs_homepath becomes
        // <ext>/.local/share/openjk, beside <ext>/JKXR.
        //
        // Only for this target. HOME is the OS's own answer to a sandboxed
        // process, Foundation resolves the container through it, and the other
        // guests here reach it through Qt — none of which needs redirecting.
        // Safe at this point for the same reason: the caller resolves the
        // container once, before kl_app_configure runs.
        if (target_is_jkxr()) setenv("HOME", g_files, 1);
        // Raw "<apk>/assets/..." opens: Unity mounts the APK into its VFS and
        // then resolves entries by concatenating onto the mount point.
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

    // ...and so does MoltenVK, for a guest whose graphics API is Vulkan
    // (BONELAB). Same rule, same reason: the app knows where its own Frameworks
    // are and the launcher does not.
    //
    // kl_vulkan.c would find it anyway — its last resort is
    // `@rpath/MoltenVK.framework/MoltenVK`, and @rpath here IS this directory.
    // Naming it explicitly costs nothing and buys the failure message: without
    // this, the two dlopens that precede the @rpath attempt are against the
    // HOST's vendored path, so a run where MoltenVK is genuinely missing from
    // the bundle reports `vendor-moltenvk/out/macos/...: no such file`, which
    // names a directory that has never existed on a headset.
    setenv("KL_MVK_DIR", g_dylibs, 0);

    // The capture knobs, resolved against the CONTAINER when they are relative.
    //
    // Every one of these names an output directory, and on device there is
    // exactly one writable place and no shell to expand a path against: the
    // process's cwd is `/`, so `KL_VK_OUT=vkcap` writes to `/vkcap` and fails,
    // silently, on a run that otherwise looks healthy. An absolute path still
    // wins outright, and the container's own UUID changes on reinstall — so
    // spelling one out at the launcher is not a workaround either, it is a value
    // that goes stale between runs.
    //
    // Relative-means-container is also what makes the file RETRIEVABLE: only the
    // app data container can be read back with `devicectl device copy from`.
    static const char *const cap_knobs[] = {
        "KL_VK_OUT", "KL_GLFB_OUT", "KL_DUMP_SHADERS", "KL_DUMP_TEXTURES",
        "KL_VTDEC_DUMP",
    };
    for (size_t i = 0; i < sizeof cap_knobs / sizeof *cap_knobs; i++) {
        const char *v = getenv(cap_knobs[i]);
        if (!v || !*v || *v == '/') continue;
        char abs[1200];
        snprintf(abs, sizeof abs, "%s/%s", container, v);
        setenv(cap_knobs[i], abs, 1);
        // ...and created, because none of the capture paths make their own
        // directory: on the host you `mkdir /tmp/vk` first, and on device there
        // is nowhere to type that. A capture whose directory does not exist
        // fails per FILE, quietly, and the run looks like one where the guest
        // never drew.
        mkdir(abs, 0755);
        printf("[app] %s is relative — resolved to %s\n", cap_knobs[i], abs);
    }

    // The guest's own sequences — boot, lifecycle, frame, reports — are
    // kl_driver's, shared verbatim with `build/m_boot`. What is left in this file
    // is the bundle: the log, the heartbeat, the guest thread, the compositor's
    // pacing and the handoff.
    kl_driver_init(g_target, g_libdir, g_door);
    kl_driver_set_phase_hook(app_phase);
    // 120 s, not the driver's 20: there is no shell here to notice a hang and no
    // way to attach, so the watchdog is the only thing that says WHERE a run
    // stopped — and a device is slower to reach its first frame than the host.
    kl_driver_set_alarm(kl_env_int("KL_ALARM", 120));

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
//
// KL_LOG_FILE=0 leaves both streams alone, which is the ONLY way anything
// outside this process can read them live: `devicectl device process launch
// --console` bridges the app's stdout and stderr, and a freopen'd stdout is
// bridged to a file nobody is watching. Polling the file instead costs a
// `devicectl device copy from` per sample, and that call is ~27 seconds on this
// hardware whatever the file's SIZE — 150 KB and 15 MB take the same time — so a
// run watched by copying is minutes of transfer per minute of run. Nothing here
// is smaller in that mode; the same bytes go somewhere the host can already see.
//
// The file stays the default because it is what survives the process: an
// unimplemented JNI call aborts by design, a launch from the Home View has no
// console at all, and the in-window boot report is read back out of this path.
static int open_log(void) {
    if (!kl_env_on("KL_LOG_FILE", 1)) {
        // Same buffering discipline either way. The reason is unchanged — a
        // fully-buffered stream loses the report when the process dies on a
        // signal — and it applies harder here, because the far end of the bridge
        // is another process that only ever sees what was actually written.
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IOLBF, 0);
        return 0;
    }
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
static void app_phase(const char *p) { g_phase = p; }

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

static int fail(const char *msg) {
    snprintf(g_status, sizeof g_status, "%s", msg);
    fprintf(stderr, "FAIL: %s\n", msg);
    fflush(NULL);
    return 1;
}

// --- Steam Link, both front doors in one process ----------------------------
//
// Map, report and initialise the VR chain. Idempotent, because two paths reach
// it: a launch that was handed a session (the door is VR from the start) and a
// handoff out of the shell (the door was SHELL and the VR chain is loaded on top
// of it). One function so those two cannot diverge.
static int steamlink_load_vr(void) {
    if (g_vr_loaded) return 0;
    // Re-describing the app, because the VR half is a DIFFERENT ACTIVITY in the
    // same package — android/app/NativeActivity with `android.app.lib_name`, not
    // SteamLink with the SDL <meta-data>. The host driver does this by re-exec'ing
    // into a fresh process; here it is a second call.
    g_door = KL_SLINK_VR;
    if (kl_slink_configure(KL_SLINK_VR, g_libdir, g_assets, g_apk, g_files,
                           stdout) != 0)
        return fail(kl_slink_error());
    kl_driver_init(g_target, g_libdir, KL_SLINK_VR);
    if (kl_driver_boot(stdout) != 0) return fail(kl_driver_error());
    g_vr_loaded = 1;
    return 0;
}

// What this target needs decided before its chain runs, in one place because
// both front doors reach it and neither can be told apart afterwards. Policy
// lives here rather than in kl_driver: the driver describes the guest, the
// frontend chooses how this platform runs it.
static void steamlink_policy(void) {
    // Steam Link's VR door composites several OpenXR projection layers: layer 0
    // is the environment/room (black in an empty space) and the streamed screen
    // is stacked ON TOP. The compositor shows one projection layer, so tell it
    // to take the topmost — the picture — rather than the base layer, which is
    // why the stream came up as a black void before. Harmless for the 2D shell
    // (it submits no projection layers); KL_XR_CAPTURE_LAYER still overrides.
    kl_openxr_set_capture_topmost_layer(1);
    // Steam Link streams FOVEATED: the wide base eye is low resolution and a
    // separate narrow inset carries the sharp centre (kl_openxr lays it over the
    // base). For that inset to keep its detail the compositor's eye texture must
    // be bigger than the base, so the inset is not scaled down to fit — allocate
    // it larger, but cap the larger side near the display's own per-eye
    // resolution so an already-large menu layer is not doubled and the allocator
    // does not churn. KL_XR_EYE_SCALE / KL_XR_EYE_MAX tune it.
    {
        int es = kl_env_int("KL_XR_EYE_SCALE", 2);
        if (es < 1) es = 1;
        if (es > 4) es = 4;
        kl_glfb_set_eye_mirror_scale(es, 1);
        kl_glfb_set_eye_mirror_cap(kl_env_int("KL_XR_EYE_MAX", 3456));
    }
    // Two things this target has always needed passed on the command line, made
    // defaults so a bare launch just works — the env still overrides either:
    //   - the 90 Hz pin (KL_DISPLAY_HZ): Steam Link's VR client dereferences a
    //     null frametime container on any rate the compositor MEASURES, so the
    //     rate it is told has to be fixed up front.
    //   - a deeper audio buffer (KL_AUDIO_LATENCY_MS): the audio is a network
    //     stream, and the 80 ms local-mixer default underruns on jitter — heard
    //     as the micro-stutters. 180 ms buffers the jitter at a latency a
    //     streamed title tolerates.
    kl_ovrp_set_forced_hz_hint((float)kl_env_int("KL_DISPLAY_HZ_DEFAULT", 90));
    kl_audio_set_latency_ms((unsigned)kl_env_int("KL_AUDIO_LATENCY_DEFAULT_MS", 240));
}

// The 2D -> VR handoff — and the one thing about it that is
// this driver's rather than the host driver's is that **there is no re-exec**.
//
// `build/m_boot` replaces its process image, which makes every question about
// tearing the shell down (Qt, SDL, ANGLE's contexts, the audio unit, the
// shell's threads) simply not arise. An app bundle cannot do that. So the two
// front doors coexist: this records the session, hands the ImmersiveSpace its
// cue, and then **parks this thread forever**.
//
// Parking is the honest shape and not a workaround. This is the guest's own
// main thread, inside startVRLink, and on Android the very next thing that
// method does is finishAndRemoveTask() on itself — an activity that stops
// running. kl_jni.h's contract says the same thing from the other end: a
// handler that RETURNS still aborts, because returning tells the guest its
// activity started when nothing did, and the shell then finishes itself with no
// picture and no reason given.
//
// The GL context is released first, and that one is load-bearing rather than
// tidy: kl_glfb hands the root context to one thread at a time, and a parked
// thread still holding it is a VR guest that can never make one current.
static char           g_sargs[4096];
static volatile int   g_handoff;

static void app_vrlink_handoff(const char *sargs) {
    snprintf(g_sargs, sizeof g_sargs, "%s", sargs ? sargs : "");
    // Through the environment as well as the variable, because that is the path
    // a hand-carried session already takes (KL_SLINK_SARGS) and the guest must
    // not be able to tell the two apart.
    setenv("KL_SLINK_SARGS", g_sargs, 1);

    printf("\n=== 2D -> VR handoff: the shell paired and the host authorized ===\n");
    printf("    the OpenXR front door opens in the ImmersiveSpace; this thread "
           "parks (no re-exec inside an app bundle)\n");
    // A stopgap, announced rather than assumed. On the window path nothing ever
    // holds a drawable, so no eye offsets are measured and the IPD is 0 — which
    // this client reads as "unchanged" and never publishes its projection, so
    // the host sends no video.
    if (!getenv("KL_OVRP_IPD")) {
        setenv("KL_OVRP_IPD", "0.063", 1);
        printf("    KL_OVRP_IPD=0.063   (STOPGAP: an IPD of 0 stops the host "
               "sending video)\n");
    }
    fflush(NULL);

    kl_glfb_release_current();
    // From here on the answer to SteamLink.isVRLinkRunning() is yes — the
    // session is recorded and the ImmersiveSpace is about to open the VR door.
    // Set before the park, because the asker is the SHELL's own background
    // threads, which outlive this thread and poll exactly that question.
    kl_jni_set_vrlink_running(1);
    // The shell's pump has nothing left to drain — its main thread is about to
    // stop existing for all practical purposes — so let whoever is pumping it
    // finish and write its report.
    g_guest_quit = 1;
    __atomic_store_n(&g_handoff, 1, __ATOMIC_RELEASE);

    for (;;) {
        struct timespec iv = { 3600, 0 };
        nanosleep(&iv, NULL);
    }
}

int kl_app_vrlink_pending(void) { return __atomic_load_n(&g_handoff, __ATOMIC_ACQUIRE); }
const char *kl_app_vrlink_sargs(void) { return g_sargs; }

// The VR front door opened on top of a process that is already running the
// shell. The other way in — a launch that was handed a session — takes
// kl_app_boot and kl_app_lifecycle_begin like any other target; this exists
// because the immersive space opens mid-run and has to do both halves itself.
//
// It MUST run on the thread that will go on to pump: onCreate takes
// ALooper_forThread() and the guest hangs its UIThreadCallbackHandler off
// exactly that looper.
static int steamlink_vr_begin(void) {
    // The handoff set this to end the SHELL's pump; a new front door is a new
    // run and the VR pump must not inherit it. Cleared here rather than in the
    // handoff because the handoff's own meaning is "stop pumping the shell" and
    // clearing it there would be a race with the loop it is trying to stop.
    g_guest_quit = 0;
    // Here as well as at the handoff, because this door is also reached
    // DIRECTLY — a launch handed KL_SLINK_SARGS never runs the shell, and
    // isVRLinkRunning() must still answer for the door that is opening.
    kl_jni_set_vrlink_running(1);
    if (steamlink_load_vr()) return 1;
    if (kl_driver_lifecycle_begin(stdout) != 0) return fail(kl_driver_error());
    return 0;
}

int kl_app_boot(void) {
    // Once, and never concurrently. The Boot button invites a second press, and
    // the second entry is not merely redundant: the runtime's JNI tables are
    // process-global, so another JNI_OnLoad re-registers every native onto the
    // same table, while open_log() truncates the file the first run is still
    // writing. Five presses on device produced "natives registered: 61" instead
    // of 45 with a chunk of the log missing, which reads as a platform
    // difference and is not one.
    static pthread_mutex_t once_mu = PTHREAD_MUTEX_INITIALIZER;
    static int entered;
    pthread_mutex_lock(&once_mu);
    int already = entered;
    entered = 1;
    pthread_mutex_unlock(&once_mu);
    if (already) return fail("kl_app_boot was already run in this process");

    if (!*g_libdir) return fail("kl_app_configure was not called");
    if (open_log()) return fail("could not open the log file");

    // Every thread that runs guest code must seed bionic's stack-guard canary
    // into TSD slot 5 first, and this is the thread the whole boot runs on.
    // Beat Saber's chain does not expose the gap: nothing on the path to initJni
    // has a stack protector on it. libSDL3's JNI_OnLoad does, and without this
    // dies with `stack smashing detected in libSDL3.so+0xa6f9c` — a canary
    // mismatch with no buffer overflow anywhere near it. Idempotent, so calling
    // it on a thread that already has one is free.
    kl_thread_init();

    heartbeat_start();
    kl_fault_install();
    // Subscribe to the OS's memory-pressure signal before the guest allocates
    // anything. On this platform it is the only warning that arrives ahead of
    // jetsam, and jetsam is a kill with no log line of its own.
    kl_mem_pressure_init();
    // Strict: an unimplemented *call* is fatal, so the run stops exactly where
    // the surface genuinely ends. Lookups are not — the guest resolves plenty
    // it never calls.
    kl_jni_set_permissive(kl_env_on("KL_PERMISSIVE", 0));
    kl_egl_dump_textures(kl_env_str("KL_DUMP_TEXTURES", NULL));

    printf("=== Klepton on visionOS ===\n");
    printf("  target    : %s\n", g_target->name);
    // The front door, with the reason. It is chosen from the ENVIRONMENT in
    // kl_app_configure — which runs before the log file is open, so it cannot
    // announce itself there — and an app launched from the Home View has no
    // environment at all. A run that was meant to carry a session and silently
    // took the shell instead is indistinguishable from a broken handoff, and
    // the first thing it does is open a screen that looks like it is working.
    if (kl_app_target_is_steamlink())
        printf("  front door: %s   (KL_SLINK_SARGS %s, KL_SLINK_VR %s, "
               "KL_SLINK_SHELL %s)\n",
               g_door == KL_SLINK_VR ? "VR (libvrlink_scene)" : "SHELL (libshell + Qt)",
               kl_env_str("KL_SLINK_SARGS", NULL) ? "carried" : "UNSET",
               kl_env_on("KL_SLINK_VR", 0) ? "on" : "off",
               kl_env_on("KL_SLINK_SHELL", 0) ? "ON, which overrides the others" : "off");
    printf("  libraries : %s\n", g_libdir);
    printf("  dylibs    : %s%s\n", g_dylibs,
           have_translations() ? "" : "  (none embedded — falling back to the mmap ELF loader)");
    printf("  assets    : %s\n", g_assets);
    printf("  apk       : %s\n", g_apk);
    printf("  files     : %s\n\n", g_files);
    fflush(NULL);

    // Before the chain runs, so nothing it starts reads a default we meant to
    // replace.
    if (kl_app_target_is_steamlink()) steamlink_policy();

    // The door's own sequence, in kl_driver: the chain and its entry point for
    // Steam Link, Unreal and OpenJK; libmain -> NativeLoader.load -> initJni for
    // a Unity guest. It stops before the guest is STARTED, which is what makes
    // "the guest loaded" and "the guest ran" two reports rather than one.
    if (kl_driver_boot(stdout) != 0) return fail(kl_driver_error());
    if (kl_app_target_is_steamlink()) {
        if (g_door == KL_SLINK_VR) {
            g_vr_loaded = 1;
            // Not "no sArgs, therefore stop": the guest decides that for itself
            // and prints "No sArgs and release build panic" on its own way out.
            // But it exits BEFORE its first frame, and that reads exactly like a
            // compositor failure from the outside.
            if (!getenv("KL_SLINK_SARGS"))
                printf("\n  NOTE: KL_SLINK_SARGS is unset. This guest reads its session out of\n"
                       "  the launching Intent and exits before its first frame without one —\n"
                       "  pair in the 2D shell first; the handoff carries it.\n");
        }
        snprintf(g_status, sizeof g_status, "Steam Link %s chain initialised",
                 kl_slink_door_name());
    } else {
        snprintf(g_status, sizeof g_status, "%s",
                 g_target->kind == KL_GUEST_UE4  ? "the Unreal chain initialised"
                 : target_is_jkxr()              ? "the OpenJK chain initialised"
                                                 : "initJni completed");
    }
    app_phase("boot report");
    kl_jni_report(stdout);
    fflush(NULL);
    app_phase("boot done");
    return 0;
}

// The synthetic /proc, read back on device. Read it BEFORE chasing anything
// else in a device lifecycle run: proc_build() gets the free-page count from
// host_statistics64(), which links on visionOS but may be restricted inside the
// sandbox, and a silent zero there is not read by Unity as "unknown" but as a
// machine with no memory — `Cores = 0, Memory = 0mb`, after which Unity refuses
// to start with nothing anywhere mentioning memory.
static void report_proc(void) {
    static const char *files[] = { "/proc/cpuinfo", "/proc/meminfo",
                                   "/sys/devices/system/cpu/possible" };
    printf("\n=== the synthetic /proc, on device ===\n");
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
        if (!shown) printf("  %-34s (served but EMPTY)\n", files[i]);
        fclose(f);
    }
    fflush(NULL);
}

int kl_app_lifecycle_begin(void) {
    // Separate from kl_app_boot rather than folded into it, because boot is a
    // gate and refuses a second entry: keeping them apart lets the UI run the
    // gate, read its numbers, and only then go further.
    static pthread_mutex_t once_mu = PTHREAD_MUTEX_INITIALIZER;
    static int entered;
    pthread_mutex_lock(&once_mu);
    int already = entered;
    entered = 1;
    pthread_mutex_unlock(&once_mu);
    if (already) return fail("kl_app_lifecycle was already run in this process");

    // First, before anything can be misdiagnosed on top of it: proc_build() gets
    // the free-page count from host_statistics64(), which links here but may be
    // restricted inside the sandbox, and Unity reads a silent zero not as
    // "unknown" but as a machine with no memory.
    app_phase("proc");
    report_proc();

    // The 2D shell is the one door with anything to set up first: the frame
    // capture must be live before its first swap (kl_glfb captures only when a
    // sink or a dump directory is set), and the handoff handler is the shell's
    // alone — installing it on another door would let a run take a path it has no
    // business on. app_vrlink_handoff never returns.
    if (kl_app_target_is_steamlink() && g_door == KL_SLINK_SHELL) {
        kl_mono_capture_start();
        kl_jni_set_vrlink_handoff(app_vrlink_handoff);
    }

    // The guest's own start, in kl_driver. It MUST run on the thread that will go
    // on to pump: the three doors that own their frame loop take
    // ALooper_forThread() inside onCreate and hang their callbacks off exactly
    // that looper.
    if (kl_driver_lifecycle_begin(stdout) != 0) return fail(kl_driver_error());
    return 0;
}

// One guest frame. Split out of the pump loop so that Compositor Services can be
// the clock: on device the frame deadline belongs to cp_frame_predict_timing, and
// a pump that owns its own loop cannot be paced by something else.
//
// Returns what nativeRender returned, or -1 for a guest that owns its own frame
// loop (the three doors where what our thread owes it is a turning looper) or
// before kl_app_lifecycle_begin has run.
int kl_app_frame(void) { return kl_driver_frame(); }

void kl_app_lifecycle_report(void) {
    kl_driver_report(stdout);
    if (kl_app_target_is_steamlink())
        snprintf(g_status, sizeof g_status, "Steam Link %s run ended",
                 kl_slink_door_name());
    else if (g_target->kind == KL_GUEST_UE4)
        snprintf(g_status, sizeof g_status, "the Unreal run ended");
    else if (target_is_jkxr())
        snprintf(g_status, sizeof g_status, "the OpenJK run ended");
    else
        snprintf(g_status, sizeof g_status, "lifecycle ran, %u frames",
                 kl_driver_frames());
}

int kl_app_lifecycle(unsigned frames) {
    int rc = kl_app_lifecycle_begin();
    if (rc) return rc;

    // A guest that owns its own frame loop takes no frames from us, so what a
    // bounded run means for it is a bounded PUMP, in seconds — KL_SLINK_WAIT /
    // KL_UE4_WAIT / KL_JKXR_WAIT, the same budgets the command line reads.
    // `frames` would be a number with nothing behind it.
    //
    // The 2D shell is the one door with no deadline by default: a bounded pump
    // measures how far a guest got by itself, and the shell is not being
    // measured but USED — a person reads it, types a PIN into it and waits on a
    // host, and a first pairing takes ~35 s. An explicit KL_SLINK_WAIT still
    // bounds it, for a scripted run that wants that.
    if (kl_app_target_owns_frame_loop()) {
        int open_ended = kl_app_target_is_steamlink() && g_door == KL_SLINK_SHELL
                      && !kl_env_str("KL_SLINK_WAIT", NULL);
        double secs = open_ended ? -1.0 : kl_driver_pump_default();
        if (open_ended)
            printf("\n=== the shell is running; pumping until the window closes ===\n");
        else
            printf("\n=== pumping the looper for %.1f s ===\n", secs);
        fflush(NULL);
        printf("  pumped %.2f s\n", kl_driver_pump(secs, &g_guest_quit));
        kl_app_lifecycle_report();
        return 0;
    }

    printf("\n=== pumping %u frames ===\n", frames);
    fflush(NULL);
    while (kl_driver_frames() < frames && kl_app_frame() >= 0) { }
    kl_app_lifecycle_report();
    return 0;
}

// --- The guest on its own thread ---------------------------------------
// See kl_app.h for what this is for. What is here is the handoff itself, and
// it is deliberately in C rather than Swift for one reason above the language
// boundary: every thread that runs guest code must call
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
    // Counted here because on this path the guest CALLS the clock rather than
    // being called by it: kl_driver_frame never runs for a guest that owns its
    // own loop, so nothing else would count its frames.
    kl_driver_note_frame();
    pthread_mutex_unlock(&g_guest.mu);
}

static void *guest_thread(void *unused) {
    (void)unused;
    // The guest's TPIDR slot, before a single guest instruction runs here.
    kl_thread_init();
    pthread_setname_np("Klepton Guest");

    // Two ways in, and which one this is depends on what the WINDOW already
    // did. On a plain launch this thread is the first thing to run a guest, and
    // kl_app_lifecycle_begin is that. After a 2D->VR handoff the window path has
    // already spent that entry on the shell — so the immersive space is opening
    // on top of a process that is mid-run, and what it starts is the OTHER front
    // door. Both are once-per-process; they are just not the same once.
    int rc = kl_app_vrlink_pending() ? steamlink_vr_begin() : kl_app_lifecycle_begin();
    if (rc != 0) {
        printf("[guest] %s failed: %s\n",
               kl_app_vrlink_pending() ? "the VR front door" : "lifecycle_begin", g_status);
        fflush(NULL);
        guest_finished();
        return NULL;
    }
    guest_set_state(1);

    // Two shapes, because the doors differ in who owns the frame loop.
    //
    // A Unity guest has none: nativeRender is a call, so this thread makes one
    // per published pose. The other three brought their own — a thread spawned
    // inside onCreate or android_main — so what this thread owes them is a
    // turning looper and nothing else. Pacing is not lost by that: it moves to
    // where the XR API puts it (xrWaitFrame, ovrp_WaitToBeginFrame), which blocks
    // on the same published pose through guest_pace_wait. One clock either way.
    //
    // Unbounded here, unlike the window path: the immersive space's own dismissal
    // is what ends the run, through g_guest_quit.
    if (kl_app_target_owns_frame_loop()) {
        printf("\n=== guest thread pumping the activity's looper ===\n");
        fflush(NULL);
        double secs = kl_driver_pump(-1.0, &g_guest_quit);
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
        if (limit && kl_driver_frames() >= limit) {
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
    // ...and the OpenJK guest takes the same one, because it is the same API:
    // its frame clock is xrWaitFrame, wherever the engine calls it from. Which
    // pacer a guest wants is a property of the XR RUNTIME it drives, not of the
    // door it came through — Steam Link and JKXR are both OpenXR, RE4 is
    // OVRPlugin, and pairing a door with the wrong one is a guest that blocks
    // forever on a pose nobody publishes to it.
    if (kl_app_target_is_steamlink() || target_is_jkxr())
        kl_openxr_set_frame_pacer(guest_pace_wait);
    // ...and the same clock on the OVRPlugin side, for the other guest that
    // owns its loop. ovrp_WaitToBeginFrame is this API's xrWaitFrame. It is
    // installed ONLY here: every Unity guest reaches that call from inside a
    // frame this driver already initiated, so a pacer there would be the
    // compositor waiting on itself.
    if (g_target->kind == KL_GUEST_UE4) kl_ovrp_set_frame_pacer(guest_pace_wait);

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
    g_guest_quit = 1;      // the kl_slink pumps watch this one
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
