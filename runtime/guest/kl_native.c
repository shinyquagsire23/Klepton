// The generic Android NativeActivity + OpenXR door. See kl_native.h for the
// shape and why it is not one of the engine-specific families. The whole family
// is a thin skin over the shared kl_nativeactivity harness (kl_na_create /
// kl_na_start / kl_na_stop): map the one entry library, run its static
// initializers, hand it ANativeActivity_onCreate, and then turn the looper while
// the guest's own android_main thread runs the game and submits its OpenXR
// frames. GTA Vice City VR (libmiamivr) is the first guest through it.
#include "kl_native.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "klepton.h"
#include "kl_nativeactivity.h"
#include "kl_jni.h"
#include "kl_ndk.h"
#include "kl_egl.h"
#include "kl_openxr.h"
#include "kl_opensl.h"
#include "kl_xashservers.h"

static char      g_libdir[1200];
static char      g_engine_lib[256];   // e.g. "libmiamivr.so"
static kl_image *g_img;
static char      g_err[256];

const char *kl_native_error(void) { return g_err; }
static int native_fail(const char *m) {
    snprintf(g_err, sizeof g_err, "%s", m);
    return 1;
}

#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

static int kl_dir_exists(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

// Env-driven cvar tuning without re-staging the data tree. When KL_XASH_CVARS is
// set, write its contents as a userconfig.cfg into the Xash game dirs under
// `base`. Xash execs userconfig.cfg AFTER config.cfg, so these win even for
// ARCHIVED cvars (vr_height_adjust, vr_smoothturn, ...) that config.cfg would
// otherwise restore to their saved value. The value is one or more `cvar value`
// assignments separated by ';' or newlines, e.g.
//   KL_XASH_CVARS="vr_smoothturn 1; vr_turn_angle 5; vr_height_adjust -0.20"
// This is the on-device equivalent of hand-editing userconfig.cfg on the host,
// so a knob is retuned by changing an env var between launches — no restage. The
// value is the COMPLETE set the file will hold (it overwrites, it does not merge).
static void kl_xash_write_userconfig(const char *base, FILE *out) {
    const char *cvars = getenv("KL_XASH_CVARS");
    if (!cvars || !*cvars || !base || !*base) return;

    // Body: one assignment per line (';' -> newline), with a provenance header.
    char body[2048];
    int n = snprintf(body, sizeof body, "// Klepton: generated from KL_XASH_CVARS\n");
    for (const char *p = cvars; *p && n < (int)sizeof body - 2; p++)
        body[n++] = (*p == ';') ? '\n' : *p;
    if (n && body[n - 1] != '\n') body[n++] = '\n';
    body[n] = 0;

    // Drop it next to every config.cfg the tree ships: valve is always mounted and
    // is the dir the engine stats; the active -game mod (HL_Gold_HD for hl1,
    // cstrike for cs1) also gets one. A dir that is not present is skipped, not an
    // error, so the same list serves both titles.
    static const char *const rel[] = { "valve", "cstrike", "HL_Gold_HD" };
    for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
        char dir[1400];
        snprintf(dir, sizeof dir, "%s/%s", base, rel[i]);
        if (!kl_dir_exists(dir)) continue;
        char path[1500];
        snprintf(path, sizeof path, "%s/userconfig.cfg", dir);
        FILE *f = fopen(path, "w");
        if (!f) { if (out) fprintf(out, "  [xash] userconfig write FAILED: %s\n", path); continue; }
        fwrite(body, 1, (size_t)n, f);
        fclose(f);
        if (out) fprintf(out, "  [xash] wrote %s\n", path);
    }
    if (out) { fprintf(out, "  [xash] KL_XASH_CVARS applied:\n%s", body); fflush(out); }
}

