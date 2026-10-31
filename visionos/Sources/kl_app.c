// See kl_app.h.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <mach/mach.h>          // task_info(TASK_VM_INFO) — the heartbeat's footprint
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include "klepton.h"
#include "kl_jni.h"
#include "kl_fault.h"
#include "kl_egl.h"
#include "kl_driver.h"    // the guest's own sequences, shared with build/m_boot
#include "kl_ue4.h"       // kl_ue4_force_quit_obeyed
#include "kl_audio.h"     // kl_audio_set_latency_ms — streamed audio needs a deeper buffer
#include "kl_opensl.h"    // kl_opensl_heartbeat — the live audio census in the [hb] loop
#include "kl_ovrp.h"      // kl_ovrp_set_frame_pacer — the OVRPlugin frame clock
#include "kl_openxr.h"    // kl_openxr_set_frame_pacer — the OpenXR one
#include "kl_slink.h"
#include "kl_mono.h"      // the flat guest's window seam — frame out, pointer in
#include "kl_glfb.h"      // kl_glfb_release_current — the handoff hands the context on
#include "kl_x18.h"       // claim the veneers' TSD slot before anything else can
#include "kl_env.h"
#include "kl_metrics.h"   // the panel in the headset: its sample is taken on the heartbeat

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
// See kl_app.h: the target name even before configure sets g_target, so the
// launcher UI knows which game it is on the first render. Same source configure
// uses (KL_TARGET override, else the compile-time default).
const char *kl_app_target_name_or_default(void) {
    return g_target ? g_target->name : kl_env_str("KL_TARGET", KL_TARGET_DEFAULT);
}
int kl_app_target_is_steamlink(void) {
    return g_target && g_target->kind == KL_GUEST_STEAMLINK;
}
static int target_is_jkxr(void) {
    return g_target && g_target->kind == KL_GUEST_JKXR;
}
// A whole Linux process (kl_linuxproc): no APK, no assets, no Android at all.
static int target_is_linux(void) {
    return g_target && g_target->kind == KL_GUEST_LINUX;
}
int kl_app_target_wants_full(void) {
    // In-world VR titles render their MENU as a full stereo scene too, so .mixed
    // leaves it a flat window floating in passthrough (a jagged quad) until gameplay
    // starts. Default those to .full immersion from boot: the native OpenXR kind
    // (vicecity) and the folder-fed launcher games — hl1 (GLES3JNI) and
    // cs1/hl2/portal (SDL2). KL_FULL=0 forces .mixed back for development.
    // Skydance's BEHEMOTH (x5767635983306831) by name: its splash and level-
    // loading screens submit quads with no eye layer (KL_OVRP_EYELESS_FRAMES),
    // which a Quest shows over black and .mixed showed over the room.
    // Beat Saber from 1.45 on (kl_guest_beatsaber_openxr) as well: it renders
    // the menu and a level at two eye sizes and makes a new swapchain at every
    // change, and while it loads it submits nothing new — 17 to 50 compositor
    // frames with no picture at each transition, through which .mixed showed the
    // room (on device, 2026-10-12: "man sieht kurz das Passthrough"; black in
    // .full). 1.40 keeps one swapchain and stays as it was.
    extern int kl_guest_beatsaber_openxr(void);
    return g_target && (g_target->kind == KL_GUEST_NATIVE ||
                        g_target->kind == KL_GUEST_GLES3JNI ||
                        g_target->kind == KL_GUEST_LINUX ||
                        g_target->kind == KL_GUEST_SDL2 ||
                        !strcmp(g_target->name, "x5767635983306831") ||
                        kl_guest_beatsaber_openxr());
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
// The pipeline recorder's folder (runtime/gfx/kl_fossilize.cpp): a pipeline list
// that came with the game, what this device recorded beside it, and how the
// game makes its Vulkan device.
static char g_fozdir[1100];
static int  g_preparing;
const char *kl_app_assets_dir(void) { return g_assets[0] ? g_assets : NULL; }

// Where the run has got to, for the heartbeat below; the driver reports its own
// phases through it.
static void app_phase(const char *p);

const char *kl_app_log_path(void) { return g_log; }

// A cold start to look at, without losing the warm one.
//
// What makes a start "cold" is the system's own Metal cache in the app's
// container (Library/Caches/<bundle id>/com.apple.metal): with it, a shader the
// app compiled before is a lookup; without it every one is compiled again —
// minutes for a game with thousands of pipelines. Whether a game says so on
// its screen while that goes on can only be seen on a cold start, and a cold
// start is not something to give away for one look.
//
// So: a one-word file `klepton-shader-test.txt` in Documents (it can be put
// there from a Mac, `devicectl device copy to`). "cold" sets the cache aside
// as com.apple.metal.kept the first time and starts every run with an empty
// one; "restore" puts the kept one back and throws away what the test runs
// compiled. No file, or any other word: nothing is touched. Called from the
// App's init, before anything in this process has asked Metal for a shader —
// a cache the system has already opened is not replaced by renaming it.
static char g_shader_test_note[256];
static int shader_test_rm(const char *path) {
    DIR *d = opendir(path);
    if (!d) return unlink(path);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char sub[1200];
        if (snprintf(sub, sizeof sub, "%s/%s", path, de->d_name) < (int)sizeof sub) shader_test_rm(sub);
    }
    closedir(d);
    return rmdir(path);
}
void kl_app_shader_cache_test(void) {
    const char *home = getenv("HOME");
    char path[1024], word[16] = "";
    if (!home || snprintf(path, sizeof path, "%s/Documents/klepton-shader-test.txt", home) >= (int)sizeof path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fscanf(f, "%15s", word) != 1) word[0] = 0;
    fclose(f);
    int cold = !strcmp(word, "cold"), restore = !strcmp(word, "restore");
    if (!cold && !restore) return;
    char caches[1024];
    snprintf(caches, sizeof caches, "%s/Library/Caches", home);
    DIR *d = opendir(caches);
    if (!d) return;
    struct dirent *de;
    int n = 0;
    while ((de = readdir(d)) != NULL) {
        char live[1200], kept[1200];
        if (de->d_name[0] == '.' ||
            snprintf(live, sizeof live, "%s/%s/com.apple.metal", caches, de->d_name) >= (int)sizeof live ||
            snprintf(kept, sizeof kept, "%s.kept", live) >= (int)sizeof kept) continue;
        struct stat sl, sk;
        int has_live = stat(live, &sl) == 0, has_kept = stat(kept, &sk) == 0;
        if (cold && has_live) {
            if (!has_kept) { if (rename(live, kept) == 0) n++; }
            else           { if (shader_test_rm(live) == 0) n++; }
        } else if (restore && has_kept) {
            if (has_live) shader_test_rm(live);
            if (rename(kept, live) == 0) n++;
        }
    }
    closedir(d);
    // A cold start is meant to be cold where it is looked at: the replay of
    // the pipelines a device recorded (KL_VK_FOZ_REPLAY, Walkabout) would have
    // compiled them all again before the first frame. Recording goes on.
    if (cold) setenv("KL_VK_FOZ_REPLAY", "0", 1);
    snprintf(g_shader_test_note, sizeof g_shader_test_note,
             cold ? "klepton-shader-test.txt says cold — Metal's cache set aside or emptied (%d), this start compiles everything"
                  : "klepton-shader-test.txt says restore — the kept Metal cache is back (%d)", n);
}
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

