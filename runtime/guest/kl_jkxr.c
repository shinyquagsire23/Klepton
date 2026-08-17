// The JKXR target. See kl_jkxr.h for why it is its own file.
#include "kl_jkxr.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "klepton.h"
#include "kl_jni.h"
#include "kl_ndk.h"
#include "kl_egl.h"
#include "kl_openxr.h"
#include "kl_opensl.h"

// The Java front door. A plain Activity — not a NativeActivity — and the guest
// asks for it by name, so this is the class the JNI surface has to describe.
#define JKXR_ACTIVITY "com/drbeef/jkxr/GLES3JNIActivity"

// The natives are declared on a SEPARATE class from the Activity that calls
// them, and exported statically rather than registered, so the symbol is the
// whole binding.
#define JKXR_NATIVE_PREFIX "Java_com_drbeef_jkxr_GLES3JNILib_"

// GL 1.x over GLES. The Quake 3 renderer is fixed-function, so the port carries
// a translator and every gl* call the engine makes lands in gl4es before it
// reaches ours. It is the engine's only non-system DT_NEEDED and has to be
// mapped first: `initialize_gl4es` is bound at RELOCATION time.
#define JKXR_GL4ES "libgl4es.so"

// One row per game. The token is the entry library's suffix and everything else
// here follows from it; the DATA DIRECTORY is the one field that could not be
// derived, because it is a name each engine has baked in rather than built
// (`strings libopenjk_ja.so | grep /sdcard` answers `/sdcard/JKXR/JK3`, and the
// Outcast build answers JK2). Getting it wrong stages the pk3s into a directory
// the engine never reads and produces no error anywhere — the engine reports
// missing game data the way Quake 3 always has, by refusing to start, and says
// nothing about where it looked.
//
// `vr_pk3` is what the Activity copies out of the APK for this game; `weapons`
// and the shared pair are the ones it only copies when the user has not left a
// `no_copy` marker in the directory, which is how JKXR lets someone keep their
// own mods. Both halves are transcribed from GLES3JNIActivity.create().
typedef struct {
    const char *token;      // "ja"
    const char *dir;        // "JK3"
    const char *vr_pk3;     // the game's own VR assets
    const char *weapons;    // the weapon-model pack, skipped if `no_copy`
    const char *extra;      // one game gets a shader pk3 the other does not
} jkxr_game;

static const jkxr_game GAMES[] = {
    { "ja", "JK3", "z_vr_assets_jka.pk3", "z_vr_weapons_jka_Crusty_and_Elin.pk3", NULL },
    { "jo", "JK2", "z_vr_assets_jko.pk3", "z_vr_weapons_jko_Crusty_and_Elin.pk3",
      "assets6_vr_weapons_shaders.pk3" },
};

// Copied for both games, and named here rather than in the rows because a
// constant that appears in two rows is a constant that goes stale in one.
#define JKXR_SHARED_PK3    "z_vr_assets_base.pk3"
#define JKXR_MODS_PK3      "GGDynamicWeapons.pk3"
#define JKXR_MODS_CREDITS  "packaged_mods_credits.txt"

static const jkxr_game *g_game;
static char       g_libdir[1024];
static char       g_engine_lib[128];
static char       g_err[512] = "no error";
static kl_image  *g_engine;
static long long  g_handle;
static char       g_basedir[1024];   // <external>/JKXR/<JK2|JK3>/base

static int jkxr_fail(const char *why) {
    snprintf(g_err, sizeof g_err, "%s", why);
    return 1;
}

const char *kl_jkxr_error(void) { return g_err; }

// A native of the guest's, by exported symbol — the same door kl_ue4 opens for
// GameActivity's Java, and for the same reason: the JNI surface does not know
// which image is the guest, and this file does.
static void *jkxr_native_symbol(const char *symbol) {
    return (g_engine && symbol) ? kl_sym(g_engine, symbol) : NULL;
}

// ---- staging ----
//
// The whole of this section stands in for GLES3JNIActivity.create(), which on
// Android runs BEFORE the first native call and copies the port's own pk3s out
// of the APK into external storage. It is not optional and it does not fail by
// name: the engine looks in one directory, and an empty one reads to it exactly
// like a missing installation.

