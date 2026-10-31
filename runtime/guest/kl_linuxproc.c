// A whole Linux process as the guest. See kl_linuxproc.h.
#include "kl_linuxproc.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "kl_env.h"
#include "kl_openxr.h"
#include "../linux/kl_lx.h"

#define LP_ARGS 64
#define LP_ENV  96

static char  g_error[512];
static char  g_root[PATH_MAX];
static char  g_prog[PATH_MAX];
static char  g_cwd[PATH_MAX];
static char *g_argv[LP_ARGS + 2];
static int   g_argc;
static char *g_envp[LP_ENV + 1];
static int   g_envc;
static FILE *g_out;
static volatile int g_exited, g_status;

const char *kl_linuxproc_error(void) { return g_error; }
int         kl_linuxproc_exited(void) { return g_exited; }

static int fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof g_error, fmt, ap);
    va_end(ap);
    return -1;
}

static void P(FILE *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void P(FILE *out, const char *fmt, ...) {
    if (!out) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
}

// K=V into the environment, replacing an earlier value for the same key.
static void put_env(const char *kv) {
    const char *eq = strchr(kv, '=');
    size_t klen = eq ? (size_t)(eq - kv) + 1 : 0;
    if (!klen) return;
    for (int i = 0; i < g_envc; i++)
        if (!strncmp(g_envp[i], kv, klen)) { free(g_envp[i]); g_envp[i] = strdup(kv); return; }
    if (g_envc < LP_ENV) g_envp[g_envc++] = strdup(kv);
}

// Blank-separated words; no quoting, because nothing that is passed needs it.
static int split(const char *s, void (*each)(const char *word)) {
    int n = 0;
    char *copy = strdup(s), *save = NULL;
    for (char *w = strtok_r(copy, " \t\n", &save); w; w = strtok_r(NULL, " \t\n", &save), n++)
        each(w);
    free(copy);
    return n;
}
static void add_arg(const char *w) { if (g_argc < LP_ARGS) g_argv[g_argc++] = strdup(w); }

static void mkdirs(const char *path) {
    char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(buf, 0755); *p = '/'; }
    mkdir(buf, 0755);
}

// The symbolic links of the staged trees. A sysroot is full of them (every
// libfoo.so.1 is one) and so is the game's bin directory, and a copy onto a
// device carries only regular files — so the staging side writes them down and
// this side makes them. Idempotent; a link that already says the right thing is
// left alone, which is every one of them on every run but the first.
static int apply_links(FILE *out) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/links.tsv", g_root);
    FILE *f = fopen(path, "r");
    if (!f) return 0;                       // a host tree has real links and no manifest
    char line[2 * PATH_MAX];
    int made = 0, kept = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t'), *nl = strchr(line, '\n');
        if (!tab) continue;
        *tab = 0;
        if (nl) *nl = 0;
        char at[PATH_MAX], have[PATH_MAX];
        snprintf(at, sizeof at, "%s/%s", g_root, line);
        ssize_t n = readlink(at, have, sizeof have - 1);
        if (n >= 0) {
            have[n] = 0;
            if (!strcmp(have, tab + 1)) { kept++; continue; }
            unlink(at);
        } else if (errno != ENOENT && errno != EINVAL) {
            bad++;
            continue;
        } else if (errno == EINVAL) {
            // Something real is where a link belongs: the trees disagree, and
            // the real thing is the one somebody put there on purpose — unless
            // it is empty. A Steam depot cannot carry links and ships zero-byte
            // placeholders in their place (libpango-1.0.so and friends), which
            // an earlier staging copied here; ld.so stops at those.
            struct stat st;
            if (lstat(at, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0) {
                unlink(at);
            } else {
                kept++;
                continue;
            }
        }
        char *slash = strrchr(at, '/');
        if (slash) { *slash = 0; mkdirs(at); *slash = '/'; }
        if (symlink(tab + 1, at) == 0) made++; else bad++;
    }
    fclose(f);
    P(out, "  links     : %d made, %d already there%s\n", made, kept,
      bad ? " — SOME FAILED" : "");
    if (bad) P(out, "  links     : %d could not be made (last: %s)\n", bad, strerror(errno));
    return 0;
}

