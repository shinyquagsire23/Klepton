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
#include "guest/kl_driver.h"   // target name, for the per-target KL_PLAT_USER default
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
    // The GetList completion's readers. They match the "AssetDetails"/"AssetFile"
    // DRM markers by substring, but they only READ the empty list GetList
    // returned - no id, no size, no delivery - so they carve out for the same
    // reason GetList does: strictly more restrictive than the truth. The
    // DELIVERY calls (ovr_AssetFile_Download*, DownloadById) are not here and
    // stay refused.
    "ovr_Message_GetAssetFileDeliveryList",
    "ovr_AssetFileDeliveryList_GetSize",
    "ovr_Message_GetAssetDetailsArray",
    "ovr_AssetDetailsArray_GetSize",
};

static int plat_is_absent_ok(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_absent / sizeof g_plat_absent[0]; i++)
        if (strcmp(name, g_plat_absent[i]) == 0) return 1;
    return 0;
}

// The IAP CATALOG calls — enumeration of what is for sale, not what is owned.
// ovr_IAP_GetProductsBySKU returns a ProductArray of {sku, name, description,
// formatted price}: the store's shop window, read to DECIDE whether to offer a
// purchase. It reports no ownership and delivers no content, and on a host with
// no store the truthful answer is "the request could not be made" (request 0),
// the same answer ovr_User_GetLoggedInUser gives. This is the IAP twin of the
// ovr_AssetFile_GetList carve-out: a pure enumeration is strictly MORE
// restrictive than the truth. The OWNERSHIP half of the family is untouched and
// still refuses — ovr_IAP_GetViewerPurchases (what the user bought),
// ovr_IAP_ConsumePurchase, ovr_IAP_LaunchCheckoutFlow and every delivery call
// keep matching the "IAP"/"Purchase" markers below, because this list is exact
// names checked first, not a substring. GetNextProductArrayPage walks the same
// catalog array, so it carves out for the same reason.
static int plat_is_iap_catalog(const char *name) {
    return strcmp(name, "ovr_IAP_GetProductsBySKU") == 0 ||
           strcmp(name, "ovr_IAP_GetNextProductArrayPage") == 0;
}

// The read side of the OWNED half: "which IAPs has this viewer already bought?"
// This looks like an ownership query, and it is — but answering it truthfully on
// a host with no Oculus platform is "the request could not be made" (request id
// 0), exactly what ovr_User_GetLoggedInUser and the IAP catalog return. That is
// the OPPOSITE of circumvention: the guest is handed no purchase message, so it
// learns of no entitlement it did not already have. Fabrication would be
// DELIVERING a PurchaseArray that names something as owned, or answering the
// WRITE side — ovr_IAP_ConsumePurchase / ovr_IAP_LaunchCheckoutFlow and the
// AssetFile delivery calls — all of which stay in plat_is_drm and keep aborting.
// So the honest failure is carved out and the grant-shaped calls are not.
static int plat_is_iap_owned_query(const char *name) {
    return strcmp(name, "ovr_IAP_GetViewerPurchases") == 0 ||
           strcmp(name, "ovr_IAP_GetViewerPurchasesDeltaAsync") == 0;
}