// The same walk klj_mkdir_p does inside the JNI surface. Not shared, because
// kl_jni_int.h is the jni/ directory's own seam and a guest door is not one of
// its families; six lines is the cheaper of the two couplings.
static void jkxr_mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static int jkxr_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// `overwrite` is the Activity's own third argument to copy_asset: the VR pk3s
// are refreshed every launch (they belong to the port and a stale one is a bug),
// the mod packs are copied only when absent (they belong to the user).
static int jkxr_copy_asset(const char *name, int overwrite, FILE *out) {
    char src[1200], dst[1200];
    snprintf(src, sizeof src, "%s/%s", kl_jni_assets_dir(), name);
    snprintf(dst, sizeof dst, "%s/%s", g_basedir, name);
    if (!overwrite && jkxr_exists(dst)) return 0;

    FILE *in = fopen(src, "rb");
    if (!in) {
        if (out) fprintf(out, "  [jkxr] %s — not in the APK's assets, skipped\n", name);
        return 0;
    }
    FILE *o = fopen(dst, "wb");
    if (!o) {
        fclose(in);
        if (out) fprintf(out, "  [jkxr] %s — cannot write %s: %s\n",
                         name, dst, strerror(errno));
        return 1;
    }
    char buf[65536];
    size_t got, total = 0;
    while ((got = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, got, o) != got) { total = 0; break; }
        total += got;
    }
    fclose(in);
    fclose(o);
    if (out) fprintf(out, "  [jkxr] staged %-40s %6.2f MB\n", name, total / 1048576.0);
    return 0;
}

// The retail game data, linked in from an existing install.
//
// It is not in the APK and it is not an OBB: JKXR is an engine port, so the
// user supplies assets0.pk3 and up from a copy of the game they own. KL_JKXR_DATA
// names the directory holding them — an OpenJK `base` directory, a mounted
// install, anywhere — and this runs ONCE per userdata: the links persist, so
// later runs need no knob.
//
// SYMLINKS rather than copies, and only for the `assets*.pk3` set. The retail
// pk3s are a gigabyte or more and are opened read-only by the engine, so a copy
// buys nothing and costs the disk twice; and linking only the assets set leaves
// the user's own mods, configs and maps where they are rather than importing a
// directory this file has no business interpreting.
//
// A link that cannot be made is COPIED rather than skipped: the two are the
// same file to the engine, and a partial stage would be a directory that looks
// staged and is not.
static void jkxr_link_retail(const char *from, FILE *out) {
    DIR *d = opendir(from);
    if (!d) {
        if (out) fprintf(out, "  [jkxr] KL_JKXR_DATA=%s: %s\n", from, strerror(errno));
        return;
    }
    struct dirent *e;
    unsigned linked = 0, copied = 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "assets", 6) != 0) continue;
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".pk3") != 0) continue;
        char src[1200], dst[1200];
        snprintf(src, sizeof src, "%s/%s", from, e->d_name);
        snprintf(dst, sizeof dst, "%s/%s", g_basedir, e->d_name);
        if (jkxr_exists(dst)) continue;
        if (symlink(src, dst) == 0) { linked++; continue; }
        FILE *in = fopen(src, "rb"), *o = fopen(dst, "wb");
        if (in && o) {
            char buf[65536];
            size_t got;
            while ((got = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, got, o);
            copied++;
        } else if (out) {
            fprintf(out, "  [jkxr] %s: %s\n", e->d_name, strerror(errno));
        }
        if (in) fclose(in);
        if (o) fclose(o);
    }
    closedir(d);
    if (out && (linked || copied))
        fprintf(out, "  [jkxr] retail data: %u linked, %u copied, from %s\n",
                linked, copied, from);
}

