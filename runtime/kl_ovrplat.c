// libovrplatformloader.so — see kl_ovrplat.h for why this is a replacement and
// where the DRM line is drawn.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>              // the cloud-save directory is created
#include "klepton.h"
#include "kl_jni.h"
#include "kl_env.h"
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

// ---------------------------------------------------------------------------
// The DRM classifier
//
// Matched on substrings of the entry point name rather than an enumerated list,
// deliberately: the export surface is 1335 wide and a list would be the thing
// that goes stale. A new ovr_Entitlement_* or ovr_IAP_* added by a future SDK
// lands on the refusal by default, which is the direction an error should fall.
//
// What is covered and why:
//   Entitle*  — "is this user allowed to run this", the licence check. Note the
//               g_plat_absent carve-out below: the APP's own entitlement is
//               answered, everything else in the family still refuses.
//   IAP       — purchases and their ownership records.
//   AssetFile — download and unlock of purchasable DLC; the delivery half of the
//               same question. ovr_AssetFile_* is how paid content arrives.
//               Note the g_plat_absent carve-out above: the pure *enumeration*
//               call is answered with "nothing here", which grants no access.
//   Purchase  — belt and braces for the SDK's other spelling of IAP.
static const char *const g_drm_markers[] = {
    "Entitle", "Entitlement", "IAP", "Purchase", "AssetFile", "AssetDetails",
};

