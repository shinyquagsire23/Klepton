// libovrplatformloader.so — see kl_ovrplat.h for why this is a replacement and
// where the DRM line is drawn.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "klepton.h"
#include "kl_ovrplat.h"

#define KL_OVRPLAT_MAX 512
static struct { const char *name; unsigned calls; int drm; } g_plat[KL_OVRPLAT_MAX];
static unsigned g_nplat;
static unsigned g_drm_calls;

static int plat_slot(const char *name) {
    for (unsigned i = 0; i < g_nplat; i++)
        if (strcmp(g_plat[i].name, name) == 0) return (int)i;
    if (g_nplat >= KL_OVRPLAT_MAX) return -1;
    g_plat[g_nplat].name  = strdup(name);
    g_plat[g_nplat].calls = 0;
    g_plat[g_nplat].drm   = 0;
    return (int)g_nplat++;
}

static void plat_hit(const char *name) {
    int s = plat_slot(name);
    if (s >= 0) g_plat[s].calls++;
}

static int g_permissive = -1;
static int permissive(void) {
    if (g_permissive < 0) g_permissive = getenv("KL_PERMISSIVE") != NULL;
    return g_permissive;
}

// ---------------------------------------------------------------------------
// The DRM classifier
//
// Matched on substrings of the entry point name rather than an enumerated list,
// deliberately: the export surface is 1335 wide and a list would be the thing
// that goes stale. A new ovr_Entitlement_* or ovr_IAP_* added by a future SDK
// lands on the refusal by default, which is the direction an error should fall.
//
// What is covered and why:
//   Entitle*  — "is this user allowed to run this", the app-level licence check.
//   IAP       — purchases and their ownership records.
//   AssetFile — download and unlock of purchasable DLC; the delivery half of the
//               same question. ovr_AssetFile_* is how paid content arrives.
//               Note the g_plat_absent carve-out above: the pure *enumeration*
//               call is answered with "nothing here", which grants no access.
//   Purchase  — belt and braces for the SDK's other spelling of IAP.
static const char *const g_drm_markers[] = {
    "Entitle", "Entitlement", "IAP", "Purchase", "AssetFile", "AssetDetails",
};

// The carve-out: ownership-ADJACENT calls we answer, because the honest answer
// grants nothing.
//
// The line PLANNING 8 draws is against fabricating a POSITIVE entitlement —
// telling the app the user owns something they do not. Reporting that no DLC is
// present is the opposite of that. It unlocks nothing, it is strictly more
// restrictive than the truth, and it is the same "the platform is absent" story
// ovr_IsPlatformInitialized and ovr_PopMessage already tell. No content is
// reachable through a run that answers this which was not reachable through a
// run that aborted on it.
//
// Exact names, not substrings, and checked BEFORE the markers below — so the
// marker list keeps its fail-closed default and anything new still lands on the
// refusal. Each entry has to earn its place on the "enumerates, never grants"
// test:
//
//   ovr_AssetFile_GetList — enumerates the DLC asset files present on this
//     device. There are none, and saying so is a fact about this host rather
//     than a licence decision. It carries no entitlement flag: the ownership
//     question is asked by the Entitlement/IAP families, which stay refused,
//     as does everything that could DELIVER content (ovr_AssetFile_Download*).
//
// Answered through klplat_init_fails, i.e. request id 0: the platform request
// APIs are asynchronous and ovr_PopMessage never produces a message, so a
// plausible id would leave the caller polling forever for a completion that
// cannot come. 0 puts it on the error path it already has.
static const char *const g_plat_absent[] = {
    "ovr_AssetFile_GetList",
};

static int plat_is_absent_ok(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_absent / sizeof g_plat_absent[0]; i++)
        if (strcmp(name, g_plat_absent[i]) == 0) return 1;
    return 0;
}

static int plat_is_drm(const char *name) {
    if (plat_is_absent_ok(name)) return 0;
    for (size_t i = 0; i < sizeof g_drm_markers / sizeof g_drm_markers[0]; i++)
        if (strstr(name, g_drm_markers[i])) return 1;
    return 0;
}

