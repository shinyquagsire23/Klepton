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
#include "kl_nativeactivity.h"
#include "kl_egl.h"
#include "kl_ovrp.h"
#include "kl_ovrplat.h"
#include "kl_mediandk.h"
#include "kl_aaudio.h"
#include "kl_opensl.h"

// The library the chain ENDS at. Not a target-table lookup: the table says
// "libUE4" as the entry (which is what proves the right guest was embedded on
// device) and this is the file name, which is the same fact spelled for the
// loader. One string, here, rather than two spellings in two files.
#define UE4_LIB "libUE4.so"

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
static const char *const UE4_CHAIN[] = {
    "libc++_shared.so",
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
#define UE4_ACTIVITY "com/epicgames/ue4/GameActivity"

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
    snprintf(dir, sizeof dir, "%s/UE4Game", files);

    const char *names[8];
    char        held[8][1024];
    size_t      n = 0;
    snprintf(held[n], sizeof held[n], "%s/UE4CommandLine.txt", dir);
    names[n] = held[n]; n++;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < sizeof names / sizeof *names) {
            if (e->d_name[0] == '.') continue;
            snprintf(held[n], sizeof held[n], "%s/%s/UE4CommandLine.txt",
                     dir, e->d_name);
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

int kl_ue4_configure(const char *libdir, FILE *out) {
    if (!libdir || !*libdir) return ue4_fail("no library directory");
    snprintf(g_libdir, sizeof g_libdir, "%s", libdir);
    // Everything else — assets, apk, files, native lib dir — has already been
    // set from the target row by kl_target_apply_host(). Restating any of it
    // here is how two descriptions of one guest start to drift.
    kl_jni_set_activity_class(UE4_ACTIVITY);
    // ...and the way back. UE4's Java front end calls natives on itself, so a
    // driver standing in for that Java has to be able to make those calls; the
    // JNI surface does not know which image is the guest, and this file does.
    kl_jni_set_guest_native_resolver(ue4_native_symbol);
    if (out) {
        fprintf(out, "  [ue4] activity: %s\n", UE4_ACTIVITY);
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
        // until something joins the two. m_slink has printed this since SL-1;
        // this door never did, which made the sampler useless on the one target
        // that needs it most.
        if (out) {
            fprintf(out, "  mapped %-22s @%p %7.2f MB\n", UE4_CHAIN[i],
                    kl_base(img), kl_span(img) / 1048576.0);
            fflush(out);
        }
    }
    if (!g_ue4) return ue4_fail(UE4_LIB " is not in the chain");

    for (size_t i = 0; i < sizeof UE4_CHAIN / sizeof *UE4_CHAIN; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", g_libdir, UE4_CHAIN[i]);
        kl_image *img = kl_find_image(path);
        if (!img) continue;
        if (out) { fprintf(out, "  init %s\n", UE4_CHAIN[i]); fflush(out); }
        kl_run_init(img);
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

#define UE4_NATIVE_PREFIX "Java_com_epicgames_ue4_GameActivity_"

static void *ue4_native(const char *name) {
    if (!g_ue4) return NULL;
    char sym[256];
    snprintf(sym, sizeof sym, UE4_NATIVE_PREFIX "%s", name);
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
static ue4_jboolean ue4_meta_bool(const char *key) {
    const char *v = kl_jni_manifest_meta(key);
    return (ue4_jboolean)(v && strcmp(v, "true") == 0);
}

static int ue4_meta_int(const char *key, int dflt) {
    const char *v = kl_jni_manifest_meta(key);
    return v ? (int)strtol(v, NULL, 10) : dflt;
}

static const char *ue4_meta_str(const char *key) {
    const char *v = kl_jni_manifest_meta(key);
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
    // fork into a tree that does not exist (trap 45's shape — a resource with
    // two doors has to resolve to one place by construction).
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
    UE4_CALL(out, "nativeSetAndroidVersionInformation", ue4_fn_sssss,
             jrel, jmake, jmodel, jbuild, jloc);

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
    if (!g_ue4) return ue4_fail(UE4_LIB " was never loaded");
    if (kl_na_create(g_ue4, "ANativeActivity_onCreate", out) != 0)
        return ue4_fail(UE4_LIB " exports no ANativeActivity_onCreate");
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
        // only definition that matters (trap 35: "am I the UI thread" is
        // answered by WHO DRAINS THE QUEUE).
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