// The carve-outs: names in the ownership families that we ANSWER rather than
// refuse. There are exactly two, they earn it for two DIFFERENT reasons, and
// neither reason generalises to the rest of the family — which is why these are
// exact names, not substrings, and are checked BEFORE the markers below, so the
// marker list keeps its fail-closed default and anything new still lands on the
// refusal.
//
//   ovr_AssetFile_GetList — enumerates the DLC asset files present on this
//     device. There are none, and saying so is a fact about this host rather
//     than a licence decision. It unlocks nothing and is strictly MORE
//     restrictive than the truth: no content is reachable through a run that
//     answers it which was not reachable through a run that aborted on it. It
//     carries no entitlement flag — the ownership question is asked by the
//     Entitlement/IAP families, and everything that could DELIVER content
//     (ovr_AssetFile_Download*) stays refused.
//
//   ovr_Entitlement_GetIsViewerEntitled — the APPLICATION's own licence check,
//     and the one question in this family we can actually answer. Klepton runs
//     an APK the user unpacked from a device they own, i.e. from a copy of the
//     app they already bought; answering yes describes that, it does not
//     manufacture it. This is a deliberate move of the line and it is confined
//     to the app: DLC ownership stays refused because it is genuinely
//     unanswerable here — paid content is out-of-band asset data plus a licence
//     held by a platform that is absent from this host, so neither half is
//     present to check, and claiming it IS the circumvention. It is answered
//     for real (klplat_Entitlement_GetIsViewerEntitled) rather than through the
//     request path below, because it returns its answer directly.
//
// The remaining entries are answered through klplat_init_fails, i.e. request id
// 0: the platform request APIs are asynchronous and ovr_PopMessage never
// produces a message, so a plausible id would leave the caller polling forever
// for a completion that cannot come. 0 puts it on the error path it already has.
static const char *const g_plat_absent[] = {
    "ovr_AssetFile_GetList",
    "ovr_Entitlement_GetIsViewerEntitled",
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

// Ownership and licence queries — everything the carve-out above does NOT name,
// which after it is the CONTENT half: IAP, purchase records, asset details, and
// the delivery calls. This aborts unconditionally — KL_PERMISSIVE is a scouting
// knob for things we intend to implement, and this is not one of them. Answering
// 0 would be worse than useless here: for a call shaped like ovr_IAP_GetProducts
// or ovr_AssetFile_DownloadById a fabricated answer is the circumvention itself,
// and a permissive run would produce it silently.
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
    if (kl_permissive()) {
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
// The completion queue — an asynchronous request has to ARRIVE somewhere
//
// This is the half the "report the platform as absent" design was missing, and
// Beat Saber 1.40 is where it stopped being free. Every request-shaped entry
// point here answered `ovrRequest` 0 under a comment saying that a plausible id
// "would be a lie with a tail: the caller would then poll ovr_PopMessage
// forever for a completion that cannot come". The measurement says **0 has the
// same tail**: the SDK builds its `Request` object either way and awaits it, so
// what 0 bought was not an error path but silence.
//
// 1.40 gates a screen on that silence. Its `HealthWarningFlowCoordinator` hands
// the epilepsy screen's Continue to `WaitForUserAgeCategory`, a coroutine
// waiting on a category that `OculusInit.CheckUserAgeCategoryAsync` never sets
// because `InitializeOculusAsync` is still awaiting the platform init — so the
// button did nothing, silently, with the frame loop running normally. The whole
// visible symptom was one un-delivered message.
//
// **What is asserted here, and by whose decision.** Answering the age category
// at all means claiming an initialized platform, which is a real change from
// "genuinely absent" — an absent platform cannot answer a demographic question.
// The user asked for it and declared themselves over 18 (2026-08-12), which is
// the only place that answer can legitimately come from here: it is a
// self-declaration by the person running their own copy, not a fact we
// discovered. **The DRM line does not move with it** — IAP, purchase records,
// asset details and every delivery call still abort unconditionally, and
// `check_drm_guard` still asserts both directions. What changes is that the
// application's own entitlement, already answered yes, is now answered through
// the channel the guest actually reads.
//
// The three completions are all this queues, and each is issued only in
// response to the guest's own request:
//
//   ovr_UnityInitWrapperAsynchronous  -> Platform_InitializeAndroidAsynchronous,
//                                        result Success
//   ovr_Entitlement_GetIsViewerEntitled -> Entitlement_GetIsViewerEntitled, no
//                                        payload; success IS the answer
//   ovr_UserAgeCategory_Get           -> UserAgeCategory_Get, category Ad(ult)
//
// **The numbers are the guest's own**, read out of the running IL2CPP runtime
// with `KL_PROBE_ENUM` (kl_mprobe.c) rather than taken from an SDK header we do
// not have — the same move as OVRP_HEADSET_OCULUS_QUEST_2 in kl_ovrp.c, and
// necessary rather than tidy: `Message.ParseMessageHandle` switches on the type
// and its `default` arm logs "Unrecognized message type" and produces NO
// message, so a guessed number is indistinguishable from the silence above.
#define KLPLAT_MSG_PLATFORM_INIT   450037684u   // Message.MessageType
#define KLPLAT_MSG_ENTITLEMENT     409688241u   //   .Platform_InitializeAndroidAsynchronous
#define KLPLAT_MSG_AGE_CATEGORY    567009472u   //   .Entitlement_GetIsViewerEntitled
#define KLPLAT_MSG_APP_VERSION    1751583246u   //   .UserAgeCategory_Get
                                                //   .Application_GetVersion
#define KLPLAT_MSG_CLOUD_DIR      1990471406u   //   .CloudStorage2_GetUserDirectoryPath
#define KLPLAT_INIT_SUCCESS        0            // PlatformInitializeResult.Success
#define KLPLAT_AGE_ADULT           3            // AccountAgeCategory.Ad

// A ring, because the guest holds a message only until it has read it and calls
// ovr_FreeMessage; three per run is the whole traffic, so 16 slots cannot wrap
// under a live message. The mutex is not defensive — the requests are issued
// from managed code on the main thread and the pump runs there too, but nothing
// in the API promises that.
#define KLPLAT_MSGQ 16
// `str` is the STRING payload, for the messages whose managed wrapper is a
// `Message<string>` — it is read through ovr_Message_GetString rather than
// through the int payload above, and a message carries one or the other.
typedef struct {
    uint32_t type; uint64_t request; int32_t payload; const char *str;
} klplat_msg;
static klplat_msg      g_msgq[KLPLAT_MSGQ];
static unsigned        g_msg_head, g_msg_tail;
static uint64_t        g_next_request = 1;
static int             g_platform_up;
static pthread_mutex_t g_msg_mu = PTHREAD_MUTEX_INITIALIZER;

// Issue a request and queue its completion. Returns the ovrRequest the guest
// will wait on — non-zero, and matched by the message, which together are the
// whole point.
static uint64_t klplat_request_str(const char *what, uint32_t type,
                                   int32_t payload, const char *str) {
    pthread_mutex_lock(&g_msg_mu);
    uint64_t id = g_next_request++;
    if (g_msg_tail - g_msg_head < KLPLAT_MSGQ) {
        klplat_msg *m = &g_msgq[g_msg_tail++ % KLPLAT_MSGQ];
        m->type = type; m->request = id; m->payload = payload; m->str = str;
    }
    pthread_mutex_unlock(&g_msg_mu);
    fprintf(stderr, "  [plat] %s -> request %llu, completion queued "
                    "(type 0x%08x, payload %d%s%s)\n",
            what, (unsigned long long)id, type, payload,
            str ? ", string " : "", str ? str : "");
    return id;
}

// ...and the int-payload spelling, which is all but one caller.
static uint64_t klplat_request(const char *what, uint32_t type, int32_t payload) {
    return klplat_request_str(what, type, payload, NULL);
}

// The message pump. NULL still means "no messages queued", which is the
// documented way to say it and is the steady state for all but three calls a
// run — what changed is that it is no longer the ONLY thing this can say.
static void *klplat_PopMessage(void) {
    plat_hit("ovr_PopMessage");
    void *out = NULL;
    pthread_mutex_lock(&g_msg_mu);
    if (g_msg_head != g_msg_tail) out = &g_msgq[g_msg_head++ % KLPLAT_MSGQ];
    pthread_mutex_unlock(&g_msg_mu);
    return out;
}

// The accessors the SDK's Message base class and the two payload wrappers call
// on that handle. Real functions rather than named trampolines, because these
// take the handle in x0 and a trampoline would put the entry point's NAME there.
//
// ovr_Message_GetError is deliberately absent: nothing here is ever an error, so
// the guest never asks, and the fail-closed default would name it if that
// changed. Same for every other Message accessor — a message shape we do not
// produce should stop the run by name rather than answer.
static uint64_t klplat_Message_GetType(const klplat_msg *m) {
    plat_hit("ovr_Message_GetType");
    return m ? m->type : 0;
}
static uint64_t klplat_Message_IsError(const klplat_msg *m) {
    plat_hit("ovr_Message_IsError");
    (void)m;
    return 0;
}
static uint64_t klplat_Message_GetRequestID(const klplat_msg *m) {
    plat_hit("ovr_Message_GetRequestID");
    return m ? m->request : 0;
}
// The payload handle IS the message: there is one payload per message here, so
// a separate allocation would only be a second thing to keep alive.
static uint64_t klplat_Message_GetPayload(const klplat_msg *m) {
    plat_hit("ovr_Message_GetPayload");
    return (uint64_t)(uintptr_t)m;
}
static uint64_t klplat_payload_int(const klplat_msg *m) {
    plat_hit("ovr_Payload_GetInt");
    return m ? (uint64_t)(uint32_t)m->payload : 0;
}
// ...and the string one. `Message<string>` reads its data with this rather than
// through the payload handle, so a message whose managed wrapper is a string
// carries `str` and answers here. NULL for every other message, which is what
// the SDK expects for one that has no string.
static const char *klplat_Message_GetString(const klplat_msg *m) {
    plat_hit("ovr_Message_GetString");
    return m ? m->str : NULL;
}
static uint64_t klplat_FreeMessage(klplat_msg *m) {
    plat_hit("ovr_FreeMessage");
    (void)m;                      // the ring owns it; see the KLPLAT_MSGQ note
    return 0;
}

// ---------------------------------------------------------------------------
// What we do implement: the platform reporting its own absence, honestly.
//
// ovr_IsPlatformInitialized answers false, because it is not initialised and
// never will be. That is the same answer a real headset would give before init,
// so the guest's own "platform unavailable" path is a path it already has — its
// metadata carries "Oculus Platform failed to initialize." and
// "Initialize Error: Oculus platform failed to initialize due to exception."
// ...and it answers TRUE once an init request has been completed, because the
// SDK gates every other request on it — `Entitlements.IsUserEntitledToApplication`
// and `Users.GetUserAgeCategory` both begin `if (Core.IsInitialized())`, and
// Core.IsInitialized() IS this call. Answering false there is not a cautious
// answer, it is a guarantee that no request is ever made and so no completion
// is ever awaited-and-delivered; the two halves have to agree.
static uint64_t klplat_IsPlatformInitialized(void) {
    plat_hit("ovr_IsPlatformInitialized");
    return (uint64_t)g_platform_up;
}

// ...and the SYNCHRONOUS Android wrapper, which is the same decision reached
// through a different door — and a different RETURN TYPE, which is what made it
// worth measuring rather than grouping.
//
// `Oculus.Platform.Core.Initialize(appId)` calls this one and THROWS when it is
// false: `UnityException("Oculus Platform failed to initialize.")`. It is not an
// ovrRequest and not a PlatformInitializeResult — the real export builds a byte
// (`and w8, w8, #1; strb w8, [sp, ...]`), i.e. a plain bool, checked in both
// this tree's loaders (superhot +0x51934, beatsaber +0x8aae4). Trap 10's shape
// exactly: grouped with the request-returning init family it would answer 0,
// and 0 in a bool is false.
//
// Why it answers TRUE, given the file's standing objection to inventing success:
// that objection is about a request id nothing completes, and it no longer
// applies. This branch has a message queue, the async init already answers
// success and sets g_platform_up, and the app's own entitlement completes
// through it. Answering false HERE while answering success THERE is the two
// halves disagreeing — and the cost of the disagreement is not an error path,
// it is the whole title: a guest running the stock
// `Oculus.Platform.Samples.EntitlementCheck` behaviour catches the throw, runs
// its failure handler, and calls `UnityEngine.Application.Quit()`. Unity's quit
// gate then makes `nativeRender` a no-op for the rest of the process's life, so
// what a person sees is not an error message but a frozen frame, reprojected
// forever, with every counter healthy. SUPERHOT VR is such a guest.
//
// **The DRM line does not move with this.** Asking to connect to the platform is
// not an ownership question (that is why this family was never among the
// refusals); IAP, purchase records, asset details and every delivery call still
// abort unconditionally and check_drm_guard still asserts both directions.
// What init being answered buys is that the app's own licence check — the one
// carve-out this project deliberately made — becomes REACHABLE. A guest that
// quits before it can ask is not a guest being refused; it is one that never got
// to the question.
//
// Deliberately narrow: only this name. The Asynchronous / Standalone spellings
// have their own answers above, and the `ovr_PlatformInitialize*` family returns
// an `ovrPlatformInitializeResult` (a THIRD convention, whose Success is 0).
static uint64_t klplat_UnityInitWrapper(const char *app_id) {
    plat_hit("ovr_UnityInitWrapper");
    g_platform_up = 1;
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] ovr_UnityInitWrapper(\"%s\") -> true (the "
                        "synthetic platform reports it came up, so the app's own "
                        "entitlement check can be asked and answered; every "
                        "ownership query still refuses)\n", app_id ? app_id : "");
    }
    return 1;
}