// Ownership and licence queries. This aborts unconditionally — KL_PERMISSIVE is a
// scouting knob for things we intend to implement, and this is not one of them.
// Answering 0 would be worse than useless here: for a call shaped like
// ovr_Entitlement_GetIsViewerEntitled a fabricated answer is the circumvention
// itself, and a permissive run would produce it silently.
static uint64_t klplat_drm(const char *name) {
    int s = plat_slot(name);
    if (s >= 0) { g_plat[s].calls++; g_plat[s].drm = 1; }
    g_drm_calls++;
    fprintf(stderr,
            "\n[klepton] REFUSED: the guest called '%s'.\n"
            "  This is an entitlement / purchase-ownership query. Klepton does not\n"
            "  implement these and does not answer them permissively — inventing a\n"
            "  result is DRM circumvention, which is out of scope (PLANNING 8).\n"
            "  The Oculus platform is genuinely absent on this host; that is the\n"
            "  true state of affairs, and it is the app's decision what to do with\n"
            "  it. If this call is unavoidable, the target is wrong: pick an APK\n"
            "  without entitlement checks (Steam Link, PLANNING 11).\n", name);
    kl_ovrplat_report(stderr);
    kl_fatal_prepare();
    abort();
}

// Everything else: named, so the guest says which of the 1335 it wants.
static uint64_t klplat_called(const char *name) {
    plat_hit(name);
    if (permissive()) {
        int s = plat_slot(name);
        if (s >= 0 && g_plat[s].calls == 1)
            fprintf(stderr, "  [plat] call (permissive, returning 0): %s\n", name);
        return 0;
    }
    fprintf(stderr, "\n[klepton] fatal: guest called unimplemented Oculus Platform "
                    "entry point '%s'\n", name);
    kl_ovrplat_report(stderr);
    kl_fatal_prepare();
    abort();
}

// ---------------------------------------------------------------------------
// What we do implement: the platform reporting its own absence, honestly.
//
// ovr_IsPlatformInitialized answers false, because it is not initialised and
// never will be. That is the same answer a real headset would give before init,
// so the guest's own "platform unavailable" path is a path it already has — its
// metadata carries "Oculus Platform failed to initialize." and
// "Initialize Error: Oculus platform failed to initialize due to exception."
static uint64_t klplat_IsPlatformInitialized(void) {
    plat_hit("ovr_IsPlatformInitialized");
    return 0;
}

// The message pump. Returning NULL means "no messages queued", which is true and
// is the documented way to say it — and it is what stops the poll loop that
// tripped the real library's assert. Note this is *not* a refusal: an empty queue
// is a legitimate steady state, not a fabricated answer.
static void *klplat_PopMessage(void) {
    plat_hit("ovr_PopMessage");
    return NULL;
}

// Unity's native plugin interface. Every entry point in it returns void, and the
// real platform loader uses them only to keep hold of the IUnityInterfaces
// registry and the render event queue — there is nothing here to bind either to.
// Taken as a group because it is one fixed interface Unity calls on every plugin,
// and the same group is already answered this way in kl_ovrp.c.
static uint64_t klplat_void(const char *name) {
    plat_hit(name);
    return 0;
}

static const char *const g_plat_unity[] = {
    "UnityPluginLoad", "UnityPluginUnload", "UnitySetEventQueue",
    "UnitySetGraphicsDevice", "UnityShaderCompilerExtEvent", "UnityRenderEvent",
    // The rendering-extension pair. UnityRenderingExtEvent is void; the Query
    // returns int, and 0 is the truthful answer — this plugin wants no
    // render-thread extension events, so Unity will never issue them.
    "UnityRenderingExtEvent", "UnityRenderingExtQuery",
    // Message lifecycle. ovr_PopMessage never hands out a message, so there is
    // never anything to free — and a free of nothing is genuinely nothing to do,
    // not a stub standing in for something.
    "ovr_FreeMessage",
};

static int plat_is_unity_hook(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_unity / sizeof g_plat_unity[0]; i++)
        if (strcmp(g_plat_unity[i], name) == 0) return 1;
    return 0;
}

// Platform initialisation. Note this is *not* one of the refused calls: asking to
// connect to the platform is not an ownership question, and it has a truthful
// answer here — it fails, because there is no com.oculus.horizon to connect to.
//
// An ovrRequest of 0 is the SDK's "the request could not be made". That is the
// honest report, and it lands the guest on an error path it already has: its own
// metadata carries "Oculus Platform failed to initialize." and "Initialize Error:
// Oculus platform failed to initialize due to exception."
//
// Returning a plausible request id instead would be a lie with a tail: the caller
// would then poll ovr_PopMessage forever for a completion that cannot come.
static uint64_t klplat_init_fails(const char *name) {
    plat_hit(name);
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] %s -> 0 (no Oculus platform service on this host; "
                        "reporting failure rather than inventing success)\n", name);
    }
    return 0;
}