static int plat_is_drm(const char *name) {
    if (plat_is_absent_ok(name)) return 0;
    if (plat_is_iap_catalog(name)) return 0;
    if (plat_is_iap_owned_query(name)) return 0;
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
            "  result is DRM circumvention, which is out of scope.\n"
            "  The Oculus platform is genuinely absent on this host; that is the\n"
            "  true state of affairs, and it is the app's decision what to do with\n"
            "  it. If this call is unavoidable, the target is wrong: pick an APK\n"
            "  without entitlement checks (Steam Link).\n", name);
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
// An async request gets a real id and a message that arrives via
// ovr_Message_Get*. Answering `ovrRequest` 0 has the SAME "poll forever" tail
// as a fabricated id — the SDK builds its `Request` object either way and
// awaits it — so 0 buys silence, not an error path.
//
// Beat Saber 1.40 gates a screen on that silence: HealthWarningFlowCoordinator
// hands the epilepsy screen's Continue to WaitForUserAgeCategory, a coroutine
// waiting on a category OculusInit.CheckUserAgeCategoryAsync never sets because
// InitializeOculusAsync is still awaiting platform init. The button does
// nothing, silently, with the frame loop running normally.
//
// What is asserted, and by whose decision. Answering the age category at all
// claims an initialized platform, which is a real change from "genuinely
// absent" — an absent platform cannot answer a demographic question. The user
// asked for it and declared themselves over 18 (2026-08-12), which is the only
// place that answer can legitimately come from here: a self-declaration by the
// person running their own copy, not a discovered fact. THE DRM LINE DOES NOT
// MOVE WITH IT — IAP, purchase records, asset details and every delivery call
// still abort unconditionally, and check_drm_guard still asserts both
// directions. What changes is that the application's own entitlement, already
// answered yes, is answered through the channel the guest actually reads.
//
// The three completions are all this queues, each issued only in response to
// the guest's own request:
//
//   ovr_UnityInitWrapperAsynchronous  -> Platform_InitializeAndroidAsynchronous,
//                                        result Success
//   ovr_Entitlement_GetIsViewerEntitled -> Entitlement_GetIsViewerEntitled, no
//                                        payload; success IS the answer
//   ovr_UserAgeCategory_Get           -> UserAgeCategory_Get, category Ad(ult)
//
// The type numbers are the guest's own, read out of the running IL2CPP runtime
// with KL_PROBE_ENUM (kl_mprobe.c) rather than from an SDK header. Necessary
// rather than tidy: Message.ParseMessageHandle switches on the type and its
// default arm logs "Unrecognized message type" and produces NO message, so a
// guessed number is indistinguishable from the silence above.
#define KLPLAT_MSG_PLATFORM_INIT   450037684u   // Message.MessageType
#define KLPLAT_MSG_ENTITLEMENT     409688241u   //   .Platform_InitializeAndroidAsynchronous
#define KLPLAT_MSG_AGE_CATEGORY    567009472u   //   .Entitlement_GetIsViewerEntitled
#define KLPLAT_MSG_APP_VERSION    1751583246u   //   .UserAgeCategory_Get
                                                //   .Application_GetVersion
#define KLPLAT_MSG_CLOUD_DIR      1990471406u   //   .CloudStorage2_GetUserDirectoryPath
#define KLPLAT_MSG_ASSET_LIST     1258057588u   //   .AssetFile_GetList (KL_PROBE_ENUM, this APK)
#define KLPLAT_MSG_ACCESS_TOKEN     0x06A85ABEu  // .User_GetAccessToken (Message<string>)
#define KLPLAT_MSG_ACHIEVEMENT_DEFS 0x03D3458Du  // .Achievements_GetAllDefinitions (empty array)
#define KLPLAT_MSG_LOGGED_IN_USER  0x436F345Du  // .User_GetLoggedInUser (Message<User>)
#define KLPLAT_MSG_USER_PROOF      0x22810483u  // .User_GetUserProof (Message<UserProof>);
                                                //   confirmed as a movz/movk pair in
                                                //   Ancient Dungeon's libil2cpp
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
// this tree's loaders (superhot +0x51934, beatsaber +0x8aae4). Grouped with the
// request-returning init family it would answer 0, and 0 in a bool is false.
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
// The platform's asset-file list. On a real Quest this is answered LOCALLY,
// offline included: the platform reports which asset files are installed, and
// for this title - whose whole 16 GB ships in the obb - the true answer is an
// EMPTY list. Klepton used to refuse the request outright (request id 0), the
// SDK surfaced "Request failed", and the game's loading orchestrator waited on
// a content check that could never complete. An empty list delivers no
// content, so the DRM line does not move: delivery, purchase and IAP queries
// keep refusing.
static uint64_t klplat_AssetFile_GetList(void) {
    plat_hit("ovr_AssetFile_GetList");
    return klplat_request("ovr_AssetFile_GetList", KLPLAT_MSG_ASSET_LIST, 0);
}

