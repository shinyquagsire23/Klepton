//
// Environment var helper functions
//
#include "kl_env.h"

#include <crt_externs.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// The lookup every helper below uses, and why it is not getenv().
//
// libc's getenv takes a lock. These helpers are called from everywhere,
// including once or more per guest frame and from klb_pthread_kill — and an
// IL2CPP/Boehm guest stops the world by SIGNALLING its threads, which park in
// sigsuspend inside the handler wherever they happened to be. A thread stopped
// inside getenv holds that lock; the collector thread then calls
// klb_pthread_kill to signal the next one, which reads a knob, which waits for
// the lock — held by a thread that only the collector can resume. Seen as a
// host run frozen for good at frame 300 with three threads in
// _os_unfair_lock_lock_slow under getenv and the main thread in sigsuspend
// under it; on a headset the same thing is a picture that stops.
//
// Walking environ takes no lock. What that gives up is safety against a
// concurrent setenv moving the array, and the only setenv calls here are the
// per-target defaults made during boot, before the guest has threads.
static const char *env_get(const char *name) {
    char ***ep = _NSGetEnviron();
    char **e = ep ? *ep : NULL;
    if (!e || !name) return NULL;
    size_t n = strlen(name);
    for (; *e; e++)
        if (!strncmp(*e, name, n) && (*e)[n] == '=') return *e + n + 1;
    return NULL;
}

// KL_VERBOSE_ALL / KL_FULL: a master switch for a full-debug run. When on, any
// boolean flag whose NAME reads diagnostic (a trace / probe / verbose / log /
// debug / dump / mirror knob) defaults ON without having to be listed. Behavioural
// flags — the ones that change what the runtime DOES, not what it PRINTS — do not
// match these tokens and are untouched, so this only ever adds logging. The launch
// script (build_run_vpro.sh) additionally expands it to the numeric/string diag
// knobs (KL_SAMPLE_MS, KL_TRACE_FS=fail, ...) that a bool switch can't express.
static int verbose_all(void) {
    static int v = -1;
    if (v < 0) {
        const char *s = env_get("KL_VERBOSE_ALL");
        if (!s) s = env_get("KL_FULL");
        v = s && *s && strcmp(s, "0") && strcasecmp(s, "off")
            && strcasecmp(s, "no") && strcasecmp(s, "false");
    }
    return v;
}

// Only PURE-LOGGING knobs: names that merely make the runtime print more. The
// PROBE/DUMP/DEBUG/MIRROR knobs are deliberately NOT here — several of them
// trigger BEHAVIOUR, not just logging (e.g. KL_OVRP_DUMP_VRDEVICE walks a guest
// object at a build-specific hardcoded offset and crashes on a libunity it was not
// written for). The launch script (build_run_vpro.sh) enables the specific,
// vetted probe/dump flags it wants explicitly; this runtime net stays conservative
// so KL_FULL can never silently arm a risky diagnostic on an unknown build.
static int is_diag_name(const char *n) {
    // The per-call GL trace family (KL_GLFB_TRACE / _TEX / _FBO / _BUF) routes EVERY
    // guest GL call through a logging trampoline — ~22M lines / ~1GB for a single hl2
    // session, which slows GL-heavy titles (ANGLE) to a crawl: the map never finishes
    // loading, so the loading eye-capture path holds a STALE menu frame on the eyes
    // (looks like "the last menu frame flashed during gameplay"). Far too heavy to
    // ride KL_FULL — same opt-in rationale as KL_GLFB_BLIT_LOG. Enable it explicitly
    // (KL_GLFB_TRACE=1) when you actually need the per-call stream; the getenv path
    // below still honours that. Only the KL_FULL auto-arm is suppressed here.
    if (!strncmp(n, "KL_GLFB_TRACE", 13)) return 0;
    return strstr(n, "VERBOSE") || strstr(n, "TRACE") || strstr(n, "_LOG");
}

int kl_env_on(const char *name, int dflt) {
    const char *v = env_get(name);
    if (!v) {
        if (!dflt && verbose_all() && is_diag_name(name)) return 1;
        return dflt;
    }
    return !(!*v || !strcmp(v, "0") || !strcasecmp(v, "no") || !strcasecmp(v, "n")
             || !strcasecmp(v, "off") || !strcasecmp(v, "false"));
}

const char *kl_env_str(const char *name, const char *dflt) {
    const char *v = env_get(name);
    if (!v) return dflt;
    return v;
}

int kl_env_int(const char *name, int dflt) {
    const char *v = env_get(name);
    if (!v) return dflt;
    return strtol(v, NULL, 0);
}

unsigned int kl_env_uint(const char *name, unsigned int dflt) {
    const char *v = env_get(name);
    if (!v) return dflt;
    return strtoul(v, NULL, 0);
}


float kl_env_float(const char *name, float dflt) {
    const char *v = env_get(name);
    if (!v) return dflt;
    return atof(v);
}

static int g_permissive = -1;
int kl_permissive(void) {
    if (g_permissive < 0) g_permissive = env_get("KL_PERMISSIVE") != NULL;
    return g_permissive;
}