// Surface Xash's own console into our log. The engine's `-log` writes precache
// lists, map spawn, scripted_sequence and AI node-graph messages to a file in the
// tree (engine.log / qconsole.log) — none of which reach our stderr log, so
// gameplay bugs (an NPC that won't path, a missing model) are invisible here.
// We dump it at STARTUP, which reads the PREVIOUS session's log before the engine
// truncates it: re-run once after a bad session and its console lands in our log.
// KL_XASH_NO_ENGINELOG suppresses it if the dump is ever unwanted.
static void kl_xash_dump_enginelog(const char *base, FILE *out) {
    if (!out || !base || !*base) return;
    if (getenv("KL_XASH_NO_ENGINELOG")) return;
    static const char *const rel[] = {
        "engine.log", "valve/qconsole.log", "HLGOLD/HL_Gold_HD/qconsole.log",
        "cstrike/qconsole.log",
    };
    for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
        char path[1500];
        snprintf(path, sizeof path, "%s/%s", base, rel[i]);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        fprintf(out, "  [xash] ==== previous-session engine console: %s ====\n", rel[i]);
        char line[1024];
        while (fgets(line, sizeof line, f)) fprintf(out, "  [elog] %s", line);
        fprintf(out, "  [xash] ==== end %s ====\n", rel[i]);
        fclose(f);
    }
    fflush(out);
}

// Recursively find the first *.log file under `dir` (depth-limited). Reports each
// candidate it sees; returns 1 and fills `out` on the first hit. Used to locate the
// engine's console log without hard-coding a path the fork may not use.
static int kl_elog_find(const char *dir, int depth, char *out, size_t osz) {
    if (depth < 0) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int got = 0;
    while (!got && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;              // skip "." ".." and dotfiles
        char p[1600];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISREG(st.st_mode)) {
            size_t l = strlen(e->d_name);
            if (l >= 4 && !strcmp(e->d_name + l - 4, ".log")) {
                fprintf(stderr, "  [elog-scan] found %s\n", p);
                snprintf(out, osz, "%s", p);
                got = 1;
            }
        } else if (S_ISDIR(st.st_mode)) {
            got = kl_elog_find(p, depth - 1, out, osz);
        }
    }
    closedir(d);
    return got;
}

// Live-tail the engine's -log into stderr as it is written, so the engine console
// (precache / AI node-graph / scripted_sequence) is visible in our streamed log no
// matter how the session ends. A full reinstall wipes the data container, so
// reading a PRIOR session's log is unreliable — tailing the CURRENT one is not.
//
// We do not know the engine's exact log path (the fork may write engine.log under
// the rootdir, a gamedir, or not at all if -log's Sys_InitLog failed), so DISCOVER
// it: enumerate *.log under the tree and tail the first match (preferring
// engine.log). Keeps re-scanning until one appears, since the engine creates it
// only once it starts. Keeps a byte offset across calls; cheap to call every frame.
// KL_XASH_NO_ENGINELOG opts out. Called from the guest pump loop.
void kl_xash_tail_enginelog(void) {
    if (getenv("KL_XASH_NO_ENGINELOG")) return;
    const char *base = getenv("XASH3D_BASEDIR");
    if (!base || !*base) return;
    static char path[1500];
    static long off = 0;
    static int  found = 0, scanned_once = 0;

    if (!found) {
        // Recursively hunt for any *.log the engine may have written (engine.log,
        // qconsole.log, ...), wherever it landed under the tree. First match wins.
        found = kl_elog_find(base, 4, path, sizeof path);
        if (found) { fprintf(stderr, "  [elog-scan] tailing %s\n", path); fflush(stderr); }
        else if (!scanned_once) {
            scanned_once = 1;
            fprintf(stderr, "  [elog-scan] no *.log under %s yet (waiting; -log may have failed)\n", base);
            fflush(stderr);
        }
        if (!found) return;
    }

    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) == 0) {
        long end = ftell(f);
        if (end < off) off = 0;              // truncated / rotated -> re-read from start
        if (end > off) {
            fseek(f, off, SEEK_SET);
            char line[1024];
            while (fgets(line, sizeof line, f)) fprintf(stderr, "  [elog] %s", line);
            off = ftell(f);
            fflush(stderr);
        }
    }
    fclose(f);
}