// The app's own licence check — see the g_plat_absent carve-out above for why
// this one is answered and the rest of the family is not: the user unpacked
// their own APK, so a licence to the application itself is something we can
// assert, while DLC needs out-of-band asset data and a licence we genuinely
// cannot verify. Non-zero is load-bearing rather than incidental — 0 here is
// the guest's licence-FAILURE path, not a neutral answer — so m_boot's
// check_drm_guard asserts the value, in both directions along with the
// refusals it sits beside.
// It is a REQUEST, not a predicate — `Entitlements.IsUserEntitledToApplication()`
// wraps its return in a `Request` and awaits the completion, so the old bare 1
// was request id 1 with nothing ever arriving for it. The answer is unchanged
// and so is the reasoning above; it is now delivered where the guest reads it,
// as a non-error completion, which is how this API spells "entitled".
static uint64_t klplat_Entitlement_GetIsViewerEntitled(void) {
    plat_hit("ovr_Entitlement_GetIsViewerEntitled");
    return klplat_request("ovr_Entitlement_GetIsViewerEntitled",
                          KLPLAT_MSG_ENTITLEMENT, 0);
}

// Where the user's saves live — and the one member of the CloudStorage family
// that is answered rather than refused.
//
// The rest of that family stays refused, and the split is the same one the DRM
// line is drawn along: `ovr_CloudStorage_Load` / `_Save` move DATA to and from a
// service that is not here, so 0 ("the request could not be made") is the truth
// about them. This one moves no data at all. It asks where on the local
// filesystem the save directory IS — the directory the platform would later
// synchronise — and that is a question this host can answer completely.
//
// Refusing it is what a black launch screen looks like. SUPERHOT's
// `VRSaveManager::LoadAsync` chains its whole load off this request, so a 0
// leaves the SDK waiting on a completion it will never post: no save data, no
// scene, and the launch precache's own `ClearRenderTarget` command buffer parked
// on the camera in the meantime — which renders as black, forever, with every
// frame counter healthy. "A game that cannot reach the cloud writes locally" is
// only true if something tells it where local is.
//
// It is a REQUEST returning `Request<string>` (the SDK's `MessageWithString`),
// so the path arrives as a message payload read through ovr_Message_GetString —
// not as a return value. The type number is the guest's own, read out of the
// running IL2CPP runtime with KL_PROBE_ENUM like the three above it.
//
// The directory is created, and that is not a convenience: the guest calls its
// own IsDirectoryWritable on the answer before it uses it, and a path that does
// not exist fails that test — which is the same stall wearing a different hat.
// It sits beside the guest's files directory, so it is per-target and follows
// KL_FILES_DIR like everything else the guest writes.
static const char *klplat_cloud_dir(void) {
    static char dir[1024];
    if (!*dir) {
        const char *env = kl_env_str("KL_PLAT_CLOUD_DIR", NULL);
        if (env && *env) snprintf(dir, sizeof dir, "%s", env);
        else snprintf(dir, sizeof dir, "%s/cloudstorage", kl_jni_files_dir());
        mkdir(dir, 0755);
    }
    return dir;
}

