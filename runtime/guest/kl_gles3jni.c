// kl_gles3jni — see kl_gles3jni.h. The GLES3JNILib call sequence, lifted from
// kl_jkxr (which owns the JKXR-specific data layout) and made engine-agnostic:
// the native prefix is discovered from the engine's own exports, and there is no
// asset copying — the data tree is staged as-is.
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "klepton.h"
#include "kl_gles3jni.h"
#include "kl_native.h"
#include "kl_jni.h"
#include "kl_ndk.h"
#include "kl_egl.h"
#include "kl_openxr.h"
#include "kl_env.h"

static char        g_libdir[1024];
static char        g_entry[256];         // "libxash.so"
static char        g_prefix[128];        // "Java_com_drbeef_lambda1vr_GLES3JNILib_"
static kl_image   *g_engine;
static long long   g_handle;
static char        g_error[256];

static int g3_fail(const char *msg) { snprintf(g_error, sizeof g_error, "%s", msg); return -1; }
const char *kl_gles3jni_error(void) { return g_error[0] ? g_error : "(none)"; }

// ---------------------------------------------------------------------------
// Doom3Quest (com.drbeef.doom3quest, libdoom3) — the same door with two
// differences, both read out of its GLES3JNIActivity smali:
//
//   * NO HANDLE. onCreate(Activity, String cmdline, String manufacturer) returns
//     void, and onStart / onSurfaceCreated / onSurfaceChanged take only their one
//     object. The engine keeps its state in globals.
//   * THE ACTIVITY STAGES THE DATA ITSELF before the native call: it creates
//     /sdcard/Doom3Quest/base, copies the port's pak399.pk4 and the three
//     quest<N>_default.cfg into it, copies the bundled DEMO (demo00-03.pk4) when
//     there is no retail pak000.pk4 beside them, drops commandline.txt next to
//     base/, and sets USER_FILES, GAMELIBDIR and GAMETYPE. The engine reads all
//     of that and stages none of it, so with the Java gone it is done here.
static int g_d3;

static void d3_mkdirs(const char *path) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}
static long long d3_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}
// Copied when absent or when the size differs: the port's own files are
// refreshed by a new APK, and a 100 MB demo pak is not rewritten every launch.
static void d3_copy_asset(const char *name, const char *dstdir, FILE *out) {
    char src[1300], dst[1300];
    snprintf(src, sizeof src, "%s/%s", kl_jni_assets_dir(), name);
    snprintf(dst, sizeof dst, "%s/%s", dstdir, name);
    long long want = d3_size(src);
    if (want < 0) {
        if (out) fprintf(out, "  [d3] %s — not in the APK's assets, skipped\n", name);
        return;
    }
    if (d3_size(dst) == want) return;
    FILE *in = fopen(src, "rb"), *o = in ? fopen(dst, "wb") : NULL;
    if (!o) {
        if (out) fprintf(out, "  [d3] %s — cannot stage to %s: %s\n", name, dst, strerror(errno));
        if (in) fclose(in);
        return;
    }
    static char buf[1 << 20];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, got, o) != got) break;
    fclose(in); fclose(o);
    if (out) fprintf(out, "  [d3] staged %-22s %7.2f MB\n", name, (double)want / 1048576.0);
}
static void d3_stage(FILE *out) {
    char root[1200], base[1250], probe[1300];
    snprintf(root, sizeof root, "%s/Doom3Quest", kl_jni_files_dir());
    snprintf(base, sizeof base, "%s/base", root);
    d3_mkdirs(base);
    d3_copy_asset("pak399.pk4", base, out);
    d3_copy_asset("quest1_default.cfg", base, out);
    d3_copy_asset("quest2_default.cfg", base, out);
    d3_copy_asset("quest3_default.cfg", base, out);
    snprintf(probe, sizeof probe, "%s/pak000.pk4", base);
    int retail = d3_size(probe) > 0;
    if (!retail) {
        for (int i = 0; i < 4; i++) {
            char n[32];
            snprintf(n, sizeof n, "demo0%d.pk4", i);
            d3_copy_asset(n, base, out);
        }
    } else {
        // The full game has arrived beside a demo this function staged earlier.
        // The two must not share base/: the demo paks carry their own maps, GUIs
        // and declarations and the engine mounts every pk4 it finds. They are
        // copies of the APK's assets, so they are moved out of the search path
        // rather than kept — and moved, not deleted, so nothing here destroys a
        // file on the strength of a guess about which one the user wants.
        char aside[1250];
        snprintf(aside, sizeof aside, "%s/demo-unused", root);
        for (int i = 0; i < 4; i++) {
            char from[1300], to[1300];
            snprintf(from, sizeof from, "%s/demo0%d.pk4", base, i);
            if (d3_size(from) < 0) continue;
            d3_mkdirs(aside);
            snprintf(to, sizeof to, "%s/demo0%d.pk4", aside, i);
            if (rename(from, to) == 0 && out)
                fprintf(out, "  [d3] demo0%d.pk4 moved out of base/ (retail data present)\n", i);
        }
    }
    snprintf(probe, sizeof probe, "%s/commandline.txt", root);
    if (d3_size(probe) < 0) d3_copy_asset("commandline.txt", root, out);
    // Guest spellings, as the Activity sets them: the engine also hardcodes
    // /sdcard/Doom3Quest, and both go through the same external-storage redirect.
    setenv("USER_FILES", "/sdcard/Doom3Quest", 1);
    // "%s/libd3es_game.so" is dlopened from here — AFTER the engine has chdir'd
    // into its data directory, so a relative libdir (the host's
    // "doom3quest/lib/arm64-v8a") names nothing by then. Absolute when it can be.
    {
        char abs[1200];
        setenv("GAMELIBDIR", realpath(g_libdir, abs) ? abs : g_libdir, 1);
    }
    setenv("GAMETYPE", "16", 1);
    if (out) {
        fprintf(out, "  [d3] data: %s (%s)\n", base,
                retail ? "retail pak000.pk4 present" : "the bundled DEMO — no retail pak000.pk4");
        fflush(out);
    }
}