// What the user supplies and this project cannot: the retail pk3s. Reported by
// count and by name, because the alternative is the engine's own silence — it
// refuses to start with a message about a missing `assets0.pk3` that never
// mentions the directory it looked in, and on the first run of a new target
// that is indistinguishable from the shim being broken.
static void jkxr_census_retail(FILE *out) {
    if (!out) return;
    DIR *d = opendir(g_basedir);
    unsigned retail = 0;
    double bytes = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "assets", 6) != 0) continue;
            const char *dot = strrchr(e->d_name, '.');
            if (!dot || strcmp(dot, ".pk3") != 0) continue;
            char p[1200];
            snprintf(p, sizeof p, "%s/%s", g_basedir, e->d_name);
            struct stat st;
            if (stat(p, &st) != 0) continue;
            retail++;
            bytes += (double)st.st_size;
            fprintf(out, "  [jkxr] retail %-24s %7.1f MB\n", e->d_name,
                    st.st_size / 1048576.0);
        }
        closedir(d);
    }
    if (retail)
        fprintf(out, "  [jkxr] %u retail pk3%s, %.1f MB, in %s\n",
                retail, retail == 1 ? "" : "s", bytes / 1048576.0, g_basedir);
    else
        fprintf(out, "  [jkxr] NO RETAIL GAME DATA in %s — this is an engine port "
                     "and the game's own assets*.pk3 are not in the APK. The engine "
                     "will refuse to start and will not say where it looked. "
                     "Set KL_JKXR_DATA to a directory holding them (once — the "
                     "links persist).\n",
                g_basedir);
}

// The file the Java reads to decide which engine to load, and the string it
// then hands to the native onCreate as the command line. We already know which
// engine this run is — it is the target row — so writing the file is not how
// the choice is made here. It is written anyway because the ENGINE parses the
// same text as its command line (`Sys_ParseArgs`), and because leaving a file
// from the other game's run behind is exactly the shape of trap 50: a stale
// line in userdata silently configuring every run after it.
static void jkxr_write_command_line(FILE *out) {
    char path[1200];
    snprintf(path, sizeof path, "%s/JKXR/commandline.txt", kl_jni_files_dir());
    const char *extra = kl_env_str("KL_JKXR_ARGS", NULL);

    FILE *f = fopen(path, "wb");
    if (!f) {
        if (out) fprintf(out, "  [jkxr] cannot write %s: %s\n", path, strerror(errno));
        return;
    }
    fprintf(f, "%s%s%s\n", g_game->token, extra ? " " : "", extra ? extra : "");
    fclose(f);
    if (out)
        fprintf(out, "  [jkxr] command line: \"%s%s%s\"  (%s)\n", g_game->token,
                extra ? " " : "", extra ? extra : "", path);
}

static void jkxr_stage(FILE *out) {
    snprintf(g_basedir, sizeof g_basedir, "%s/JKXR/%s/base",
             kl_jni_files_dir(), g_game->dir);
    jkxr_mkdirs(g_basedir);

    // The user's `no_copy` marker, honoured exactly as the Activity honours it:
    // the port's own VR assets are refreshed regardless, the mod packs are not
    // touched at all.
    char marker[1200];
    snprintf(marker, sizeof marker, "%s/no_copy", g_basedir);
    int mods = !jkxr_exists(marker);

    jkxr_copy_asset(JKXR_SHARED_PK3, 1, out);
    jkxr_copy_asset(g_game->vr_pk3, 1, out);
    if (g_game->extra) jkxr_copy_asset(g_game->extra, 1, out);
    if (mods) {
        jkxr_copy_asset(JKXR_MODS_CREDITS, 0, out);
        jkxr_copy_asset(JKXR_MODS_PK3, 0, out);
        jkxr_copy_asset(g_game->weapons, 1, out);
    } else if (out) {
        fprintf(out, "  [jkxr] no_copy present — the mod packs are the user's\n");
    }

    const char *data = kl_env_str("KL_JKXR_DATA", NULL);
    if (data && *data) jkxr_link_retail(data, out);

    jkxr_write_command_line(out);
    jkxr_census_retail(out);
}

// ---- configure ----