// The Oculus user *access token* — an opaque credential the app hands to its own
// backend (Ubisoft Connect) to open a session. It is NOT an ownership answer: the
// entitlement check is a separate request (klplat_Entitlement_GetIsViewerEntitled
// above) and this token grants nothing on its own. Returning 0 — "the request
// could not be made" — is what we did before, and AC Nexus does not treat it as a
// soft failure: its platform-init chain logs "Request failed" and the loading->menu
// transition aborts back to the loading loop. So we COMPLETE the request instead,
// with an opaque offline token. With KL_NET_OFFLINE the token is never exchanged
// against a server; the only point is that the request the game gates its own menu
// on RESOLVES. Read back through ovr_Message_GetString, like CloudStorage2's path.
static uint64_t klplat_User_GetAccessToken(void) {
    plat_hit("ovr_User_GetAccessToken");
    static const char tok[] = "OCACkleptonOfflineAccessToken000000000000";
    return klplat_request_str("ovr_User_GetAccessToken",
                              KLPLAT_MSG_ACCESS_TOKEN, 0, tok);
}

// Achievement *definitions* — the catalogue the game's achievement manager reads
// at startup. No Oculus service here defines any, so the honest answer is an EMPTY
// set, the same shape ovr_AssetFile_GetList's empty list takes. Delivered as a
// completed request (IsError false, an AchievementDefinitionArray of size 0) rather
// than as 0/"request could not be made", because AC Nexus gates its menu on the
// request resolving and a bare 0 lands it on the same "Request failed" path that
// aborts the transition. Achievements record what a player has DONE, not what they
// own, so an empty definition set invents no entitlement. The size-0 array is read
// through GetAchievementDefinitionArray (-> the message payload) and the existing
// Array_GetSize answer (plat_is_empty_array -> 0).
static uint64_t klplat_Achievements_GetAllDefinitions(void) {
    plat_hit("ovr_Achievements_GetAllDefinitions");
    return klplat_request("ovr_Achievements_GetAllDefinitions",
                          KLPLAT_MSG_ACHIEVEMENT_DEFS, 0);
}

// Who is signed in — the ASYNC request form (the synchronous ovr_GetLoggedInUserID
// is separate, below). Its completion is Message<User>. By DEFAULT we still answer
// 0, "the request could not be made", which is the offline path Beat Saber wants
// (see g_plat_request). But a title can run its online-init as a chain that ABORTS
// on a failed request: AC Nexus's log shows the Oculus entitlement check PASS and
// then this call fail with "Request failed", stranding it on the loading screen.
// KL_PLAT_USER opts such a title into a synthetic OFFLINE user so the chain
// completes. Not an ownership answer — entitlement is a separate request
// (klplat_Entitlement_GetIsViewerEntitled) that already passed — only "who is
// playing", which a real headset answers even with no network.
// Default ON for titles that HARD-QUIT on a failed login rather than falling
// back to offline. ZIX prints "Oculus Auth Failed! Quitting" and exits when
// GetLoggedInUser returns 0, so for it the synthetic offline user is not
// optional; the knob still overrides in either direction. Beat Saber and the
// rest keep the default-0 offline path (they tolerate no user).
static int klplat_user_default(void) {
    // Titles that ABORT when ovr_GetLoggedInUserID / GetLoggedInUser returns 0 —
    // they put up "Unable to get a valid UserID ... check you have an Entitlement"
    // and then FMessageDialog->abort on the game thread. A synthetic offline user
    // is strictly better for these: there is no Oculus service to sign in to, and
    // the alternative is a hard exit before the menu. Unity titles that read 0 as
    // "offline, carry on" are NOT in this list and keep the 0.
    static const char *on[] = { "zix", "intotheradius", "redmatter2", "vampire", "twd2" };
    const char *t = kl_driver_target_name();
    if (t) for (unsigned i = 0; i < sizeof on / sizeof on[0]; i++)
        if (strcmp(t, on[i]) == 0) return 1;
    // Ancient Dungeon is a Unity title of Walkabout's kind: it does not quit, it
    // WAITS. Its GenerateOculusNoncesForPhoton coroutine asks for the logged-in
    // user and then for a user proof, and the loading screen spins on "Waiting
    // for Oculus Nonce generation..." until both complete; with the 0 answer its
    // callback logs "Request failed" and the flag is never set. Keyed on the
    // guest's package rather than a target name, so it holds under whatever
    // name a packaging tool gives the target.
    extern const char *klj_guest_package(void);   // runtime/jni, the manifest's package
    const char *pkg = klj_guest_package();
    if (pkg && strcmp(pkg, "de.erthu.ancientdungeonfull") == 0) return 1;
    return 0;
}
static uint64_t klplat_User_GetLoggedInUser(void) {
    plat_hit("ovr_User_GetLoggedInUser");
    if (!kl_env_on("KL_PLAT_USER", klplat_user_default())) {
        static int said;
        if (!said) { said = 1;
            fprintf(stderr, "  [plat] ovr_User_GetLoggedInUser -> 0 (no platform "
                    "user; set KL_PLAT_USER=1 for a synthetic offline user)\n"); }
        return 0;
    }
    return klplat_request("ovr_User_GetLoggedInUser", KLPLAT_MSG_LOGGED_IN_USER, 0);
}