static uint64_t klplat_CloudStorage2_GetUserDirectoryPath(void) {
    plat_hit("ovr_CloudStorage2_GetUserDirectoryPath");
    return klplat_request_str("ovr_CloudStorage2_GetUserDirectoryPath",
                              KLPLAT_MSG_CLOUD_DIR, 0, klplat_cloud_dir());
}

// The age category, and the ONE answer in this file that is a declaration
// rather than a measurement. See the completion-queue comment above: the user
// declared themselves over 18 and this reports it; nothing here discovered it.
// `Ad` is Oculus.Platform.AccountAgeCategory's own spelling, read out of the
// guest, and Beat Saber maps it to its `UserAgeCategory.Adult`.
static uint64_t klplat_UserAgeCategory_Get(void) {
    plat_hit("ovr_UserAgeCategory_Get");
    return klplat_request("ovr_UserAgeCategory_Get",
                          KLPLAT_MSG_AGE_CATEGORY, KLPLAT_AGE_ADULT);
}

// The application's own version, which is the one question in this family we can
// answer from something we actually have: the unpacked tree's apktool.yml, i.e.
// exactly what PackageManager already tells the guest. `OculusInit`'s
// GetAppVersionQuestAsync asks it during startup, behind the init completion.
//
// LATEST is answered as CURRENT, and that is a statement rather than a dodge:
// there is no store here to ask what the newest build is, and "the installed
// one is the newest one" is the only self-consistent thing to say — it means
// "no update available", which is true of this host in the only sense available
// to it. Reporting a HIGHER latest would advertise an update that cannot be
// fetched, which is what drives Application_StartAppDownload.
static uint64_t klplat_Application_GetVersion(void) {
    plat_hit("ovr_Application_GetVersion");
    return klplat_request("ovr_Application_GetVersion", KLPLAT_MSG_APP_VERSION, 0);
}