int kl_jkxr_configure(const char *libdir, const char *entry_lib, FILE *out) {
    if (!libdir || !*libdir) return jkxr_fail("no library directory");
    if (!entry_lib || !*entry_lib) return jkxr_fail("no entry library");
    // ABSOLUTE, and this one is load-bearing rather than tidy. The host passes
    // a repo-relative libdir, and this guest CHANGES DIRECTORY: its thread entry
    // chdirs into the game's data directory so that the id Tech 3 filesystem can
    // take the cwd as fs_basepath. Every relative path handed to it before that
    // point stops resolving at exactly that moment — and the one that matters is
    // JK_LIBDIR, which the engine dlopens its renderer out of, several thousand
    // file operations later, reporting only that the renderer is missing.
    if (!realpath(libdir, g_libdir)) snprintf(g_libdir, sizeof g_libdir, "%s", libdir);

    // Which game, from the one token the two target rows differ in.
    const char *token = strrchr(entry_lib, '_');
    token = token ? token + 1 : entry_lib;
    for (size_t i = 0; i < sizeof GAMES / sizeof *GAMES; i++)
        if (!strcmp(GAMES[i].token, token)) g_game = &GAMES[i];
    if (!g_game) {
        snprintf(g_err, sizeof g_err,
                 "entry library %s names no game this file knows — the token "
                 "after the underscore must be one of \"ja\" or \"jo\"", entry_lib);
        return 1;
    }
    snprintf(g_engine_lib, sizeof g_engine_lib, "%s.so", entry_lib);

    kl_jni_set_activity_class(JKXR_ACTIVITY);
    kl_jni_set_guest_native_resolver(jkxr_native_symbol);

    // The two environment variables the Activity sets before loading anything.
    //
    // JK_LIBDIR is where the engine dlopens its renderer and its game module
    // from — it builds `<JK_LIBDIR>/lib<name>` and loads librd-gles-<game>_arm.so
    // and lib<game>gamearm.so — and on Android it is
    // ApplicationInfo.nativeLibraryDir, which here is the unpacked tree's lib
    // directory. Unset, the engine finds no renderer and exits before drawing.
    //
    // OPENXR_HMD is the manufacturer token, and the Activity derives it rather
    // than hardcoding it: Build.MANUFACTURER lowercased, with "oculus" rewritten
    // to "meta". Read through kl_jni's Build table so this guest is told the
    // same device every other one is (kl_jni.c's g_fields is the authority; we
    // present a Quest).
    setenv("JK_LIBDIR", g_libdir, 1);
    const char *make = kl_jni_build_string("MANUFACTURER");
    char hmd[64];
    size_t n = 0;
    for (const char *p = make ? make : ""; *p && n + 1 < sizeof hmd; p++)
        hmd[n++] = (char)((*p >= 'A' && *p <= 'Z') ? *p - 'A' + 'a' : *p);
    hmd[n] = 0;
    if (strstr(hmd, "oculus")) snprintf(hmd, sizeof hmd, "meta");
    setenv("OPENXR_HMD", hmd, 1);

    if (out) {
        fprintf(out, "  [jkxr] game:     %s (%s), engine %s\n",
                g_game->token, g_game->dir, g_engine_lib);
        fprintf(out, "  [jkxr] activity: %s\n", JKXR_ACTIVITY);
        fprintf(out, "  [jkxr] userdata: %s\n", kl_jni_files_dir());
        fprintf(out, "  [jkxr] JK_LIBDIR=%s  OPENXR_HMD=%s\n", g_libdir, hmd);
        fflush(out);
    }
    jkxr_stage(out);
    if (out) fflush(out);
    return 0;
}

// ---- load ----