// The User that completion carries. The message handle IS the user handle (one
// payload per message), and these read its fields. An offline user: a fixed
// non-zero ovrID — 0 would read as "no user" and defeat the point — and a plain
// name. Every other User field the SDK's model reads is optional, and NULL/0 is
// how it spells "absent" (see the launch-details note), so only these three are
// set; the rest answer 0 permissively and are never even reached unless
// KL_PLAT_USER handed out the user above.
static uint64_t klplat_User_GetID(const void *h) {
    plat_hit("ovr_User_GetID"); (void)h;
    return 1000000000000042ull;          // fixed, non-zero synthetic ovrID
}
static const char *klplat_User_GetOculusID(const void *h) {
    plat_hit("ovr_User_GetOculusID"); (void)h;
    return "kleptonplayer";
}
static const char *klplat_User_GetDisplayName(const void *h) {
    plat_hit("ovr_User_GetDisplayName"); (void)h;
    return "Klepton Player";
}

// The user PROOF — a nonce the game forwards to its own online services (Unity
// Authentication's SignInWithOculus, Photon Fusion's custom auth) so THEY can
// ask Meta who this is. It is not an ownership answer: entitlement is the
// separate request above, and a nonce grants nothing by itself — whoever
// receives it verifies it against Meta, which will not know this one, so the
// online sign-in fails exactly as it would with no network. Gated like the
// user it proves: without KL_PLAT_USER there is no user to prove, and the
// answer is 0, "the request could not be made". With it the request completes
// (Message<UserProof>, read through ovr_Message_GetUserProof ->
// ovr_UserProof_GetNonce) so that a title which WAITS on the completion —
// Ancient Dungeon's "Waiting for Oculus Nonce generation..." — moves on.
static uint64_t klplat_User_GetUserProof(void) {
    plat_hit("ovr_User_GetUserProof");
    if (!kl_env_on("KL_PLAT_USER", klplat_user_default())) {
        static int said;
        if (!said) { said = 1;
            fprintf(stderr, "  [plat] ovr_User_GetUserProof -> 0 (no platform user "
                    "to prove; set KL_PLAT_USER=1 for a synthetic offline user)\n"); }
        return 0;
    }
    static const char nonce[] = "kleptonOfflineNonce00000000000000000000000000000";
    return klplat_request_str("ovr_User_GetUserProof", KLPLAT_MSG_USER_PROOF, 0, nonce);
}
// The UserProof handle IS the message (one payload per message, as with User);
// its single field is the nonce string the request above carried.
static const char *klplat_UserProof_GetNonce(const klplat_msg *m) {
    plat_hit("ovr_UserProof_GetNonce");
    return m ? m->str : NULL;
}
// The list payload's one accessor pair: the handle is the message (the same
// convention as every other payload here), and its size is zero.
static uint64_t klplat_AssetFileDeliveryList_GetSize(const void *h) {
    plat_hit("ovr_AssetDetailsArray_GetSize");
    (void)h;
    return 0;
}

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
    // The IAP catalog (see plat_is_iap_catalog): a store query with no store to
    // ask, so 0 = "request could not be made", the honest failure. Ownership IAP
    // calls never reach here — plat_is_drm refuses them first.
    "ovr_IAP_GetProductsBySKU", "ovr_IAP_GetNextProductArrayPage",
    // Owned-inventory query: honest failure, grants nothing (see plat_is_iap_owned_query).
    "ovr_IAP_GetViewerPurchases", "ovr_IAP_GetViewerPurchasesDeltaAsync",
    "ovr_RichPresence_Clear", "ovr_RichPresence_Set",
    "ovr_RichPresence_SetDestination", "ovr_RichPresence_SetIsJoinable",
    // Asking who is logged in is not an ownership question; with no platform
    // service the request cannot be made, which is what 0 says. Beat Saber
    // 1.28 takes its offline path from there (~swap 34k of the boot).
    "ovr_User_GetLoggedInUser",
    // The signed-in user's SOCIAL graph — friends, and the room/party a friend
    // is in. Pure social decoration, no ownership: with no platform service the
    // request cannot be made (0). Asgard's Wrath 2 walks this whole family right
    // after KL_PLAT_USER hands it a user; each is honestly "could not be made".
    "ovr_User_GetLoggedInUserFriends",
    "ovr_User_GetLoggedInUserFriendsAndRooms",
    "ovr_User_GetLoggedInUserFriendsV2",
    "ovr_User_GetNextUserArrayPage",
    "ovr_User_GetOrgScopedID",
    "ovr_User_LaunchFriendRequestFlow",
    "ovr_User_GetLoggedInUserManagedInfo",
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
    // KL_PLAT_USER opts a title into a synthetic OFFLINE user, the same switch
    // klplat_User_GetLoggedInUser reads for the async form. Some titles gate boot
    // on "is anyone signed in?" and ForceQuit when this returns 0 — Asgard's
    // Wrath 2 does exactly that (ovr_PlatformInitializeAndroid succeeds,
    // ovr_GetLoggedInUserID == 0, then AndroidThunkJava_ForceQuit). The ID is
    // the SAME synthetic ovrID ovr_User_GetID hands back, so the sync and async
    // views of "who is playing" agree. This is a session identity, NOT an
    // ownership answer: entitlement stays a separate request
    // (ovr_Entitlement_GetIsViewerEntitled) that still returns "could not be
    // made" and grants nothing. The default is the per-target one — the SAME
    // klplat_user_default() the async form reads — so a title that fatals on a
    // 0 here (Into the Radius, Red Matter 2: "Unable to get a valid UserID")
    // gets the synthetic id without a flag, while titles fine with 0 (RE4) keep
    // it. Previously this hardcoded 0 and only the async form honoured the
    // per-target default, so the sync callers still fatalled.
    if (kl_env_on("KL_PLAT_USER", klplat_user_default())) {
        static int saidu;
        if (!saidu) { saidu = 1;
            fprintf(stderr, "  [plat] ovr_GetLoggedInUserID -> synthetic offline "
                    "user (KL_PLAT_USER=1); NOT an entitlement grant\n"); }
        return 1000000000000042ull;   // == klplat_User_GetID's synthetic ovrID
    }
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] ovr_GetLoggedInUserID -> 0 (no platform user; "
                        "there is no service here to be signed in to; set "
                        "KL_PLAT_USER=1 for a synthetic offline user)\n");
    }
    return 0;
}