static uint64_t klplat_AppVersion_code(const void *h) {
    plat_hit("ovr_ApplicationVersion_GetCode");
    (void)h;
    long code = 0; kl_jni_guest_version(&code, NULL);
    return (uint64_t)(int64_t)code;
}
// The two fields of the same model we cannot answer from anything here. A
// release date and a download size are properties of a STORE listing, and there
// is no store; 0 is the SDK's own "not published" rather than a stand-in, and
// the model reads every field at construction whether or not it uses them.
static uint64_t klplat_AppVersion_unknown(const void *h) {
    plat_hit("ovr_ApplicationVersion_unknown_field");
    (void)h;
    return 0;
}

static const char *klplat_AppVersion_name(const void *h) {
    plat_hit("ovr_ApplicationVersion_GetName");
    (void)h;
    const char *name = NULL; kl_jni_guest_version(NULL, &name);
    return name ? name : "";
}

// The ASYNCHRONOUS init. Its completion is what everything else waits behind,
// so this is also where the platform becomes "up" for ovr_IsPlatformInitialized.
// Only the async spellings come here: the synchronous ones return a
// PlatformInitializeResult directly and keep their old answer.
static uint64_t klplat_init_async(const char *name) {
    plat_hit(name);
    g_platform_up = 1;
    return klplat_request(name, KLPLAT_MSG_PLATFORM_INIT, KLPLAT_INIT_SUCCESS);
}