int kl_jkxr_load(FILE *out) {
    const char *chain[2] = { JKXR_GL4ES, g_engine_lib };

    if (out) {
        fprintf(out, "=== the chain (2 libraries, dependencies first) ===\n");
        fflush(out);
    }
    // Mapped and relocated in order, then initialised in the same order rather
    // than interleaved — the engine's static initializers reach into gl4es.
    //
    // The renderer (librd-gles-<game>_arm.so) and the game module
    // (lib<game>gamearm.so) are deliberately NOT here: the engine dlopens both
    // by path out of JK_LIBDIR when it starts, the way Quake 3 has always
    // loaded its renderer, and kl_load_auto resolves them then.
    for (size_t i = 0; i < 2; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", g_libdir, chain[i]);
        kl_image *img = kl_load_auto(path);
        if (!img) {
            snprintf(g_err, sizeof g_err, "%s: %s", chain[i], kl_error());
            return 1;
        }
        kl_register_image(chain[i], img);
        if (i == 1) g_engine = img;
        // gl4es IS this target's GL, and the shim has to know by name — the
        // renderer's fixed-function stream and its ordinary draws must reach
        // the same implementation or nothing is ever drawn with a program
        // bound. See kl_shim_set_guest_gl.
        if (!strcmp(chain[i], JKXR_GL4ES)) kl_shim_set_guest_gl(img);
        // In the shape tools/symbolize_sample.py parses. This guest owns its
        // own render thread, so `sample <pid>` is the instrument for "what is
        // it doing", and without the load address every guest frame in it reads
        // as `??? (in <unknown binary>)`.
        if (out) {
            fprintf(out, "  mapped %-22s @%p %7.2f MB\n", chain[i],
                    kl_base(img), kl_span(img) / 1048576.0);
            fflush(out);
        }
    }
    for (size_t i = 0; i < 2; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", g_libdir, chain[i]);
        kl_image *img = kl_find_image(path);
        if (!img) continue;
        if (out) { fprintf(out, "  init %s\n", chain[i]); fflush(out); }
        kl_run_init(img);
    }

    // JNI_OnLoad. Both engines export it, and on Android System.loadLibrary
    // calls it before the Activity reaches any of the GLES3JNILib natives — but
    // it registers nothing here (the natives are static exports), so it is
    // reported and not required.
    typedef int (*jni_onload_fn)(void *vm, void *reserved);
    jni_onload_fn onload = (jni_onload_fn)kl_sym(g_engine, "JNI_OnLoad");
    if (!onload) {
        if (out) fprintf(out, "  no JNI_OnLoad — the exported natives are the entry\n");
        return 0;
    }
    kl_jni_local_frame_push();
    int version = onload(kl_jni_vm(), NULL);
    kl_jni_local_frame_pop();
    if (out) { fprintf(out, "  JNI_OnLoad returned 0x%08x\n", version); fflush(out); }
    return 0;
}

unsigned kl_jkxr_gap(FILE *out) {
    if (!g_engine) return 0;
    unsigned n = 0;
    const char *const *miss = kl_missing_imports(g_engine, &n);
    if (!out) return n;
    fprintf(out, "  %s: %u unique unresolved import%s\n",
            g_engine_lib, n, n == 1 ? "" : "s");
    for (unsigned i = 0; i < n; i++) {
        if (i % 4 == 0) fprintf(out, "      ");
        fprintf(out, "%-28s", miss[i]);
        if (i % 4 == 3 || i + 1 == n) fprintf(out, "\n");
    }
    fflush(out);
    return n;
}

// ---- GLES3JNIActivity's own half of the boot ----
//
// The nine natives, in the Activity's order. A `jlong` handle threads through
// all of them: onCreate builds the engine and returns it, and every call after
// that is keyed on it — a zero handle is how the Activity itself decides a
// lifecycle callback has nothing to deliver to yet.

typedef long long jkxr_jlong;
typedef jkxr_jlong (*jkxr_fn_create)(void *env, void *cls, void *activity, void *cmdline);
typedef void (*jkxr_fn_h)(void *env, void *cls, jkxr_jlong h);
typedef void (*jkxr_fn_ho)(void *env, void *cls, jkxr_jlong h, void *obj);

static void *jkxr_native(const char *name) {
    if (!g_engine) return NULL;
    char sym[256];
    snprintf(sym, sizeof sym, JKXR_NATIVE_PREFIX "%s", name);
    return kl_sym(g_engine, sym);
}