// hl1's HL_Gold_HD mod ships NESTED at <base>/HLGOLD/HL_Gold_HD, one level below
// the rootdir, where Xash's depth-1 game scan can't see it — so `-game HL_Gold_HD`
// can't MOUNT it and the engine runs with the wrong content search paths (its
// menu/cursor/sound/save/transition state then disagree, and the seamless
// c0a0->c0a0a tram handoff dies with "Can't find connection"). Promote each nested
// mod (a dir two levels down with gameinfo.txt/liblist.gam) UP to the rootdir with
// a rename — same filesystem, so it is instant — making it a first-class depth-1
// game whose gameinfo `basedir "valve"` fallback (a real rootdir sibling) resolves
// normally. Renames are deferred so the directory is not mutated mid-readdir.
static void kl_xash_promote_nested_mods(const char *base, FILE *out) {
    DIR *d = opendir(base);
    if (!d) return;
    char moves[4][2][1400];
    int nmoves = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nmoves < 4) {
        if (e->d_name[0] == '.') continue;
        char subdir[1300];
        snprintf(subdir, sizeof subdir, "%s/%s", base, e->d_name);
        if (!kl_dir_exists(subdir)) continue;
        DIR *sd = opendir(subdir);
        if (!sd) continue;
        struct dirent *se;
        while ((se = readdir(sd)) && nmoves < 4) {
            if (se->d_name[0] == '.') continue;
            struct stat st;
            char probe[1500];
            snprintf(probe, sizeof probe, "%s/%s/gameinfo.txt", subdir, se->d_name);
            int is_mod = (stat(probe, &st) == 0);
            if (!is_mod) {
                snprintf(probe, sizeof probe, "%s/%s/liblist.gam", subdir, se->d_name);
                is_mod = (stat(probe, &st) == 0);
            }
            if (!is_mod) continue;
            char dest[1400];
            snprintf(dest, sizeof dest, "%s/%s", base, se->d_name);
            if (kl_dir_exists(dest)) continue;              // already at rootdir
            snprintf(moves[nmoves][0], sizeof moves[0][0], "%s/%s", subdir, se->d_name);
            snprintf(moves[nmoves][1], sizeof moves[0][1], "%s", dest);
            nmoves++;
        }
        closedir(sd);
    }
    closedir(d);
    for (int i = 0; i < nmoves; i++) {
        if (rename(moves[i][0], moves[i][1]) == 0) {
            if (out) fprintf(out, "  [xash] promoted nested mod %s -> %s\n", moves[i][0], moves[i][1]);
        } else if (out) {
            fprintf(out, "  [xash] promote %s FAILED: %s\n", moves[i][0], strerror(errno));
        }
    }
}

// Override numeric TBXR launch args (--supersampling, --msaa) in the on-device
// commandline.txt from env, so they can be tuned WITHOUT re-staging that file.
// These are raw launch args, not cvars, so userconfig can't reach them; we replace
// the value token that follows each matched flag, in place. Only touches an arg
// whose env is set; idempotent. e.g. KL_XASH_SUPERSAMPLING=1.0 for perf.
// Where the effective commandline.txt lives. The launcher UI can hand us an edited
// copy in a writable in-app file (KL_XASH_COMMANDLINE_FILE) so the args still take
// effect when the picked game folder is read-only; otherwise it is base/commandline.txt.
// klb_fopen redirects the guest engine's own commandline.txt read to the same file.
static void kl_xash_clpath(const char *base, char *out, size_t cap) {
    const char *clf = getenv("KL_XASH_COMMANDLINE_FILE");
    if (clf && *clf) snprintf(out, cap, "%s", clf);
    else             snprintf(out, cap, "%s/commandline.txt", base);
}