static const char *const g_plat_init_async[] = {
    "ovr_UnityInitWrapperAsynchronous",
    "ovr_PlatformInitializeAndroidAsynchronous",
    "ovr_PlatformInitializeAndroidAsynchronousWithOptions",
    "ovr_PlatformInitializeWindowsAsynchronous",
};

static int plat_is_init_async(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_init_async / sizeof g_plat_init_async[0]; i++)
        if (strcmp(g_plat_init_async[i], name) == 0) return 1;
    return 0;
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
    // Audio-plugin enumeration, dlsym'd speculatively by libunity's
    // AudioPluginManager on every plugin handle. A COUNT of effect definitions,
    // so 0 is "none here" — which is what the real loader says by not exporting
    // it. Same answer, same reasoning, as kl_ovrp.c's copy.
    "UnityGetAudioEffectDefinitions",
    // ovr_FreeMessage used to sit here, under "ovr_PopMessage never hands out a
    // message, so there is never anything to free". It does now, so it has a
    // real implementation above.
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

// ovr_UnityInitWrapper is NOT here — it is a bool, not an ovrRequest, and it has
// its own implementation above. It is the only one of these whose contract can
// report success without owing anybody a completion message.
static const char *const g_plat_init[] = {
    "ovr_UnityInitWrapperAsynchronous",
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
//   ovr_Voip_SetMicrophoneMuted(ovrVoipMuteState) — the local mic gate for
//     in-game voice chat. RE4's OculusPlatform module pushes it during startup.
//     Nothing here captures audio at all — kl_aaudio refuses capture streams by
//     design and no microphone is presented — so there is no stream to mute and
//     nothing the guest can learn from the call. It is deliberately NOT filed
//     with the enumeration-answers-none group: those state an absence, and this
//     one is a command with no reply.
static const char *const g_plat_drop[] = {
    "ovr_Log_NewEvent",
    "ovr_Voip_SetMicrophoneMuted",
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

// ---------------------------------------------------------------------------
// Launch details: HOW the app was started.
//
// Not a request — a plain accessor returning an opaque handle that the managed
// SDK immediately picks apart, reading a dozen ovr_LaunchDetails_* fields off it
// in `Models.LaunchDetails`'s constructor. SUPERHOT asks while building its main
// scene.
//
// The truthful answer is "normally, by the user, with nothing attached": no
// deeplink message, no destination, no room, no invited users, LaunchType 0
// (Unknown). That is precisely what every accessor answers below, because 0 is
// simultaneously the zero enum, the zero integer and the NULL pointer — and the
// SDK null-checks each optional list before wrapping it, so NULL is how it
// spells "there was none", not a hole.
//
// The handle itself must be NON-NULL for the same reason the options cookie is:
// it is carried straight into those accessors, and on a real platform a NULL
// there is a null dereference inside the library rather than an empty answer.
static const char g_launch_cookie[] = "klepton-launch-details";

static uint64_t klplat_launch_details(const char *name) {
    plat_hit(name);
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] %s -> opaque handle: launched normally, by the "
                        "user, with nothing attached (no deeplink, no destination, "
                        "no invited users)\n", name);
    }
    return (uint64_t)(uintptr_t)g_launch_cookie;
}