int kl_linuxproc_configure(const char *root, const char *dylibs, FILE *out) {
    if (!root || !*root) return fail("no staged tree was named");
    if (!realpath(root, g_root)) snprintf(g_root, sizeof g_root, "%s", root);

    char sys[PATH_MAX], p[PATH_MAX];
    snprintf(sys, sizeof sys, "%s/sysroot", g_root);
    struct stat st;
    snprintf(p, sizeof p, "%s/lib/aarch64-linux-gnu/libc.so.6", sys);
    if (stat(p, &st) != 0)
        return fail("the Linux sysroot is not staged (%s is missing — run visionos/stage_sync.py)", p);

    apply_links(out);

    // An app starts with 256 descriptors and this guest opens more than that
    // before its first frame: Source 2 keeps every pak archive it has read from
    // open. A shell hides it on the host (its limit is in the millions). 4096 is
    // the Linux runtime's own table size (kl_lx_vfs.c), so nothing above it
    // could be used anyway.
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < 4096) {
        rlim_t was = rl.rlim_cur;
        rl.rlim_cur = rl.rlim_max < 4096 ? rl.rlim_max : 4096;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
            P(out, "  descriptors: the limit stays at %llu (%s)\n", (unsigned long long)was,
              strerror(errno));
        else
            P(out, "  descriptors: limit raised from %llu to %llu\n", (unsigned long long)was,
              (unsigned long long)rl.rlim_cur);
    }

    #define SUB(buf, fmt, ...) (snprintf(buf, sizeof buf, fmt, __VA_ARGS__), buf)
    kl_lx_mount("/usr", sys);
    kl_lx_mount("/lib", SUB(p, "%s/lib", sys));
    kl_lx_mount("/bin", SUB(p, "%s/bin", sys));
    kl_lx_mount("/etc", SUB(p, "%s/etc", sys));
    mkdirs(SUB(p, "%s/game", g_root));
    kl_lx_mount("/game", p);
    mkdirs(SUB(p, "%s/tmp/xdg", g_root));
    kl_lx_mount("/tmp", SUB(p, "%s/tmp", g_root));
    mkdirs(SUB(p, "%s/home", g_root));
    kl_lx_mount("/home/user", p);
    int have_xrizer = stat(SUB(p, "%s/xrizer/bin/linuxarm64/vrclient.so", g_root), &st) == 0;
    if (have_xrizer) {
        kl_lx_mount_ro("/opt/xrizer", SUB(p, "%s/xrizer", g_root));
        // Where the game's own libopenvr_api.so is told the runtime lives.
        mkdirs(SUB(p, "%s/home/.config/openvr", g_root));
        FILE *f = fopen(SUB(p, "%s/home/.config/openvr/openvrpaths.vrpath", g_root), "w");
        if (f) {
            fputs("{\n  \"config\": [\"/home/user/.local/share/Steam/config\"],\n"
                  "  \"external_drivers\": null,\n  \"jsonid\": \"vrpathreg\",\n"
                  "  \"log\": [\"/home/user/.local/share/Steam/logs\"],\n"
                  "  \"runtime\": [\"/opt/xrizer\"],\n  \"version\": 1\n}\n", f);
            fclose(f);
        }
    }
    #undef SUB

    char *dirs = strdup(dylibs ? dylibs : ""), *save = NULL;
    for (char *d = strtok_r(dirs, ":", &save); d; d = strtok_r(NULL, ":", &save))
        kl_lx_add_dylib_dir(d);
    free(dirs);

    snprintf(g_prog, sizeof g_prog, "%s",
             kl_env_str("KL_LX_PROG", "/game/bin/linuxsteamrtarm64/hlvr"));
    snprintf(g_cwd, sizeof g_cwd, "%s", kl_env_str("KL_LX_CWD", "/game"));
    g_argc = 0;
    add_arg(g_prog);
    // KL_LX_ARGS_EXTRA: more arguments, ahead of the rest. A second variable so
    // that something which only wants to ADD a console variable (the app
    // window's picture setting) does not have to restate, and thereby freeze,
    // the whole command line.
    split(kl_env_str("KL_LX_ARGS_EXTRA", ""), add_arg);
    // No hlvr_auto_dismiss_loading here any more: after a load the game waits
    // behind its loading screen for a trigger pull, and that screen is now
    // drawn — the skybox as an equirect layer (kl_openxr.c) and the words the
    // game sends for it as a card (kl_lx_interstitial.c). While neither was,
    // the wait was a black picture with nothing on it to say what it wanted
    // (100 s of exactly that after "New Game" on a headset), and the variable
    // was set to skip it; the app's window can still add it (KleptonAlyx.swift).
    //
    // r_vulkan_sw_cmd_lists_suspend_resume 0: the engine otherwise records its
    // main scene pass (both eyes, multisampled) into three command buffers and
    // joins them with dynamic rendering's SUSPENDING/RESUMING. MoltenVK cannot
    // keep one Metal render pass open across command buffers, so each join is a
    // store of the whole multisampled target and a load of it back — measured
    // with KL_VK_PROFILE as three passes where one was meant, and ~8% of the
    // GPU time of a frame on an M4.
    //
    // vr_symmetric_fov 0: this build's default is to draw both eyes with one
    // symmetric cone (the union of the two) into a widened texture and to cut
    // each eye's own part out with a per-view render area — a Qualcomm Vulkan
    // extension the Steam Frame has and MoltenVK does not. Without it both eyes
    // get the LEFT eye's part: on a display whose cones are mirrored (60/45
    // degrees on a Vision Pro) the right eye is drawn 15 degrees off, which was
    // the "two different pictures" of the first run on a headset. Off, the
    // engine draws each eye with its own cone, the classic way, and nothing the
    // display cannot show is drawn at all.
    split(kl_env_str("KL_LX_ARGS", "-vr -vulkan "
                                   "+r_vulkan_sw_cmd_lists_suspend_resume 0 "
                                   "+vr_symmetric_fov 0 +map startup"),
          add_arg);
    g_argv[g_argc] = NULL;

    g_envc = 0;
    put_env("PATH=/usr/bin:/bin");
    put_env("LANG=C");
    put_env("TERM=xterm");
    put_env("HOME=/home/user");
    put_env("USER=user");
    put_env("XDG_RUNTIME_DIR=/tmp/xdg");
    put_env("LD_LIBRARY_PATH=/game/bin/linuxsteamrtarm64");
    // No window system: the picture leaves through OpenXR, and the desktop
    // window the engine also opens is SDL's offscreen one.
    put_env("SDL_VIDEODRIVER=offscreen");
    // Sound leaves through SDL's disk driver into a host thread that plays it
    // (runtime/linux/kl_lx_audio.c, which says why each of these is what it is).
    // Both spellings of the driver variable: SDL 2's, which sdl2-compat reads,
    // and SDL 3's underneath it.
    put_env("SDL_AUDIODRIVER=disk");
    put_env("SDL_AUDIO_DRIVER=disk");
    put_env("SDL_AUDIO_DISK_OUTPUT_FILE=/dev/klepton-audio");
    put_env("SDL_AUDIO_DISK_TIMESCALE=0");
    split(kl_env_str("KL_LX_ENV", ""), put_env);
    g_envp[g_envc] = NULL;

    kl_lx_vulkan_init();
    kl_lx_steam_init();
    kl_lx_openxr_init();
    kl_lx_diag_init();

    P(out, "  linux root: %s\n", g_root);
    P(out, "  program   : %s  (cwd %s)\n  arguments :", g_prog, g_cwd);
    for (int i = 1; i < g_argc; i++) P(out, " %s", g_argv[i]);
    P(out, "\n  OpenVR    : %s\n", have_xrizer ? "xrizer at /opt/xrizer"
                                               : "NOT STAGED — -vr will find no runtime");
    return 0;
}