static const char *const g_plat_init[] = {
    "ovr_UnityInitWrapper", "ovr_UnityInitWrapperAsynchronous",
    "ovr_UnityInitWrapperStandalone", "ovr_UnityInitGlobals",
    "ovr_PlatformInitializeAndroid", "ovr_PlatformInitializeAndroidAsynchronous",
    "ovr_PlatformInitializeAndroidAsynchronousWithOptions",
    "ovr_PlatformInitializeWithAccessToken", "ovr_PlatformInitializeStandaloneOculus",
};

static int plat_is_init(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_init / sizeof g_plat_init[0]; i++)
        if (strcmp(g_plat_init[i], name) == 0) return 1;
    return 0;
}

// Request-returning calls with the same truthful answer as init: 0, "the
// request could not be made", because there is no platform service to make it
// of. Answering with a fabricated request id would have the caller polling
// ovr_PopMessage for a completion that cannot come.
static const char *const g_plat_request[] = {
    "ovr_RichPresence_Clear", "ovr_RichPresence_Set",
    "ovr_RichPresence_SetDestination", "ovr_RichPresence_SetIsJoinable",
    // Asking who is logged in is not an ownership question; with no platform
    // service the request cannot be made, which is what 0 says. Beat Saber
    // 1.28 takes its offline path from there (~swap 34k of the boot).
    "ovr_User_GetLoggedInUser",
};

// Whole families where EVERY entry point is request-returning, so the same
// answer is right for all of them and an exact list would only stop the run
// once per name for nothing.
//
//   ovr_Achievements_* — progress and definitions for achievements. All nine
//     calls (AddCount, AddFields, Unlock, GetAllDefinitions, GetAllProgress,
//     GetDefinitionsByName, GetProgressByName and the two ArrayPage walkers)
//     return an ovrRequest. This is not an ownership question: achievements
//     record what the player has done, not what they have bought, and 0 says
//     the request could not be made rather than reporting any progress. The
//     entitlement families are unaffected — plat_is_drm runs first.
//   ovr_GroupPresence_* — "who are you playing with", the multiplayer presence
//     the invite/roster panels read. Ownership does not enter into it, and the
//     trailing underscore keeps this clear of ovr_GroupPresenceOptions_* below,
//     which is a different shape entirely.
static const char *const g_plat_request_prefix[] = {
    "ovr_Achievements_", "ovr_GroupPresence_",
};

// ---------------------------------------------------------------------------
// Options objects: ovr_<Thing>Options_Create / _Set<Field> / _Destroy.
//
// Not requests and not queries — a local builder the caller fills in and then
// hands to a request call. Create returns an opaque handle, so 0 is the one
// answer that is NOT safe here: the guest would carry a NULL through to the
// setters, and on a real platform that is a null deref inside the library.
//
// We hand back a non-NULL cookie instead. Nothing reads it: the setters are
// no-ops because the request the options would feed cannot be made anyway
// (ovr_GroupPresence_Set and friends answer 0 above), so there is no state
// worth keeping and one shared cookie serves every live options object. That
// is invented behaviour in the narrow sense, but it invents no ANSWER — the
// guest learns nothing from it that it did not itself put in.
//
// Entitlement spellings never reach here: plat_is_drm runs first, so
// ovr_IAPOptions_* and the rest still refuse.
// Outbound-only calls: the guest hands us something and asks nothing back.
// Accepted and dropped, which is not a stub standing in for an answer — there
// is no answer involved. Same treatment as the haptics commands in kl_ovrp.c.
//
//   ovr_Log_NewEvent(name, ovrKeyValuePair *, count) — analytics. Telemetry
//     with no service behind it goes nowhere, and a headset that is not
//     attached to an Oculus account has nowhere to send it anyway.
static const char *const g_plat_drop[] = {
    "ovr_Log_NewEvent",
};

static int plat_is_drop(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_drop / sizeof g_plat_drop[0]; i++)
        if (strcmp(g_plat_drop[i], name) == 0) return 1;
    return 0;
}

static const char g_options_cookie[] = "klepton-ovr-options";

static uint64_t klplat_options_create(const char *name) {
    plat_hit(name);
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] %s -> opaque cookie (options objects are local "
                        "state; the request they feed cannot be made)\n", name);
    }
    return (uint64_t)(uintptr_t)g_options_cookie;
}

static int plat_is_options_create(const char *name) {
    return strstr(name, "Options_Create") != NULL;
}