static uint64_t klplat_launch_field(const char *name) {
    plat_hit(name);
    return 0;                 // the zero enum, the zero int and NULL, all at once
}

static int plat_is_launch_details(const char *name) {
    return strcmp(name, "ovr_ApplicationLifecycle_GetLaunchDetails") == 0;
}

static int plat_is_launch_field(const char *name) {
    return strncmp(name, "ovr_LaunchDetails_", 18) == 0;
}

// ...and the empty ARRAY that a NULL list handle turns into one call later.
//
// The managed `DeserializableList<T>` reads three things off a list handle —
// GetSize, GetElement and GetNextUrl — and this SDK version wraps the pointer
// WITHOUT null-checking it first, so answering ovr_LaunchDetails_GetUsers with
// NULL (correctly: nobody was invited) still ends in ovr_UserArray_GetSize(NULL)
// one call later. Size 0 is the same answer the NULL was: there is nobody in it.
//
// GetElement is deliberately NOT here. It can only be reached on an array with
// elements, which is an array we did not produce, so it keeps the fail-closed
// default and would stop the run by name rather than hand back a User the guest
// would then read fields off.
static int plat_is_empty_array(const char *name) {
    const char *p = strstr(name, "Array_Get");
    if (!p) return 0;
    return strcmp(p, "Array_GetSize") == 0 || strcmp(p, "Array_GetNextUrl") == 0;
}

static uint64_t klplat_empty_array(const char *name) {
    plat_hit(name);
    return 0;                 // no elements, and no next page to fetch them from
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

// Who is signed in — the C SDK's synchronous accessor, not a request. It
// returns an `ovrID`, and 0 is that type's own "no such user": the managed side
// spells the same thing `User == null`, and every caller of it null-checks,
// because a user really can be absent on a real headset (offline, or before the
// platform has initialized).
//
// Not an ownership question and not a request, so neither classifier is right
// for it: `ovr_User_GetLoggedInUser` beside them is the ASYNC form and correctly
// answers "the request could not be made", while this one is asked and answered
// on the spot. RE4 asks during engine init.
static uint64_t klplat_GetLoggedInUserID(void) {
    plat_hit("ovr_GetLoggedInUserID");
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] ovr_GetLoggedInUserID -> 0 (no platform user; "
                        "there is no service here to be signed in to)\n");
    }
    return 0;
}

// The real libovrplatformloader exports JNI_OnLoad and caches the JavaVM out of
// it. Ours has no JNI surface to set up, so the whole body is the version
// number Android checks for.
//
// Reached only because System.load() now honours the Android contract and calls
// JNI_OnLoad on what it loaded (kl_jni.c). Before that, this library was
// dlopen'd and never initialized, and nothing noticed — which is exactly the
// bug that change fixes, so this is the cost of fixing it rather than a
// workaround. Answering the version is not a stub: a library that returns an
// unrecognised version is one that REFUSED to initialize, and Android unloads it.
//
// Below plat_is_drm in kl_ovrplat_sym, like everything in this table, so the
// entitlement classifier still runs first and this cannot be a way around it.
static uint64_t klplat_JNI_OnLoad(void *vm, void *reserved) {
    (void)vm; (void)reserved;
    plat_hit("JNI_OnLoad");
    return 0x00010006;                 // JNI_VERSION_1_6
}