// ovr_GetLoggedInUserLocale — a SYNCHRONOUS getter (returns const char*, not an
// ovrRequest), the locale the store would report for the signed-in user. There
// is no platform user here, but the guest reads the string to pick a language,
// and a NULL would crash the read — so answer the host's, defaulting to en_US.
static uint64_t klplat_GetLoggedInUserLocale(void) {
    plat_hit("ovr_GetLoggedInUserLocale");
    static char loc[16];
    if (!loc[0]) {
        const char *lang = getenv("LANG");            // e.g. "en_US.UTF-8"
        int i = 0;
        if (lang) for (; lang[i] && lang[i] != '.' && i < (int)sizeof loc - 1; i++) loc[i] = lang[i];
        if (i == 0) { loc[0]='e'; loc[1]='n'; loc[2]='_'; loc[3]='U'; loc[4]='S'; i=5; }
        loc[i] = '\0';
    }
    return (uint64_t)(uintptr_t)loc;
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
    {"ovr_GetLoggedInUserLocale", (void *)klplat_GetLoggedInUserLocale},
    {"ovr_UnityInitWrapper",      (void *)klplat_UnityInitWrapper},
    {"ovr_PopMessage",            (void *)klplat_PopMessage},
    {"ovr_FreeMessage",           (void *)klplat_FreeMessage},
    {"ovr_Entitlement_GetIsViewerEntitled", (void *)klplat_Entitlement_GetIsViewerEntitled},
    {"ovr_UserAgeCategory_Get",   (void *)klplat_UserAgeCategory_Get},
    {"ovr_Application_GetVersion",(void *)klplat_Application_GetVersion},
    {"ovr_Message_GetApplicationVersion",      (void *)klplat_Message_GetPayload},
    {"ovr_AssetFile_GetList",                  (void *)klplat_AssetFile_GetList},
    {"ovr_Message_GetAssetFileDeliveryList",   (void *)klplat_Message_GetPayload},
    {"ovr_AssetFileDeliveryList_GetSize",      (void *)klplat_AssetFileDeliveryList_GetSize},
    // ...and the names this SDK build actually reads the same payload through
    // (measured: the GetList completion was consumed via GetAssetDetailsArray,
    // which the DRM family then refused by prefix). An EMPTY details array
    // names no content, delivers no content, and unblocks the reader.
    {"ovr_Message_GetAssetDetailsArray",       (void *)klplat_Message_GetPayload},
    {"ovr_AssetDetailsArray_GetSize",          (void *)klplat_AssetFileDeliveryList_GetSize},
    // The user access token and the achievement catalogue — two request-style
    // calls the game issues during platform init and gates its menu on. Both
    // complete with benign, empty/offline data (see the handlers above) rather
    // than the bare 0 that lands the guest on its "Request failed" path. The
    // exact-match binding here overrides the ovr_Achievements_ prefix rule.
    {"ovr_User_GetAccessToken",                (void *)klplat_User_GetAccessToken},
    {"ovr_Achievements_GetAllDefinitions",     (void *)klplat_Achievements_GetAllDefinitions},
    {"ovr_Message_GetAchievementDefinitionArray", (void *)klplat_Message_GetPayload},
    // The logged-in user (async) and the User accessors its completion feeds.
    // Gated by KL_PLAT_USER inside the handler; the accessors are inert unless a
    // user was handed out. The exact-match binding overrides ovr_User_GetLoggedInUser's
    // entry in g_plat_request.
    {"ovr_User_GetLoggedInUser",               (void *)klplat_User_GetLoggedInUser},
    {"ovr_Message_GetUser",                    (void *)klplat_Message_GetPayload},
    {"ovr_User_GetID",                         (void *)klplat_User_GetID},
    {"ovr_User_GetOculusID",                   (void *)klplat_User_GetOculusID},
    {"ovr_User_GetDisplayName",                (void *)klplat_User_GetDisplayName},
    // The user proof (nonce) and its accessor; gated by KL_PLAT_USER in the
    // handler, same as the user it proves.
    {"ovr_User_GetUserProof",                  (void *)klplat_User_GetUserProof},
    {"ovr_Message_GetUserProof",               (void *)klplat_Message_GetPayload},
    {"ovr_UserProof_GetNonce",                 (void *)klplat_UserProof_GetNonce},
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
        fprintf(f, "  --- entitlement / ownership (REFUSED by policy) ---\n");
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