// --- the shader cache outlives the driver, and must not -------------------
//
// A GLES guest that keeps a program binary cache (UE4's `ProgramBinaryCache`,
// Unity's shader cache) writes `glGetProgramBinary` blobs into its files
// directory and reads them back with `glProgramBinary` on the next launch.
// Those blobs are only valid for the driver that produced them — on Android
// the vendor blob cache is keyed on the driver build and thrown away when it
// changes. **Here there is no such key.** ANGLE hands the Metal shader compiler
// through, the binaries carry the OS build they were compiled under, and
// nothing on this platform invalidates them.
//
// The first launch after a visionOS update therefore reloads a cache into
// programs that `glProgramBinary` silently refuses to link. UE4 uses them
// anyway, every `glUseProgram` answers GL_INVALID_OPERATION, the RHI raises
// Fatal, and the guest asks to quit hundreds of times before its game thread
// crashes. Seen on Tetris Effect: "it crashes right after the Loading screen",
// i.e. at the first level whose shaders came from the cache.
//
// The fix is the key Android has and we did not: fingerprint the driver, store
// it beside the cache, and delete the cache when it changes. It lives here at
// the platform level rather than per-target, because every guest that persists
// compiled GL programs is one OS update away from the same crash.
//
// KL_SHADER_CACHE_GUARD=0 leaves the cache alone (to reproduce the failure).
#define KL_CACHE_STAMP ".klepton-gl-driver"

// What the guard decided, kept so kl_app_boot can repeat it into the boot log:
// the guard runs from kl_app_configure, before the log file exists, so its own
// line reaches only the console — and a Home View launch has none.
static char g_cache_guard_note[512];

static void rm_rf(const char *path, int depth) {
    if (depth > 8) return;
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(p, depth + 1);
        else unlink(p);
    }
    closedir(d);
    rmdir(path);
}

// The directory names guests use for compiled-program caches. Matched by name
// at any depth under the files directory, because each engine buries it
// differently (UE4 puts ProgramBinaryCache at the root, Unity puts its cache
// under a per-company subtree).
//
// Only the two that are known to hold glGetProgramBinary blobs. Upstream of
// this list also named ShaderCache, il2cpp_cache and GPUCache; a folder is
// deleted here on its name alone, so a name stays off the list until a guest
// has been seen keeping GL program binaries under it.
//
// The other half of this guard is KleptonProgramCaches (KleptonTuning.swift),
// which drops ProgramBinaryCache when the APP's build changes — a new ANGLE
// refuses binaries too. This one covers what that cannot see: the OS build.
static int is_cache_dir(const char *name) {
    static const char *const names[] = {
        "ProgramBinaryCache",   // UE4 FOpenGLProgramBinaryCache
        "UnityShaderCache",
        NULL,
    };
    for (int i = 0; names[i]; i++) if (!strcmp(name, names[i])) return 1;
    return 0;
}

static int purge_caches(const char *root, int depth) {
    if (depth > 6) return 0;
    DIR *d = opendir(root);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", root, e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (is_cache_dir(e->d_name)) {
            fprintf(stderr, "  [klepton] shader cache: dropping %s "
                            "(compiled under a different GL driver)\n", p);
            rm_rf(p, 0);
            n++;
        } else {
            n += purge_caches(p, depth + 1);
        }
    }
    closedir(d);
    return n;
}

// The fingerprint. `kern.osversion` is the OS BUILD, which is what moves when a
// beta lands and what the Metal compiler versions its output by; hw.model pins
// the GPU family so a restore onto different hardware counts as a change too;
// KL_GLFB_REWRITE_GEN is our own half of what the cache holds (the blobs are
// compiled from the text klfb_rewrite_glsl produced).
static void shader_cache_guard(const char *files, const char *target) {
    if (!kl_env_on("KL_SHADER_CACHE_GUARD", 1)) return;

    char want[256] = "", tmp[128];
    size_t n = sizeof tmp;
    if (sysctlbyname("kern.osversion", tmp, &n, NULL, 0) != 0) return;
    snprintf(want, sizeof want, "os=%s", tmp);
    n = sizeof tmp;
    if (sysctlbyname("hw.model", tmp, &n, NULL, 0) == 0) {
        size_t at = strlen(want);
        snprintf(want + at, sizeof want - at, " hw=%s", tmp);
    }
    {
        size_t at = strlen(want);
        snprintf(want + at, sizeof want - at, " glsl=%d", KL_GLFB_REWRITE_GEN);
    }
    // ...and single-pass multiview (the Array eye layer): Unreal's programs for
    // one cannot draw the other. Vader Immortal's blobs from its runs without
    // multiview would otherwise outlive the switch to it (2026-10-19).
    {
        extern int kl_ovrp_gl_array_for(const char *);
        if (kl_ovrp_gl_array_for(target)) {
            size_t at = strlen(want);
            snprintf(want + at, sizeof want - at, " mv=1");
        }
    }

    char stamp[1200];
    snprintf(stamp, sizeof stamp, "%s/%s", files, KL_CACHE_STAMP);

    char have_s[256] = "";
    FILE *f = fopen(stamp, "r");
    if (f) {
        if (!fgets(have_s, sizeof have_s, f)) have_s[0] = 0;
        fclose(f);
        for (char *p = have_s; *p; p++) if (*p == '\n') { *p = 0; break; }
    }
    if (!strcmp(have_s, want)) {          // same driver — the cache is valid
        snprintf(g_cache_guard_note, sizeof g_cache_guard_note,
                 "GL driver fingerprint unchanged (%s): compiled-program caches kept",
                 want);
        return;
    }

    // A fresh install has no stamp and no cache either, so the sweep is free;
    // it is only ever expensive when it is also necessary.
    int dropped = purge_caches(files, 0);
    snprintf(g_cache_guard_note, sizeof g_cache_guard_note,
             "GL driver fingerprint %s -> %s: %d compiled-program cache(s) dropped",
             have_s[0] ? have_s : "(none)", want, dropped);
    fprintf(stderr, "  [klepton] %s. A guest's glProgramBinary blobs do not "
                    "survive an OS update, and using them is a silent unlinked "
                    "program (KL_SHADER_CACHE_GUARD=0 keeps them).\n",
            g_cache_guard_note);

    f = fopen(stamp, "w");
    if (f) { fprintf(f, "%s\n", want); fclose(f); }
}