// Named either way, like kl_ue4's transcription of GameActivity: a native
// present and never called is the failure that produces no message at all.
#define JKXR_CALL(out, name, type, ...)                                          \
    do {                                                                          \
        type fn__ = (type)jkxr_native(name);                                       \
        if (!fn__) {                                                               \
            if (out) fprintf(out, "  [jkxr] %s — not exported, skipped\n", name);   \
        } else {                                                                   \
            if (out) { fprintf(out, "  [jkxr] %s\n", name); fflush(out); }          \
            fn__(kl_jni_env(), NULL, ##__VA_ARGS__);                               \
        }                                                                           \
    } while (0)

// The android.view.Surface the Activity hands over from its SurfaceHolder.
//
// One object, held for the whole run, because the guest is entitled to compare
// the one it was given in surfaceCreated with the one in surfaceChanged. What
// it does with it is call ANativeWindow_fromSurface, which answers the
// runtime's single synthetic window regardless of the object — so the object's
// job is to exist and to survive, not to carry geometry.
static void *jkxr_surface(void) {
    static void *surface;
    if (!surface) surface = kl_jni_new_object("android/view/Surface");
    return surface;
}

int kl_jkxr_create(FILE *out) {
    if (!g_engine) return jkxr_fail("the engine was never loaded");
    jkxr_fn_create create = (jkxr_fn_create)jkxr_native("onCreate");
    if (!create) return jkxr_fail("the engine exports no GLES3JNILib_onCreate");

    // The command line the Activity read out of commandline.txt. It is the same
    // text jkxr_write_command_line just wrote, built from the same fields, so
    // the file on disk and the string the engine parses cannot disagree — the
    // file exists for the ENGINE's own re-reads, not as the channel for this.
    char cmdline[512];
    const char *extra = kl_env_str("KL_JKXR_ARGS", NULL);
    snprintf(cmdline, sizeof cmdline, "%s%s%s ", g_game->token,
             extra ? " " : "", extra ? extra : "");

    if (out) { fprintf(out, "  [jkxr] onCreate(\"%s\")\n", cmdline); fflush(out); }
    kl_jni_local_frame_push();
    g_handle = create(kl_jni_env(), NULL, kl_jni_activity(),
                      kl_jni_new_string(cmdline));
    kl_jni_local_frame_pop();
    if (out) { fprintf(out, "  [jkxr] handle 0x%llx\n",
                       (unsigned long long)g_handle); fflush(out); }
    // Zero is the Activity's own test for "there is nothing to drive" — it
    // guards every later callback with it — so a zero here is the engine
    // saying it did not build, not a handle that happens to be null.
    if (!g_handle) return jkxr_fail("GLES3JNILib_onCreate returned a zero handle");
    return 0;
}

void kl_jkxr_start(FILE *out) {
    if (!g_handle) return;
    // onStart takes the ACTIVITY as its second argument, not the surface: the
    // engine keeps a global reference to it and calls back into it for the
    // haptics service and the on-screen keyboard.
    JKXR_CALL(out, "onStart", jkxr_fn_ho, g_handle, kl_jni_activity());
    JKXR_CALL(out, "onResume", jkxr_fn_h, g_handle);
    // ...and the surface, which is where the engine stops waiting. Its render
    // thread has been running since onCreate and blocks until a window arrives;
    // surfaceChanged follows because on Android it always does, and the engine
    // reads its geometry there.
    JKXR_CALL(out, "onSurfaceCreated", jkxr_fn_ho, g_handle, jkxr_surface());
    JKXR_CALL(out, "onSurfaceChanged", jkxr_fn_ho, g_handle, jkxr_surface());
}

void kl_jkxr_stop(FILE *out) {
    if (!g_handle) return;
    JKXR_CALL(out, "onPause", jkxr_fn_h, g_handle);
    JKXR_CALL(out, "onSurfaceDestroyed", jkxr_fn_h, g_handle);
    JKXR_CALL(out, "onStop", jkxr_fn_h, g_handle);
}

double kl_jkxr_pump(double seconds, const volatile int *quit) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double elapsed = 0;
    for (unsigned t = 0; (quit ? !*quit : 1) && (seconds < 0 || elapsed < seconds); t++) {
        kl_ndk_pump_looper(100);
        // The UI thread's task queue and the frame clock, on the same thread
        // that turns the looper — which is what makes it the UI thread by the
        // only definition that matters.
        if ((t + 1) % 10 == 0) kl_jni_drain_ui_tasks();
        kl_jni_tick_choreographer();
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - t0.tv_sec)
                + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
    }
    return elapsed;
}

void kl_jkxr_report(FILE *out) {
    if (!out) return;
    // Who is blocked on what, first. This guest owns its own render thread, so
    // there is no return value anywhere that says "the engine stopped", and a
    // run that produced no frames looks exactly like one that produced frames
    // until the mutex owner map is read.
    kl_pthread_report(out);
    kl_egl_report(out);
    kl_openxr_report(out);
    kl_opensl_report(out);
    klj_jkxr_report(out);
    fprintf(out, "\n=== JNI surface ===\n");
    kl_jni_report(out);
    fflush(out);
}