static void kl_xash_rewrite_commandline(const char *base, FILE *out) {
    struct { const char *env, *arg; } ov[] = {
        { "KL_XASH_SUPERSAMPLING", "--supersampling" },
        { "KL_XASH_MSAA",          "--msaa" },
    };
    int any = 0;
    for (size_t i = 0; i < sizeof ov / sizeof *ov; i++) if (getenv(ov[i].env)) any = 1;
    if (!any || !base || !*base) return;

    char path[1300];
    kl_xash_clpath(base, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2048];
    char *got = fgets(line, sizeof line, f);
    fclose(f);
    if (!got) return;
    line[strcspn(line, "\r\n")] = 0;

    char outbuf[2560];
    int n = 0;
    const char *pending = NULL;                 // token to override the NEXT value with
    char *save = NULL;
    for (char *tok = strtok_r(line, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        const char *emit = tok;
        if (pending) { emit = pending; pending = NULL; }
        else {
            for (size_t i = 0; i < sizeof ov / sizeof *ov; i++) {
                const char *v = getenv(ov[i].env);
                if (v && *v && !strcmp(tok, ov[i].arg)) { pending = v; break; }
            }
        }
        n += snprintf(outbuf + n, sizeof outbuf - n, "%s%s", n ? " " : "", emit);
        if (n >= (int)sizeof outbuf - 2) break;
    }
    outbuf[n++] = '\n'; outbuf[n] = 0;
    f = fopen(path, "w");
    if (f) { fwrite(outbuf, 1, (size_t)n, f); fclose(f);
             if (out) fprintf(out, "  [xash] commandline.txt -> %s", outbuf); }
}

void kl_engine_setenv(const char *libdir, const char *entry_lib, FILE *out) {
    const char *files = kl_jni_files_dir();
    if (!files) files = "";
    if (entry_lib && strstr(entry_lib, "xash")) {
        // Xash3D (cs1/hl1). The data tree stages under <files>/xash (valve,
        // cstrike, HLGOLD). Engine ref/menu/client libs are the translated ones
        // in libdir. The game picks its mod from XASH3D_GAMEDIR — cstrike if the
        // tree has it (Counter-Strike), else valve (Half-Life). KL_XASH_* override.
        //
        // The engine prints its whole console (map spawn, precache, AI node-graph,
        // scripted_sequence) to STDOUT (fd 1) via printf/puts. Our streamed log is
        // STDERR (fd 2), so none of it was visible. Point fd 1 at fd 2 and unbuffer
        // stdout so the engine console lands in our log, live. This is the runtime
        // doing the redirect (not the guest, whose own dup2 onto fd 1/2 we decline
        // in kl_libc_slink.c); KL_XASH_NO_ENGINELOG opts out.
        if (!getenv("KL_XASH_NO_ENGINELOG")) {
            dup2(STDERR_FILENO, STDOUT_FILENO);
            setvbuf(stdout, NULL, _IONBF, 0);
        }
        char base[1200], sub[1300];
        // The picked-folder override (launcher file picker) IS the game root, so
        // promotion, commandline.txt and -game all resolve against it; only the
        // unset (in-app staged) case falls back to <files>/xash.
        const char *bov = getenv("KL_XASH_BASEDIR");
        if (bov && *bov) snprintf(base, sizeof base, "%s", bov);
        else             snprintf(base, sizeof base, "%s/xash", files);
        // hl1's real mod (HL_Gold_HD) ships NESTED at HLGOLD/HL_Gold_HD, where the
        // engine's depth-1 game scan can't mount it. Promote it to the rootdir so
        // `-game HL_Gold_HD` resolves normally, with valve as its basedir fallback.
        kl_xash_promote_nested_mods(base, out);
        // Apply any env overrides to the raw launch args (KL_XASH_SUPERSAMPLING /
        // KL_XASH_MSAA) by rewriting commandline.txt on device — no restage needed.
        kl_xash_rewrite_commandline(base, out);
        const char *gamedir = getenv("KL_XASH_GAMEDIR");
        if (!gamedir) {
            // Run the mod the launch asks for: commandline.txt's `-game <mod>`
            // (hl1 = HL_Gold_HD). With the mod promoted above, the engine mounts it
            // AND valve (its gameinfo basedir) as the fallback, so content, menu,
            // sound, saves and the seamless c0a0->c0a0a tram transitions are all
            // coherent. Forcing valve here ran the base game with the wrong assets
            // and a split-brain state that crashed the tram's level transition.
            static char gbuf[128];
            char clpath[1300];
            kl_xash_clpath(base, clpath, sizeof clpath);
            FILE *cl = fopen(clpath, "r");
            if (cl) {
                char line[1024];
                if (fgets(line, sizeof line, cl)) {
                    char *g = strstr(line, "-game ");
                    if (g) {
                        g += 6;
                        int i = 0;
                        while (g[i] && g[i] != ' ' && g[i] != '\t' &&
                               g[i] != '\n' && g[i] != '\r' && i < (int)sizeof gbuf - 1)
                            gbuf[i] = g[i], i++;
                        gbuf[i] = 0;
                        if (gbuf[0]) gamedir = gbuf;
                    }
                }
                fclose(cl);
            }
        }
        if (!gamedir) {
            snprintf(sub, sizeof sub, "%s/cstrike", base);
            gamedir = kl_dir_exists(sub) ? "cstrike" : "valve";
        }
        setenv("XASH3D_BASEDIR",    getenv("KL_XASH_BASEDIR") ? getenv("KL_XASH_BASEDIR") : base, 1);
        // NOT XASH3D_RODIR: our data tree is a single writable directory, and
        // Xash aborts ("RoDir and default rootdir can't point to same directory")
        // when RODIR is set and equals the rootdir. Leaving it unset makes Xash
        // use BASEDIR as the one writable root, which is what we have.
        setenv("HOME",              base, 1);
        setenv("XASH3D_GAMEDIR",    gamedir, 1);
        setenv("XASH3D_ENGLIBDIR",  libdir, 1);
        setenv("XASH3D_GAMELIBDIR", libdir, 1);
        // The player's own servers (favourites, admin access, the menus that use it) – before the
        // userconfig is written, because the admin access adds a setting to it. See kl_xashservers.c.
        extern const char *kl_jni_assets_dir(void);
        kl_xash_servers_prepare(base, gamedir, kl_jni_assets_dir(), out);
        // Env-tunable cvars → userconfig.cfg, so knobs like vr_height_adjust can be
        // retuned between launches with no restage (see kl_xash_write_userconfig).
        kl_xash_write_userconfig(base, out);
        // Surface the previous session's engine console (precache/AI/scripted) into
        // our log, before the engine's -log truncates it (see kl_xash_dump_enginelog).
        kl_xash_dump_enginelog(base, out);
        // The VR fork reads getenv("xr_manufacturer") to pick a controller /
        // interaction profile and strcmp()s it against "PICO" / "PLAY FOR DREAM"
        // — a NULL (the Java VR launcher normally setenv's it from
        // Build.MANUFACTURER) segfaults in Host_Main. Klepton's OpenXR advertises
        // /interaction_profiles/oculus/touch_controller, so hand it an Oculus-class
        // identity: that takes the default (non-PICO) touch-controller branch.
        setenv("xr_manufacturer", getenv("KL_XR_MANUFACTURER") ? getenv("KL_XR_MANUFACTURER") : "Oculus", 1);
        // Team Beef's lambda1vr fork (hl1) reads a DIFFERENT var: OPENXR_HMD,
        // which it stores verbatim (getenv, no copy) and strstr()s for "meta" /
        // "pico" to pick the interaction profile — a NULL (unset) segfaults in
        // TBXR_InitialiseOpenXR. Give it a Meta-Quest identity so it matches the
        // "meta" branch, i.e. Klepton's /interaction_profiles/oculus/touch_controller.
        setenv("OPENXR_HMD", getenv("KL_OPENXR_HMD") ? getenv("KL_OPENXR_HMD") : "meta_quest", 1);
        if (out) { fprintf(out, "  [env] XASH3D_BASEDIR=%s GAMEDIR=%s ENGLIBDIR=%s xr_manufacturer=%s OPENXR_HMD=%s\n",
                           base, gamedir, libdir, getenv("xr_manufacturer"), getenv("OPENXR_HMD")); fflush(out); }
    } else if (entry_lib && strstr(entry_lib, "launcher")) {
        // Source (hl2/portal). The launcher (FileSystem_GetExecutableDir) builds
        // "<APP_LIB_PATH>/filesystem_stdio.so" and dlopens the engine/tier0/mod
        // modules from there — a NULL APP_LIB_PATH is the "(null)/filesystem_stdio.so"
        // abort. All the engine .so live in the guest lib dir, so APP_LIB_PATH,
        // APP_MOD_LIB and BASE_PATH all point there. (EXECUTABLE_PATH is not what
        // this port reads; keep it set anyway — it is harmless and some builds do.)
        setenv("APP_LIB_PATH",    libdir, 1);
        setenv("APP_MOD_LIB",     libdir, 1);
        setenv("BASE_PATH",       libdir, 1);
        setenv("EXECUTABLE_PATH", libdir, 1);

        // APP_DATA_PATH is the game content root: the tree stages under
        // <files>/srceng (HL2, valve variant) or <files>/Source (Portal). The
        // game folder path and which game to run come off the same root: the
        // Portal launcher reads PORTAL_GAME_PATH + SOURCE_GAME (portal/portal2),
        // the HL2 launcher reads VALVE_GAME_PATH. Set them all — each launcher
        // ignores the ones it does not use. KL_SOURCE_GAME overrides the pick.
        char dp[1300];
        int is_portal;
        // KL_SOURCE_DATA_PATH points the content root straight at a folder the user
        // supplied (the launcher's "srceng"/"Source" tree), instead of the staged
        // <files>/srceng | /Source — so the picker can hand us the game folder
        // itself. Which game to run then comes from the target, not dir-sniffing.
        const char *dpov = getenv("KL_SOURCE_DATA_PATH");
        if (dpov && *dpov) {
            snprintf(dp, sizeof dp, "%s", dpov);
            extern const char *kl_driver_target_name(void);
            const char *tn = kl_driver_target_name();
            is_portal = tn && !strcmp(tn, "portal");
        } else {
            snprintf(dp, sizeof dp, "%s/srceng", files);
            is_portal = !kl_dir_exists(dp);
            if (is_portal) snprintf(dp, sizeof dp, "%s/Source", files);
        }
        setenv("APP_DATA_PATH",    dp, 1);
        setenv("PORTAL_GAME_PATH", dp, 1);
        setenv("VALVE_GAME_PATH",  dp, 1);
        const char *game = getenv("KL_SOURCE_GAME");
        if (!game) game = is_portal ? "portal" : "hl2";
        setenv("SOURCE_GAME", game, 1);
        if (out) { fprintf(out, "  [env] APP_LIB_PATH=%s APP_DATA_PATH=%s SOURCE_GAME=%s\n",
                           libdir, dp, game); fflush(out); }

        // HL2Q3VR ships per-render-category single-pass DIAGNOSTIC env vars it reads
        // via getenv (klb_getenv -> host getenv, same path SOURCE_GAME/HL2QUEST_GAME
        // take). Forward KL_HL2_DIAG_* so we can isolate which category a still-missing
        // surface belongs to (e.g. the invisible train-car shell: walls/floor/ceiling
        // gone while the seats render) without cheats or cfg. Only set when present.
        static const struct { const char *kl, *port; } diag[] = {
            { "KL_HL2_DIAG_OPAQUE",      "HL2QUEST_DIAG_SINGLE_PASS_OPAQUE" },
            { "KL_HL2_DIAG_BRUSH",       "HL2QUEST_DIAG_SINGLE_PASS_BRUSHMODELS" },
            { "KL_HL2_DIAG_ENTITIES",    "HL2QUEST_DIAG_SINGLE_PASS_ENTITIES" },
            { "KL_HL2_DIAG_STATICPROPS", "HL2QUEST_DIAG_SINGLE_PASS_STATICPROPS" },
        };
        for (size_t i = 0; i < sizeof diag / sizeof diag[0]; i++) {
            const char *v = getenv(diag[i].kl);
            if (v && *v) {
                setenv(diag[i].port, v, 1);
                if (out) { fprintf(out, "  [env] %s=%s\n", diag[i].port, v); fflush(out); }
            }
        }
    }
}

int kl_native_configure(const char *libdir, const char *entry_lib, FILE *out) {
    (void)out;
    if (!libdir || !*libdir)       return native_fail("no library directory");
    if (!entry_lib || !*entry_lib) return native_fail("no entry library");
    snprintf(g_libdir, sizeof g_libdir, "%s", libdir);
    snprintf(g_engine_lib, sizeof g_engine_lib, "%s.so", entry_lib);
    return 0;
}

int kl_native_load(FILE *out) {
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", g_libdir, g_engine_lib);
    if (out) { fprintf(out, "=== %s entry ===\n", g_engine_lib); fflush(out); }

    // Load with the DT_NEEDED chain, dependencies FIRST, then run the target's
    // init. libmiamivr NEEDS libc++_shared.so (it imports operator new / _Znwm,
    // delete, the C++ runtime), and that dependency must be mapped, wired and
    // initialised BEFORE libmiamivr's own static initializers run — otherwise the
    // first allocation in them hits an unresolved _Znwm and aborts in
    // kl_unresolved_named. kl_load_auto loads only the one file and would leave
    // exactly that gap; kl_load_recursive walks DT_NEEDED (skipping shim-served
    // names like libopenxr_loader/libandroid/libc, which bind through the shims)
    // and register+inits each, deps first, then the target.
    //
    // Tea For God (x6323807174332876) lists libfmod/libfmodstudio in its
    // DT_NEEDED BEFORE libc++_shared, and libfmodstudio's static initializers
    // call __cxa_guard_acquire — which Android's libstdc++.so would serve, but
    // which here only libc++_shared exports. Walking DT_NEEDED in order binds
    // FMOD first and its init aborts in kl_unresolved_named. Pull libc++_shared
    // in up front, the way UE4_CHAIN does, so FMOD binds against it.
    {
        extern const char *kl_driver_target_name(void);
        const char *tn = kl_driver_target_name();
        if (tn && strcmp(tn, "x6323807174332876") == 0) {
            char cxx[1400];
            snprintf(cxx, sizeof cxx, "%s/libc++_shared.so", g_libdir);
            if (kl_can_load(cxx) && !kl_load_recursive(cxx) && out) {
                fprintf(out, "  libc++_shared.so preload failed: %s\n", kl_error());
                fflush(out);
            }
        }
    }
    kl_image *img = kl_load_recursive(path);
    if (!img) { snprintf(g_err, sizeof g_err, "%s: %s", g_engine_lib, kl_error()); return 1; }
    g_img = img;
    if (out) {
        // In the shape tools/symbolize_sample.py parses: this guest owns its own
        // render thread, so `sample <pid>` is the instrument for "what is it
        // doing", and without the load address every guest frame reads as unknown.
        fprintf(out, "  mapped %-22s @%p %7.2f MB\n", g_engine_lib,
                kl_base(img), kl_span(img) / 1048576.0);
        kl_dl_report_images(out);
        fflush(out);
    }

    // JNI_OnLoad if the library has one. A NativeActivity game usually does not —
    // its entry points are the ANativeActivity_onCreate / android_main exports —
    // so this is reported, not required.
    typedef int (*jni_onload_fn)(void *vm, void *reserved);
    jni_onload_fn onload = (jni_onload_fn)kl_sym(img, "JNI_OnLoad");
    if (onload) {
        kl_jni_local_frame_push();
        int v = onload(kl_jni_vm(), NULL);
        kl_jni_local_frame_pop();
        if (out) { fprintf(out, "  JNI_OnLoad returned 0x%08x\n", v); fflush(out); }
    } else if (out) {
        fprintf(out, "  no JNI_OnLoad — ANativeActivity_onCreate is the entry\n");
        fflush(out);
    }
    return 0;
}

unsigned kl_native_gap(FILE *out) {
    if (!g_img) return 0;
    unsigned n = 0;
    const char *const *miss = kl_missing_imports(g_img, &n);
    if (out) {
        fprintf(out, "  %u unresolved import(s)%s\n", n, n ? ":" : "");
        for (unsigned i = 0; i < n && i < 40; i++) fprintf(out, "    %s\n", miss[i]);
        if (n > 40) fprintf(out, "    ... and %u more\n", n - 40);
        fflush(out);
    }
    return n;
}

int kl_native_create(FILE *out) {
    // android_native_app_glue spawns android_main on its own thread from inside
    // onCreate; the game loop and every OpenXR frame run there. We only stand the
    // activity up — kl_native_start then delivers the window the render thread
    // has been blocking for.
    //
    // internalDataPath is getFilesDir() == <dataDir>/files on Android, and a
    // NativeActivity game builds paths straight off it: GTA Vice City VR reads
    // <internalDataPath>/gamedata. It must resolve to where stage_files="files"
    // staged the tree (<dataDir>/files/gamedata) — but the real container path is
    // ~90 chars and the game's casepath sizes fixed 128-byte path buffers for a
    // Quest's short storage, so handing it the full path overflows and FORTIFY
    // aborts. Use the SHORT /sdcard spelling instead: kl_guest_path maps /sdcard ->
    // <dataDir> (kl_guest_extstorage_map), so /sdcard/files == getFilesDir ==
    // <dataDir>/files, and klb_getcwd hands the same short spelling back after the
    // game chdir's into it. Every path the game builds then stays well under 128.
    return kl_na_create(g_img, "ANativeActivity_onCreate", "/sdcard/files", out);
}

void kl_native_start(FILE *out) { kl_na_start(out); }
void kl_native_stop(FILE *out)  { kl_na_stop(out); }

double kl_native_pump(double seconds, const volatile int *quit) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double elapsed = 0;
    for (unsigned t = 0; (quit ? !*quit : 1) && (seconds < 0 || elapsed < seconds); t++) {
        kl_ndk_pump_looper(100);
        // The UI thread's task queue and the frame clock, on the same thread that
        // turns the looper — the only definition of "UI thread" that matters here.
        if ((t + 1) % 10 == 0) kl_jni_drain_ui_tasks();
        kl_jni_tick_choreographer();
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - t0.tv_sec)
                + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
    }
    return elapsed;
}

void kl_native_report(FILE *out) {
    if (!out) return;
    // Who is blocked on what, first: this guest owns its render thread, so no
    // return value anywhere says "the engine stopped", and a run that drew nothing
    // looks like one that drew until the mutex-owner map is read.
    kl_pthread_report(out);
    kl_egl_report(out);
    kl_openxr_report(out);
    kl_opensl_report(out);
    fprintf(out, "\n=== JNI surface ===\n");
    kl_jni_report(out);
    fflush(out);
}