static void on_exit_status(int status) {
    g_status = status;
    g_exited = 1;
    if (g_out) {
        fprintf(g_out, "\n=== the Linux process exited with status %d ===\n", status);
        kl_openxr_report(g_out);
        fflush(g_out);
    }
}

static void *proc_thread(void *arg) {
    (void)arg;
    if (kl_lx_chdir(g_cwd) != 0) {
        fail("the working directory %s does not exist in the guest", g_cwd);
    } else {
        // Does not return when it works: the thread becomes the process's main
        // thread and ends with the process.
        kl_lx_exec(g_prog, g_argc, g_argv, g_envp);
        fail("could not start %s (see the lines above)", g_prog);
    }
    if (g_out) { fprintf(g_out, "FAIL: %s\n", g_error); fflush(g_out); }
    g_status = -1;
    g_exited = 1;
    return NULL;
}

int kl_linuxproc_begin(FILE *out) {
    if (!*g_prog) return fail("kl_linuxproc_configure was not called");
    g_out = out;
    kl_lx_set_exit_hook(on_exit_status);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // The guest runs on a stack kl_lx_exec maps for it; this one only carries
    // the host side of its system calls.
    pthread_attr_setstacksize(&attr, 4u << 20);
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_t th;
    int rc = pthread_create(&th, &attr, proc_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) return fail("could not create the process thread: %s", strerror(rc));
    pthread_detach(th);
    return 0;
}

double kl_linuxproc_pump(double seconds, const volatile int *quit) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double elapsed = 0;
    struct timespec iv = { 0, 100 * 1000 * 1000 };
    while ((quit ? !*quit : 1) && !g_exited && (seconds < 0 || elapsed < seconds)) {
        nanosleep(&iv, NULL);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - t0.tv_sec) + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
    }
    return elapsed;
}

void kl_linuxproc_report(FILE *out) {
    if (!out) return;
    if (g_exited) fprintf(out, "  the process ended with status %d\n", g_status);
    else          fprintf(out, "  the process is still running\n");
    kl_openxr_report(out);
    fflush(out);
}
