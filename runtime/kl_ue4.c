// The Unreal Engine 4 target. See kl_ue4.h for why it is its own file.
#include "kl_ue4.h"

#include <stdint.h>
#include <stdio.h>
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

static int ue4_fail(const char *why) {
    snprintf(g_err, sizeof g_err, "%s", why);
    return 1;
}

const char *kl_ue4_error(void) { return g_err; }

int kl_ue4_configure(const char *libdir, FILE *out) {
    if (!libdir || !*libdir) return ue4_fail("no library directory");
    snprintf(g_libdir, sizeof g_libdir, "%s", libdir);
    // Everything else — assets, apk, files, native lib dir — has already been
    // set from the target row by kl_target_apply_host(). Restating any of it
    // here is how two descriptions of one guest start to drift.
    kl_jni_set_activity_class(UE4_ACTIVITY);
    if (out) {
        fprintf(out, "  [ue4] activity: %s\n", UE4_ACTIVITY);
        fprintf(out, "  [ue4] userdata: %s\n", kl_jni_files_dir());
        fflush(out);
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
        if (out) { fprintf(out, "  mapped %s\n", UE4_CHAIN[i]); fflush(out); }
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

int kl_ue4_create(FILE *out) {
    if (!g_ue4) return ue4_fail(UE4_LIB " was never loaded");
    if (kl_na_create(g_ue4, "ANativeActivity_onCreate", out) != 0)
        return ue4_fail(UE4_LIB " exports no ANativeActivity_onCreate");
    return 0;
}

void kl_ue4_start(FILE *out) { kl_na_start(out); }
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