int kl_gles3jni_configure(const char *libdir, const char *entry_lib, FILE *out) {
    if (!libdir || !entry_lib || !*entry_lib) return g3_fail("no library directory / entry");
    snprintf(g_libdir, sizeof g_libdir, "%s", libdir);
    snprintf(g_entry,  sizeof g_entry,  "%s.so", entry_lib);
    kl_jni_set_native_lib_dir(libdir);   // nativeLibraryDir / dlopen-by-name resolve here
    g_d3 = strcmp(entry_lib, "libdoom3") == 0;
    if (g_d3) d3_stage(out);
    kl_engine_setenv(libdir, entry_lib, out);   // XASH3D_* the engine getenv's (was strdup(NULL))
    if (out) { fprintf(out, "  [g3] %s / %s\n", g_libdir, g_entry); fflush(out); }
    return kl_native_configure(libdir, entry_lib, out) == 0 ? 0 : g3_fail(kl_native_error());
}

typedef long long g3_jlong;
typedef g3_jlong (*g3_fn_create)(void *env, void *cls, void *activity, void *cmdline);
typedef void (*g3_fn_h)(void *env, void *cls, g3_jlong h);
typedef void (*g3_fn_ho)(void *env, void *cls, g3_jlong h, void *obj);

static void *g3_native(const char *name) {
    if (!g_engine || !g_prefix[0]) return NULL;
    char sym[256];
    snprintf(sym, sizeof sym, "%s%s", g_prefix, name);
    return kl_sym(g_engine, sym);
}

// Find the Java_com_drbeef_<pkg>_GLES3JNILib_ prefix the engine actually exports,
// by probing onCreate under each known Team Beef package token. Keeps the driver
// independent of which title this is.
static int g3_detect_prefix(FILE *out) {
    static const char *const PKGS[] = { "lambda1vr", "jkxr", "doom3quest" };
    for (size_t i = 0; i < sizeof PKGS / sizeof *PKGS; i++) {
        char sym[256];
        snprintf(sym, sizeof sym, "Java_com_drbeef_%s_GLES3JNILib_onCreate", PKGS[i]);
        if (kl_sym(g_engine, sym)) {
            snprintf(g_prefix, sizeof g_prefix, "Java_com_drbeef_%s_GLES3JNILib_", PKGS[i]);
            // The Activity the engine keeps a global ref to and calls back into
            // (haptics, keyboard) — its own package's GLES3JNIActivity.
            // static: kl_jni_set_activity_class keeps the POINTER. As a stack
            // buffer this dangled the moment the function returned, and the
            // engine's later GetMethodID on the activity read an empty class
            // name (".shutdown()V" — no binding could match it).
            static char act[128];
            snprintf(act, sizeof act, "com/drbeef/%s/GLES3JNIActivity", PKGS[i]);
            kl_jni_set_activity_class(act);
            if (out) { fprintf(out, "  [g3] GLES3JNILib package: com.drbeef.%s\n", PKGS[i]); fflush(out); }
            return 0;
        }
    }
    return g3_fail("no Java_com_drbeef_*_GLES3JNILib_onCreate export found");
}