// Half-Life: Alyx keeps its video settings file (hlvr/cfg/deckard/video2.txt)
// only while the VendorID and DeviceID in it are the running GPU's; otherwise it
// replaces the whole file with its automatic setup at boot — the lowest of
// everything — and the detail settings the app put there (KleptonAlyx.swift)
// are gone, with the mark that they were set still standing. The device id
// changes with a system update (kl_vulkan_gpu_ids), so a file that was right
// yesterday is thrown away after one. At every start, then, the file gets the
// ids of the GPU it is about to be read on. Glovebox found this
// (github.com/glovebox-vr/glovebox, runtime/linux/kl_alyx_app.c, MIT: measured
// on a Mac, 2026-10-11); what is done here is its match_gpu.
// KL_ALYX_GPU_IDS=0 leaves the file alone.
int kl_vulkan_gpu_ids(unsigned *vendor, unsigned *device);
static char g_alyx_ids_note[160];

// The value of the first `"key"  "value"` line in a KeyValues text set to val.
// 1 changed, 0 it was that already, -1 there is no such line.
static int alyx_kv_set(char **text, const char *key, const char *val) {
    char q[64];
    snprintf(q, sizeof q, "\"%s\"", key);
    for (char *l = *text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        char *k = l;
        while (*k == ' ' || *k == '\t') k++;
        if (strncmp(k, q, strlen(q))) continue;
        char *eol = strchr(k, '\n');
        char *v = strchr(k + strlen(q), '"');
        char *e = v ? strchr(v + 1, '"') : NULL;
        if (!e || (eol && e > eol)) return -1;
        size_t old = (size_t)(e - v - 1), nv = strlen(val);
        if (old == nv && !strncmp(v + 1, val, nv)) return 0;
        size_t head = (size_t)(v + 1 - *text), tail = strlen(e);
        char *out = malloc(head + nv + tail + 1);
        if (!out) return -1;
        memcpy(out, *text, head);
        memcpy(out + head, val, nv);
        memcpy(out + head + nv, e, tail + 1);
        free(*text);
        *text = out;
        return 1;
    }
    return -1;
}