static const struct { const char *name; void *fn; } g_plat_impl[] = {
    {"JNI_OnLoad",                (void *)klplat_JNI_OnLoad},
    {"ovr_IsPlatformInitialized", (void *)klplat_IsPlatformInitialized},
    {"ovr_GetLoggedInUserID",     (void *)klplat_GetLoggedInUserID},
    {"ovr_UnityInitWrapper",      (void *)klplat_UnityInitWrapper},
    {"ovr_PopMessage",            (void *)klplat_PopMessage},
    {"ovr_FreeMessage",           (void *)klplat_FreeMessage},
    {"ovr_Entitlement_GetIsViewerEntitled", (void *)klplat_Entitlement_GetIsViewerEntitled},
    {"ovr_UserAgeCategory_Get",   (void *)klplat_UserAgeCategory_Get},
    {"ovr_Application_GetVersion",(void *)klplat_Application_GetVersion},
    {"ovr_Message_GetApplicationVersion",      (void *)klplat_Message_GetPayload},
    {"ovr_ApplicationVersion_GetCurrentCode",  (void *)klplat_AppVersion_code},
    {"ovr_ApplicationVersion_GetLatestCode",   (void *)klplat_AppVersion_code},
    {"ovr_ApplicationVersion_GetCurrentName",  (void *)klplat_AppVersion_name},
    {"ovr_ApplicationVersion_GetLatestName",   (void *)klplat_AppVersion_name},
    {"ovr_ApplicationVersion_GetReleaseDate",  (void *)klplat_AppVersion_unknown},
    {"ovr_ApplicationVersion_GetSize",         (void *)klplat_AppVersion_unknown},
    // The message accessors. Each takes the handle in x0, so they cannot go
    // through a named trampoline — see the note above klplat_Message_GetType.
    {"ovr_Message_GetType",       (void *)klplat_Message_GetType},
    // "The native message" is this message — the SDK keeps the handle around to
    // hand back to callers that want the raw pointer, and there is nothing
    // behind ours that it is not already holding.
    {"ovr_Message_GetNativeMessage", (void *)klplat_Message_GetPayload},
    {"ovr_Message_IsError",       (void *)klplat_Message_IsError},
    {"ovr_Message_GetRequestID",  (void *)klplat_Message_GetRequestID},
    {"ovr_Message_GetPlatformInitialize",      (void *)klplat_Message_GetPayload},
    {"ovr_PlatformInitialize_GetResult",       (void *)klplat_payload_int},
    {"ovr_Message_GetUserAccountAgeCategory",  (void *)klplat_Message_GetPayload},
    {"ovr_UserAccountAgeCategory_GetAgeCategory", (void *)klplat_payload_int},
    // The string payload, and the one request that carries one.
    {"ovr_Message_GetString",     (void *)klplat_Message_GetString},
    {"ovr_CloudStorage2_GetUserDirectoryPath",
                                  (void *)klplat_CloudStorage2_GetUserDirectoryPath},
};

static const char g_plat_handle[] = "klepton-ovrplatformloader";

int kl_ovrplat_claims(const char *soname) {
    if (!soname) return 0;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    return strcmp(b, "libovrplatformloader.so") == 0 ||
           strcmp(b, "ovrplatformloader") == 0;
}

void *kl_ovrplat_dlopen(const char *soname) {
    if (!kl_ovrplat_claims(soname)) return NULL;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
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
    if (plat_is_launch_details(name))
        return kl_named_stub(name, (void *)klplat_launch_details);
    if (plat_is_launch_field(name))
        return kl_named_stub(name, (void *)klplat_launch_field);
    if (plat_is_empty_array(name))
        return kl_named_stub(name, (void *)klplat_empty_array);
    if (plat_is_options_sink(name))
        return kl_named_stub(name, (void *)klplat_void);
    if (plat_is_init_async(name))
        return kl_named_stub(name, (void *)klplat_init_async);
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