int kl_gles3jni_load(FILE *out) {
    if (kl_native_load(out) != 0) return g3_fail(kl_native_error());   // maps graph + engine JNI_OnLoad
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_libdir, g_entry);
    g_engine = kl_find_image(path);
    if (!g_engine) return g3_fail("the engine library is not in the image registry");
    return g3_detect_prefix(out);   // sets g_prefix and the activity class
}

unsigned kl_gles3jni_gap(FILE *out) { return kl_native_gap(out); }

#define G3_CALL(out, name, type, ...)                                          \
    do {                                                                        \
        type fn__ = (type)g3_native(name);                                       \
        if (!fn__) { if (out) fprintf(out, "  [g3] %s — not exported, skipped\n", name); } \
        else { if (out) { fprintf(out, "  [g3] %s\n", name); fflush(out); }      \
               fn__(kl_jni_env(), NULL, ##__VA_ARGS__); }                        \
    } while (0)

// The android.view.Surface the Activity hands over — one object for the run; the
// guest turns it into the runtime's single synthetic window via ANativeWindow_fromSurface.
static void *g3_surface(void) {
    static void *s;
    if (!s) s = kl_jni_new_object("android/view/Surface");
    return s;
}

typedef void (*g3_fn_create_d3)(void *env, void *cls, void *activity, void *cmdline,
                                void *manufacturer);
typedef void (*g3_fn_o)(void *env, void *cls, void *obj);

// Doom3Quest's onCreate. The command line is commandline.txt's lines joined
// with a trailing space each — the Activity's own loop — and the third
// argument is Build.MANUFACTURER, which the engine uses to pick its
// quest<N>_default.cfg. KL_GLES3JNI_ARGS appends.
static int d3_create(FILE *out) {
    g3_fn_create_d3 create = (g3_fn_create_d3)g3_native("onCreate");
    if (!create) return g3_fail("the engine exports no GLES3JNILib_onCreate");
    char path[1300], cmd[1024] = "";
    snprintf(path, sizeof path, "%s/Doom3Quest/commandline.txt", kl_jni_files_dir());
    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            size_t n = strlen(cmd);
            snprintf(cmd + n, sizeof cmd - n, "%s ", line);
        }
        fclose(f);
    }
    if (!cmd[0]) snprintf(cmd, sizeof cmd, "doom3quest ");
    // One render thread, by default. The port's two-thread renderer shares its
    // image queues between front end and back end without holding its lock on
    // the removing side (idImageManager::GetNextAllocImage against AddAllocList,
    // whose Append reallocates), and writes into a buffer the other thread has
    // just freed. Android does not notice; Darwin's allocator stops the process
    // with "memory corruption of free block" — fourteen crash reports on device,
    // all at the first frames of a level started from the menu. r_multithread 0
    // is the engine's own switch for running the back end on the calling thread,
    // which removes the second party. KL_D3_MULTITHREAD=1 gives the two-thread
    // renderer back (faster, and it crashes).
    if (!kl_env_on("KL_D3_MULTITHREAD", 0)) {
        size_t n = strlen(cmd);
        snprintf(cmd + n, sizeof cmd - n, "+set r_multithread 0 ");
    }
    const char *extra = kl_env_str("KL_GLES3JNI_ARGS", "");
    if (*extra) { size_t n = strlen(cmd); snprintf(cmd + n, sizeof cmd - n, "%s", extra); }
    const char *make = kl_jni_build_string("MANUFACTURER");
    if (out) { fprintf(out, "  [g3] onCreate(\"%s\", \"%s\")\n", cmd, make ? make : ""); fflush(out); }
    kl_jni_local_frame_push();
    create(kl_jni_env(), NULL, kl_jni_activity(), kl_jni_new_string(cmd),
           kl_jni_new_string(make ? make : ""));
    kl_jni_local_frame_pop();
    g_handle = 1;      // there is none; non-zero is "created" to the rest of this file
    return 0;
}