// The setters and the destructor. Both are genuinely nothing to do, not stubs
// standing in for something: there is no options state to keep.
static int plat_is_options_sink(const char *name) {
    return strstr(name, "Options_Destroy") != NULL ||
           strstr(name, "Options_Set") != NULL;
}

static int plat_is_request(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_request / sizeof g_plat_request[0]; i++)
        if (strcmp(g_plat_request[i], name) == 0) return 1;
    for (size_t i = 0; i < sizeof g_plat_request_prefix / sizeof g_plat_request_prefix[0]; i++)
        if (strncmp(name, g_plat_request_prefix[i],
                    strlen(g_plat_request_prefix[i])) == 0) return 1;
    return 0;
}

static const struct { const char *name; void *fn; } g_plat_impl[] = {
    {"ovr_IsPlatformInitialized", (void *)klplat_IsPlatformInitialized},
    {"ovr_PopMessage",            (void *)klplat_PopMessage},
};

static const char g_plat_handle[] = "klepton-ovrplatformloader";

void *kl_ovrplat_dlopen(const char *soname) {
    if (!soname) return NULL;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    if (strcmp(b, "libovrplatformloader.so") != 0 && strcmp(b, "ovrplatformloader") != 0)
        return NULL;
    fprintf(stderr, "  [plat] guest dlopen(\"%s\") -> synthetic Oculus Platform "
                    "(the real one forwards to com.oculus.horizon, which is not "
                    "here; see kl_ovrplat.h)\n", b);
    return (void *)g_plat_handle;
}

int kl_ovrplat_is_handle(const void *h) { return h == (const void *)g_plat_handle; }

void *kl_ovrplat_sym(const char *name) {
    if (!name) return NULL;
    int s = plat_slot(name);                    // resolved, whatever happens next

    // Classified at *resolve* time as well as call time, so kl_ovrplat_report can
    // show what the guest went looking for even on a run where nothing aborted.
    // Resolving is still only a measurement — the refusal is at the call.
    if (plat_is_drm(name)) {
        if (s >= 0) g_plat[s].drm = 1;
        return kl_named_stub(name, (void *)klplat_drm);
    }
    for (size_t i = 0; i < sizeof g_plat_impl / sizeof g_plat_impl[0]; i++)
        if (strcmp(g_plat_impl[i].name, name) == 0) return g_plat_impl[i].fn;
    if (plat_is_unity_hook(name))
        return kl_named_stub(name, (void *)klplat_void);
    if (plat_is_drop(name))
        return kl_named_stub(name, (void *)klplat_void);
    if (plat_is_options_create(name))
        return kl_named_stub(name, (void *)klplat_options_create);
    if (plat_is_options_sink(name))
        return kl_named_stub(name, (void *)klplat_void);
    if (plat_is_absent_ok(name) || plat_is_init(name) || plat_is_request(name))
        return kl_named_stub(name, (void *)klplat_init_fails);
    return kl_named_stub(name, (void *)klplat_called);
}

void kl_ovrplat_report(FILE *f) {
    static int done;
    if (done || !g_nplat) return;
    done = 1;
    unsigned called = 0, drm = 0;
    for (unsigned i = 0; i < g_nplat; i++) {
        if (g_plat[i].calls) called++;
        if (g_plat[i].drm)   drm++;
    }
    fprintf(f, "\n=== Oculus Platform surface ===\n");
    fprintf(f, "  resolved: %u, of which called: %u\n", g_nplat, called);
    if (drm) {
        fprintf(f, "  --- entitlement / ownership (REFUSED by policy, see PLANNING 8) ---\n");
        for (unsigned i = 0; i < g_nplat; i++)
            if (g_plat[i].drm)
                fprintf(f, "    %-52s %s\n", g_plat[i].name,
                        g_plat[i].calls ? "CALLED" : "resolved only");
        fprintf(f, "  (%u DRM call%s refused)\n", g_drm_calls, g_drm_calls == 1 ? "" : "s");
    }
    fprintf(f, "  --- called ---\n");
    for (unsigned i = 0; i < g_nplat; i++)
        if (g_plat[i].calls && !g_plat[i].drm)
            fprintf(f, "    %-52s x%u\n", g_plat[i].name, g_plat[i].calls);
    fprintf(f, "  --- resolved but never called ---\n");
    for (unsigned i = 0; i < g_nplat; i++)
        if (!g_plat[i].calls && !g_plat[i].drm) fprintf(f, "    %s\n", g_plat[i].name);
}