static void alyx_video_gpu_ids(const char *container) {
    if (!kl_env_on("KL_ALYX_GPU_IDS", 1)) {
        snprintf(g_alyx_ids_note, sizeof g_alyx_ids_note, "left alone (KL_ALYX_GPU_IDS=0)");
        return;
    }
    char path[1200], tmp[1300];
    snprintf(path, sizeof path, "%s/lx/game/hlvr/cfg/deckard/video2.txt", container);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(g_alyx_ids_note, sizeof g_alyx_ids_note, "no file yet"); return; }
    enum { kMax = 1 << 18 };            // the file is 2 KB
    char *text = malloc(kMax + 1);
    size_t n = text ? fread(text, 1, kMax, f) : 0;
    int whole = text && feof(f);
    fclose(f);
    if (!whole) { free(text); snprintf(g_alyx_ids_note, sizeof g_alyx_ids_note, "not read"); return; }
    text[n] = 0;
    unsigned id[2];
    if (!kl_vulkan_gpu_ids(&id[0], &id[1])) {
        free(text);
        snprintf(g_alyx_ids_note, sizeof g_alyx_ids_note, "GPU ids not known, file left alone");
        return;
    }
    static const char *const keys[] = { "VendorID", "DeviceID" };
    int changed = 0, missing = 0;
    for (int i = 0; i < 2; i++) {
        char want[24];
        snprintf(want, sizeof want, "%u", id[i]);
        int r = alyx_kv_set(&text, keys[i], want);
        if (r < 0) missing++; else changed += r;
    }
    const char *what = missing ? "the file has no id lines, left alone" : changed ? "put into the file" : "the file has them";
    if (changed && !missing) {
        snprintf(tmp, sizeof tmp, "%s.klnew", path);
        FILE *out = fopen(tmp, "wb");
        size_t len = strlen(text);
        int bad = !out || fwrite(text, 1, len, out) != len;
        if (out && fclose(out) != 0) bad = 1;
        if (bad || rename(tmp, path) != 0) { unlink(tmp); what = "NOT WRITTEN"; }
    }
    free(text);
    snprintf(g_alyx_ids_note, sizeof g_alyx_ids_note, "GPU ids %u/%u (%#x): %s", id[0], id[1], id[1], what);
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
    // A Linux process's staged tree is lx/, and the list comes with it
    // (visionos/stage_sync.py).
    snprintf(g_fozdir, sizeof g_fozdir, target_is_linux() ? "%s/lx/klfz" : "%s/klfz", container);
    setenv("KL_VK_FOZ_DIR", g_fozdir, 0);

    // Check before running, not after failing. A missing asset tree otherwise
    // surfaces three layers up inside Unity as something that reads like a shim
    // bug.
    // With translations embedded, g_libdir never has to exist: it is only the
    // string NativeLoader.load() is handed and the prefix the DT_NEEDED walk
    // builds paths from, and kl_load_auto turns "<libdir>/libunity.so" into
    // "Frameworks/libunity.framework/libunity" before touching the disk. The ELF
    // tree is therefore not in the bundle — 80 MB that would buy only an A/B
    // against the mmap loader the translations replace.
    //
    // A Linux process is neither: its libraries are frameworks named by content
    // (runtime/linux/kl_lx_name.h), found one at a time as its own ld.so opens
    // them, and what has to exist up front is the staged tree — which
    // kl_linuxproc_configure checks, by name, at boot.
    if (target_is_linux()) {
        char root[1200];
        snprintf(root, sizeof root, "%s/lx", container);
        setenv("KL_LX_ROOT", root, 0);
        setenv("KL_LX_DYLIBS", g_dylibs, 0);
        // <container>/lx/env.txt: KEY=VALUE lines, applied to this process's
        // environment. A launch from the Home View carries no environment at
        // all, so without this every setting a run.sh launch can pass (the
        // command line in KL_LX_ARGS, the frame budget, the eye texture
        // ceiling) is fixed at build time for exactly the launches a person
        // makes by hand. An explicit launch environment still wins.
        char envfile[1300];
        snprintf(envfile, sizeof envfile, "%s/env.txt", root);
        FILE *ef = fopen(envfile, "r");
        if (ef) {
            char line[1024];
            while (fgets(line, sizeof line, ef)) {
                char *nl = strpbrk(line, "\r\n"), *eq = strchr(line, '=');
                if (nl) *nl = 0;
                if (line[0] == '#' || !eq || eq == line) continue;
                *eq = 0;
                setenv(line, eq + 1, 0);
            }
            fclose(ef);
        }
    } else
    if (!have_translations() && !have(g_libdir))
        return missing("guest libraries (neither translations nor an ELF tree)", g_libdir);
    // Assets and the APK are required only for an engine that reads its data out
    // of the APK's assets/ tree (Unity, UE4, JKXR, …). The FOLDER-FED guests carry
    // no assets/ and no .apk: a plain NativeActivity port (vicecity), and the
    // Xash/Source launcher titles (hl1 = GLES3JNI, cs1/hl2/portal = SDL2) whose
    // whole data set is the game folder the PLAYER supplies through the launcher —
    // there is nothing to stage, so the guard would wrongly refuse to boot. The
    // path strings above are still set (harmless — those guests never resolve an
    // APK/asset path); only the existence check is skipped.
    int folder_fed = g_target->kind == KL_GUEST_NATIVE   ||
                     g_target->kind == KL_GUEST_GLES3JNI ||
                     g_target->kind == KL_GUEST_LINUX    ||
                     g_target->kind == KL_GUEST_SDL2;
    if (!folder_fed && !have(g_apk))
        return missing("staged APK (run stage_assets.sh)", g_apk);
    // An APK with no assets/ in it at all is a correct staging, not a missing
    // one: Vader Immortal I ships its UE4 data entirely in the OBB, apktool
    // unpacks no assets/ dir, and both stagers stage none. Refusing here failed
    // the launch before klepton-boot.log existed — no log, no crash report, a
    // game that simply did not start. The tree's apktool.yml beside it says the
    // unpack happened; an empty assets/ is then what that APK holds.
    if (!folder_fed && !have(g_assets)) {
        char yml[1200];
        snprintf(yml, sizeof yml, "%s/%s/apktool.yml", container, g_target->tree);
        if (!have(yml) || mkdir(g_assets, 0755) != 0)
            return missing("staged assets (run stage_assets.sh)", g_assets);
    }
    mkdir(g_files, 0755);
    // Before any guest code runs, and before the guest's own file layer can
    // hand it a stale blob. See shader_cache_guard.
    shader_cache_guard(g_files, g_target ? g_target->name : NULL);

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
        // The OBB layout is per-target and kl_jni must be TOLD it: on device
        // KL_TARGET is unset, so kl_jni's own fallback resolves the default
        // guest and hands a UE4 title the wrong OBB directory. g_target is the
        // real one this app was built for.
        kl_jni_set_obb_rel(g_target->obb);
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
    if (!target_is_linux() && have_translations()) setenv("KL_DYLIB_DIR", g_dylibs, 1);

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
    // After kl_driver_init, which puts the MoltenVK settings into the
    // environment: reading the GPU's ids makes MoltenVK's first instance.
    if (!strcmp(g_target->name, "alyx")) alyx_video_gpu_ids(container);

    // The two PCM captures (kl_audio.h, KL_AUDIO_DUMP / KL_AAUDIO_DUMP) are off
    // unless set. A bare filename resolves against the data container, which is
    // the one directory `devicectl device copy from` can read back — see
    // visionos/pull_audio.sh:
    //
    //   KL_AUDIO_DUMP=klepton-audio-out.wav KL_AAUDIO_DUMP=klepton-audio-guest.wav
    //
    // These two name a FILE, not a directory, so they get the container prefix
    // without the mkdir the loop above does — an mkdir here would create a
    // directory exactly where the file needs to be written.
    static const char *const cap_files[] = { "KL_AUDIO_DUMP", "KL_AAUDIO_DUMP" };
    for (size_t i = 0; i < sizeof cap_files / sizeof *cap_files; i++) {
        const char *v = getenv(cap_files[i]);
        if (!v || !*v || *v == '/') continue;
        char abs[1200];
        snprintf(abs, sizeof abs, "%s/%s", container, v);
        setenv(cap_files[i], abs, 1);
        printf("[app] %s is relative — resolved to %s\n", cap_files[i], abs);
    }

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
    // The run before this one keeps its log, once, as klepton-boot.prev.log: a
    // player whose game froze or quit starts it again before anyone asks for
    // the log, and that start would otherwise be all that is left. (After
    // Glovebox, github.com/glovebox-vr/glovebox, MIT.)
    size_t n = strlen(g_log);
    if (n > 4 && strcmp(g_log + n - 4, ".log") == 0) {
        char prev[sizeof g_log + 8];
        snprintf(prev, sizeof prev, "%.*s.prev.log", (int)(n - 4), g_log);
        (void)rename(g_log, prev);
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

// ---- how the game runs, in numbers ------------------------------------------
//
// Three counters that cost an increment each: the frames the game started
// (guest_pace_wait), the poses the compositor offered it (kl_app_guest_publish —
// one per display refresh the app was given), and the GPU time of the command
// buffers that carry the game's scene pass (kl_mtl_foveate.m). The heartbeat
// turns them once a minute into a [perf] line in the log: a short test run says
// little about a session, and what the system does to an app that has been warm
// for twenty minutes — halve its display rate, most of all — is not visible in
// the first two. (What the headset shows of a running game is kl_metrics.c,
// which counts where a picture is shown and so has a rate for every guest.)
static _Atomic unsigned long long g_perf_frames, g_perf_publishes;
static _Atomic int g_perf_thermal = -1;
double kl_mtl_scene_gpu_ms(void);     // kl_mtl_foveate.m: mean since the last call, <0 for none

void kl_app_note_thermal(int state) { g_perf_thermal = state; }

// The least memory the app had left before its limit while the game drew, in
// MB; -1 where the system does not say or the game never drew. The start
// window shows it beside the texture budget, which is what spends it.
static _Atomic long long g_perf_min_avail = -1;
long long kl_app_min_avail_mb(void) { return g_perf_min_avail; }

static double perf_seconds(void) {
    struct timespec m;
    clock_gettime(CLOCK_MONOTONIC, &m);
    return (double)m.tv_sec + (double)m.tv_nsec * 1e-9;
}

// Called four times a second from the heartbeat.
static void perf_tick(void) {
    static double t_sec, t_min, t_first;
    static unsigned long long f_min, p_min;
    static double gpu_sum; static int gpu_n;
    static int on = -1, every;
    if (on < 0) {
        on = kl_env_on("KL_PERF_LOG", 1);
        // KL_PERF_LOG_EVERY=<seconds>: a minute is the right grain for a
        // session and too coarse for a trial of three — the second minute is
        // then the only one that is all play.
        every = kl_env_int("KL_PERF_LOG_EVERY", 60);
        if (every < 5) every = 5;
    }
    unsigned long long f = g_perf_frames, pub = g_perf_publishes;
    if (!f) return;                                    // nothing of the game yet
    double t = perf_seconds();
    if (!t_first) { t_first = t_sec = t_min = t; f_min = f; p_min = pub; return; }
    if (t - t_sec >= 1.0) {
        double g = kl_mtl_scene_gpu_ms();
        if (g >= 0) { gpu_sum += g; gpu_n++; }
        long long left = kl_avail_memory();
        if (left > 0 && (g_perf_min_avail < 0 || (left >> 20) < g_perf_min_avail)) g_perf_min_avail = left >> 20;
        t_sec = t;
    }
    if (t - t_min >= (double)every) {
        double dt = t - t_min;
        // A minute in which the app was offered no pose at all (the immersive
        // space was down, the headset off) is not a measurement of the game.
        if (on && pub != p_min) {
            char gpu[32] = "";
            if (gpu_n) snprintf(gpu, sizeof gpu, ", GPU %.1f ms a frame", gpu_sum / gpu_n);
            fprintf(stderr, "[perf] %5.2f min: game %.1f fps, display %.1f Hz%s, thermal %d, "
                            "memory %lld MB used / %lld MB left\n",
                    (t - t_first) / 60.0, (double)(f - f_min) / dt, (double)(pub - p_min) / dt, gpu,
                    (int)g_perf_thermal, kl_phys_footprint() >> 20, kl_avail_memory() >> 20);
        }
        t_min = t; f_min = f; p_min = pub; gpu_sum = 0; gpu_n = 0;
    }
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
            // The x18 refusal count rides the heartbeat because the failure it
            // predicts cannot report itself: a refused site that writes x18
            // leaves the shadow stale, and the fault that follows can be a
            // silent SIGKILL. Printed only when non-zero.
            unsigned x18ref = kl_x18_refusals();
            fprintf(stderr, "[hb] %02d:%02d:%02d  phase=%s  foot=%lldM avail=%lldM",
                    tm.tm_hour, tm.tm_min, tm.tm_sec, phase,
                    kl_phys_footprint() >> 20, kl_avail_memory() >> 20);
            if (x18ref) fprintf(stderr, "  x18-REFUSED=%u", x18ref);
            fprintf(stderr, "\n");
            fflush(stderr);
            last_phase = phase;
        }
        // Every 10 s, a quarter as often as the memory line: whether the guest
        // is feeding the audio stream and whether what it feeds is silence. The
        // end-of-run report never prints on a headset (the app is killed, not
        // exited), so this is the only audio census a device log carries.
        if (ticks % 40 == 0) { kl_opensl_heartbeat(stderr); fflush(stderr); }
        perf_tick();
        kl_metrics_tick();
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

// "Preparing graphics": every pipeline a list holds is compiled before the game
// starts, on a device made as the game makes its own, so that the game finds
// each one in the system's Metal cache instead of waiting for the compiler in
// the middle of a frame (runtime/gfx/kl_fossilize.cpp has the story). Minutes
// the first time, seconds after it. Here rather than at the game's own device
// because here the window is still up and can say what is happening; the
// immersive space opens when boot is done.
//
// How the game makes its device is written down by the game's own first run
// (KL_VK_DEVICE_INFO), and that record is the one that counts. Before there is
// one, a copy that came with the list stands in; the game's own replay then
// runs once more at its device, which costs seconds if the stand-in was right
// and is the cure if it was not.
int  kl_foz_prewarm(const char *info_path);
void kl_foz_prewarm_exact(int exact);
int  kl_foz_progress(unsigned *done, unsigned *total, double *elapsed);
int  kl_foz_databases(void);

static void prepare_graphics(void) {
    if (!kl_env_on("KL_VK_FOZ_PREWARM", 1) || !kl_env_str("KL_VK_FOZ_REPLAY", NULL)) return;
    int n = kl_foz_databases();
    if (n <= 0) {
        printf("  [app] preparing graphics: no pipeline list in %s — the game compiles as it goes "
               "and records what it compiled\n", g_fozdir);
        return;
    }
    char own[1200], stand_in[1200];
    snprintf(own, sizeof own, "%s/device-info.json", g_fozdir);
    snprintf(stand_in, sizeof stand_in, "%s/device-info.default.json", g_fozdir);
    app_phase("preparing graphics");
    g_preparing = 1;
    int exact = 1;
    int rc = kl_foz_prewarm(own);
    if (rc == -2) { exact = 0; rc = kl_foz_prewarm(stand_in); }
    g_preparing = 0;
    if (rc == 0) kl_foz_prewarm_exact(exact);
    unsigned done = 0, total = 0; double s = 0;
    kl_foz_progress(&done, &total, &s);
    printf("  [app] preparing graphics: %s (%d list(s), %u of %u pipelines, %.1f s, on %s)\n",
           rc == 0 ? "done" : rc == -2 ? "no usable record of the game's device yet — left to the game's own start"
                                       : "could not — left to the game's own start",
           n, done, total, s, rc ? "nothing" : exact ? "the game's own device settings" : "the stand-in device settings");
    fflush(NULL);
}

int kl_app_prepare_progress(unsigned *done, unsigned *total) {
    unsigned d = 0, t = 0;
    int st = g_preparing ? kl_foz_progress(&d, &t, NULL) : 0;
    if (done) *done = d;
    if (total) *total = t;
    return g_preparing && st == 1;
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
    // shader_cache_guard ran in kl_app_configure, before the log existed.
    if (g_cache_guard_note[0])
        fprintf(stderr, "  [klepton] shader cache guard: %s\n", g_cache_guard_note);

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
    if (g_alyx_ids_note[0]) printf("  video file: %s\n", g_alyx_ids_note);
    { const char *chosen = getenv("KL_APP_SETTINGS");      // what the start window's dials were at (KleptonAlyx.swift)
      if (chosen && *chosen) printf("  settings  : %s\n", chosen); }
    if (g_shader_test_note[0]) printf("  shader test: %s\n", g_shader_test_note);
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
    if (target_is_linux()) {
        printf("  frameworks: %s\n\n", g_dylibs);
    } else {
    printf("  libraries : %s\n", g_libdir);
    printf("  dylibs    : %s%s\n", g_dylibs,
           have_translations() ? "" : "  (none embedded — falling back to the mmap ELF loader)");
    printf("  assets    : %s\n", g_assets);
    printf("  apk       : %s\n", g_apk);
    printf("  files     : %s\n\n", g_files);
    }
    fflush(NULL);

    // Before the chain runs, so nothing it starts reads a default we meant to
    // replace.
    if (kl_app_target_is_steamlink()) steamlink_policy();

    prepare_graphics();

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
                 : target_is_linux()             ? "the Linux process is ready to start"
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
    else if (target_is_linux())
        snprintf(g_status, sizeof g_status, "the Linux process run ended");
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
    uint64_t        published_ns; // when the last one was (guest_now_ns)
    uint64_t        consumed;     // poses the guest has started a frame for
    unsigned        limit;        // KL_FRAMES, or 0 for "until stopped"
    // volatile because the Steam Link pump reads it WITHOUT the lock: that loop
    // is inside kl_slink and is handed a plain flag, which is the honest shape
    // for "somebody else will set this once". Every other reader takes the mutex.
    volatile int    stop;
    int             state;        // kl_app_guest_state()
    int             asked_quit;   // the loop ended on the game's own quit (kl_app_guest_asked_quit)
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

int kl_app_guest_asked_quit(void) {
    pthread_mutex_lock(&g_guest.mu);
    int q = g_guest.asked_quit;
    pthread_mutex_unlock(&g_guest.mu);
    return q;
}

// An SDL guest is an engine with a shutdown of its own, and that shutdown is
// where it writes what the player changed: Counter-Strike VR saved neither the
// player's name nor any setting, because this app's answer to being backgrounded
// is exit(0) and the engine never heard about it. Android destroys the activity
// first, and SDL turns that into SDL_QUIT.
int kl_app_guest_quit_done(void) {
    extern int kl_sdl2_main_returned(void);
    return g_target && g_target->kind == KL_GUEST_SDL2 ? kl_sdl2_main_returned() : 0;
}
// What every guest thread was doing as the app leaves, into the log — only for
// the targets whose runs keep that account (kl_pthread_stuck_diag).
void kl_app_exit_report(void) { kl_pthread_exit_report(stderr); }
int kl_app_guest_request_quit(void) {
    extern int kl_sdl2_send_quit(void);
    if (!g_target || g_target->kind != KL_GUEST_SDL2) return 0;
    if (kl_app_guest_state() <= 0) return 0;
    return kl_sdl2_send_quit();
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
static uint64_t guest_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
// See the frame loop. Default on for the one target measured.
static int guest_fresh_pose(void) {
    static int v = -1;
    if (v < 0) {
        const char *t = kl_driver_target_name();
        v = kl_env_on("KL_GUEST_FRESH_POSE", t && strcmp(t, "x5812519008825194") == 0);
    }
    return v;
}
// KL_UNITY_QUIT_EXITS: nativeRender returning false ends the run.
//
// On Android that return is how a Unity game quits: UnityPlayer's frame loop
// finishes the activity when nativeRender answers false, which it does once the
// game has called Application.Quit(). Here the loop went on calling an engine
// that had stopped drawing, so quitting from a game's own menu left its last
// picture standing, reprojected, until the app was closed by hand. With this the
// loop ends there and the app leaves (KleptonCompositor, at "the guest thread
// has ended").
//
// Default on for the titles it was run with — Real VR Fishing
// (x2582932495064035), on its sender's headset (fix 37, 2026-10-14), Synth
// Riders (x2436558143118760, fix 39, 2026-10-15), whose exit its sender saw
// work on a Vision Pro, and Gun Club VR (x2285803881494677), whose in-game
// Exit its sender ran with this and KL_UNITY_QUIT_TEARDOWN (kl_driver.c) for a
// clean exit with its settings kept (fix 44, 2026-10-15); none run here. Every
// other guest keeps the loop as it was until a false has been seen to mean the
// same for it: a game that answers false once and draws on would be ended by
// this.
static int guest_quit_exits(void) {
    static int v = -1;
    if (v < 0) {
        const char *t = kl_driver_target_name();
        v = kl_env_on("KL_UNITY_QUIT_EXITS", t && (strcmp(t, "x2582932495064035") == 0
                                                   || strcmp(t, "x2436558143118760") == 0
                                                   || strcmp(t, "x2285803881494677") == 0));
    }
    return v;
}
// KL_GUEST_PACE_DIV=<n>: the guest starts a frame on every nth pose it is
// offered, while it is offered them at a panel's full rate. A game whose frame
// takes a little longer than one refresh otherwise lands on the next refresh
// most of the time and on the one after that every so often — motion that
// stutters at no fixed rate (Half-Life: Alyx, 2026-10-13: 12.5 ms of GPU a
// frame against the panel's 10, 70 frames a second) — and it keeps the graphics
// chip busy without pause until the system halves the app's display rate for
// heat. Every second refresh is an even 50 (or 45, or 60) a second with the
// chip idle for part of each frame. Glovebox runs the game this way
// (KL_GUEST_PACE_HZ there).
//
// "While at the full rate" is the part that matters. The system halves an
// app's rate when it sees fit — at once, in a session that starts warm — and
// then the poses themselves come every second refresh: dividing those again
// makes a quarter. And the rate cannot be taken from a measurement at the
// start: the first version of this did (the compositor's "display … Hz
// measured"), was told 50 in a 120 Hz space the system was showing at 60, and
// paced the game to 20 frames a second (headset, 2026-10-13).
//
// So the rate is read where it is true, from the poses as they arrive: the
// median of the last sixteen gaps between them. Above 77 a second (a gap under
// 13 ms — every panel rate there is, 90 to 120) a frame starts on the first
// pose that comes n gaps, less a quarter of one, after the last frame started;
// at or below it the app is being shown at half rate already and every pose
// starts a frame, which is the same number of frames a second as before.
enum { kPubGaps = 16 };
static uint32_t g_pub_gap_ns[kPubGaps];        // under g_guest.mu
static unsigned g_pub_gap_at;
static uint64_t guest_pose_gap_ns(void) {      // under g_guest.mu; 0 until there are enough
    uint32_t v[kPubGaps];
    int n = 0;
    for (int i = 0; i < kPubGaps; i++) if (g_pub_gap_ns[i]) v[n++] = g_pub_gap_ns[i];
    if (n < kPubGaps) return 0;
    for (int i = 1; i < n; i++) { uint32_t x = v[i]; int j = i; while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; } v[j] = x; }
    return v[n / 2];
}
void kl_app_set_panel_hz(float hz) { (void)hz; }   // kept for the compositor's call; the rate is read from the poses
static int guest_pace_div(void) {
    static int v = -1;
    if (v < 0) {
        // Moss: every second refresh. At the display's own eye size it needs about 8 ms of GPU a frame,
        // which holds neither 120 nor 100 a second evenly; the owner runs it at an even 60 of 120
        // (headset, 2026-10-13). KL_GUEST_PACE_DIV=1 is every refresh.
        v = kl_env_int("KL_GUEST_PACE_DIV", kl_driver_target_is_moss() ? 2 : 1);
        if (v < 1) v = 1;
        if (v > 4) v = 4;
        if (v > 1) fprintf(stderr, "[guest] paced to every %d. refresh of the panel (KL_GUEST_PACE_DIV)\n", v);
    }
    return v;
}
// KL_OVRP_HZ_FOLLOW=1: the refresh rate the guest is told follows the rate it
// is really given frames at, where the compositor tells it the panel's rate
// once, as measured at boot. The two part when the system changes the app's
// rate later (50 at boot, 60 a few seconds on, in a 120 Hz space) and when
// KL_GUEST_PACE_DIV halves the frames — and a guest that derives its physics
// step from the rate then steps its physics at one rate and draws at another.
// Floor Plan 2 does exactly that: its TButt.TBSettings.SetTimestep sets
// Time.fixedDeltaTime to 1/rate, and TBCore.FixedUpdate re-checks the rate every
// step (ValidateTimestep). Told 49.97 and drawing 60, an object in the hand —
// moved on the physics step — jumped back and forth against the hand, which is
// posed every frame; let go, nothing was out of step (headset, 2026-10-18).
// The rate is the median of the last 32 gaps between frame starts, snapped to
// a panel rate or a half of one; a new one is told once it has held for two
// readings a second apart. Default on for Floor Plan 2 only: elsewhere a rate
// read once and then changed under the guest has not been tried.
static int guest_rate_follow_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *t = kl_driver_target_name();
        v = kl_env_on("KL_OVRP_HZ_FOLLOW", t && strcmp(t, "x2401648309868679") == 0);
        if (v) fprintf(stderr, "[guest] the refresh rate told follows the frames given (KL_OVRP_HZ_FOLLOW)\n");
    }
    return v;
}
static void guest_rate_follow(uint64_t turn_ns) {   // frame thread only
    enum { kGaps = 32 };
    static uint32_t gaps[kGaps];
    static unsigned at, n;
    static uint64_t last_ns, next_ns;
    static float pending;
    if (!guest_rate_follow_on()) return;
    uint64_t prev = last_ns;
    last_ns = turn_ns;
    if (!prev || turn_ns <= prev) return;
    uint64_t gap = turn_ns - prev;
    if (gap > 100000000ull) return;                 // a pause, not a rate
    gaps[at++ % kGaps] = (uint32_t)gap;
    if (n < kGaps) n++;
    if (n < kGaps || turn_ns < next_ns) return;
    next_ns = turn_ns + 1000000000ull;
    uint32_t v[kGaps];
    memcpy(v, gaps, sizeof v);
    for (int i = 1; i < kGaps; i++) { uint32_t x = v[i]; int j = i; while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; } v[j] = x; }
    float hz = 1.0e9f / (float)v[kGaps / 2];
    static const float rates[] = { 120, 100, 96, 90, 72, 60, 50, 48, 45, 36, 30 };
    float snap = 0;
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++)
        if (fabsf(hz - rates[i]) <= 0.03f * rates[i]) { snap = rates[i]; break; }
    float told = kl_ovrp_display_frequency();
    if (!snap || fabsf(snap - told) < 1.0f) { pending = 0; return; }
    if (pending != snap) { pending = snap; return; }   // once more, a second on
    pending = 0;
    kl_ovrp_set_display_frequency(snap);
    fprintf(stderr, "[guest] frames come at %.1f a second (median gap %.2f ms): the guest is told %.0f Hz, "
                    "was %.2f (KL_OVRP_HZ_FOLLOW)\n", (double)hz, (double)v[kGaps / 2] / 1e6,
            (double)kl_ovrp_display_frequency(), (double)told);
}
static void guest_pace_wait(void) {
    static uint64_t last_turn_ns;                 // the pose the last frame started on (frame thread only)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    int div = guest_pace_div();
    pthread_mutex_lock(&g_guest.mu);
    for (;;) {
        int timed_out = 0;
        while (!g_guest.stop && g_guest.published == g_guest.consumed)
            if (pthread_cond_timedwait(&g_guest.cv, &g_guest.mu, &ts) == ETIMEDOUT) { timed_out = 1; break; }
        if (div <= 1 || timed_out || g_guest.stop || !last_turn_ns) break;
        uint64_t gap = guest_pose_gap_ns();
        if (!gap || gap >= 13000000ull) break;      // not known yet, or shown at half rate already
        if (g_guest.published_ns - last_turn_ns >= (uint64_t)div * gap - gap / 4) break;
        g_guest.consumed = g_guest.published;      // too soon after the last frame: let this pose go by
    }
    last_turn_ns = g_guest.published_ns;
    guest_rate_follow(last_turn_ns);
    g_perf_frames++;
    kl_metrics_note_game_frame();
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
        // An Unreal game whose ForceQuit was obeyed (KL_UE4_FORCE_QUIT) has
        // quit by itself: leave the app, as for a Unity guest's quit.
        if (g_target && g_target->kind == KL_GUEST_UE4 && kl_ue4_force_quit_obeyed()) {
            printf("[guest] the game asked to quit (ForceQuit) — the run ends here\n");
            pthread_mutex_lock(&g_guest.mu);
            g_guest.asked_quit = 1;
            pthread_mutex_unlock(&g_guest.mu);
        }
        fflush(NULL);
        kl_app_lifecycle_report();
        guest_finished();
        return NULL;
    }

    printf("\n=== guest thread running, one frame per published pose ===\n");
    fflush(NULL);

    // KL_GUEST_PACE_DIV holds here as well, the way guest_pace_wait applies it
    // to a guest that owns its loop: a pose that comes too soon after the one
    // the last frame started on is let go by. Until 2026-10-18 this loop never
    // read it, so the Test switch did nothing for any Unity game (Arkham Shadow
    // and LEGO Bricktales on the headset: no "[guest] paced" line, the full
    // rate shown).
    int pace_div = guest_pace_div();
    uint64_t last_turn_ns = 0;
    for (;;) {
        pthread_mutex_lock(&g_guest.mu);
        for (;;) {
            while (!g_guest.stop && g_guest.published == g_guest.consumed)
                pthread_cond_wait(&g_guest.cv, &g_guest.mu);
            if (pace_div <= 1 || g_guest.stop || !last_turn_ns) break;
            uint64_t gap = guest_pose_gap_ns();
            if (!gap || gap >= 13000000ull) break;  // not known yet, or shown at half rate already
            if (g_guest.published_ns - last_turn_ns >= (uint64_t)pace_div * gap - gap / 4) break;
            g_guest.consumed = g_guest.published;  // too soon after the last frame: let this pose go by
        }
        last_turn_ns = g_guest.published_ns;
        guest_rate_follow(last_turn_ns);
        kl_metrics_note_game_frame();
        int stop = g_guest.stop;
        // Coalesce, do not queue: take everything published since the last
        // frame as a single turn. This is what makes "the guest is late" mean
        // "frames were skipped" — which reprojection covers — rather than
        // "frames are owed", which is a backlog that only grows.
        g_guest.consumed = g_guest.published;
        unsigned limit = g_guest.limit;
        pthread_mutex_unlock(&g_guest.mu);
        if (stop) break;

        // KL_GUEST_FRAME_STATS=<seconds> — every so many seconds, how long the
        // guest's frames took and how late they started. One frame is one call
        // here, offered one display period; "it stutters a little, regularly"
        // is either frames that take longer than that or frames that start
        // late, and from outside the two look the same. Off (0) unless set.
        static int stats_s = -1;
        static uint64_t st_since, st_n, st_over, st_over2, st_late;
        static double st_sum, st_max, st_wait_sum, st_wait_max;
        if (stats_s < 0) stats_s = (int)kl_env_uint("KL_GUEST_FRAME_STATS", 0);
        uint64_t f0 = stats_s ? guest_now_ns() : 0;
        double waited_ms = 0;
        if (stats_s) {
            pthread_mutex_lock(&g_guest.mu);
            waited_ms = (double)(f0 - g_guest.published_ns) / 1.0e6;   // the offer's age at the start
            pthread_mutex_unlock(&g_guest.mu);
        }

        int rendered = kl_app_frame();
        if (rendered < 0) {
            printf("[guest] nativeRender is gone — the frame loop ends here\n");
            fflush(NULL);
            break;
        }
        if (rendered == 0 && guest_quit_exits()) {
            printf("[guest] nativeRender returned false: the game asked to quit — "
                   "the frame loop ends here (KL_UNITY_QUIT_EXITS)\n");
            fflush(NULL);
            pthread_mutex_lock(&g_guest.mu);
            g_guest.asked_quit = 1;
            pthread_mutex_unlock(&g_guest.mu);
            break;
        }
        if (stats_s) {
            uint64_t f1 = guest_now_ns();
            double ms = (double)(f1 - f0) / 1.0e6, period = 1000.0 / (double)kl_ovrp_display_frequency();
            if (!st_since) st_since = f0;
            st_n++; st_sum += ms; st_wait_sum += waited_ms;
            if (ms > st_max) st_max = ms;
            if (waited_ms > st_wait_max) st_wait_max = waited_ms;
            if (ms > period) st_over++;
            if (ms > 2.0 * period) st_over2++;
            if (waited_ms > 0.5 * period) st_late++;
            if (f1 - st_since > (uint64_t)stats_s * 1000000000ull) {
                fprintf(stderr, "  [guest] %llu frames in %.1f s: %.2f ms a frame on average, longest %.1f; "
                                "%llu took over one display period (%.1f ms), %llu over two; started on an "
                                "offer %.1f ms old on average (oldest %.1f), %llu on one over half a period old\n",
                        (unsigned long long)st_n, (double)(f1 - st_since) / 1.0e9, st_sum / (double)st_n, st_max,
                        (unsigned long long)st_over, period, (unsigned long long)st_over2,
                        st_wait_sum / (double)st_n, st_wait_max, (unsigned long long)st_late);
                st_since = f1; st_n = st_over = st_over2 = st_late = 0;
                st_sum = st_max = st_wait_sum = st_wait_max = 0;
            }
        }
        // KL_GUEST_FRESH_POSE: start each frame ON a display frame. A pose
        // that was offered while the frame was still running is not carried
        // into the next one; the loop waits for the next offer, so a guest
        // frame lasts a whole number of display frames and is rendered from a
        // pose that is new when it starts. Without it a guest whose frames run
        // a little over one display period starts each on a pose already most
        // of a period old and its own period beats against the display's
        // (measured on AC Nexus). A frame that ended only just after an offer
        // keeps it, so one long frame costs a late start and not a whole period.
        if (guest_fresh_pose()) {
            pthread_mutex_lock(&g_guest.mu);
            if (g_guest.published != g_guest.consumed &&
                guest_now_ns() - g_guest.published_ns > 2500000ull)
                g_guest.consumed = g_guest.published;
            pthread_mutex_unlock(&g_guest.mu);
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
    // ...and a Linux process reaching OpenXR through xrizer is the same again.
    // ...and Counter-Strike VR (Xash through SDL2, its own OpenXR loop). Unpaced,
    // its xrWaitFrame returned at once and the engine drew as fast as it could:
    // 390 to 612 frames a second submitted in a round on the device, against a
    // panel showing 120. The first sign was the compositor starved to 35-48 Hz
    // at the larger eye size; at the smaller one everything looked healthy for
    // five minutes and then the system halved the app's cadence to 60 Hz and
    // kept it there, with the compositor's own pass idle — a headset shedding
    // load it should never have been given.
    // ...and an Unreal guest whose build drives OpenXR: Moss from Steam. Unpaced on the headset
    // (2026-10-14) it drew as fast as it could at the display's own eye size, and the system showed
    // the app at half rate from the start – 50 and 60 Hz where the Quest build, paced, is shown at 100
    // and 120. Told apart from the Quest build of the same target by the Steam library it ships; the
    // OVRPlugin pacer below is installed as well and is simply never reached by this build.
    // ...and HITMAN 3 VR: Reloaded, a NativeActivity that drives OpenXR from its own main loop. Without
    // the clock its xrWaitFrame returned at once: the game drew as fast as its own frame cap let it (65
    // a second at 100 Hz once that cap was lowered, kl_guestpatch.c hitman3-frame-cap), never on a
    // refresh, and "every second picture" (KL_GUEST_PACE_DIV) was never read (headset, 2026-10-20).
    extern int kl_steamapi_shipped(void);
    if (kl_app_target_is_steamlink() || target_is_jkxr() || target_is_linux() ||
        (g_target->name && strcmp(g_target->name, "cs1") == 0) ||
        (g_target->name && strcmp(g_target->name, "x8012387035448196") == 0) ||
        (g_target->kind == KL_GUEST_UE4 && kl_steamapi_shipped()))
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
    // Drop Dead: Dual Strike Edition (x1935017546592600) overflowed this thread's
    // stack the moment the player took hold of the first weapon: the crash report
    // (SIGILL on the stack guard page, headset 2026-10-28) is TextMeshProUGUI
    // .GenerateTextMesh calling down a deep chain on the guest's main thread, in
    // a title that ran fine on Quest because Android's main thread has 8 MB of
    // stack while a plain Darwin thread has 512 KB. Give THIS title the 8 MB its
    // platform gives the main thread. Per target: other titles are known to run
    // inside the default, and a bigger stack is address space they do not need.
    if (g_target->name && strcmp(g_target->name, "x1935017546592600") == 0)
        pthread_attr_setstacksize(&attr, 8u * 1024u * 1024u);
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
    g_perf_publishes++;
    pthread_mutex_lock(&g_guest.mu);
    g_guest.published++;
    { uint64_t now = guest_now_ns(), gap = g_guest.published_ns ? now - g_guest.published_ns : 0;
      // A gap of a quarter second or more is a pause, not a rate.
      if (gap && gap < 250000000ull) g_pub_gap_ns[g_pub_gap_at++ % kPubGaps] = (uint32_t)gap;
      g_guest.published_ns = now; }
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