int kl_gles3jni_create(FILE *out) {
    if (!g_engine) return g3_fail("the engine was never loaded");
    // These engines call back into their Activity from threads of their own and
    // leave the strings they pass behind (haptics, every frame).
    kl_jni_reclaim_frameless_strings(kl_env_on("KL_JNI_RECLAIM_STRINGS", 1));
    if (g_d3) return d3_create(out);
    g3_fn_create create = (g3_fn_create)g3_native("onCreate");
    if (!create) return g3_fail("the engine exports no GLES3JNILib_onCreate");
    // Command line: the engine tokenizes this string into argv and also reads
    // commandline.txt itself, so args here are supplementary. It must NOT be empty:
    // lambda1vr's onCreate guards its FIRST strdup against an empty/NULL commandline
    // (leaving the pointer NULL) but then strdup()s that pointer a second time
    // UNGUARDED — an empty string segfaults in strdup->strlen(NULL). Hand it a
    // benign argv[0] program name (the engine ignores argv[0]) so the token is
    // never empty; KL_GLES3JNI_ARGS overrides for real args.
    const char *extra = kl_env_str("KL_GLES3JNI_ARGS", "xash3d");
    // The engine selects its game from the argv `-game <mod>` parm; XASH3D_GAMEDIR
    // env alone was not enough (it kept falling back to valve). commandline.txt
    // holds the real `-game HL_Gold_HD`, but the guest doesn't tokenize it into the
    // argv the game-selection reads — so pass it here. Only when the driver resolved
    // a non-valve gamedir (kl_native.c's -game parse -> XASH3D_GAMEDIR) and the
    // caller hasn't already put a -game in KL_GLES3JNI_ARGS.
    char argbuf[512];
    const char *gd = getenv("XASH3D_GAMEDIR");
    if (gd && *gd && strcmp(gd, "valve") != 0 && !strstr(extra, "-game")) {
        snprintf(argbuf, sizeof argbuf, "%s -game %s", extra, gd);
        extra = argbuf;
    }
    if (out) { fprintf(out, "  [g3] onCreate(\"%s\")\n", extra); fflush(out); }
    kl_jni_local_frame_push();
    g_handle = create(kl_jni_env(), NULL, kl_jni_activity(), kl_jni_new_string(extra));
    kl_jni_local_frame_pop();
    if (out) { fprintf(out, "  [g3] handle 0x%llx\n", (unsigned long long)g_handle); fflush(out); }
    if (!g_handle) return g3_fail("GLES3JNILib_onCreate returned a zero handle");
    return 0;
}

void kl_gles3jni_start(FILE *out) {
    if (!g_handle) return;
    if (g_d3) {
        G3_CALL(out, "onStart", g3_fn_o, kl_jni_activity());
        G3_CALL(out, "onSurfaceCreated", g3_fn_o, g3_surface());
        G3_CALL(out, "onSurfaceChanged", g3_fn_o, g3_surface());
        return;
    }
    G3_CALL(out, "onStart", g3_fn_ho, g_handle, kl_jni_activity());
    G3_CALL(out, "onResume", g3_fn_h, g_handle);
    G3_CALL(out, "onSurfaceCreated", g3_fn_ho, g_handle, g3_surface());
    G3_CALL(out, "onSurfaceChanged", g3_fn_ho, g_handle, g3_surface());
}

double kl_gles3jni_pump(double seconds, const volatile int *quit) {
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    double elapsed = 0;
    for (unsigned t = 0; (quit ? !*quit : 1) && (seconds < 0 || elapsed < seconds); t++) {
        kl_ndk_pump_looper(100);
        if ((t + 1) % 10 == 0) kl_jni_drain_ui_tasks();
        if ((t + 1) % 20 == 0) kl_xash_tail_enginelog();   // stream engine.log -> our log
        kl_jni_tick_choreographer();
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - t0.tv_sec) + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
    }
    return elapsed;
}

// Inject a console command string into the running Xash engine via its exported
// Cbuf_AddText (the command buffer, executed next server frame). Used to drive the
// guest from Klepton — e.g. the two-grip gesture that kicks the stuck intro-guard
// sequence. No-op for any guest that isn't the Xash engine (Cbuf_AddText absent).
void kl_xash_console_cmd(const char *cmd) {
    if (!g_engine || !cmd || !*cmd) return;
    static void (*addtext)(const char *) = NULL;
    static int resolved = 0;
    if (!resolved) { resolved = 1; addtext = (void (*)(const char *))kl_sym(g_engine, "Cbuf_AddText"); }
    if (addtext) { addtext(cmd); fprintf(stderr, "  [xash] console: %s", cmd); }
}

void kl_gles3jni_report(FILE *out) {
    if (!out) return;
    kl_openxr_report(out);
    kl_egl_report(out);
    fprintf(out, "\n=== JNI surface ===\n");
    kl_jni_report(out);
    fflush(out);
}
