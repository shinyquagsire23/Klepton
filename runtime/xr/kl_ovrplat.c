// libovrplatformloader.so — see kl_ovrplat.h for why this is a replacement and
// where the DRM line is drawn.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <pthread.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>              // the cloud-save directory is created
#include "klepton.h"
#include "kl_jni.h"
#include "guest/kl_driver.h"   // target name, for the per-target KL_PLAT_USER default
#include "kl_env.h"
#include "kl_ovrplat.h"
#include "kl_assetcfg.h"

#define KL_OVRPLAT_MAX 512
static struct { const char *name; unsigned calls; int drm; } g_plat[KL_OVRPLAT_MAX];
static unsigned g_nplat;
static unsigned g_drm_calls;
// Resolution (dlsym, main thread) and calls (any guest thread) race on this
// table exactly as kl_ovrp.c's did: two concurrent inserts can split a slot and
// leave a NULL name for a later strcmp. The lock covers lookup and insert; the
// counters are atomic, so a hit needs nothing past the lookup. g_nplat is
// published with release only once its slot is filled, so the report — which
// runs on the way to abort() and does not take the lock — reads whole slots.
static pthread_mutex_t g_plat_mu = PTHREAD_MUTEX_INITIALIZER;

static int plat_slot(const char *name) {
    pthread_mutex_lock(&g_plat_mu);
    int rc = -1;
    unsigned n = __atomic_load_n(&g_nplat, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < n; i++)
        if (strcmp(g_plat[i].name, name) == 0) { rc = (int)i; goto out; }
    if (n >= KL_OVRPLAT_MAX) goto out;
    const char *dup = strdup(name);
    if (!dup) goto out;
    g_plat[n].name  = dup;
    g_plat[n].calls = 0;
    g_plat[n].drm   = 0;
    __atomic_store_n(&g_nplat, n + 1, __ATOMIC_RELEASE);
    rc = (int)n;
out:
    pthread_mutex_unlock(&g_plat_mu);
    return rc;
}

// The slot's call count including this one; 0 when the table is full.
static unsigned plat_hit(const char *name) {
    int s = plat_slot(name);
    return s >= 0 ? __atomic_add_fetch(&g_plat[s].calls, 1, __ATOMIC_RELAXED) : 0;
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
//     device. For every title but Walkabout that list is empty. For Walkabout
//     it names a course only when that course's real track bundle is already
//     on disk (klplat_scan_courses): files the user copied off their own Quest,
//     never a stub, never a course that is not there. Download and Delete stay
//     refused — this list does not fetch anything.
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
// restrictive than the truth. Ownership is a different call
// (ovr_IAP_GetViewerPurchases): request 0 for every title but Walkabout, and
// for Walkabout only the store ids of courses whose track files are on disk.
// ovr_IAP_ConsumePurchase, checkout and every delivery call keep matching the
// "IAP"/"Purchase" markers below, because this list is exact names checked
// first, not a substring. GetNextProductArrayPage walks the same catalog
// array, so it carves out for the same reason.
static int plat_is_iap_catalog(const char *name) {
    return strcmp(name, "ovr_IAP_GetProductsBySKU") == 0 ||
           strcmp(name, "ovr_IAP_GetNextProductArrayPage") == 0;
}

// The read side of the OWNED half: "which IAPs has this viewer already bought?"
// For every title but Walkabout the answer is still "the request could not be
// made" (request id 0): no purchase message, so the guest learns of nothing it
// did not already have. Walkabout is the exception, and a title under
// KL_PLAT_IAP_CONFIRMED (Maestro): there
// klplat_IAP_GetViewerPurchases returns the purchases the store confirmed for
// this viewer, read from klepton-entitlements.txt (klplat_load_entitlements).
// A bundle on disk is not a purchase, and with no such file the list is empty.
// The WRITE side — ovr_IAP_ConsumePurchase and the AssetFile delivery calls —
// stays in plat_is_drm and keeps aborting. Checkout is an error (plat_is_checkout).
static int plat_is_iap_owned_query(const char *name) {
    return strcmp(name, "ovr_IAP_GetViewerPurchases") == 0 ||
           strcmp(name, "ovr_IAP_GetViewerPurchasesDurableCache") == 0 ||
           strcmp(name, "ovr_IAP_GetViewerPurchasesDeltaAsync") == 0;
}

// Readers of a course record we actually produced: asset-file status and the
// AssetDetails / Purchase fields of an on-disk Walkabout course. Exact names,
// checked before the substring markers, so Download, Delete and ConsumePurchase
// still match "AssetFile" / "IAP" / "Purchase" and still abort. Answering one
// of these with no course files present yields an empty list or "not installed",
// which grants nothing.
static int plat_is_course_record(const char *name) {
    static const char *const n[] = {
        "ovr_AssetFile_StatusById",
        "ovr_AssetFile_StatusByName",
        "ovr_AssetFile_StatusByName_Native",
        "ovr_AssetDetails_GetAssetId",
        "ovr_AssetDetails_GetAssetType",
        "ovr_AssetDetails_GetAssetType_Native",
        "ovr_AssetDetails_GetDownloadStatus",
        "ovr_AssetDetails_GetDownloadStatus_Native",
        "ovr_AssetDetails_GetFilepath",
        "ovr_AssetDetails_GetFilepath_Native",
        "ovr_AssetDetails_GetIapStatus",
        "ovr_AssetDetails_GetIapStatus_Native",
        "ovr_AssetDetails_GetLanguage",
        "ovr_AssetDetails_GetMetadata",
        "ovr_AssetDetailsArray_GetElement",
        "ovr_Message_GetAssetDetails",
        "ovr_Message_GetPurchaseArray",
        "ovr_PurchaseArray_GetSize",
        "ovr_PurchaseArray_GetElement",
        "ovr_PurchaseArray_GetNextUrl",
        "ovr_Purchase_GetDeveloperPayload",
        "ovr_Purchase_GetExpirationTime",
        "ovr_Purchase_GetGrantTime",
        "ovr_Purchase_GetPurchaseID",
        "ovr_Purchase_GetPurchaseStrID",
        "ovr_Purchase_GetPurchaseStrID_Native",
        "ovr_Purchase_GetReportingId",
        "ovr_Purchase_GetReportingId_Native",
        "ovr_Purchase_GetSKU",
        "ovr_Purchase_GetSKU_Native",
    };
    for (size_t i = 0; i < sizeof n / sizeof n[0]; i++)
        if (strcmp(name, n[i]) == 0) return 1;
    return 0;
}

// ...and the one WRITE-side call that is answered, with a failure.
// ovr_IAP_LaunchCheckoutFlow is the guest opening the store for a SKU. There is
// no store here, and the truthful completion of that request is an ERROR
// message — "the purchase did not happen" — which grants nothing and delivers
// nothing. Aborting instead cost the whole session for pressing a button in the
// song list. Only this entry point: ConsumePurchase, the Purchase accessors and
// every AssetFile delivery call still match the markers and still refuse, and a
// failed checkout never reaches them (the SDK reads a Purchase only from a
// message that is not an error).
static int plat_is_checkout(const char *name) {
    return strcmp(name, "ovr_IAP_LaunchCheckoutFlow") == 0;
}

// A base game's own LANGUAGE PACKS, for the one target measured to need them.
//
// batman (Batman: Arkham Shadow) ships its voice-over as one OBB per locale and
// a manifest beside them (localized-asset-manifest.txt: "<locale>:<file>:<size>").
// Every owner gets every locale; none is sold. At startup it asks
// ovr_AssetFile_StatusByName whether the pack for the current language is on the
// device, and the refusal below killed it right after its content had loaded.
//
// What is answered is a FACT ABOUT THE DISK and nothing else: a name that is in
// the game's own manifest, present with the size the manifest gives, is
// "installed" at its real path; a manifest name that is not there is
// "available"; any other name still refuses. No download, delete, purchase or
// list call moves — a pack that is not on disk cannot be fetched from here, and
// nothing this answers was ever for sale. Target-scoped on purpose: the same
// call on another title can be asking about paid content.
//
// AC Nexus VR (x5812519008825194) is the second, and asks differently: its
// voice-over is one `<tag>.lang` Wwise package per language beside the rest of
// its data in the OBB directory, part of the base game like batman's, and it
// learns which of them are there from ovr_AssetFile_GetList. With the empty
// list every title gets, its audio-language menu greyed out every language
// but the one it had already loaded by path. For it the list names the .lang
// files that are really in that directory (klplat_scan_obb_langpacks) — again
// a fact about the disk; delivery, delete and purchase calls are untouched.
// KL_PLAT_LANGPACK_LIST=0 restores the empty list.
//
// Asgard's Wrath 2 is the third, with the same files (`de.lang`, `fr.lang`, …:
// free voice packs, one per language, that the store hands every owner) and
// one question more. Its engine mounts every .lang beside its OBBs by itself
// (FAndroidPlatformFile::FMountOBBVisitor); what it asks the platform is which
// packs are installed (ovr_AssetFile_GetList, as above) and which one is the
// CURRENT one (ovr_LanguagePack_GetCurrent), and its language menu changes
// that with ovr_LanguagePack_SetCurrent(tag). On a headset both went
// unanswered and the German pack a user had fetched could not be chosen
// (2026-10-11). The current pack is a choice between files that are on the
// disk, remembered in a file of ours beside them (klplat_langpack_current);
// choosing one that is not there is an error, as before — nothing is fetched
// from here, and nothing in this family is for sale.
//
// Marvel's Deadpool VR (x3240283705987790) has the same `<tag>.lang` voice
// packs, loaded by libUE4.so, and greyed out every language but the one it
// had loaded. It is the first title whose list comes from its INSTALLATION
// rather than from a directory listing: the installer writes the store's own
// asset id, file name and size of every pack (kl_assetcfg.h), and a pack is
// installed only when its file is there at exactly that size. Any title with
// such a list takes this path — the list is the installer's statement that
// these files are the title's free asset files — and a title without one is
// untouched. Downloads stay refused either way.
static int klplat_asset_cfg_path(char *out, size_t cap) {
    const char *env = getenv(KL_ASSETCFG_ENV);
    if (env && env[0]) return snprintf(out, cap, "%s", env) < (int)cap;
    const char *root = kl_jni_files_dir();
    return root && root[0] && snprintf(out, cap, "%s/%s", root, KL_ASSETCFG_FILE) < (int)cap;
}
int kl_ovrplat_asset_files_configured(void) {
    static int on = -1;
    if (on < 0) {
        const char *env = getenv(KL_ASSETCFG_ENV);
        char path[PATH_MAX];
        struct stat st;
        int v = (env && env[0]) ||
                (klplat_asset_cfg_path(path, sizeof path) && stat(path, &st) == 0 && S_ISREG(st.st_mode));
        if (v) fprintf(stderr, "  [plat] asset files listed by the installation (%s): "
                               "asset-file questions are answered from that list\n",
                       env && env[0] ? KL_ASSETCFG_ENV : KL_ASSETCFG_FILE);
        on = v;
    }
    return on;
}
static int plat_is_obb_langpack_target(void) {
    static int v = -1;
    if (v < 0) {
        const char *t = kl_driver_target_name();
        v = kl_env_on("KL_PLAT_LANGPACK_LIST", (t && (strcmp(t, "x5812519008825194") == 0 ||
                                                      strcmp(t, "wrath2") == 0)) ||
                                               kl_ovrplat_asset_files_configured());
    }
    return v;
}

// KL_PLAT_TRACE=1 — one line per DISTINCT asset-file, language-pack and locale
// call the guest makes (name, arguments, our answer), and one per ovr_* name it
// resolves. Off by default. A line already said is not said again, so a menu
// that polls the same getter every frame costs one line; past
// KLPLAT_TRACE_MAX distinct lines it goes quiet. This is how Deadpool VR's
// language packs were found to hinge on the metadata and the path spelling.
#define KLPLAT_TRACE_MAX 1024
int kl_ovrplat_trace_on(void) {
    static int v = -1;
    if (v < 0) v = kl_env_on("KL_PLAT_TRACE", 0);
    return v;
}
static void klplat_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void klplat_trace(const char *fmt, ...) {
    if (!kl_ovrplat_trace_on()) return;
    char line[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    uint64_t h = 14695981039346656037ull;
    for (const unsigned char *c = (const unsigned char *)line; *c; c++) { h ^= *c; h *= 1099511628211ull; }
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t seen[KLPLAT_TRACE_MAX];
    static int nseen, full;
    int say = 1;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nseen && say; i++) if (seen[i] == h) say = 0;
    if (say && nseen == KLPLAT_TRACE_MAX) { say = 0; if (!full) { full = 1; say = 2; } }
    if (say == 1) seen[nseen++] = h;
    pthread_mutex_unlock(&mu);
    if (say == 1) fprintf(stderr, "  [plat-trace] %s\n", line);
    else if (say == 2) fprintf(stderr, "  [plat-trace] (%d distinct lines; quiet from here)\n", KLPLAT_TRACE_MAX);
}
static int plat_is_langpack_target(void) {
    const char *t = kl_driver_target_name();
    return (t && strcmp(t, "batman") == 0) || plat_is_obb_langpack_target();
}
static int plat_is_langpack_status(const char *name) {
    static const char *const ok[] = {
        "ovr_AssetFile_StatusByName",
        "ovr_Message_GetAssetDetails",
        "ovr_AssetDetails_GetAssetId",       "ovr_AssetDetails_GetAssetType",
        "ovr_AssetDetails_GetDownloadStatus", "ovr_AssetDetails_GetFilepath",
        "ovr_AssetDetails_GetIapStatus",     "ovr_AssetDetails_GetLanguage",
        "ovr_AssetDetails_GetMetadata",
    };
    if (!plat_is_langpack_target()) return 0;
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        if (strcmp(name, ok[i]) == 0) return 1;
    return 0;
}
// The completion of ovr_LanguagePack_SetCurrent is an AssetFileDownloadResult
// by name, and for a pack that is already on the disk it is read for one thing:
// that it names an asset. Only where the .lang files are listed at all; every
// other title's "DownloadResult" is a delivery and stays refused.
static int plat_is_langpack_select(const char *name) {
    if (!plat_is_obb_langpack_target()) return 0;
    return strcmp(name, "ovr_Message_GetAssetFileDownloadResult") == 0 ||
           strcmp(name, "ovr_AssetFileDownloadResult_GetAssetId") == 0 ||
           strcmp(name, "ovr_AssetFileDownloadResult_GetFilepath") == 0;
}

static int plat_is_drm(const char *name) {
    if (plat_is_absent_ok(name)) return 0;
    if (plat_is_iap_catalog(name)) return 0;
    if (plat_is_iap_owned_query(name)) return 0;
    if (plat_is_checkout(name)) return 0;
    if (plat_is_course_record(name)) return 0;
    if (plat_is_langpack_status(name)) return 0;
    if (plat_is_langpack_select(name)) return 0;
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
    if (s >= 0) {
        __atomic_add_fetch(&g_plat[s].calls, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_plat[s].drm, 1, __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&g_drm_calls, 1, __ATOMIC_RELAXED);
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
    unsigned calls = plat_hit(name);
    if (kl_permissive()) {
        if (calls == 1)
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
#define KLPLAT_MSG_ACHIEVEMENT_PROG 0x4F9FDE1Du  // .Achievements_GetAllProgress (empty array)
#define KLPLAT_MSG_LOGGED_IN_USER  0x436F345Du  // .User_GetLoggedInUser (Message<User>)
#define KLPLAT_MSG_RICH_DESTINATIONS 0x586F2D14u // .RichPresence_GetDestinations (empty array).
                                                //   Read from AC Nexus's, Demeo's and Premium
                                                //   Bowling's global-metadata, where it stands
                                                //   before GetNextDestinationArrayPage's and
                                                //   Set's; Real VR Fishing's own was not here
#define KLPLAT_MSG_USER_GET        0x6BCF9E47u  // .User_Get (Message<User>; KL_PROBE_ENUM, Wooorld 2.4.0)
#define KLPLAT_MSG_USER_PROOF      0x22810483u  // .User_GetUserProof (BRINK Traveler's
                                                //   global-metadata, beside GetLoggedInUser's)
#define KLPLAT_MSG_IAP_CHECKOUT    0x3F9B0D0Du  // .IAP_LaunchCheckoutFlow (KL_PROBE_ENUM, 1.28)
#define KLPLAT_MSG_APP_CANCEL_DL   0x7C2060DEu  // .Application_CancelAppDownload (Cave Crave's
                                                //   global-metadata, beside GetVersion's)
// Walkabout's own Message.MessageType values, read from its global-metadata
// default-value blob (the same encoding that reproduces the constants above).
// Queued only for that title — another game's enum is not these numbers.
#define KLPLAT_MSG_ASSET_STATUS_ID    1570069816u  // AssetFile_StatusById
#define KLPLAT_MSG_ASSET_STATUS_NAME  1104140880u  // AssetFile_StatusByName
#define KLPLAT_MSG_IAP_PURCHASES       974095385u  // IAP_GetViewerPurchases
#define KLPLAT_MSG_IAP_PURCHASES_CACHE 1666817579u // IAP_GetViewerPurchasesDurableCache
#define KLPLAT_MSG_IAP_PRODUCTS       2124073717u  // IAP_GetProductsBySKU
// Read from Keep Talking and Nobody Explodes' global-metadata default-value blob
// the same way (the same read reproduces 0x186B58B1 and 0x436F345D above), and
// found again in Eye of the Temple's.
#define KLPLAT_MSG_LEADERBOARD_GET_ENTRIES 0x5DB3474Cu // Leaderboard_GetEntries
#define KLPLAT_MSG_LEADERBOARD_WRITE_ENTRY 0x117FC8FEu // Leaderboard_WriteEntry
// The rest of the family's plain requests, read out of Beat Saber 1.45's running
// IL2CPP runtime (KL_PROBE_ENUM=Oculus.Platform.Message/MessageType on a Mac
// host, 2026-10-12); the two values above came back identical in the same list.
#define KLPLAT_MSG_LEADERBOARD_GET             0x6AD44EF8u // Leaderboard_Get
#define KLPLAT_MSG_LEADERBOARD_GET_AFTER_RANK  0x18378BEFu // Leaderboard_GetEntriesAfterRank
#define KLPLAT_MSG_LEADERBOARD_GET_BY_IDS      0x39607BFCu // Leaderboard_GetEntriesByIds
#define KLPLAT_MSG_LEADERBOARD_WRITE_ENTRY_SM  0x72C692FAu // Leaderboard_WriteEntryWithSupplementaryMetric
#define KLPLAT_INIT_SUCCESS        0            // PlatformInitializeResult.Success
#define KLPLAT_AGE_ADULT           3            // AccountAgeCategory.Ad

// A pool of message slots and a FIFO of the queued ones. A slot is the guest's
// from ovr_PopMessage until its ovr_FreeMessage and is reused only after that,
// so a handle the guest holds is never rewritten under it. The pool grows by a
// block when every slot is queued or held — the fixed 16-slot ring it replaces
// dropped the completion of a 17th outstanding request while still handing out
// its id, which is the "poll forever" the queue exists to prevent, and it
// rewrote popped handles on wrap. A request id is issued only together with its
// slot; with no slot to be had — no memory, or the pool at its limit below —
// the request fails (id 0) instead. Blocks are
// never freed, so even a handle used after its FreeMessage reads a slot, not
// freed memory. The mutex is not defensive — the requests are issued from
// managed code on the main thread and the pump runs there too, but nothing in
// the API promises that.
#define KLPLAT_MSGQ 16                 // slots per pool block
// How far the pool may grow. Every guest in the corpus has at most a handful
// of requests outstanding, so a pool past KLPLAT_MSG_WARN_BLOCKS (128 slots)
// means a guest that requests without pumping or pops without freeing; that
// is said once, with the count, because it is the first line anyone will want
// when the run later goes wrong. KLPLAT_MSG_MAX_BLOCKS (2048 slots, about
// 2.4 MB of message slots) is the hard stop: past it a request fails cleanly
// with id 0 — the SDK reports that as a failed request rather than awaiting it
// — instead of growing a leak until the process is killed for memory.
#define KLPLAT_MSG_WARN_BLOCKS 8
#define KLPLAT_MSG_MAX_BLOCKS  128
// A language pack's details (see plat_is_langpack_status), here because a
// message carries a copy of them.
typedef struct {
    uint64_t id; char path[1024]; char tag[32];
    const char *status;          // "installed" or "available"
    char meta[64];               // the store's metadata, from the installation's list; "" elsewhere
} klplat_asset;
// `str` is the STRING payload, for the messages whose managed wrapper is a
// `Message<string>` — it is read through ovr_Message_GetString rather than
// through the int payload above, and a message carries one or the other.
// `err` marks the one completion that is a FAILURE (a refused checkout): the
// SDK's Message constructor then reads ovr_Message_GetError and the three
// ovr_Error_* accessors, and does not touch the payload at all.
// `kind` says which array the payload count refers to. A message that is not a
// course list leaves it 0, and the array getters then answer empty — an
// achievement list must not grow a purchase.
enum {
    KLPLAT_KIND_NONE = 0,
    KLPLAT_KIND_ASSETS,
    KLPLAT_KIND_ASSET,
    KLPLAT_KIND_PURCHASES,
    KLPLAT_KIND_PRODUCTS,
    KLPLAT_KIND_LANGPACK_SET,   // obj is the pack ovr_LanguagePack_SetCurrent chose
};
// `asset` is the storage `obj` points at when a message carries language-pack
// details: a copy taken when the message is queued, so the handle lives exactly
// as long as the message and a rescan of the packs cannot change it under the
// guest.
//
// `elems` is the same for a whole LIST of language packs (ovr_AssetFile_GetList
// on an OBB language-pack target): the packs as they were when the message was
// queued, `payload` of them, so ovr_AssetDetailsArray_GetElement hands out
// handles into the message rather than into the shared scan list a rescan
// rewrites. They live in `list`, which belongs to the SLOT: allocated the
// first time the slot carries a list, reused after, never freed — the same
// lifetime as the slot itself. NULL `elems` for every other message.
enum { KLPLAT_SLOT_FREE = 0, KLPLAT_SLOT_QUEUED, KLPLAT_SLOT_POPPED };
#define KLPLAT_LANG_MAX KL_ASSETCFG_MAX // language packs per list (an installation's list)
#define KLPLAT_LANG_SCAN_MAX 8         // ...and found by listing the OBB directory
typedef struct klplat_msg {
    uint32_t type; uint64_t request; int32_t payload; const char *str; int err;
    int kind;
    const void *obj;   // an AssetDetails, for the one message that has one
    klplat_asset asset;
    const klplat_asset *elems;    // the list's details, or NULL
    klplat_asset *list;           // KLPLAT_LANG_MAX of storage for them, or NULL
    int state;                    // KLPLAT_SLOT_*, under g_msg_mu
    struct klplat_msg *next;      // the FIFO while queued, the free list while free
} klplat_msg;
typedef struct klplat_msg_block {
    struct klplat_msg_block *next;
    klplat_msg m[KLPLAT_MSGQ];
} klplat_msg_block;
static klplat_msg_block *g_msg_blocks;          // every block, never freed
static int              g_msg_nblocks;          // how many, under g_msg_mu
static int              g_msg_warned;           // the growth warning was said
static klplat_msg      *g_msg_free;             // slots no one holds
static klplat_msg      *g_msg_head, *g_msg_tail; // queued, oldest first
static uint64_t        g_next_request = 1;
static int             g_platform_up;
static pthread_mutex_t g_msg_mu = PTHREAD_MUTEX_INITIALIZER;
// The language packs on the disk as last scanned (klplat_scan_obb_langpacks),
// under g_msg_mu. Here because a list message copies them in as it is queued.
static klplat_asset g_lang[KLPLAT_LANG_MAX];
static int          g_nlang;
// ...and, where the installation lists them (kl_assetcfg.h), every pack it
// lists, installed or not, so a status question about one that is not on the
// disk is answered "available" rather than refused. Under g_msg_mu.
static klplat_asset g_lang_listed[KLPLAT_LANG_MAX];
static int          g_nlang_listed;

// A free slot, growing the pool by a block when there is none — up to
// KLPLAT_MSG_MAX_BLOCKS, past which there is no slot (NULL). Under g_msg_mu.
static klplat_msg *klplat_msg_alloc_locked(void) {
    if (!g_msg_free) {
        if (g_msg_nblocks >= KLPLAT_MSG_MAX_BLOCKS) return NULL;
        klplat_msg_block *b = calloc(1, sizeof *b);
        if (!b) return NULL;
        b->next = g_msg_blocks;
        g_msg_blocks = b;
        if (++g_msg_nblocks >= KLPLAT_MSG_WARN_BLOCKS && !g_msg_warned) {
            g_msg_warned = 1;
            fprintf(stderr, "  [plat] WARNING: %d platform messages queued or held "
                            "(pool limit %d) — the guest is not pumping or not freeing "
                            "its messages\n", g_msg_nblocks * KLPLAT_MSGQ - KLPLAT_MSGQ,
                    KLPLAT_MSG_MAX_BLOCKS * KLPLAT_MSGQ);
        }
        for (int i = KLPLAT_MSGQ - 1; i >= 0; i--) {
            b->m[i].next = g_msg_free;
            g_msg_free = &b->m[i];
        }
    }
    klplat_msg *m = g_msg_free;
    g_msg_free = m->next;
    return m;
}

// Whether `p` is a slot of the pool, checked by address before anything is
// read through it. Under g_msg_mu.
static klplat_msg *klplat_msg_slot_locked(const void *p) {
    for (klplat_msg_block *b = g_msg_blocks; b; b = b->next) {
        const char *lo = (const char *)b->m, *hi = (const char *)(b->m + KLPLAT_MSGQ);
        const char *c = p;
        if (c >= lo && c < hi && (size_t)(c - lo) % sizeof b->m[0] == 0)
            return (klplat_msg *)p;
    }
    return NULL;
}

// Fill a slot taken from klplat_msg_alloc_locked, issue its id and queue it.
// Everything a message carries is attached by the caller in the same critical
// section, so a pump on another thread never pops it half-built. Under g_msg_mu.
static uint64_t klplat_commit_locked(klplat_msg *m, uint32_t type, int32_t payload,
                                     const char *str, int err, int kind) {
    uint64_t id = g_next_request++;
    m->type = type; m->request = id; m->payload = payload; m->str = str;
    m->err = err; m->kind = kind; m->obj = NULL; m->elems = NULL;
    m->state = KLPLAT_SLOT_QUEUED;
    m->next = NULL;
    if (g_msg_tail) g_msg_tail->next = m; else g_msg_head = m;
    g_msg_tail = m;
    return id;
}

// Issue a request and queue its completion, as one step: the id comes from the
// same critical section that takes the slot, so no id is ever handed out
// without a completion to match it. Returns 0 — a failed request, which the SDK
// reports rather than awaits — only when no slot can be had at all.
static uint64_t klplat_enqueue(uint32_t type, int32_t payload, const char *str,
                               int err, int kind, const klplat_asset *asset) {
    pthread_mutex_lock(&g_msg_mu);
    klplat_msg *m = klplat_msg_alloc_locked();
    uint64_t id = 0;
    if (m) {
        id = klplat_commit_locked(m, type, payload, str, err, kind);
        if (asset) { m->asset = *asset; m->obj = &m->asset; }
    }
    pthread_mutex_unlock(&g_msg_mu);
    return id;
}

// ...and a list of the language packs, snapshotted from g_lang into the
// slot's own storage in the same critical section that queues it, so the
// count and the elements are one scan's and stay that scan's for as long as
// the guest holds the message. Returns the id (0 for no slot, or no memory for
// the list — the slot then goes straight back) and the count in *n.
static uint64_t klplat_enqueue_lang_list(uint32_t type, int *n) {
    pthread_mutex_lock(&g_msg_mu);
    klplat_msg *m = klplat_msg_alloc_locked();
    if (m && !m->list) m->list = calloc(KLPLAT_LANG_MAX, sizeof *m->list);
    if (m && !m->list) { m->next = g_msg_free; g_msg_free = m; m = NULL; }
    uint64_t id = 0;
    *n = 0;
    if (m) {
        *n = g_nlang;
        memcpy(m->list, g_lang, (size_t)g_nlang * sizeof *m->list);
        id = klplat_commit_locked(m, type, g_nlang, NULL, 0, KLPLAT_KIND_ASSETS);
        m->elems = m->list;
    }
    pthread_mutex_unlock(&g_msg_mu);
    return id;
}

static uint64_t klplat_request_failed(const char *what) {
    fprintf(stderr, "  [plat] %s -> no slot for its completion (pool at its limit "
                    "or out of memory), request refused (0)\n", what);
    return 0;
}

// Issue a request and queue its completion. Returns the ovrRequest the guest
// will wait on — non-zero, and matched by the message, which together are the
// whole point.
static uint64_t klplat_request_str(const char *what, uint32_t type,
                                   int32_t payload, const char *str) {
    uint64_t id = klplat_enqueue(type, payload, str, 0, KLPLAT_KIND_NONE, NULL);
    if (!id) return klplat_request_failed(what);
    fprintf(stderr, "  [plat] %s -> request %llu, completion queued "
                    "(type 0x%08x, payload %d%s%s)\n",
            what, (unsigned long long)id, type, payload,
            str ? ", string " : "", str ? str : "");
    return id;
}

// ...and the failing spelling: the same request/completion pair, with the
// completion an error.
static uint64_t klplat_request_err(const char *what, uint32_t type) {
    uint64_t id = klplat_enqueue(type, 0, NULL, 1, KLPLAT_KIND_NONE, NULL);
    if (!id) return klplat_request_failed(what);
    fprintf(stderr, "  [plat] %s -> request %llu, ERROR completion queued "
                    "(type 0x%08x)\n", what, (unsigned long long)id, type);
    return id;
}

// ...and the int-payload spelling, which is all but one caller.
static uint64_t klplat_request(const char *what, uint32_t type, int32_t payload) {
    return klplat_request_str(what, type, payload, NULL);
}

// ...with the language-pack details riding on the message (copied into it; NULL
// for none). The details are attached in the same step that queues the message,
// so a pump on another thread cannot pop it without them.
static uint64_t klplat_request_asset(const char *what, uint32_t type, int kind,
                                     const klplat_asset *asset) {
    uint64_t id = klplat_enqueue(type, 0, NULL, 0, kind, asset);
    if (!id) return klplat_request_failed(what);
    fprintf(stderr, "  [plat] %s -> request %llu, completion queued "
                    "(type 0x%08x, %s)\n", what, (unsigned long long)id, type,
            kind == KLPLAT_KIND_NONE ? "payload 0" : "0 items");
    return id;
}

// Same queue, with the array kind the course getters switch on. `payload` is
// the element count captured at request time.
static uint64_t klplat_request_kind(const char *what, uint32_t type,
                                    int32_t payload, int kind) {
    uint64_t id = klplat_enqueue(type, payload, NULL, 0, kind, NULL);
    if (!id) return klplat_request_failed(what);
    fprintf(stderr, "  [plat] %s -> request %llu, completion queued "
                    "(type 0x%08x, %d item%s)\n",
            what, (unsigned long long)id, type, payload, payload == 1 ? "" : "s");
    return id;
}

// The language-pack list, as the last scan left it, copied into its message.
static uint64_t klplat_request_lang_list(const char *what, uint32_t type) {
    int n;
    uint64_t id = klplat_enqueue_lang_list(type, &n);
    if (!id) return klplat_request_failed(what);
    fprintf(stderr, "  [plat] %s -> request %llu, completion queued "
                    "(type 0x%08x, %d language pack%s)\n",
            what, (unsigned long long)id, type, n, n == 1 ? "" : "s");
    return id;
}

// The message pump. NULL still means "no messages queued", which is the
// documented way to say it and is the steady state for all but three calls a
// run — what changed is that it is no longer the ONLY thing this can say.
static void *klplat_PopMessage(void) {
    plat_hit("ovr_PopMessage");
    klplat_msg *out = NULL;
    pthread_mutex_lock(&g_msg_mu);
    if (g_msg_head) {
        out = g_msg_head;
        g_msg_head = out->next;
        if (!g_msg_head) g_msg_tail = NULL;
        out->next = NULL;
        out->state = KLPLAT_SLOT_POPPED;
    }
    pthread_mutex_unlock(&g_msg_mu);
    if (out)
        klplat_trace("ovr_PopMessage -> type 0x%08x request %llu payload %d err %d kind %d",
                     out->type, (unsigned long long)out->request, out->payload, out->err, out->kind);
    return out;
}

// The accessors the SDK's Message base class and the two payload wrappers call
// on that handle. Real functions rather than named trampolines, because these
// take the handle in x0 and a trampoline would put the entry point's NAME there.
//
// Every other Message accessor is deliberately absent — a message shape we do
// not produce should stop the run by name rather than answer.
static uint64_t klplat_Message_GetType(const klplat_msg *m) {
    plat_hit("ovr_Message_GetType");
    return m ? m->type : 0;
}
static uint64_t klplat_Message_IsError(const klplat_msg *m) {
    plat_hit("ovr_Message_IsError");
    return m ? (uint64_t)m->err : 0;
}
// The error handle IS the message, as the payload handle is. One error is ever
// produced, so its text is a constant: what happened, in words a person reading
// the game's own dialog can act on.
#define KLPLAT_ERR_NO_STORE \
    "The Oculus store is not available on this device, so nothing was purchased."
static uint64_t klplat_Message_GetError(const klplat_msg *m) {
    plat_hit("ovr_Message_GetError");
    return (uint64_t)(uintptr_t)m;
}
static uint64_t klplat_Error_GetCode(const klplat_msg *m) {
    plat_hit("ovr_Error_GetCode");
    (void)m;
    return 1;                     // non-zero, and no code the SDK assigns meaning to
}
static uint64_t klplat_Error_GetHttpCode(const klplat_msg *m) {
    plat_hit("ovr_Error_GetHttpCode");
    (void)m;
    return 0;                     // no request left this machine
}
static const char *klplat_Error_GetMessage(const klplat_msg *m) {
    plat_hit("ovr_Error_GetMessage");
    (void)m;
    return KLPLAT_ERR_NO_STORE;
}
static const char *klplat_Error_GetDisplayableMessage(const klplat_msg *m) {
    plat_hit("ovr_Error_GetDisplayableMessage");
    (void)m;
    return KLPLAT_ERR_NO_STORE;
}
// The checkout itself. A real function for the same reason the accessors are:
// the SKU arrives in x0.
static uint64_t klplat_IAP_LaunchCheckoutFlow(const char *sku) {
    plat_hit("ovr_IAP_LaunchCheckoutFlow");
    fprintf(stderr, "  [plat] checkout asked for SKU '%s' — no store on this host, "
                    "answering with an error\n", sku ? sku : "");
    return klplat_request_err("ovr_IAP_LaunchCheckoutFlow", KLPLAT_MSG_IAP_CHECKOUT);
}

// ---- language-pack status, continued ---------------------------------------
// A status answer's details ride IN its message (klplat_msg.asset): they live
// until the guest frees that message, and no later request can overwrite them.
// The eight-entry round-robin they replace (g_assets) was shared between
// threads with no lock and wrapped under any handle still held.

// The game's own manifest, in the OBB directory it reads its packs from.
// Returns 1 and fills size/tag if `name` is one of its lines.
static int langpack_in_manifest(const char *dir, const char *name,
                                unsigned long long *size, char *tag, size_t tagcap) {
    char mf[1200];
    snprintf(mf, sizeof mf, "%s/localized-asset-manifest.txt", dir);
    FILE *f = fopen(mf, "r");
    if (!f) return 0;
    char line[512];
    int hit = 0;
    while (!hit && fgets(line, sizeof line, f)) {
        char *c1 = strchr(line, ':');
        char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        if (!c2) continue;
        *c1 = 0; *c2 = 0;
        if (strcmp(c1 + 1, name) != 0) continue;
        *size = strtoull(c2 + 1, NULL, 10);
        snprintf(tag, tagcap, "%s", line);
        hit = 1;
    }
    fclose(f);
    return hit;
}

// The `<tag>.lang` files in AC Nexus's OBB directory (plat_is_obb_langpack_target).
// Rescanned on every question, so the answer is what is on disk now.
static const char *klplat_obb_lang_pkg(void) {
    const char *t = kl_driver_target_name();
    return t && strcmp(t, "wrath2") == 0 ? "com.Sanzaru.Wrath2" : "com.Ubisoft.ACNexusVR";
}
// g_lang and g_nlang (with the message pool) are written and read under
// g_msg_mu. The scan reads the disk into a private list first and commits it
// in one critical section. No handle the guest holds points into g_lang: a
// list message carries its own copy (klplat_enqueue_lang_list), as a status
// message does.
//
// Where the installation lists the packs (kl_ovrplat_asset_files_configured), the list
// is that one instead of the directory's: the store's ids, and installed only
// at exactly the listed size (klplat_read_asset_cfg).
// A file under the guest's files directory, in the guest's own spelling:
// /sdcard/<rest>. That directory IS the guest's external storage (kl_jni.c maps
// /sdcard and its aliases onto it), and /sdcard/Android/obb/<package>/de.lang
// is the path the platform reports on a headset. Unreal's Android file layer
// takes an asset file's path as a pak to mount only when it begins with
// GOBBFilePathBase/Android/obb/<package> (GOBBFilePathBase being /sdcard, the
// answer to getExternalStorageDirectory); any other absolute path it appends
// to its own Content/Paks directory. Deadpool VR then mounted nothing, offered
// German and played no voice at all (2026-10-16). Anything outside the files
// directory is left as it is.
static void klplat_guest_spelling(char *path, size_t cap) {
    const char *root = kl_jni_files_dir();
    size_t rl = root ? strlen(root) : 0;
    while (rl > 1 && root[rl - 1] == '/') rl--;
    if (!rl || strncmp(path, root, rl) != 0 || path[rl] != '/') return;
    char out[1024];
    if (snprintf(out, sizeof out, "/sdcard%s", path + rl) >= (int)sizeof out) return;
    snprintf(path, cap, "%s", out);
}
static int klplat_read_asset_cfg(klplat_asset *found, klplat_asset *listed, int *nlisted) {
    *nlisted = 0;
    kl_assetcfg_entry *e = calloc(KL_ASSETCFG_MAX, sizeof *e);
    char path[PATH_MAX];
    if (!e || !klplat_asset_cfg_path(path, sizeof path)) { free(e); return 0; }
    // Rejected lines are said the first time, not at every rescan.
    static int logged;
    int m = kl_assetcfg_load(path, kl_jni_obb_dir(), e, KL_ASSETCFG_MAX, logged ? NULL : stderr);
    if (m < 0 && !logged)
        fprintf(stderr, "  [plat] asset-file list %s cannot be read (or the OBB directory %s is "
                        "not absolute): no asset files reported\n", path, kl_jni_obb_dir());
    logged = 1;
    int n = 0;
    for (int i = 0; i < m; i++) {
        klplat_asset *a = &listed[(*nlisted)++];
        memset(a, 0, sizeof *a);
        a->id = e[i].id;
        snprintf(a->path, sizeof a->path, "%s", e[i].path);
        klplat_guest_spelling(a->path, sizeof a->path);
        snprintf(a->tag, sizeof a->tag, "%s", e[i].tag);
        snprintf(a->meta, sizeof a->meta, "%s", e[i].meta);
        a->status = e[i].installed ? "installed" : "available";
        if (e[i].installed) found[n++] = *a;
    }
    free(e);
    return n;
}
static int klplat_scan_obb_langpacks(void) {
    if (kl_ovrplat_asset_files_configured()) {
        klplat_asset *found = calloc(2 * KLPLAT_LANG_MAX, sizeof *found);
        if (!found) return 0;
        klplat_asset *listed = found + KLPLAT_LANG_MAX;
        int nlisted, n = klplat_read_asset_cfg(found, listed, &nlisted);
        static int said_n = -1, said_listed = -1;
        int say = 0;
        pthread_mutex_lock(&g_msg_mu);
        for (int i = 0; i < n; i++) g_lang[i] = found[i];
        g_nlang = n;
        for (int i = 0; i < nlisted; i++) g_lang_listed[i] = listed[i];
        g_nlang_listed = nlisted;
        if (said_n != n || said_listed != nlisted) { said_n = n; said_listed = nlisted; say = 1; }
        pthread_mutex_unlock(&g_msg_mu);
        if (say)
            for (int i = 0; i < nlisted; i++)
                fprintf(stderr, "  [plat] asset file %llu '%s' (%s): %s\n",
                        (unsigned long long)listed[i].id, listed[i].path, listed[i].tag,
                        strcmp(listed[i].status, "installed") == 0
                            ? "on disk at the listed size -> installed"
                            : "not on disk at the listed size -> not installed (it cannot be fetched from here)");
        free(found);
        return n;
    }
    klplat_asset found[KLPLAT_LANG_SCAN_MAX];
    int n = 0;
    const char *root = kl_jni_files_dir();
    char dir[PATH_MAX];
    DIR *d = NULL;
    if (root && root[0] &&
        snprintf(dir, sizeof dir, "%s/Android/obb/%s", root, klplat_obb_lang_pkg()) < (int)sizeof dir)
        d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d)) != NULL && n < (int)(sizeof found / sizeof found[0])) {
        size_t nl = strlen(de->d_name);
        if (nl <= 5 || strcmp(de->d_name + nl - 5, ".lang") != 0) continue;
        klplat_asset *a = &found[n];
        memset(a, 0, sizeof *a);
        if (nl - 5 >= sizeof a->tag ||
            snprintf(a->path, sizeof a->path, "%s/%s", dir, de->d_name) >= (int)sizeof a->path)
            continue;
        struct stat st;
        if (stat(a->path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        memcpy(a->tag, de->d_name, nl - 5);
        for (const char *p = de->d_name; *p; p++) a->id = a->id * 131 + (unsigned char)*p;
        if (!a->id) a->id = 1;
        a->status = "installed";
        n++;
    }
    if (d) closedir(d);
    static int said = -1;
    int say = 0;
    pthread_mutex_lock(&g_msg_mu);
    for (int i = 0; i < n; i++) g_lang[i] = found[i];
    g_nlang = n;
    if (said != n) { said = n; say = 1; }
    pthread_mutex_unlock(&g_msg_mu);
    if (say) {
        char tags[160] = ""; size_t o = 0;
        for (int i = 0; i < n && o + 34 < sizeof tags; i++)
            o += (size_t)snprintf(tags + o, sizeof tags - o, "%s%s", i ? " " : "", found[i].tag);
        fprintf(stderr, "  [plat] language packs on disk: %d (%s)\n", n, tags);
    }
    return n;
}
// A status request about one of them: the details ride on the message, as for
// batman. A name or id that is not one of these files is not a language pack,
// and the refusal stands. A pack the installation lists that is not on the
// disk at its size is "available": not installed, and not fetched from here.
static int klplat_langpack_is(const klplat_asset *a, const char *name, uint64_t id) {
    const char *base = strrchr(a->path, '/');
    return name ? (base && strcmp(base + 1, name) == 0) : a->id == id;
}
static uint64_t klplat_obb_langpack_status(const char *what, uint32_t type,
                                           const char *name, uint64_t id) {
    klplat_scan_obb_langpacks();
    klplat_asset a;
    int have = 0;
    pthread_mutex_lock(&g_msg_mu);
    for (int i = 0; i < g_nlang && !have; i++)
        if (klplat_langpack_is(&g_lang[i], name, id)) { a = g_lang[i]; have = 1; }
    for (int i = 0; i < g_nlang_listed && !have; i++)
        if (klplat_langpack_is(&g_lang_listed[i], name, id)) { a = g_lang_listed[i]; have = 1; }
    pthread_mutex_unlock(&g_msg_mu);
    if (!have) return klplat_drm(what);
    return klplat_request_asset(what, type, KLPLAT_KIND_NONE, &a);
}

// ---- the CURRENT language pack (Asgard's Wrath 2; plat_is_obb_langpack_target) ----
// The SDK's own numbers for the two completions (OVR_MessageType.h). Unreal's
// Oculus subsystem hands a completion to whoever made the request, by request
// id, so nothing here depends on them being right.
#define KLPLAT_MSG_LANGPACK_GET_CURRENT 0x1F90F0D5u
#define KLPLAT_MSG_LANGPACK_SET_CURRENT 0x5B4FBBE0u
static int klplat_langpack_choice_path(char *out, size_t cap) {
    const char *root = kl_jni_files_dir();
    return root && root[0] && snprintf(out, cap, "%s/klepton-language-pack.txt", root) < (int)cap;
}
// Which of the packs on the disk is the current one: the one that was chosen
// (a tag in klepton-language-pack.txt, written by SetCurrent below — and a
// chosen tag with no pack, "en", means none); with nothing chosen yet, the
// pack of the language the game is told the device speaks, when it is there.
// Copied into `out` under the lock; returns whether there is one.
static int klplat_langpack_current(klplat_asset *out) {
    if (klplat_scan_obb_langpacks() == 0) return 0;
    char tag[32] = "", path[PATH_MAX];
    int chosen = 0;
    if (klplat_langpack_choice_path(path, sizeof path)) {
        FILE *f = fopen(path, "r");
        if (f) { chosen = fscanf(f, "%31s", tag) == 1; fclose(f); }
    }
    if (!chosen) {
        char country[16];
        kl_jni_locale_parts(tag, sizeof tag, country, sizeof country);
    }
    int have = 0;
    pthread_mutex_lock(&g_msg_mu);
    for (int i = 0; i < g_nlang && !have; i++)
        if (strcasecmp(g_lang[i].tag, tag) == 0) { *out = g_lang[i]; have = 1; }
    pthread_mutex_unlock(&g_msg_mu);
    return have;
}
static uint64_t klplat_LanguagePack_GetCurrent(void) {
    plat_hit("ovr_LanguagePack_GetCurrent");
    if (!plat_is_obb_langpack_target())
        return klplat_request_err("ovr_LanguagePack_GetCurrent", KLPLAT_MSG_LANGPACK_GET_CURRENT);
    klplat_asset a;
    int have = klplat_langpack_current(&a);
    fprintf(stderr, "  [plat] current language pack: %s\n", have ? a.tag : "none");
    klplat_trace("ovr_LanguagePack_GetCurrent() -> %s", have ? a.tag : "none");
    // With none, the completion still succeeds and its language tag reads "".
    return klplat_request_asset("ovr_LanguagePack_GetCurrent", KLPLAT_MSG_LANGPACK_GET_CURRENT,
                                KLPLAT_KIND_NONE, have ? &a : NULL);
}
static uint64_t klplat_LanguagePack_SetCurrent(const char *tag) {
    plat_hit("ovr_LanguagePack_SetCurrent");
    klplat_trace("ovr_LanguagePack_SetCurrent(\"%s\")", tag ? tag : "(null)");
    if (!plat_is_obb_langpack_target() || !tag || !tag[0] || strlen(tag) > 31 || strpbrk(tag, "/ \t\r\n"))
        return klplat_request_err("ovr_LanguagePack_SetCurrent", KLPLAT_MSG_LANGPACK_SET_CURRENT);
    klplat_scan_obb_langpacks();
    klplat_asset a;
    int have = 0;
    pthread_mutex_lock(&g_msg_mu);
    for (int i = 0; i < g_nlang && !have; i++)
        if (strcasecmp(g_lang[i].tag, tag) == 0) { a = g_lang[i]; have = 1; }
    pthread_mutex_unlock(&g_msg_mu);
    // Remembered either way: a language with no pack ("en") is how a player
    // goes back to the voices the game ships with.
    char path[PATH_MAX];
    if (klplat_langpack_choice_path(path, sizeof path)) {
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "%s\n", tag); fclose(f); }
    }
    fprintf(stderr, "  [plat] language pack '%s' chosen: %s\n", tag,
            have ? "on the device -> current" : "not on the device (it cannot be fetched from here) -> no pack is current");
    if (!have) return klplat_request_err("ovr_LanguagePack_SetCurrent", KLPLAT_MSG_LANGPACK_SET_CURRENT);
    return klplat_request_asset("ovr_LanguagePack_SetCurrent", KLPLAT_MSG_LANGPACK_SET_CURRENT,
                                KLPLAT_KIND_LANGPACK_SET, &a);
}
static uint64_t klplat_Message_GetAssetFileDownloadResult(const klplat_msg *m) {
    plat_hit("ovr_Message_GetAssetFileDownloadResult");
    klplat_trace("ovr_Message_GetAssetFileDownloadResult(request %llu, kind %d)",
                 m ? (unsigned long long)m->request : 0ull, m ? m->kind : -1);
    return m && m->kind == KLPLAT_KIND_LANGPACK_SET ? (uint64_t)(uintptr_t)m->obj : 0;
}
static uint64_t klplat_AssetFileDownloadResult_GetAssetId(const klplat_asset *a) {
    plat_hit("ovr_AssetFileDownloadResult_GetAssetId");
    uint64_t r = a ? a->id : 0;
    klplat_trace("ovr_AssetFileDownloadResult_GetAssetId(id %llu %s) -> %llu", a ? (unsigned long long)a->id : 0ull, a ? a->tag : "NULL", (unsigned long long)r);
    return r;
}
static const char *klplat_AssetFileDownloadResult_GetFilepath(const klplat_asset *a) {
    plat_hit("ovr_AssetFileDownloadResult_GetFilepath");
    const char *r = a ? a->path : "";
    klplat_trace("ovr_AssetFileDownloadResult_GetFilepath(id %llu %s) -> \"%s\"", a ? (unsigned long long)a->id : 0ull, a ? a->tag : "NULL", r);
    return r;
}

static uint64_t klplat_langpack_StatusByName(const char *name) {
    plat_hit("ovr_AssetFile_StatusByName");
    if (plat_is_obb_langpack_target())
        return klplat_obb_langpack_status("ovr_AssetFile_StatusByName",
                                          KLPLAT_MSG_ASSET_STATUS_NAME, name ? name : "", 0);
    const char *files = kl_jni_files_dir();
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/obb", files ? files : ".");
    unsigned long long want = 0;
    char tag[32] = "";
    // A name outside the manifest is not a language pack, and what it is cannot
    // be known from here: that is the refusal's case, unchanged.
    if (!name || strchr(name, '/') || !langpack_in_manifest(dir, name, &want, tag, sizeof tag))
        return klplat_drm("ovr_AssetFile_StatusByName");

    klplat_asset a;
    memset(&a, 0, sizeof a);
    snprintf(a.path, sizeof a.path, "%s/%s", dir, name);
    snprintf(a.tag, sizeof a.tag, "%s", tag);
    for (const char *p = name; *p; p++) a.id = a.id * 131 + (unsigned char)*p;
    if (!a.id) a.id = 1;
    struct stat st;
    int there = stat(a.path, &st) == 0 && (unsigned long long)st.st_size == want;
    a.status = there ? "installed" : "available";
    fprintf(stderr, "  [plat] language pack '%s' (%s): %s\n", name, tag,
            there ? "on disk at the manifest's size -> installed"
                  : "not on disk -> available (it cannot be fetched from here)");

    return klplat_request_asset("ovr_AssetFile_StatusByName", KLPLAT_MSG_ASSET_STATUS_NAME,
                                KLPLAT_KIND_NONE, &a);
}
static uint64_t klplat_langpack_Message_GetAssetDetails(const klplat_msg *m) {
    plat_hit("ovr_Message_GetAssetDetails");
    const klplat_asset *a = m ? m->obj : NULL;
    klplat_trace("ovr_Message_GetAssetDetails(request %llu) -> %s (id %llu %s)",
                 m ? (unsigned long long)m->request : 0ull, a ? "details" : "NULL",
                 a ? (unsigned long long)a->id : 0ull, a ? a->status : "");
    return (uint64_t)(uintptr_t)a;
}
static uint64_t klplat_langpack_GetAssetId(const klplat_asset *a) {
    plat_hit("ovr_AssetDetails_GetAssetId");
    uint64_t r = a ? a->id : 0;
    klplat_trace("ovr_AssetDetails_GetAssetId(id %llu %s) -> %llu", a ? (unsigned long long)a->id : 0ull, a ? a->tag : "NULL", (unsigned long long)r);
    return r;
}
static const char *klplat_langpack_GetAssetType(const klplat_asset *a) {
    plat_hit("ovr_AssetDetails_GetAssetType");
    (void)a;
    const char *r = "language_pack";
    klplat_trace("ovr_AssetDetails_GetAssetType(id %llu %s) -> \"%s\"", a ? (unsigned long long)a->id : 0ull, a ? a->tag : "NULL", r);
    return r;
}
static const char *klplat_langpack_GetDownloadStatus(const klplat_asset *a) {
    plat_hit("ovr_AssetDetails_GetDownloadStatus");
    const char *r = a ? a->status : "available";
    klplat_trace("ovr_AssetDetails_GetDownloadStatus(id %llu %s) -> \"%s\"", a ? (unsigned long long)a->id : 0ull, a ? a->tag : "NULL", r);
    return r;
}
static const char *klplat_langpack_GetFilepath(const klplat_asset *a) {
    plat_hit("ovr_AssetDetails_GetFilepath");
    const char *r = a ? a->path : "";
    klplat_trace("ovr_AssetDetails_GetFilepath(id %llu %s) -> \"%s\"", a ? (unsigned long long)a->id : 0ull, a ? a->tag : "NULL", r);
    return r;
}
// Not for sale, so there is no purchase to have or lack.
static const char *klplat_langpack_GetIapStatus(const klplat_asset *a) {
    plat_hit("ovr_AssetDetails_GetIapStatus");
    (void)a;
    const char *r = "free";
    klplat_trace("ovr_AssetDetails_GetIapStatus(id %llu %s) -> \"%s\"", a ? (unsigned long long)a->id : 0ull, a ? a->tag : "NULL", r);
    return r;
}
// The language handle is the details: tag and both names are the locale.
static uint64_t klplat_AssetDetails_GetLanguage(const klplat_asset *a) {
    plat_hit("ovr_AssetDetails_GetLanguage");
    uint64_t r = (uint64_t)(uintptr_t)a;
    klplat_trace("ovr_AssetDetails_GetLanguage(id %llu %s) -> %p", a ? (unsigned long long)a->id : 0ull, a ? a->tag : "NULL", (void *)(uintptr_t)r);
    return r;
}
// The store's metadata of the file where the installation's list gives it
// (meta=, kl_assetcfg.h); "" for every pack that list does not describe.
static const char *klplat_langpack_GetMetadata(const klplat_asset *a) {
    plat_hit("ovr_AssetDetails_GetMetadata");
    const char *r = a ? a->meta : "";
    klplat_trace("ovr_AssetDetails_GetMetadata(id %llu %s) -> \"%s\"", a ? (unsigned long long)a->id : 0ull, a ? a->tag : "NULL", r);
    return r;
}
static const char *klplat_LanguagePackInfo_tag(const klplat_asset *a) {
    plat_hit("ovr_LanguagePackInfo_Get*");
    const char *r = a ? a->tag : "";
    klplat_trace("ovr_LanguagePackInfo_Get*(id %llu %s) -> \"%s\"", a ? (unsigned long long)a->id : 0ull, a ? a->tag : "NULL", r);
    return r;
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
// The slot goes back to the pool — only a popped one, and only once: a second
// free, a queued message or a pointer that is not a slot is ignored, so a guest
// mistake cannot put a slot on the free list twice.
static uint64_t klplat_FreeMessage(klplat_msg *m) {
    plat_hit("ovr_FreeMessage");
    pthread_mutex_lock(&g_msg_mu);
    klplat_msg *s = m ? klplat_msg_slot_locked(m) : NULL;
    if (s && s->state == KLPLAT_SLOT_POPPED) {
        s->state = KLPLAT_SLOT_FREE;
        s->next = g_msg_free;
        g_msg_free = s;
    }
    pthread_mutex_unlock(&g_msg_mu);
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
// ---------------------------------------------------------------------------
// Walkabout courses that are actually on disk
//
// The menu loads from Addressables. Starting a course is a second question:
// does this viewer own it? The game asks the Oculus platform (GetViewerPurchases
// and the asset-file list) and will not start a course the answer leaves out.
//
// The tracks themselves are already here when the user copied them off a Quest
// they own. They sit beside the packaged OBB, one
// `<prefix>common_assets_.bundle` per course. A real bundle is megabytes. The
// placeholder tools/walkabout_stub_bundles.py writes is a few kilobytes, and
// that is not a course — glooplair stays locked while it is a stub.
//
// So the platform reports a course installed when that bundle is present and
// large. Purchased is a separate fact and does not follow from the file: its
// store ids are reported only for a course the store itself confirmed (see
// klplat_load_entitlements). Download, Delete and ConsumePurchase still abort:
// nothing is fetched, and a course with no file is absent from the asset list. Every other title keeps the old answers (an empty
// asset list, and request id 0 for purchases).
//
// The store ids are the ones serialized in the game's own CourseIAP assets
// (the product id, the course key, and the numeric Oculus SKUs). Whichever of
// those DoesUserOwnProduct compares against, it is one of that course's ids
// and no other course's.

// 48 held Walkabout's courses; a title with one add-on file per song has hundreds.
enum { KLPLAT_COURSE_MAX = 384, KLPLAT_PURCHASE_MAX = 512, KLPLAT_PRODUCT_MAX = 256 };
enum { KLPLAT_COURSE_MIN_BYTES = 1 << 20 };

typedef struct {
    char prefix[48];
    char path[768];
    uint64_t id;
    int iap_row;                 // index into k_course_iap, or -1 for a base course
    int delivered;               // 1: an add-on file the store itself delivered (see klplat_scan_delivered)
} klplat_course;

typedef struct {
    const char *sku;
    uint64_t id;
} klplat_purchase;

typedef struct {
    const char *sku;
} klplat_product;

static klplat_course   g_courses[KLPLAT_COURSE_MAX];
static int             g_ncourses;
static klplat_course   g_missing = { .iap_row = -1 };
static klplat_purchase g_purchases[KLPLAT_PURCHASE_MAX];
static int             g_npurchases;
/* "Tiki" is also asked as "Tiki-Easy" / "Tiki-Hard": the game's own IAP-suffix
   check rejects '_' and requires '-'. Built at scan time, only for a course
   whose bundle is present. */
static char            g_suffix_sku[160][96];
static int             g_nsuffix;
static klplat_product  g_products[KLPLAT_PRODUCT_MAX];
static int             g_nproducts;

static const char *const k_sku_alice[] = {
    "Alice", "alice",
    "0508570229647579", "0418993343719872", "0458073381106966", "0923353526946195", NULL };
static const char *const k_sku_amusementpark[] = {
    "Fairyland", "ForgottenFairyland", "AmusementPark", "fairyland", "amusementpark",
    "0231896114137487", "0118522714705222", "0979900971745119", "0929255417249534", NULL };
static const char *const k_sku_aroundtheworld[] = {
    "AroundTheWorld", "around_the_world", "aroundtheworld",
    "0809673849171947", "0381387977600117", "0464846572721676", "0089224930412381", NULL };
static const char *const k_sku_atlantis[] = {
    "Atlantis", "atlantis",
    "0193615521010795", "0126263252455057", "0785996778995100", "0070235305715118", NULL };
static const char *const k_sku_babylon[] = {
    "GardensOfBabylon", "Babylon", "gardens_of_babylon", "babylon",
    "0883465875498324", "0468433596909097", "0357028625964721", "0440901715502036", NULL };
static const char *const k_sku_blokhaven[] = {
    "Blokhaven", "blokhaven",
    "0792874719224960", "0448237864330080", "0186685347544838", "0833133854582451", NULL };
static const char *const k_sku_christmas[] = {
    "HolidayHideaway", "Christmas", "holiday_hideaway", "christmas",
    "0539445275378225", "0738663383377858", "0353303721845295", "0465685194506867", NULL };
static const char *const k_sku_crystallair[] = {
    "CrystalLair", "crystal_lair", "crystallair",
    "0595272659142648", "0692494830833590", "0389051848841963", "0791551254406180", NULL };
static const char *const k_sku_dinotrek[] = {
    "RaptorCliffs", "DinoTrek", "raptor_cliffs", "dinotrek",
    "0280952767786223", "0657765835048147", "0757357488987015", "0295622698767353", NULL };
static const char *const k_sku_eldorado[] = {
    "ElDorado", "el_dorado", "eldorado",
    "0782009790857091", "0608881966116657", "0736786255141041", "0353137664124339", NULL };
static const char *const k_sku_glooplair[] = {
    "GloopLair", "glooplair",
    "0039385890638181", "0393122296438243", "0432479671626154", "0208424449522073", NULL };
static const char *const k_sku_hollywood[] = {
    "Hollywood", "hollywood",
    "0851582513712615", "0268100986231193", "0499491591065399", "0333623169506999", NULL };
static const char *const k_sku_journey[] = {
    "Journey", "journey",
    "0201630851737472", "0924377305170762", "0368521921284294", "0554013985097819", NULL };
static const char *const k_sku_labyrinth[] = {
    "Labyrinth", "labyrinth",
    "0386676296379164", "0519765801230615", "0794463215268664", "0975451948660469", NULL };
static const char *const k_sku_mars[] = {
    "MarsGardens", "Mars", "mars_gardens", "mars",
    "0447460470797810", "0584906650980393", "0842789959214737", "0488127592926092", NULL };
static const char *const k_sku_meowwolf[] = {
    "MeowWolf", "meow_wolf", "meowwolf",
    "0176441726772604", "0558530222121807", "0625332469605345", "0815433443254192", NULL };
static const char *const k_sku_myst[] = {
    "Myst", "myst",
    "0652801418511180", "0370509882972218", "0053133987085667", "0282333553843204", NULL };
static const char *const k_sku_olympus[] = {
    "Olympus", "olympus",
    "0981989358512753", "0527839967097480", "0491689512026715", "0325529197532150", NULL };
static const char *const k_sku_tiki[] = {
    "Tiki", "tiki",
    "0747504920995321", "0843678180221081", "0822281563296852", "0237479462796302", NULL };
static const char *const k_sku_tokyo[] = {
    "Tokyo", "tokyo",
    "0284462869323706", "0944106475990195", "0427314937628732", "0357998668137809", NULL };
static const char *const k_sku_vegas[] = {
    "Vegas", "vegas", "VivaLasElvis", "viva_las_elvis",
    "0342954105855049", "0680995419979357", "0192938135998486", "0919699210002063", NULL };
static const char *const k_sku_venice[] = {
    "Venice", "venice",
    "0818815108952217", "0711539433621922", "0449413071930315", "0814372382135912", NULL };
static const char *const k_sku_villains8bit[] = {
    "8BitLair", "8Bit", "Villains8Bit", "8bit_lair", "villains8bit",
    "0457632226017672", "0955888416106811", "0820833223912158", "0708386203900748", NULL };
static const char *const k_sku_villainsarctic[] = {
    "IceLair", "VillainsArctic", "ice_lair", "villainsarctic",
    "0927683480793557", "0242239932998891", "0336458101349780", "0189649934441094", NULL };
static const char *const k_sku_wallace[] = {
    "WallaceAndGromit", "wallace_and_gromit", "wallaceandgromit",
    "0569395155549669", "0436490607169566", "0706704701080124", "0413363645758406", NULL };
static const char *const k_sku_widows[] = {
    "Widows", "Haunted", "widowswalkabout", "widows_walkabout",
    "0953970477189139", "0056559795905160", "0151583285449966", "0493989045534602", NULL };
/* Laser Lair's course bundle is real, but this build's CourseIAP catalogue has
   no numeric store id for it. The names are the course's own (courseinfos). */
static const char *const k_sku_laserlair[] = {
    "LaserLair", "VillainsTropical", "laserlair", NULL };
static const char *const k_sku_zerzura[] = {
    "Zerzura", "zerzura",
    "0246728596865682", "0590424361368553", "0094566846310398", "0125621824615283", NULL };

// Four courses the table above did not know were sold. Their SKUs are the ones
// the store's own purchase list (viewer_purchases) names; the prefixes are the
// bundle names in the build's asset list. Without a row a course reported "free"
// to every owner of the base game.
static const char *const k_sku_20k[]       = { "20000leagues", NULL };
static const char *const k_sku_alfheim[]   = { "alfheim", NULL };
static const char *const k_sku_shangrila[] = { "ShangriLa", NULL };
static const char *const k_sku_sweetopia[] = { "sweetopia", NULL };

static const struct { const char *prefix; const char *const *skus; } k_course_iap[] = {
    { "20k",              k_sku_20k },
    { "alfheim",          k_sku_alfheim },
    { "shangri-la",       k_sku_shangrila },
    { "sweetopia",        k_sku_sweetopia },
    { "alice",            k_sku_alice },
    { "amusementpark",    k_sku_amusementpark },
    { "aroundtheworld",   k_sku_aroundtheworld },
    { "atlantis",         k_sku_atlantis },
    { "babylon",          k_sku_babylon },
    { "blokhaven",        k_sku_blokhaven },
    { "christmas",        k_sku_christmas },
    { "crystallair",      k_sku_crystallair },
    { "dinotrek",         k_sku_dinotrek },
    { "eldorado",         k_sku_eldorado },
    { "glooplair",        k_sku_glooplair },
    { "hollywood",        k_sku_hollywood },
    { "journey",          k_sku_journey },
    { "labyrinth",        k_sku_labyrinth },
    { "laserlair",        k_sku_laserlair },
    { "mars",             k_sku_mars },
    { "meowwolf",         k_sku_meowwolf },
    { "myst",             k_sku_myst },
    { "olympus",          k_sku_olympus },
    { "tiki",             k_sku_tiki },
    { "tokyo",            k_sku_tokyo },
    { "vegas",            k_sku_vegas },
    { "venice",           k_sku_venice },
    { "villains8bit",     k_sku_villains8bit },
    { "villainsarctic",   k_sku_villainsarctic },
    { "wallaceandgromit", k_sku_wallace },
    { "widowswalkabout",  k_sku_widows },
    { "zerzura",          k_sku_zerzura },
};

static int klplat_is_walkabout(void) {
    const char *t = kl_driver_target_name();
    return t && strcmp(t, "walkabout") == 0;
}

static uint64_t klplat_fnv(const char *s) {
    uint64_t h = 14695981039346656037ull;
    if (!s) return 1;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ull;
    }
    return h ? h : 1;
}

static int klplat_course_cmp(const void *a, const void *b) {
    return strcmp(((const klplat_course *)a)->prefix,
                  ((const klplat_course *)b)->prefix);
}

/* A product id ("Tiki", "8BitLair") rather than a numeric store SKU or a
   snake_case key. Those are the ids the difficulty suffix attaches to. */
static int klplat_is_product_id(const char *s) {
    if (!s || !s[0]) return 0;
    int upper = 0, digit_only = 1;
    for (const char *p = s; *p; p++) {
        int ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                 (*p >= '0' && *p <= '9');
        if (!ok) return 0;
        if (*p >= 'A' && *p <= 'Z') upper = 1;
        if (*p < '0' || *p > '9') digit_only = 0;
    }
    return upper && !digit_only;
}

static void klplat_add_purchase(const char *sku) {
    if (!sku || !sku[0] || g_npurchases >= KLPLAT_PURCHASE_MAX) return;
    for (int i = 0; i < g_npurchases; i++)
        if (strcmp(g_purchases[i].sku, sku) == 0) return;
    g_purchases[g_npurchases].sku = sku;
    g_purchases[g_npurchases].id = klplat_fnv(sku);
    g_npurchases++;
}

// ---- purchases the store has confirmed --------------------------------------
//
// Which courses this viewer bought is not something the disk can say: the
// build's own expansion-files-config.json marks every course bundle `required`,
// so every owner of the base game has every paid course's files. Reporting a
// course as purchased because its bundle is present therefore unlocked all of
// them for everyone.
//
// The answer comes from the store instead. The Mac side asks it
// (GET graph.oculus.com/<app>/viewer_purchases, with the user's own token) and
// writes the SKUs it returned to <files>/klepton-entitlements.txt, one
// `sku <name>` per line. This side only reads that file: no token is ever on
// the device, and with no file there are no purchases. A failed refresh leaves
// the last confirmed file in place — that is decided where the file is written,
// not here.
enum { KLPLAT_OWNED_MAX = 512 };
static char g_owned[KLPLAT_OWNED_MAX][96];
static int  g_nowned;

static void klplat_load_entitlements(void) {
    static long long seen_mtime = -1, seen_size = -1;
    const char *root = kl_jni_files_dir();
    char path[PATH_MAX];
    struct stat st;
    if (!root || !root[0] ||
        snprintf(path, sizeof path, "%s/klepton-entitlements.txt", root) >= (int)sizeof path ||
        stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (seen_mtime != 0 || seen_size != 0)
            fprintf(stderr, "  [plat] no klepton-entitlements.txt in %s — no purchases reported\n",
                    root ? root : "(no files dir)");
        seen_mtime = 0; seen_size = 0;
        g_nowned = 0;
        return;
    }
    // Unchanged file, unchanged list: a guest may still hold a SKU string from
    // an earlier answer, so the buffers are only rewritten when the file was.
    if ((long long)st.st_mtime == seen_mtime && (long long)st.st_size == seen_size) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    g_nowned = 0;
    char line[256], verified[64] = "";
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (strncmp(line, "verified ", 9) == 0) snprintf(verified, sizeof verified, "%s", line + 9);
        if (strncmp(line, "sku ", 4) != 0 || !line[4]) continue;
        if (g_nowned >= KLPLAT_OWNED_MAX || n - 4 >= sizeof g_owned[0]) continue;
        memcpy(g_owned[g_nowned++], line + 4, n - 4 + 1);
    }
    fclose(f);
    seen_mtime = (long long)st.st_mtime; seen_size = (long long)st.st_size;
    fprintf(stderr, "  [plat] store-confirmed purchases: %d SKU%s (verified %s)\n",
            g_nowned, g_nowned == 1 ? "" : "s", verified[0] ? verified : "at an unknown time");
}

// The store's SKUs are compared without case: it says "Blokhaven" where a
// game's own asset may spell the key "blokhaven".
static int klplat_owns_sku(const char *sku) {
    if (!sku) return 0;
    for (int i = 0; i < g_nowned; i++)
        if (strcasecmp(g_owned[i], sku) == 0) return 1;
    return 0;
}

// KL_PLAT_IAP_CONFIRMED — the viewer's purchases are what the store confirmed.
//
// A title whose add-ons unlock on the purchase list alone needs no table here:
// Maestro (x6963344583787383, UE4) asks ovr_IAP_GetViewerPurchases, takes each
// SKU as the name of an add-on of its own (Maestro_DLC_Classical), and looks
// the pak up in its own config ([Maestro/DLCToAssetName] in DefaultGame.ini:
// Maestro_DLC_Classical=pakchunk4-). The pak it mounts at start from its OBB
// directory, bought or not — the store does not deliver one that is not, and
// the purchase is still asked. Refused (request id 0), the add-on stayed
// locked with its pak mounted.
//
// So for such a title the answer is the file's SKUs as they are, nothing
// derived and nothing added, and with no file the list is empty.
// Default on for that title alone; the switch overrides in either direction.
static int klplat_iap_confirmed(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_PLAT_IAP_CONFIRMED", t && (strcmp(t, "x6963344583787383") == 0 ||
                                                    // BODYCOMBAT: its add-ons follow the purchase list
                                                    strcmp(t, "x4015163475201433") == 0));
    }
    return on;
}

// A course is bought when the store confirmed any one of its ids.
static int klplat_course_owned(int row) {
    if (row < 0) return 0;
    for (const char *const *s = k_course_iap[row].skus; *s; s++)
        if (klplat_owns_sku(*s)) return 1;
    return 0;
}

// ---- add-on files the store delivered ----------------------------------------
//
// A title whose add-ons are separate asset files (Beat Saber's songs) asks
// ovr_AssetFile_GetList which of them are on the device and reads each one's
// id, path and purchase status. For such a title the store refuses to deliver a
// file the account does not own ("Not entitled to download"), so a file that
// was delivered IS an owned one, and what is reported here is exactly that set:
//
//   <files>/klepton-assets/index.txt     one `asset <store asset id> <name>` per line
//   <files>/klepton-assets/<name>        the file as the store delivered it
//
// Both are written on the Mac from the store's own listing and download. A
// line whose file is missing or empty is not reported, and with no index the
// list is empty — the answer every title had before. Nothing is fetched,
// deleted or purchased from here.
static int klplat_scan_delivered(void) {
    const char *root = kl_jni_files_dir();
    char dir[PATH_MAX], path[PATH_MAX];
    if (!root || !root[0] ||
        snprintf(dir, sizeof dir, "%s/klepton-assets", root) >= (int)sizeof dir ||
        snprintf(path, sizeof path, "%s/index.txt", dir) >= (int)sizeof path)
        return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    int listed = 0;
    while (fgets(line, sizeof line, f) && g_ncourses < KLPLAT_COURSE_MAX) {
        unsigned long long id = 0;
        char name[128];
        if (sscanf(line, "asset %llu %127s", &id, name) != 2 || !id) continue;
        if (strchr(name, '/') || strstr(name, "..")) continue;
        listed++;
        klplat_course *c = &g_courses[g_ncourses];
        if (strlen(name) >= sizeof c->prefix) continue;
        int pw = snprintf(c->path, sizeof c->path, "%s/%s", dir, name);
        if (pw < 0 || (size_t)pw >= sizeof c->path) continue;
        struct stat st;
        if (stat(c->path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        snprintf(c->prefix, sizeof c->prefix, "%s", name);
        c->id = (uint64_t)id;
        c->iap_row = -1;
        c->delivered = 1;
        g_ncourses++;
    }
    fclose(f);
    static int said = -1;
    if (said != g_ncourses) {
        said = g_ncourses;
        fprintf(stderr, "  [plat] add-on files delivered by the store: %d on disk of %d indexed\n",
                g_ncourses, listed);
    }
    return g_ncourses;
}

// KL_PLAT_OBB_ASSETS — the title's own content files as its asset files.
//
// Some titles ship their content as the store's "asset files" beside the OBB —
// Tales from the Galaxy's Edge: Season1Taco-…pak and two sound-bank paks, each
// a required download with a store asset id of its own. The engine mounts them
// like any pak; the GAME, separately, asks ovr_AssetFile_GetList which of them
// are installed before it starts its first level, and with an empty answer it
// waits (a black room behind a full fade, with sound).
//
// So for such a title the list is what is in the OBB directory: every loose
// container file there, as installed. They are there because the store's own
// tool delivered them to this account, which is the rule the index above
// rests on too — a file the account is not entitled to is not delivered, and
// a file that is not there is not listed (this title's paid second part).
// The id is the store's where the index names one for the file; without, it
// is a number made from the file name, which the title only hands back to
// requests that are refused here anyway (download, delete).
//
// Maestro (x6963344583787383) asks the same list at every start, after its
// purchases, to see which of its add-on paks the store has a newer file for
// (UAsyncAction_GetUpdatableDLCs_Oculus: mounted, "entitled" and "available"),
// and again when an owned add-on is to be mounted. Its paks are asset files in
// the OBB directory too, so the answer is the same one: what is there, as
// installed — never "available", so it asks for no download.
// Default on for those two; the switch overrides.
static int klplat_obb_assets_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_PLAT_OBB_ASSETS", t && (strcmp(t, "x3484270071659289") == 0 ||
                                                   strcmp(t, "x6963344583787383") == 0));
    }
    return on;
}

static int klplat_scan_obb_assets(void) {
    if (!klplat_obb_assets_on()) return g_ncourses;
    const char *dir = kl_jni_obb_dir();
    DIR *d = dir && dir[0] ? opendir(dir) : NULL;
    if (!d) return g_ncourses;
    int added = 0;
    struct dirent *e;
    while ((e = readdir(d)) && g_ncourses < KLPLAT_COURSE_MAX) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || (strcmp(dot, ".pak") && strcmp(dot, ".utoc") && strcmp(dot, ".ucas"))) continue;
        int have = 0;
        for (int i = 0; i < g_ncourses; i++)
            if (strcmp(g_courses[i].prefix, e->d_name) == 0) { have = 1; break; }
        if (have) continue;                       // the index named it, with the store's id
        klplat_course *c = &g_courses[g_ncourses];
        if (strlen(e->d_name) >= sizeof c->prefix) continue;
        int pw = snprintf(c->path, sizeof c->path, "%s/%s", dir, e->d_name);
        if (pw < 0 || (size_t)pw >= sizeof c->path) continue;
        struct stat st;
        if (stat(c->path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        snprintf(c->prefix, sizeof c->prefix, "%s", e->d_name);
        c->id = klplat_fnv(e->d_name) >> 12;      // fits the 53 bits a script-side number keeps
        if (!c->id) c->id = 1;
        c->iap_row = -1;
        c->delivered = 1;
        g_ncourses++;
        added++;
    }
    closedir(d);
    static int said = -1;
    if (said != added) {
        said = added;
        fprintf(stderr, "  [plat] content files in the OBB directory reported as installed asset files: %d\n", added);
    }
    return g_ncourses;
}

static void klplat_scan_courses(void) {
    g_ncourses = 0;
    g_npurchases = 0;
    g_nsuffix = 0;
    if (!klplat_is_walkabout()) {
        if (klplat_iap_confirmed()) {
            klplat_load_entitlements();
            for (int i = 0; i < g_nowned; i++) klplat_add_purchase(g_owned[i]);
        }
        klplat_scan_delivered();
        klplat_scan_obb_assets();
        return;
    }
    // What the store confirmed is reported as it is, whether or not this
    // course table knows the SKU or the course's files are here.
    klplat_load_entitlements();
    for (int i = 0; i < g_nowned; i++) klplat_add_purchase(g_owned[i]);
    const char *root = kl_jni_files_dir();
    if (!root || !root[0]) return;
    char dir[PATH_MAX];
    int nw = snprintf(dir, sizeof dir,
                      "%s/Android/obb/com.MightyCoconut.WalkaboutMiniGolf", root);
    if (nw < 0 || (size_t)nw >= sizeof dir) return;
    DIR *d = opendir(dir);
    if (!d) return;

    static const char suf[] = "common_assets_.bundle";
    const size_t sl = sizeof suf - 1;
    int stubs = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_ncourses < KLPLAT_COURSE_MAX) {
        const char *name = de->d_name;
        size_t nl = strlen(name);
        if (nl <= sl || strcmp(name + (nl - sl), suf) != 0) continue;
        size_t plen = nl - sl;
        if (plen == 0 || plen >= sizeof g_courses[0].prefix) continue;
        char path[PATH_MAX];
        int pw = snprintf(path, sizeof path, "%s/%s", dir, name);
        if (pw < 0 || (size_t)pw >= sizeof path) continue;
        if ((size_t)pw >= sizeof g_courses[0].path) continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (st.st_size < KLPLAT_COURSE_MIN_BYTES) { stubs++; continue; }

        klplat_course *c = &g_courses[g_ncourses];
        memcpy(c->prefix, name, plen);
        c->prefix[plen] = '\0';
        memcpy(c->path, path, (size_t)pw + 1);
        c->id = klplat_fnv(c->prefix);
        c->iap_row = -1;
        c->delivered = 0;
        for (int i = 0; i < (int)(sizeof k_course_iap / sizeof k_course_iap[0]); i++) {
            if (strcmp(k_course_iap[i].prefix, c->prefix) == 0) {
                c->iap_row = i;
                break;
            }
        }
        g_ncourses++;
    }
    closedir(d);

    if (g_ncourses > 1)
        qsort(g_courses, (size_t)g_ncourses, sizeof g_courses[0], klplat_course_cmp);

    for (int i = 0; i < g_ncourses; i++) {
        int row = g_courses[i].iap_row;
        if (row < 0) continue;
        // The other spellings of a course's id (its key, the numeric store
        // SKUs, the -Easy/-Hard forms) follow only a course the store confirmed.
        if (!klplat_course_owned(row)) continue;
        for (const char *const *s = k_course_iap[row].skus; *s; s++) {
            klplat_add_purchase(*s);
            if (!klplat_is_product_id(*s) || g_nsuffix + 2 > (int)(sizeof g_suffix_sku / sizeof g_suffix_sku[0]))
                continue;
            snprintf(g_suffix_sku[g_nsuffix], sizeof g_suffix_sku[0], "%s-Easy", *s);
            klplat_add_purchase(g_suffix_sku[g_nsuffix++]);
            snprintf(g_suffix_sku[g_nsuffix], sizeof g_suffix_sku[0], "%s-Hard", *s);
            klplat_add_purchase(g_suffix_sku[g_nsuffix++]);
        }
    }

    char listed[2048];
    size_t u = 0;
    listed[0] = '\0';
    for (int i = 0; i < g_ncourses && u + 1 < sizeof listed; i++) {
        int w = snprintf(listed + u, sizeof listed - u, "%s%s",
                         u ? " " : "", g_courses[i].prefix);
        if (w < 0) break;
        if ((size_t)w >= sizeof listed - u) { listed[sizeof listed - 1] = '\0'; break; }
        u += (size_t)w;
    }
    static char sig[2048];
    static int sig_stubs = -1;
    if (strcmp(listed, sig) != 0 || stubs != sig_stubs) {
        snprintf(sig, sizeof sig, "%s", listed);
        sig_stubs = stubs;
        int sold = 0, bought = 0;
        for (int i = 0; i < g_ncourses; i++) {
            if (g_courses[i].iap_row < 0) continue;
            sold++;
            bought += klplat_course_owned(g_courses[i].iap_row);
        }
        fprintf(stderr, "  [plat] walkabout courses on disk: %s "
                        "(%d sold separately, %d of those store-confirmed; "
                        "%d purchase ids, %d stub%s ignored)\n",
                g_ncourses ? listed : "(none)", sold, bought,
                g_npurchases, stubs, stubs == 1 ? "" : "s");
    }
}

static int klplat_course_named(const klplat_course *c, const char *name) {
    if (!c || !name || !name[0]) return 0;
    if (strcmp(name, c->prefix) == 0 || strcmp(name, c->path) == 0) return 1;
    const char *base = strrchr(c->path, '/');
    base = base ? base + 1 : c->path;
    if (strcmp(name, base) == 0) return 1;
    if (c->iap_row >= 0) {
        for (const char *const *s = k_course_iap[c->iap_row].skus; *s; s++) {
            if (strcmp(name, *s) == 0) return 1;
            if (!klplat_is_product_id(*s)) continue;
            size_t n = strlen(*s);
            if (strncmp(name, *s, n) == 0 &&
                (strcmp(name + n, "-Easy") == 0 || strcmp(name + n, "-Hard") == 0))
                return 1;
        }
    }
    return 0;
}

static int klplat_find_course_name(const char *name) {
    for (int i = 0; i < g_ncourses; i++)
        if (klplat_course_named(&g_courses[i], name)) return i;
    return -1;
}

static int klplat_find_course_id(uint64_t id) {
    if (!id) return -1;
    for (int i = 0; i < g_ncourses; i++)
        if (g_courses[i].id == id) return i;
    return -1;
}

static const klplat_course *klplat_as_course(const void *h) {
    if (!h || h == &g_missing) return &g_missing;
    const klplat_course *c = h;
    if (c >= g_courses && c < g_courses + g_ncourses) return c;
    return &g_missing;
}

static const klplat_purchase *klplat_as_purchase(const void *h) {
    if (!h) return NULL;
    const klplat_purchase *p = h;
    if (p >= g_purchases && p < g_purchases + g_npurchases) return p;
    return NULL;
}

static const klplat_product *klplat_as_product(const void *h) {
    if (!h) return NULL;
    const klplat_product *p = h;
    if (p >= g_products && p < g_products + g_nproducts) return p;
    return NULL;
}

static uint64_t klplat_AssetFile_GetList(void) {
    plat_hit("ovr_AssetFile_GetList");
    if (plat_is_obb_langpack_target()) {
        klplat_scan_obb_langpacks();
        uint64_t r = klplat_request_lang_list("ovr_AssetFile_GetList", KLPLAT_MSG_ASSET_LIST);
        klplat_trace("ovr_AssetFile_GetList() -> request %llu", (unsigned long long)r);
        return r;
    }
    klplat_scan_courses();
    return klplat_request_kind("ovr_AssetFile_GetList", KLPLAT_MSG_ASSET_LIST,
                               g_ncourses, KLPLAT_KIND_ASSETS);
}

static uint64_t klplat_AssetFile_StatusByName(const char *name) {
    klplat_trace("ovr_AssetFile_StatusByName(\"%s\")", name ? name : "(null)");
    // batman asks this about its language packs, Walkabout about its courses.
    if (plat_is_langpack_target()) return klplat_langpack_StatusByName(name);
    plat_hit("ovr_AssetFile_StatusByName");
    klplat_scan_courses();
    if (!klplat_is_walkabout() && g_ncourses == 0) klplat_drm("ovr_AssetFile_StatusByName");
    int i = klplat_find_course_name(name);
    fprintf(stderr, "  [plat] AssetFile_StatusByName('%s') -> %s\n",
            name ? name : "", i >= 0 ? "installed" : "not present");
    return klplat_request_kind("ovr_AssetFile_StatusByName",
                               KLPLAT_MSG_ASSET_STATUS_NAME, i, KLPLAT_KIND_ASSET);
}

static uint64_t klplat_AssetFile_StatusById(uint64_t id) {
    plat_hit("ovr_AssetFile_StatusById");
    klplat_trace("ovr_AssetFile_StatusById(%llu)", (unsigned long long)id);
    if (plat_is_obb_langpack_target())
        return klplat_obb_langpack_status("ovr_AssetFile_StatusById",
                                          KLPLAT_MSG_ASSET_STATUS_ID, NULL, id);
    klplat_scan_courses();
    // Outside Walkabout this is answered only where the store delivered add-on
    // files (klplat_scan_delivered); with none, the refusal is unchanged.
    if (!klplat_is_walkabout() && g_ncourses == 0) klplat_drm("ovr_AssetFile_StatusById");
    int i = klplat_find_course_id(id);
    return klplat_request_kind("ovr_AssetFile_StatusById",
                               KLPLAT_MSG_ASSET_STATUS_ID, i, KLPLAT_KIND_ASSET);
}

static uint64_t klplat_AssetDetailsArray_GetSize(const klplat_msg *m) {
    plat_hit("ovr_AssetDetailsArray_GetSize");
    uint64_t r = !m || m->kind != KLPLAT_KIND_ASSETS || m->payload < 0 ? 0 : (uint64_t)m->payload;
    klplat_trace("ovr_AssetDetailsArray_GetSize(request %llu) -> %llu",
                 m ? (unsigned long long)m->request : 0ull, (unsigned long long)r);
    return r;
}

static void *klplat_AssetDetailsArray_GetElement(const klplat_msg *m, int index) {
    plat_hit("ovr_AssetDetailsArray_GetElement");
    if (!m || m->kind != KLPLAT_KIND_ASSETS) return NULL;
    // A language-pack list answers out of its own copy, which nothing but a
    // reuse of the slot after ovr_FreeMessage ever writes — no lock to take,
    // and the handle stays good while the message is held.
    if (m->elems) {
        const klplat_asset *a = index >= 0 && index < m->payload ? &m->elems[index] : NULL;
        klplat_trace("ovr_AssetDetailsArray_GetElement(request %llu, %d) -> %s",
                     (unsigned long long)m->request, index, a ? a->tag : "NULL");
        return (void *)(uintptr_t)a;
    }
    if (plat_is_obb_langpack_target()) return NULL;
    if (index < 0 || index >= m->payload || index >= g_ncourses) return NULL;
    return &g_courses[index];
}

static uint64_t klplat_Message_GetAssetDetails(const klplat_msg *m) {
    if (plat_is_langpack_target()) return klplat_langpack_Message_GetAssetDetails(m);
    plat_hit("ovr_Message_GetAssetDetails");
    if (!m || m->kind != KLPLAT_KIND_ASSET) return 0;
    if (m->payload < 0 || m->payload >= g_ncourses)
        return (uint64_t)(uintptr_t)&g_missing;
    return (uint64_t)(uintptr_t)&g_courses[m->payload];
}

// Delivery is still refused, so the delivery list stays empty even when the
// details list above names courses that are already on disk.
static uint64_t klplat_AssetFileDeliveryList_GetSize(const void *h) {
    plat_hit("ovr_AssetFileDeliveryList_GetSize");
    (void)h;
    return 0;
}

static uint64_t klplat_AssetDetails_GetAssetId(const void *h) {
    if (plat_is_langpack_target()) return klplat_langpack_GetAssetId(h);
    plat_hit("ovr_AssetDetails_GetAssetId");
    return klplat_as_course(h)->id;
}
static const char *klplat_AssetDetails_GetAssetType(const void *h) {
    if (plat_is_langpack_target()) return klplat_langpack_GetAssetType(h);
    plat_hit("ovr_AssetDetails_GetAssetType");
    (void)h;
    return "default";
}
static const char *klplat_AssetDetails_GetDownloadStatus(const void *h) {
    if (plat_is_langpack_target()) return klplat_langpack_GetDownloadStatus(h);
    plat_hit("ovr_AssetDetails_GetDownloadStatus");
    return klplat_as_course(h)->path[0] ? "installed" : "unknown";
}
static const char *klplat_AssetDetails_GetFilepath(const void *h) {
    if (plat_is_langpack_target()) return klplat_langpack_GetFilepath(h);
    plat_hit("ovr_AssetDetails_GetFilepath");
    return klplat_as_course(h)->path;
}
static const char *klplat_AssetDetails_GetIapStatus(const void *h) {
    if (plat_is_langpack_target()) return klplat_langpack_GetIapStatus(h);
    plat_hit("ovr_AssetDetails_GetIapStatus");
    const klplat_course *c = klplat_as_course(h);
    if (!c->path[0]) return "not-entitled";
    if (c->delivered) return "entitled";      // the store delivered it to this account
    if (c->iap_row < 0) return "free";
    return klplat_course_owned(c->iap_row) ? "entitled" : "not-entitled";
}
static const char *klplat_AssetDetails_GetMetadata(const void *h) {
    if (plat_is_langpack_target()) return klplat_langpack_GetMetadata(h);
    plat_hit("ovr_AssetDetails_GetMetadata");
    (void)h;
    return "";
}

// The language of an asset file. Only a language pack has one (batman, above);
// for a course or a delivered add-on the SDK's own spelling of "none" is NULL,
// and it reads the three LanguagePackInfo names off that NULL without checking.
static uint64_t klplat_AssetDetails_GetLanguage_any(const void *h) {
    if (plat_is_langpack_target()) return klplat_AssetDetails_GetLanguage(h);
    plat_hit("ovr_AssetDetails_GetLanguage");
    (void)h;
    return 0;
}

// KL_PLAT_IAP_EMPTY — answer the two add-on LISTS (products by SKU, the
// viewer's purchases) with an empty one instead of refusing the request.
// Refusing (request id 0) is right for a title that goes on without its
// store, and wrong for one whose start is a chain that waits for each answer:
// Tales from the Galaxy's Edge (x3484270071659289) asks for its products, then
// its purchases, then its asset files, and only then loads its first level.
// An empty list says "nothing is for sale here and nothing was bought" —
// less than the truth, never more: its paid second part stays locked.
// Shave & Stuff (x7532973430077887) has the same serial dependency: after its
// application entitlement succeeds, its base-salon setup waits for the product
// and purchase lists before it creates GameManager.  An empty pair keeps the
// sole paid chapter locked while allowing normal progression in the base game.
//
// Beat Saber 1.40 (beatsaber140) is the third. At start it lists its add-on
// files (ovr_AssetFile_GetList); when that list does not cover its songs — an
// installation without the purchased songs, a player who bought none — it asks
// ovr_IAP_GetViewerPurchases. Refused, the health warning's "fetching owned
// songs" waits out its own timeout (about 5 s, "Fetching songs failed" and an
// OperationCanceledException in the log), and the OK button only leaves the
// warning when it ends: the warning scene stays up for those seconds instead of
// the black loading screen. With every song on disk the list says "All
// Entitled" and the question is never asked, which is why only some
// installations showed it (2026-10-15).
//
// Default on for those titles alone; the switch overrides in either direction.
static int klplat_iap_empty(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_PLAT_IAP_EMPTY", t &&
                       (strcmp(t, "x3484270071659289") == 0 ||
                        strcmp(t, "x7532973430077887") == 0 ||
                        strcmp(t, "beatsaber140") == 0) ||
                       kl_untested_profile());   // ...and untested titles (kl_driver.c)
    }
    return on;
}

static uint64_t klplat_iap_purchases(const char *what, uint32_t type) {
    if (!klplat_is_walkabout()) {
        if (klplat_iap_confirmed()) {
            klplat_scan_courses();
            fprintf(stderr, "  [plat] %s -> %d store-confirmed purchase%s (KL_PLAT_IAP_CONFIRMED)\n",
                    what, g_npurchases, g_npurchases == 1 ? "" : "s");
            return klplat_request_kind(what, type, g_npurchases, KLPLAT_KIND_PURCHASES);
        }
        if (!klplat_iap_empty()) return 0;
        g_npurchases = 0;
        return klplat_request_kind(what, type, 0, KLPLAT_KIND_PURCHASES);
    }
    klplat_scan_courses();
    return klplat_request_kind(what, type, g_npurchases, KLPLAT_KIND_PURCHASES);
}

static uint64_t klplat_IAP_GetViewerPurchases(void) {
    plat_hit("ovr_IAP_GetViewerPurchases");
    return klplat_iap_purchases("ovr_IAP_GetViewerPurchases", KLPLAT_MSG_IAP_PURCHASES);
}

static uint64_t klplat_IAP_GetViewerPurchasesDurableCache(void) {
    plat_hit("ovr_IAP_GetViewerPurchasesDurableCache");
    return klplat_iap_purchases("ovr_IAP_GetViewerPurchasesDurableCache",
                                KLPLAT_MSG_IAP_PURCHASES_CACHE);
}

static uint64_t klplat_IAP_GetProductsBySKU(const char **skus, int count) {
    plat_hit("ovr_IAP_GetProductsBySKU");
    if (!klplat_is_walkabout()) {
        if (!klplat_iap_empty()) return 0;
        g_nproducts = 0;
        fprintf(stderr, "  [plat] ovr_IAP_GetProductsBySKU(%d sku%s%s%s) -> an empty list (KL_PLAT_IAP_EMPTY)\n",
                count, count == 1 ? "" : "s", skus && count > 0 && skus[0] ? ", first: " : "",
                skus && count > 0 && skus[0] ? skus[0] : "");
        return klplat_request_kind("ovr_IAP_GetProductsBySKU", KLPLAT_MSG_IAP_PRODUCTS,
                                   0, KLPLAT_KIND_PRODUCTS);
    }
    klplat_scan_courses();
    g_nproducts = 0;
    if (skus && count > 0) {
        if (count > 512) count = 512;
        for (int i = 0; i < count && g_nproducts < KLPLAT_PRODUCT_MAX; i++) {
            const char *s = skus[i];
            if (!s) continue;
            const char *owned = NULL;
            for (int j = 0; j < g_npurchases; j++) {
                if (strcmp(g_purchases[j].sku, s) == 0) { owned = g_purchases[j].sku; break; }
            }
            if (!owned) continue;
            int dup = 0;
            for (int k = 0; k < g_nproducts; k++)
                if (g_products[k].sku == owned) { dup = 1; break; }
            if (!dup) g_products[g_nproducts++].sku = owned;
        }
    }
    return klplat_request_kind("ovr_IAP_GetProductsBySKU", KLPLAT_MSG_IAP_PRODUCTS,
                               g_nproducts, KLPLAT_KIND_PRODUCTS);
}

static uint64_t klplat_Message_GetPurchaseArray(const klplat_msg *m) {
    plat_hit("ovr_Message_GetPurchaseArray");
    return (uint64_t)(uintptr_t)m;
}
static uint64_t klplat_PurchaseArray_GetSize(const klplat_msg *m) {
    plat_hit("ovr_PurchaseArray_GetSize");
    if (!m || m->kind != KLPLAT_KIND_PURCHASES || m->payload < 0) return 0;
    return (uint64_t)m->payload;
}
static void *klplat_PurchaseArray_GetElement(const klplat_msg *m, int index) {
    plat_hit("ovr_PurchaseArray_GetElement");
    if (!m || m->kind != KLPLAT_KIND_PURCHASES) return NULL;
    if (index < 0 || index >= m->payload || index >= g_npurchases) return NULL;
    return &g_purchases[index];
}
static const char *klplat_Purchase_GetSKU(const void *h) {
    plat_hit("ovr_Purchase_GetSKU");
    const klplat_purchase *p = klplat_as_purchase(h);
    return p ? p->sku : "";
}
static const char *klplat_Purchase_GetPurchaseStrID(const void *h) {
    plat_hit("ovr_Purchase_GetPurchaseStrID");
    const klplat_purchase *p = klplat_as_purchase(h);
    return p ? p->sku : "";
}
static uint64_t klplat_Purchase_GetPurchaseID(const void *h) {
    plat_hit("ovr_Purchase_GetPurchaseID");
    const klplat_purchase *p = klplat_as_purchase(h);
    return p ? p->id : 0;
}
static uint64_t klplat_Purchase_GetGrantTime(const void *h) {
    plat_hit("ovr_Purchase_GetGrantTime");
    (void)h;
    return 1704067200ull;            // 2024-01-01, unix seconds
}
static uint64_t klplat_Purchase_GetExpirationTime(const void *h) {
    plat_hit("ovr_Purchase_GetExpirationTime");
    (void)h;
    return 4102444800ull;            // 2100-01-01, so the grant has not lapsed
}
static const char *klplat_Purchase_GetDeveloperPayload(const void *h) {
    plat_hit("ovr_Purchase_GetDeveloperPayload");
    (void)h;
    return "";
}
static const char *klplat_Purchase_GetReportingId(const void *h) {
    plat_hit("ovr_Purchase_GetReportingId");
    (void)h;
    return "";
}

static uint64_t klplat_Message_GetProductArray(const klplat_msg *m) {
    plat_hit("ovr_Message_GetProductArray");
    return (uint64_t)(uintptr_t)m;
}
static uint64_t klplat_ProductArray_GetSize(const klplat_msg *m) {
    plat_hit("ovr_ProductArray_GetSize");
    if (!m || m->kind != KLPLAT_KIND_PRODUCTS || m->payload < 0) return 0;
    return (uint64_t)m->payload;
}
static void *klplat_ProductArray_GetElement(const klplat_msg *m, int index) {
    plat_hit("ovr_ProductArray_GetElement");
    if (!m || m->kind != KLPLAT_KIND_PRODUCTS) return NULL;
    if (index < 0 || index >= m->payload || index >= g_nproducts) return NULL;
    return &g_products[index];
}
static const char *klplat_Product_GetSKU(const void *h) {
    plat_hit("ovr_Product_GetSKU");
    const klplat_product *p = klplat_as_product(h);
    return p ? p->sku : "";
}
static const char *klplat_Product_GetName(const void *h) {
    plat_hit("ovr_Product_GetName");
    const klplat_product *p = klplat_as_product(h);
    return p ? p->sku : "";
}
static const char *klplat_Product_GetDescription(const void *h) {
    plat_hit("ovr_Product_GetDescription");
    (void)h;
    return "";
}
static const char *klplat_Product_GetPrice(const void *h) {
    plat_hit("ovr_Product_GetPrice");
    (void)h;
    return "";
}
static const char *klplat_Product_GetFormattedPrice(const void *h) {
    plat_hit("ovr_Product_GetFormattedPrice");
    (void)h;
    return "";
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

// The user proof — a one-time nonce from Meta's servers that a game backend
// checks to verify who is playing. There is no service here to issue one, and a
// made-up nonce would be a lie, so the answer is a FAILURE. But not 0: the SDK
// meets a 0 request with "Request failed" and never calls OnComplete, and BRINK
// Traveler waits on that callback on a black screen forever. An ERROR completion
// is the same truth delivered to the callback, so the game's own error path runs.
static uint64_t klplat_User_GetUserProof(void) {
    plat_hit("ovr_User_GetUserProof");
    return klplat_request_err("ovr_User_GetUserProof", KLPLAT_MSG_USER_PROOF);
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

// ...and the player's achievement *progress*, the other half of the same
// startup read. Same answer for the same reason: an empty set (IsError false, an
// AchievementProgressArray of size 0, i.e. "nothing achieved yet"), not the bare
// 0 that lands the guest on "Request failed". Virtual Virtual Reality (1.8.48)
// logs exactly that right after its logos and then sits on a black screen.
// Progress records what a player has DONE, not what they own; no entitlement is
// invented. Read through GetAchievementProgressArray (-> the message payload)
// and the existing Array_GetSize answer (plat_is_empty_array -> 0).
static uint64_t klplat_Achievements_GetAllProgress(void) {
    plat_hit("ovr_Achievements_GetAllProgress");
    return klplat_request("ovr_Achievements_GetAllProgress",
                          KLPLAT_MSG_ACHIEVEMENT_PROG, 0);
}

// The rich-presence DESTINATIONS — the places inside the game its developer
// registered with the store, which a game reads to offer "join me here". They
// say where a player can be, not what a player owns, and no service here has a
// list of them: the answer is an EMPTY one, completed like the two achievement
// lists above (IsError false, a DestinationArray of size 0) and read the same
// way, through GetDestinationArray (-> the message payload) and the existing
// Array_GetSize answer (plat_is_empty_array -> 0). The name had no entry at
// all, so the first call ended the run ("unimplemented Oculus Platform entry
// point"); Real VR Fishing (x2582932495064035) asks at start and, by its
// sender, waits for the completion (fix 37, 2026-10-14; not run here). The
// setters beside it (ovr_RichPresence_Set, _Clear, ...) stay "could not be
// made" in g_plat_request.
static uint64_t klplat_RichPresence_GetDestinations(void) {
    plat_hit("ovr_RichPresence_GetDestinations");
    return klplat_request("ovr_RichPresence_GetDestinations",
                          KLPLAT_MSG_RICH_DESTINATIONS, 0);
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
static int klplat_user_listed(const char *t) {
    // Titles that ABORT when ovr_GetLoggedInUserID / GetLoggedInUser returns 0 —
    // they put up "Unable to get a valid UserID ... check you have an Entitlement"
    // and then FMessageDialog->abort on the game thread. A synthetic offline user
    // is strictly better for these: there is no Oculus service to sign in to, and
    // the alternative is a hard exit before the menu. Unity titles that read 0 as
    // "offline, carry on" are NOT in this list and keep the 0.
    // Walkabout is the Unity exception: it does not quit, it WAITS. XRRig logs
    // "WaitForPlatform(): Platform is not connected and or is not ready" and
    // re-asks GetLoggedInUser every few frames on a black screen, forever.
    // Moss does neither: with no user it carries on into its first room and
    // then dereferences the subtitle overlay AMossGameMode::CreateSubtitleOverlay
    // could not make without a primary player controller — a NULL read in
    // UGameSubtitleManager::NotifyRoomEnterBegin, about 630 frames in.
    // AC Nexus as a store install (x5812519008825194) is the chain described
    // above klplat_User_GetLoggedInUser: the entitlement check passes, this
    // request then fails with "An unknown error occurred. Request failed." and
    // the game never leaves its loading screen.
    // Maestro (x6963344583787383, UE4): UMaestroGameInstance binds its QuitGame
    // to the OnlineSubsystem's login-complete delegate; with no user the login
    // fails and the game calls FGenericPlatformMisc::RequestExit(true) — abort —
    // right after its first controller poll, before a frame is drawn.
    // Saints & Sinners as a store install (x2897337400373711) is twd2's
    // predecessor and gets the same offline user as twd2 (fix 15, 2026-10-11).
    // Chapter 2: Retribution as a store install (x5190288877703616) IS twd2 and
    // was left out: ovr_GetLoggedInUserID answered 0 and it stayed black.
    // BRINK Traveler (x3635172946605196) is the same chain in Unity: after the
    // entitlement request, GetLoggedInUser fails with "Request failed." and the
    // game renders black frames with no draws, never loading its first scene
    // (fix 9, 2026-10-11; the game does not run yet for other reasons).
    // Tales from the Galaxy's Edge (x3484270071659289, UE4) waits as well: with
    // no user its OnlineSubsystem never comes up, the entitlement reply it asked
    // for is never popped, and it draws its black start room behind a full fade
    // for as long as it is left running (seen on a Mac host, 2026-10-11).
    static const char *on[] = { "zix", "intotheradius", "x5817243991680545",
                                "redmatter2", "vampire", "twd2",
                                "walkabout", "moss", "x4395292760584049", "x5812519008825194", "x6963344583787383",
                                "x2897337400373711", "x5190288877703616",
                                "x3635172946605196", "x3484270071659289",
                                // Sniper Elite VR: with no user its Init scene
                                // activates, the loading screen fades out, and it
                                // stops submitting frames for good.
                                "x2544573735586536",
                                // Wooorld: the AC Nexus chain again. Its boot
                                // logs "OVR Initialized: Success", the
                                // entitlement request goes out, GetLoggedInUser
                                // fails with "An unknown error occurred. Request
                                // failed." and the game stays in its boot scene:
                                // sound, and two draws a frame of black (on
                                // device, 2026-10-11).
                                "x4360608937312293",
                                // Premium Bowling: the same chain. "Entitlement
                                // Check OK => Start GetLoggedInOculusUserRoutine",
                                // then "Request failed." and nothing more: no
                                // file is opened and no scene loaded for 2400
                                // frames (Mac host, 2026-10-12). With the user
                                // it goes on to load its first hall.
                                "x2773034772778845",
                                // Demeo: its lobby loads and stays black behind
                                // "[GameStartup] Waiting for startup to
                                // finish..." — "AccountID not set, will fetch
                                // from provider", then this request's "Request
                                // failed." and a sign-in that times out every
                                // ten seconds for want of a user id (on device,
                                // 2026-10-13). With the user: "PlayerData loaded
                                // successfully", "Startup finished!" (Mac host).
                                // Its user proof stays the error it is.
                                "x3634830803298285",
                                // In Death: Unchained (UE4): with no user it
                                // stays in its sky with the ghost gloves, one
                                // unchanging picture and no music, for as long
                                // as it is left (on device and Mac host,
                                // 2026-10-13). With the user its music starts,
                                // it asks for its leaderboards and the pointer
                                // appears.
                                "x2334376869949242",
                                // BattleGroupVR2 0.251: its start waits for
                                // player data. The entitlement request is
                                // answered as for every game, then
                                // GetLoggedInUser comes back 0 and the wait
                                // never ends (the sender's device log, fix 38,
                                // 2026-10-13). With the user it reaches its
                                // menu and tutorial, as the sender saw; not
                                // run by anyone here.
                                "x9386827918099701",
                                // Real VR Fishing: the AC Nexus chain again, by
                                // its sender — the entitlement request
                                // completes, GetLoggedInUser fails and the
                                // game's online start gives up in its loading
                                // scene (fix 37, 2026-10-14; not run here).
                                "x2582932495064035",
                                // Shave & Stuff: entitlement succeeds, but
                                // GetLoggedInUser returns 0 and its salon cards
                                // remain behind a permanent loading spinner.
                                // The offline user supplies only player identity;
                                // ownership and purchases remain separate.
                                "x7532973430077887",
                                // Cubism 1.8.0: entitlement succeeds, then
                                // GetLoggedInUser returns 0 with "Request
                                // failed." while the picture stays black
                                // (owner's device log). Supply offline identity
                                // through the existing path; no purchase proof.
                                "x2264524423619421",
                                // LES MILLS BODYCOMBAT: without a user its PlatformInitializer
                                // fails twice and the game drops to offline mode.
                                "x4015163475201433" };
    // Beat Saber from 1.45 on (Unity 6; kl_guest_beatsaber_openxr): its start
    // asks for the user FIRST and goes no further without one — no entitlement
    // request, no scene, one draw a frame of black and silence (on device,
    // 2026-10-12). 1.40 asks the same question fourth and carries on with a 0,
    // so it is the build and not the row that decides.
    if (t) for (unsigned i = 0; i < sizeof on / sizeof on[0]; i++)
        if (strcmp(t, on[i]) == 0) return 1;
    return 0;
}
static int klplat_user_default(void) {
    extern int kl_guest_beatsaber_openxr(void);
    if (kl_guest_beatsaber_openxr()) return 1;
    // ...and every title under the profile for untested games: nine of the
    // first 52 fixes users sent in were a start that waits for a user forever.
    return klplat_user_listed(kl_driver_target_name()) || kl_untested_profile();
}
// For the start window, which is shown before the driver has a target: is this
// a game that is handed the offline user (and so one a player name means
// something to)?
int kl_ovrplat_offline_user_for(const char *target) {
    return kl_env_on("KL_PLAT_USER", klplat_user_listed(target) || kl_untested_target(target));
}

// The offline user's name and id.
//
// The name is the player's to choose (KL_PLAT_USER_NAME, set by the start
// window's "Player name"): a game shows it as the account's name and offers no
// way to change it, because on a Quest it is the Meta account's. Demeo reads
// ovr_User_GetOculusID ("Got Oculus Account Name: kleptonplayer") and its owner
// asked for a way to set it (2026-10-13). Unset, the names are what they were.
// The display name is the text as typed; the Oculus ID — a handle, which on a
// Quest has no spaces — is the same with anything but letters, digits, '.',
// '_' and '-' taken out.
static char g_user_display[64] = "Klepton Player", g_user_handle[64] = "kleptonplayer";
static uint64_t g_user_id = 1000000000000042ull;   // fixed, non-zero synthetic ovrID
static pthread_once_t g_user_once = PTHREAD_ONCE_INIT;
static void klplat_user_init(void) {
    const char *n = kl_env_str("KL_PLAT_USER_NAME", NULL);
    if (n && *n) {
        char d[sizeof g_user_display], h[sizeof g_user_handle];
        size_t dn = 0, hn = 0;
        for (const unsigned char *c = (const unsigned char *)n; *c && dn < 40; c++) {
            if (*c < 0x20 || *c == 0x7f) continue;                 // control characters
            d[dn++] = (char)*c;
            if (hn < 40 && ((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') ||
                            (*c >= 'a' && *c <= 'z') || *c == '.' || *c == '_' || *c == '-'))
                h[hn++] = (char)*c;
        }
        // Not in the middle of a UTF-8 sequence.
        while (dn > 0 && ((unsigned char)d[dn - 1] & 0xC0) == 0x80) dn--;
        if (dn > 0 && ((unsigned char)d[dn - 1] & 0x80)) dn--;
        d[dn] = 0; h[hn] = 0;
        if (dn) memcpy(g_user_display, d, dn + 1);
        if (hn) memcpy(g_user_handle, h, hn + 1);
        fprintf(stderr, "  [plat] offline user named \"%s\" (Oculus ID \"%s\") — KL_PLAT_USER_NAME\n",
                g_user_display, g_user_handle);
    }
    // The id is one number for everybody unless told otherwise — and a game
    // that keys an account of its own on it then has ONE account for every
    // player of this runtime. Demeo does ("Fetch account ID completed, ID is:
    // 1000000000000042", then its anonymous sign-in), so for it the start window
    // makes a number of this installation's own and hands it over here
    // (KL_PLAT_USER_ID; kl_ovrplat_user_id_per_install). Every other game keeps
    // the fixed one, which their saves are filed under.
    const char *e = kl_env_str("KL_PLAT_USER_ID", NULL);
    uint64_t v = e && *e ? strtoull(e, NULL, 10) : 0;
    if (v) {
        g_user_id = v;
        fprintf(stderr, "  [plat] offline user has this installation's own id (KL_PLAT_USER_ID)\n");
    }
}
static uint64_t klplat_user_id(void) { pthread_once(&g_user_once, klplat_user_init); return g_user_id; }
// Walkabout Mini Golf keys its account with its own backend on the id as well:
// PlayFabController.Login signs in with a custom id made of the platform's user
// id and the platform's name ("1000000000000042Meta"), with nothing else to
// prove it. Since its HTTP requests go through (the socket() fix), that
// sign-in succeeds - and with one id for everybody, every player of this
// runtime would sign in to the same account and sync the same cloud profiles
// (seen on a Mac host, 2026-10-13: signed in after 0.4 s, then two seconds of
// download and an upload). The game itself carries a profile over when the id
// changes: it copies it to the new id's folder (LAST_DATAPATH_Oculus).
//
// BattleGroupVR2 got the offline user with fix 38 (2026-10-14) and keeps its
// per-install id. Real VR Fishing must retain the legacy fixed id: its local
// save is encrypted/keyed under User_1000000000000042, and changing the id on
// reinstall makes the game reject that save and deliberately quit after load.
// ...and every title under the profile for untested games, for the same
// reason: nobody here knows whether it keys an account on the id.
int kl_ovrplat_user_id_per_install(const char *target) {
    return target && (strcmp(target, "x3634830803298285") == 0 || strcmp(target, "walkabout") == 0 ||
                      strcmp(target, "x9386827918099701") == 0 ||
                      kl_untested_target(target));
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

// ovr_User_Get(userID) — one user's profile, by id. Its completion is
// Message<User>, read through the same accessors as GetLoggedInUser's. Wooorld
// asks it for the id GetLoggedInUser just gave it — the SDK's own advice, since
// the logged-in-user message need not carry the display name — and there was
// no entry point: "unimplemented Oculus Platform entry point 'ovr_User_Get'",
// one frame after the offline user was handed out (on a Mac host, 2026-10-11).
// Asked for the offline user itself, the answer is that same user; nothing is
// added to what GetLoggedInUser already said. Asked for anyone else — or with
// no user handed out at all — there is no service to look them up in, and that
// is an ERROR completion rather than a 0, so the caller's callback still runs
// (see the user-proof note above).
static uint64_t klplat_User_Get(uint64_t user_id) {
    plat_hit("ovr_User_Get");
    if (user_id == klplat_user_id() &&             // == klplat_User_GetID's synthetic ovrID
        kl_env_on("KL_PLAT_USER", klplat_user_default()))
        return klplat_request("ovr_User_Get", KLPLAT_MSG_USER_GET, 0);
    return klplat_request_err("ovr_User_Get", KLPLAT_MSG_USER_GET);
}

// ovr_User_GetOrgScopedID(userID) — the id a developer's own backend knows the
// user by. Nothing here can issue one, and a made-up id sent to somebody's
// server would be a lie about who is playing, so the request fails for every
// guest. What differs is the SHAPE of the failure, as with the user proof
// above. The default stays 0 through klplat_init_fails further down, "the
// request could not be made" (Asgard's Wrath 2 walks past that). Wooorld
// (x4360608937312293) does not walk past it: its sign-in is GetLoggedInUser ->
// User_Get -> GetOrgScopedID -> a nonce -> its own cloud, the SDK meets a 0
// with "Request failed" and never calls the callback, and the game waits on
// that callback in a black boot scene. An ERROR completion runs the game's own
// handler instead ("Error getting OrgScopedId: ..." in its log; it asks again
// some time later). It does NOT sign the game in: Wooorld's cloud wants an id
// and a proof only Meta can issue.
#define KLPLAT_MSG_ORG_SCOPED_ID   0x18F0B01Bu  // .User_GetOrgScopedID (KL_PROBE_ENUM, Wooorld 2.4.0)
static uint64_t klplat_init_fails(const char *name);
static uint64_t klplat_User_GetOrgScopedID(uint64_t user_id) {
    (void)user_id;
    static int err = -1;
    if (err < 0) {
        const char *t = kl_driver_target_name();
        err = t && strcmp(t, "x4360608937312293") == 0;
    }
    if (!err) return klplat_init_fails("ovr_User_GetOrgScopedID");
    plat_hit("ovr_User_GetOrgScopedID");
    return klplat_request_err("ovr_User_GetOrgScopedID", KLPLAT_MSG_ORG_SCOPED_ID);
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
    return klplat_user_id();
}
static const char *klplat_User_GetOculusID(const void *h) {
    plat_hit("ovr_User_GetOculusID"); (void)h;
    pthread_once(&g_user_once, klplat_user_init);
    return g_user_handle;
}
static const char *klplat_User_GetDisplayName(const void *h) {
    plat_hit("ovr_User_GetDisplayName"); (void)h;
    pthread_once(&g_user_once, klplat_user_init);
    return g_user_display;
}
// The rest of the User model, which the SDK's C# constructor reads in one go
// (Walkabout builds the whole object from the GetLoggedInUser completion): the
// profile pictures, the presence block, and the Quest-for-Business ManagedInfo.
// An offline user has none of them, and 0 is the SDK's own spelling of that — a
// NULL string, presence status Unknown, and a NULL ManagedInfo whose ten getters
// are then called on that NULL anyway and must answer the same.
static int plat_is_user_field(const char *name) {
    static const char *const f[] = {
        "ovr_User_GetImageUrl", "ovr_User_GetSmallImageUrl",
        "ovr_User_GetPresence", "ovr_User_GetPresenceStatus",
        "ovr_User_GetPresenceDeeplinkMessage", "ovr_User_GetPresenceDestinationApiName",
        "ovr_User_GetPresenceLobbySessionId", "ovr_User_GetPresenceMatchSessionId",
        "ovr_User_GetManagedInfo",
        // Newer SDKs' User model also reads the invite token (Sniper Elite VR's
        // does, right after the GetLoggedInUser completion). An offline user
        // has none; NULL is the SDK's "absent", as for the fields above.
        "ovr_User_GetInviteToken",
    };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++)
        if (strcmp(name, f[i]) == 0) return 1;
    return strncmp(name, "ovr_ManagedInfo_Get", 19) == 0;
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

// Cancel an in-progress update download. Cave Crave asks it right after the
// version check above. GetVersion reports no update, so no download was ever
// started, and with no store there is nothing that could be cancelled: the
// request completes as an error, as cancelling a download that does not exist
// does — rather than a success claiming something was stopped.
static uint64_t klplat_Application_CancelAppDownload(void) {
    plat_hit("ovr_Application_CancelAppDownload");
    return klplat_request_err("ovr_Application_CancelAppDownload", KLPLAT_MSG_APP_CANCEL_DL);
}

// Leaderboards. Keep Talking and Nobody Explodes reads one when a bomb is picked
// from the binder, Eye of the Temple reads one while it starts, and both aborted
// here on the unimplemented trampoline. There is no leaderboard service on this
// host, so the read and the write complete as an error — what a Quest without a
// network says — rather than as an empty or accepted board that would claim a
// service answered. Nothing about ownership rides on a score.
//
// For every title: the two message types are the Platform SDK's own constants
// (ovrMessage_Leaderboard_GetEntries / _WriteEntry in OVR_MessageType.h), and
// they were read back identically from the global-metadata of both games, built
// years apart (Unity 2022.3 and 2019.4) — the same check that reproduces the
// Entitlement and User values above.
static uint64_t klplat_Leaderboard_GetEntries(const char *name, int limit,
                                              int filter, int start_at) {
    (void)name; (void)limit; (void)filter; (void)start_at;
    plat_hit("ovr_Leaderboard_GetEntries");
    return klplat_request_err("ovr_Leaderboard_GetEntries",
                              KLPLAT_MSG_LEADERBOARD_GET_ENTRIES);
}
static uint64_t klplat_Leaderboard_WriteEntry(const char *name, long long score,
                                              const void *extra, unsigned extra_size,
                                              int force) {
    (void)name; (void)score; (void)extra; (void)extra_size; (void)force;
    plat_hit("ovr_Leaderboard_WriteEntry");
    return klplat_request_err("ovr_Leaderboard_WriteEntry",
                              KLPLAT_MSG_LEADERBOARD_WRITE_ENTRY);
}
// The family's other plain requests, answered the same way and for the same
// reason. Beat Saber 1.45 asks GetEntriesAfterRank when its menu comes up, once
// it has a user; there was no entry point and the run ended there by name (on
// device, 2026-10-12). The three beside it are what the same C# class reaches
// next. The paging requests (GetNextEntries / GetPreviousEntries) take a handle
// out of a successful answer, which nothing here ever gives.
static uint64_t klplat_Leaderboard_Get(const char *name) {
    (void)name;
    plat_hit("ovr_Leaderboard_Get");
    return klplat_request_err("ovr_Leaderboard_Get", KLPLAT_MSG_LEADERBOARD_GET);
}
// ovr_Voip_GetMicrophoneAvailability() — "may this app use the microphone for
// platform voice chat?", an async request whose answer is a
// MicrophoneAvailabilityState. There is no Meta voice service here to ask, so
// it fails like the leaderboards: an ERROR completion, so the caller's
// callback runs and takes its no-voice path. Ghostbusters: Rise of the Ghost
// Lord (x4746232908818706) asks it right after its achievements, and the run
// ended there by name (Mac host, 2026-10-18). Its Unreal OnlineSubsystemOculus
// matches completions to callbacks by request id, so the type only has to be
// the right family; this one is ovrMessage_Voip_GetMicrophoneAvailability as
// OVR_MessageType.h spells it, not read back from a guest.
#define KLPLAT_MSG_VOIP_MIC_AVAILABILITY 0x744CE345u
static uint64_t klplat_Voip_GetMicrophoneAvailability(void) {
    plat_hit("ovr_Voip_GetMicrophoneAvailability");
    return klplat_request_err("ovr_Voip_GetMicrophoneAvailability",
                              KLPLAT_MSG_VOIP_MIC_AVAILABILITY);
}
// Two more requests that used to answer 0 ("could not be made"), for the same
// reason as the leaderboards above and with the same cure: an ERROR completion.
// A 0 request id makes the SDK drop the call's callback without ever running it,
// so a title that waits on the callback waits forever. Drop Dead: Dual Strike
// Edition (x1935017546592600) asks ovr_Room_GetCurrent at start (does the player
// already sit in a room?) and ovr_Voip_SetSystemVoipSuppressed after its
// leaderboard writes; with 0 its loading screen stopped after "Fetched Title
// News" and never moved on (on device, 2026-10-28, frame loop running, every
// thread parked). There is no room and no voice service on this host, so an error
// is the true answer and the title takes its own "no room / no voice" path.
// Nothing about ownership rides on either request.
//
// Both type values were read out of the title's running IL2CPP runtime with
// KL_PROBE_ENUM=Oculus.Platform.Message/MessageType (Room_GetCurrent =
// 161916164, Voip_SetSystemVoipSuppressed = 1161808298); the same run reproduced
// the Leaderboard_WriteEntry (0x117FC8FE) and Voip_GetMicrophoneAvailability
// (0x744CE345) values already used above.
#define KLPLAT_MSG_ROOM_GET_CURRENT          0x09A6A504u // Room_GetCurrent
#define KLPLAT_MSG_VOIP_SET_SYSTEM_SUPPRESSED 0x453FC9AAu // Voip_SetSystemVoipSuppressed
// Drop Dead only: its boot waits on ApplicationLaunchManager.AssessCoordinatedLaunch,
// which loops until PTSocialPlatformController_Oculus.HasCheckedLaunchState. Read
// from the game's IL2CPP code, that flag is set by exactly one path that matters
// here: the Rooms.GetCurrent completion, and only when it is NOT an error and
// carries a Room that is not in state 4 and has no users (the error branch leaves
// without ever checking the launch state). The honest answer for "the player is in
// no room" is therefore a successful completion carrying an empty Room whose every
// field reads as absent (see plat_is_room_field). Nobody is reported to be in a
// room, so no multiplayer path starts; ownership plays no part. Every other title
// keeps the error completion above.
static int plat_is_request(const char *name);
static int klplat_room_empty_ok(void) {
    const char *t = kl_driver_target_name();
    return t && strcmp(t, "x1935017546592600") == 0;
}
// ...and the second thing the same boot waits on. DeepLinkHelper.Ready, which
// VrBootManager.LoadScene polls (state 4) before it loads anything, is only ever
// raised by the callback the game registers for
// Notification_ApplicationLifecycle_LaunchIntentChanged (0x04b34ca3, read out of
// the running game with KL_PROBE_ENUM). The real platform posts that notification
// once when an app starts; here nothing did, so the logo never went away. For a
// normal launch the callback only logs the message, reads the launch details
// (answered above: started normally, nothing attached) and raises Ready.
// A notification carries request id 0, which is how the SDK tells it from a
// reply (a reply with no waiting request is "Request with no handler").
#define KLPLAT_MSG_LAUNCH_INTENT_CHANGED 0x04B34CA3u
static void klplat_post_launch_intent_changed(void) {
    static int posted;
    pthread_mutex_lock(&g_msg_mu);
    if (!posted) {
        klplat_msg *m = klplat_msg_alloc_locked();
        if (m) {
            posted = 1;
            klplat_commit_locked(m, KLPLAT_MSG_LAUNCH_INTENT_CHANGED, 0, "", 0,
                                 KLPLAT_KIND_NONE);
            m->request = 0;
        }
    }
    pthread_mutex_unlock(&g_msg_mu);
    fprintf(stderr, "  [plat] posted Notification_ApplicationLifecycle_LaunchIntentChanged "
                    "(launched normally, nothing attached)\n");
}
static uint64_t klplat_Room_GetCurrent(void) {
    plat_hit("ovr_Room_GetCurrent");
    if (klplat_room_empty_ok()) {
        klplat_post_launch_intent_changed();
        return klplat_request("ovr_Room_GetCurrent", KLPLAT_MSG_ROOM_GET_CURRENT, 0);
    }
    return klplat_request_failed("ovr_Room_GetCurrent");   // every other title: as before
}
// The empty Room's fields, and its DataStore: all absent (0 = zero ID, NULL
// string/list, zero enum). Only reachable for the target above, because only it
// is handed a Room handle; the requests of the family stay requests.
static int plat_is_room_field(const char *name) {
    if (!klplat_room_empty_ok()) return 0;
    if (strncmp(name, "ovr_DataStore_", 14) == 0) return 1;
    return strncmp(name, "ovr_Room_Get", 12) == 0 && !plat_is_request(name);
}
static uint64_t klplat_room_field(const char *name) {
    plat_hit(name);
    return 0;
}
static uint64_t klplat_Voip_SetSystemVoipSuppressed(int suppressed) {
    (void)suppressed;
    plat_hit("ovr_Voip_SetSystemVoipSuppressed");
    if (!klplat_room_empty_ok())          // this title only; others: as before
        return klplat_request_failed("ovr_Voip_SetSystemVoipSuppressed");
    return klplat_request_err("ovr_Voip_SetSystemVoipSuppressed",
                              KLPLAT_MSG_VOIP_SET_SYSTEM_SUPPRESSED);
}
static uint64_t klplat_Leaderboard_GetEntriesAfterRank(const char *name, int limit,
                                                       unsigned long long after_rank) {
    (void)name; (void)limit; (void)after_rank;
    plat_hit("ovr_Leaderboard_GetEntriesAfterRank");
    return klplat_request_err("ovr_Leaderboard_GetEntriesAfterRank",
                              KLPLAT_MSG_LEADERBOARD_GET_AFTER_RANK);
}
static uint64_t klplat_Leaderboard_GetEntriesByIds(const char *name, int limit, int start_at,
                                                   const uint64_t *user_ids, unsigned n_ids) {
    (void)name; (void)limit; (void)start_at; (void)user_ids; (void)n_ids;
    plat_hit("ovr_Leaderboard_GetEntriesByIds");
    return klplat_request_err("ovr_Leaderboard_GetEntriesByIds",
                              KLPLAT_MSG_LEADERBOARD_GET_BY_IDS);
}
static uint64_t klplat_Leaderboard_WriteEntryWithSupplementaryMetric(
        const char *name, long long score, long long metric,
        const void *extra, unsigned extra_size, int force) {
    (void)name; (void)score; (void)metric; (void)extra; (void)extra_size; (void)force;
    plat_hit("ovr_Leaderboard_WriteEntryWithSupplementaryMetric");
    return klplat_request_err("ovr_Leaderboard_WriteEntryWithSupplementaryMetric",
                              KLPLAT_MSG_LEADERBOARD_WRITE_ENTRY_SM);
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
    // Walkabout's GetProductsBySKU / GetViewerPurchases are real functions in
    // g_plat_impl (file-backed). What remains here is the page walker and the
    // delta query: still "could not be made", still no extra grant.
    "ovr_IAP_GetNextProductArrayPage",
    "ovr_IAP_GetViewerPurchasesDeltaAsync",
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
    // Rooms — the platform's multiplayer rooms and their invites. Every one of
    // these returns an ovrRequest, and with no platform service none can be
    // made (0); the ovr_Room_Get<Field> accessors read a room handle, which
    // only a completion could hand out, so they are never reached. Medal of
    // Honor: Above and Beyond (x3647259232032222) asks GetInvitableUsers2 at
    // start and stopped on it as an unimplemented entry point (2026-10-18).
    "ovr_Room_CreateAndJoinPrivate", "ovr_Room_CreateAndJoinPrivate2",
    "ovr_Room_Get", "ovr_Room_GetCurrent", "ovr_Room_GetCurrentForUser",
    "ovr_Room_GetInvitableUsers", "ovr_Room_GetInvitableUsers2",
    "ovr_Room_GetModeratedRooms", "ovr_Room_GetNextRoomArrayPage",
    "ovr_Room_InviteUser", "ovr_Room_Join", "ovr_Room_Join2", "ovr_Room_KickUser",
    "ovr_Room_LaunchInvitableUserFlow", "ovr_Room_Leave", "ovr_Room_UpdateDataStore",
    "ovr_Room_UpdateMembershipLockStatus", "ovr_Room_UpdateOwner",
    "ovr_Room_UpdatePrivateRoomJoinPolicy",
    // ...and the pending room invites, which Medal of Honor asks right after.
    "ovr_Notification_GetRoomInvites", "ovr_Notification_GetNextRoomInviteNotificationArrayPage",
    "ovr_Notification_MarkAsRead",
    // Device attestation: the platform service signs a token for the nonce the
    // game passes, and the game's own server checks it. There is no service
    // here to sign anything, and a made-up token would be a forged attestation;
    // 0 says the request could not be made. Premium Bowling
    // (x2773034772778845) asks once at start, beside its entitlement request,
    // and logs "Request failed." before carrying on.
    "ovr_DeviceApplicationIntegrity_GetIntegrityToken",
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
//     the request could not be made rather than reporting any progress. Two
//     of them have answers of their own further down, both empty: the
//     definitions (AC Nexus gates its menu on that list arriving) and all
//     progress (Virtual Virtual Reality waits for it; fix 19, 2026-10-11) —
//     "there is nothing, and nothing is unlocked". The entitlement families
//     are unaffected — plat_is_drm runs first.
//   ovr_GroupPresence_* — "who are you playing with", the multiplayer presence
//     the invite/roster panels read. Ownership does not enter into it, and the
//     trailing underscore keeps this clear of ovr_GroupPresenceOptions_* below,
//     which is a different shape entirely.
//   ovr_Party_* — Meta system parties. All entry points return an ovrRequest.
//     Ownership does not enter into it; with no platform service the request
//     cannot be made (0). Warplanes: WW1 Fighters (x3922378427824210) and
//     Warplanes: Battles over Pacific (x3984056454948095) call
//     ovr_Party_GetCurrent at start.
//   ovr_Matchmaking_* — multiplayer matchmaking queues and rooms.
//   ovr_RichPresence_* — destination and activity presence.
//   ovr_CloudStorage_* — saves moved to and from Meta's cloud (Load, Save,
//     the metadata and conflict queries). The data is not here to move, as the
//     note at ovr_CloudStorage2_GetUserDirectoryPath says; that one keeps its
//     own answer (the underscore after "CloudStorage" keeps it out, and the
//     implemented names are matched first anyway). Space Pirate Trainer DX
//     (x1663790613725314) asks LoadBucketMetadata once its start screen's
//     timer runs out, and the refusal ended the game there (2026-10-28).
static const char *const g_plat_request_prefix[] = {
    "ovr_Achievements_", "ovr_GroupPresence_", "ovr_Party_",
    "ovr_Matchmaking_", "ovr_RichPresence_", "ovr_CloudStorage_",
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
//   ovr_Voip_SetNewConnectionOptions(ovrVoipOptionsHandle) — the options every
//     later voice connection is made with (an options cookie, above). No voice
//     connection is ever made here, so there is nothing for them to apply to.
//     Space Pirate Trainer DX sets it while its start screen's hold-the-button
//     timer runs, and the refusal ended the game there (2026-10-28).
static const char *const g_plat_drop[] = {
    "ovr_Log_NewEvent",
    "ovr_Voip_SetMicrophoneMuted",
    "ovr_Voip_SetNewConnectionOptions",
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

// ovr_Microphone_*: the platform's own mic capture for in-game voice chat.
// Arizona Sunshine creates one on boot. As with ovr_Voip_SetMicrophoneMuted
// above, nothing here captures audio, so this is a microphone that hears
// silence: Create hands back a non-NULL cookie (the managed MicrophoneInput
// wrapper Starts it with no NULL check), Start/Stop/Destroy have nothing to
// do, and every read — GetNumSamplesAvailable, GetPCM/GetPCMFloat,
// ReadData — reports 0 samples.
static const char g_mic_cookie[] = "klepton-ovr-microphone";

static uint64_t klplat_microphone_create(const char *name) {
    plat_hit(name);
    fprintf(stderr, "  [plat] %s -> silent microphone (no audio capture on this "
                    "host; every read returns 0 samples)\n", name);
    return (uint64_t)(uintptr_t)g_mic_cookie;
}

static int plat_is_microphone(const char *name) {
    return strncmp(name, "ovr_Microphone_", 15) == 0;
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

// The STRING fields, for a title that hands them on unchecked. The real
// platform answers an absent launch string with "" rather than NULL, and the
// SDK's StringFromNative turns NULL into a C# null; a title that copies one into
// another platform call then dies in CAPI.StringToNative ("StringFromNative:
// null argument"). Sniper Elite VR (x2544573735586536) does exactly that while
// setting up rich presence on its Init scene, and the exception kills the boot
// sequence: the loading screen fades out and nothing is loaded after it, so the
// headset stays black. Per target, because NULL is what every other title here
// was verified against and some test the field against null, not "".
static int plat_launch_strings_empty(void) {
    static const char *on[] = { "x2544573735586536" };
    const char *t = kl_driver_target_name();
    if (t) for (unsigned i = 0; i < sizeof on / sizeof on[0]; i++)
        if (strcmp(t, on[i]) == 0) return 1;
    return 0;
}
static int plat_is_launch_string(const char *name) {
    static const char *const f[] = {
        "ovr_LaunchDetails_GetDeeplinkMessage", "ovr_LaunchDetails_GetDestinationApiName",
        "ovr_LaunchDetails_GetLaunchSource",    "ovr_LaunchDetails_GetLobbySessionID",
        "ovr_LaunchDetails_GetMatchSessionID",  "ovr_LaunchDetails_GetTrackingID",
    };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++)
        if (strcmp(name, f[i]) == 0) return 1;
    return 0;
}

static uint64_t klplat_launch_field(const char *name) {
    plat_hit(name);
    if (plat_is_launch_string(name) && plat_launch_strings_empty())
        return (uint64_t)(uintptr_t)"";   // present but empty, as the platform says it
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
//
// HasNextPage / HasPreviousPage are the native SDK's spelling of GetNextUrl:
// UE's OnlineSubsystemOculus reads GetSize and then asks HasNextPage before it
// completes the query. Every list we hand out is the whole list, so "no other
// page" is the true answer. Into The Radius 2 (x6605328856182465, UE 5.5) asks
// ovr_AchievementProgressArray_HasNextPage on the empty progress array of
// Achievements_GetAllProgress on its first frame; without this the name had no
// answer and ended the run. Purchase arrays never reach here: plat_is_drm runs
// first and keeps ovr_PurchaseArray_HasNextPage refused.
static int plat_is_empty_array(const char *name) {
    const char *p = strstr(name, "Array_");
    if (!p) return 0;
    return strcmp(p, "Array_GetSize") == 0 || strcmp(p, "Array_GetNextUrl") == 0 ||
           strcmp(p, "Array_HasNextPage") == 0 || strcmp(p, "Array_HasPreviousPage") == 0;
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
        return klplat_user_id();      // == klplat_User_GetID's synthetic ovrID
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
//
// On a headset nothing sets LANG. For a title whose installation lists its
// asset files — language packs, chosen by the language it is told — the answer
// is the one java.util.Locale and AConfiguration give (kl_jni_locale_parts:
// the device's language where KL_LOCALE_DEVICE is on, else en_US), so the
// platform and the engine do not name two languages. Every other title keeps
// the LANG-only answer it always had.
static uint64_t klplat_GetLoggedInUserLocale(void) {
    plat_hit("ovr_GetLoggedInUserLocale");
    static char loc[16];
    const char *host = getenv("LANG");
    if (!loc[0] && (!host || !host[0]) && kl_ovrplat_asset_files_configured()) {
        char lang[16], country[16];
        kl_jni_locale_parts(lang, sizeof lang, country, sizeof country);
        snprintf(loc, sizeof loc, country[0] ? "%s_%s" : "%s", lang, country);
    }
    if (!loc[0]) {
        const char *lang = getenv("LANG");            // e.g. "en_US.UTF-8"
        int i = 0;
        if (lang) for (; lang[i] && lang[i] != '.' && i < (int)sizeof loc - 1; i++) loc[i] = lang[i];
        if (i == 0) { loc[0]='e'; loc[1]='n'; loc[2]='_'; loc[3]='U'; loc[4]='S'; i=5; }
        loc[i] = '\0';
    }
    klplat_trace("ovr_GetLoggedInUserLocale() -> \"%s\"", loc);
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

// ovrMessageType_ToString(type) — the SDK's name for a message type, which a
// guest asks for when it LOGS a message it did not expect. Maestro does that
// with the error completion its leaderboard request gets here, and the import
// was unresolved: the first failed request ended the run ("called unresolved
// import"), a minute into play. The names are the SDK's own for the types this
// file completes; anything else gets the SDK's spelling of "no idea".
static const char *klplat_MessageType_ToString(uint32_t type) {
    plat_hit("ovrMessageType_ToString");
    switch (type) {
    case KLPLAT_MSG_PLATFORM_INIT:          return "ovrMessage_Platform_InitializeAndroidAsynchronous";
    case KLPLAT_MSG_ENTITLEMENT:            return "ovrMessage_Entitlement_GetIsViewerEntitled";
    case KLPLAT_MSG_ACCESS_TOKEN:           return "ovrMessage_User_GetAccessToken";
    case KLPLAT_MSG_ACHIEVEMENT_DEFS:       return "ovrMessage_Achievements_GetAllDefinitions";
    case KLPLAT_MSG_RICH_DESTINATIONS:      return "ovrMessage_RichPresence_GetDestinations";
    case KLPLAT_MSG_LOGGED_IN_USER:         return "ovrMessage_User_GetLoggedInUser";
    case KLPLAT_MSG_USER_GET:               return "ovrMessage_User_Get";
    case KLPLAT_MSG_ORG_SCOPED_ID:          return "ovrMessage_User_GetOrgScopedID";
    case KLPLAT_MSG_LEADERBOARD_GET_ENTRIES: return "ovrMessage_Leaderboard_GetEntries";
    case KLPLAT_MSG_LEADERBOARD_WRITE_ENTRY: return "ovrMessage_Leaderboard_WriteEntry";
    case KLPLAT_MSG_LEADERBOARD_GET:            return "ovrMessage_Leaderboard_Get";
    case KLPLAT_MSG_LEADERBOARD_GET_AFTER_RANK: return "ovrMessage_Leaderboard_GetEntriesAfterRank";
    case KLPLAT_MSG_LEADERBOARD_GET_BY_IDS:     return "ovrMessage_Leaderboard_GetEntriesByIds";
    case KLPLAT_MSG_LEADERBOARD_WRITE_ENTRY_SM: return "ovrMessage_Leaderboard_WriteEntryWithSupplementaryMetric";
    case KLPLAT_MSG_IAP_PURCHASES:          return "ovrMessage_IAP_GetViewerPurchases";
    case KLPLAT_MSG_IAP_PRODUCTS:           return "ovrMessage_IAP_GetProductsBySKU";
    default:                                return "ovrMessage_Unknown";
    }
}

static const char *klplat_enum_to_string(uint32_t value) {
    (void)value;
    return "Unknown";
}

// ovrID_FromString(out, text) — a decimal id out of a string, true when it
// parsed. Imported beside the one above and just as unresolved.
static uint64_t klplat_ID_FromString(uint64_t *out, const char *text) {
    plat_hit("ovrID_FromString");
    if (!out || !text || !*text) return 0;
    char *end = NULL;
    unsigned long long v = strtoull(text, &end, 10);
    if (!end || *end) return 0;
    *out = (uint64_t)v;
    return 1;
}

// ovr_Net_ReadPacket() — the peer-to-peer network's receive poll. It returns an
// ovrPacketHandle, and NULL is the SDK's own "no packet waiting". There is no
// peer on this host (the platform service is absent, so no room can be joined
// and no connection can exist), so NULL is the true answer, not a made-up one.
// Drop Dead: Dual Strike Edition (x1935017546592600) polls it from its network
// update once the platform is up and the run ended there by name (on device).
// Nothing about ownership rides on it. The packet accessors (ovr_Packet_*) read
// a handle only a non-NULL answer could hand out, so they are never reached.
static uint64_t klplat_Net_ReadPacket(void) {
    plat_hit("ovr_Net_ReadPacket");
    return 0;
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
    {"ovr_Application_CancelAppDownload", (void *)klplat_Application_CancelAppDownload},
    {"ovr_Message_GetApplicationVersion",      (void *)klplat_Message_GetPayload},
    {"ovr_AssetFile_GetList",                  (void *)klplat_AssetFile_GetList},
    {"ovr_AssetFile_StatusByName",             (void *)klplat_AssetFile_StatusByName},
    {"ovr_AssetFile_StatusByName_Native",      (void *)klplat_AssetFile_StatusByName},
    {"ovr_AssetFile_StatusById",               (void *)klplat_AssetFile_StatusById},
    {"ovr_Message_GetAssetFileDeliveryList",   (void *)klplat_Message_GetPayload},
    {"ovr_AssetFileDeliveryList_GetSize",      (void *)klplat_AssetFileDeliveryList_GetSize},
    // GetList's readers. The array is empty for every title but Walkabout; there
    // it is the courses whose real bundles are on disk. Download stays refused.
    {"ovr_Message_GetAssetDetailsArray",       (void *)klplat_Message_GetPayload},
    {"ovr_Message_GetAssetDetails",            (void *)klplat_Message_GetAssetDetails},
    {"ovr_AssetDetailsArray_GetSize",          (void *)klplat_AssetDetailsArray_GetSize},
    {"ovr_AssetDetailsArray_GetElement",       (void *)klplat_AssetDetailsArray_GetElement},
    {"ovr_AssetDetails_GetAssetId",            (void *)klplat_AssetDetails_GetAssetId},
    {"ovr_AssetDetails_GetAssetType",          (void *)klplat_AssetDetails_GetAssetType},
    {"ovr_AssetDetails_GetAssetType_Native",   (void *)klplat_AssetDetails_GetAssetType},
    {"ovr_AssetDetails_GetDownloadStatus",     (void *)klplat_AssetDetails_GetDownloadStatus},
    {"ovr_AssetDetails_GetDownloadStatus_Native", (void *)klplat_AssetDetails_GetDownloadStatus},
    {"ovr_AssetDetails_GetFilepath",           (void *)klplat_AssetDetails_GetFilepath},
    {"ovr_AssetDetails_GetFilepath_Native",    (void *)klplat_AssetDetails_GetFilepath},
    {"ovr_AssetDetails_GetIapStatus",          (void *)klplat_AssetDetails_GetIapStatus},
    {"ovr_AssetDetails_GetIapStatus_Native",   (void *)klplat_AssetDetails_GetIapStatus},
    {"ovr_AssetDetails_GetMetadata",           (void *)klplat_AssetDetails_GetMetadata},
    // Walkabout purchases, one per store id of a course with a real bundle.
    // Any other title hits the function and gets request id 0.
    {"ovr_IAP_GetViewerPurchases",             (void *)klplat_IAP_GetViewerPurchases},
    {"ovr_IAP_GetViewerPurchasesDurableCache", (void *)klplat_IAP_GetViewerPurchasesDurableCache},
    {"ovr_IAP_GetProductsBySKU",               (void *)klplat_IAP_GetProductsBySKU},
    {"ovr_Message_GetPurchaseArray",           (void *)klplat_Message_GetPurchaseArray},
    {"ovr_PurchaseArray_GetSize",              (void *)klplat_PurchaseArray_GetSize},
    {"ovr_PurchaseArray_GetElement",           (void *)klplat_PurchaseArray_GetElement},
    {"ovr_Purchase_GetSKU",                    (void *)klplat_Purchase_GetSKU},
    {"ovr_Purchase_GetSKU_Native",             (void *)klplat_Purchase_GetSKU},
    {"ovr_Purchase_GetPurchaseID",             (void *)klplat_Purchase_GetPurchaseID},
    {"ovr_Purchase_GetPurchaseStrID",          (void *)klplat_Purchase_GetPurchaseStrID},
    {"ovr_Purchase_GetPurchaseStrID_Native",   (void *)klplat_Purchase_GetPurchaseStrID},
    {"ovr_Purchase_GetGrantTime",              (void *)klplat_Purchase_GetGrantTime},
    {"ovr_Purchase_GetExpirationTime",         (void *)klplat_Purchase_GetExpirationTime},
    {"ovr_Purchase_GetDeveloperPayload",       (void *)klplat_Purchase_GetDeveloperPayload},
    {"ovr_Purchase_GetReportingId",            (void *)klplat_Purchase_GetReportingId},
    {"ovr_Purchase_GetReportingId_Native",     (void *)klplat_Purchase_GetReportingId},
    {"ovr_Message_GetProductArray",            (void *)klplat_Message_GetProductArray},
    {"ovr_ProductArray_GetSize",               (void *)klplat_ProductArray_GetSize},
    {"ovr_ProductArray_GetElement",            (void *)klplat_ProductArray_GetElement},
    {"ovr_Product_GetSKU",                     (void *)klplat_Product_GetSKU},
    {"ovr_Product_GetSKU_Native",              (void *)klplat_Product_GetSKU},
    {"ovr_Product_GetName",                    (void *)klplat_Product_GetName},
    {"ovr_Product_GetDescription",             (void *)klplat_Product_GetDescription},
    {"ovr_Product_GetPrice",                   (void *)klplat_Product_GetPrice},
    {"ovr_Product_GetFormattedPrice",          (void *)klplat_Product_GetFormattedPrice},
    // The user access token and the achievement catalogue — two request-style
    // calls the game issues during platform init and gates its menu on. Both
    // complete with benign, empty/offline data (see the handlers above) rather
    // than the bare 0 that lands the guest on its "Request failed" path. The
    // exact-match binding here overrides the ovr_Achievements_ prefix rule.
    {"ovr_User_GetAccessToken",                (void *)klplat_User_GetAccessToken},
    {"ovr_User_GetUserProof",                  (void *)klplat_User_GetUserProof},
    {"ovr_Achievements_GetAllDefinitions",     (void *)klplat_Achievements_GetAllDefinitions},
    // Leaderboards: error completions (see the handlers).
    {"ovrMessageType_ToString",                (void *)klplat_MessageType_ToString},
    {"ovrID_FromString",                       (void *)klplat_ID_FromString},
    {"ovr_Leaderboard_GetEntries",             (void *)klplat_Leaderboard_GetEntries},
    {"ovr_Leaderboard_WriteEntry",             (void *)klplat_Leaderboard_WriteEntry},
    {"ovr_Leaderboard_Get",                    (void *)klplat_Leaderboard_Get},
    {"ovr_Net_ReadPacket",                     (void *)klplat_Net_ReadPacket},
    {"ovr_Voip_GetMicrophoneAvailability",     (void *)klplat_Voip_GetMicrophoneAvailability},
    {"ovr_Room_GetCurrent",                    (void *)klplat_Room_GetCurrent},
    {"ovr_Voip_SetSystemVoipSuppressed",       (void *)klplat_Voip_SetSystemVoipSuppressed},
    {"ovr_Leaderboard_GetEntriesAfterRank",    (void *)klplat_Leaderboard_GetEntriesAfterRank},
    {"ovr_Leaderboard_GetEntriesByIds",        (void *)klplat_Leaderboard_GetEntriesByIds},
    {"ovr_Leaderboard_WriteEntryWithSupplementaryMetric",
                                               (void *)klplat_Leaderboard_WriteEntryWithSupplementaryMetric},
    {"ovr_Message_GetAchievementDefinitionArray", (void *)klplat_Message_GetPayload},
    {"ovr_Achievements_GetAllProgress",        (void *)klplat_Achievements_GetAllProgress},
    {"ovr_Message_GetAchievementProgressArray", (void *)klplat_Message_GetPayload},
    // The rich-presence destinations: an empty list, completed (see the handler).
    {"ovr_RichPresence_GetDestinations",       (void *)klplat_RichPresence_GetDestinations},
    {"ovr_Message_GetDestinationArray",        (void *)klplat_Message_GetPayload},
    // The logged-in user (async) and the User accessors its completion feeds.
    // Gated by KL_PLAT_USER inside the handler; the accessors are inert unless a
    // user was handed out. The exact-match binding overrides ovr_User_GetLoggedInUser's
    // entry in g_plat_request.
    {"ovr_User_GetLoggedInUser",               (void *)klplat_User_GetLoggedInUser},
    {"ovr_User_Get",                           (void *)klplat_User_Get},
    // The exact-match binding overrides the name's entry in g_plat_request,
    // which is still what every guest but one gets (see the handler).
    {"ovr_User_GetOrgScopedID",                (void *)klplat_User_GetOrgScopedID},
    {"ovr_Message_GetUser",                    (void *)klplat_Message_GetPayload},
    {"ovr_Message_GetRoom",                    (void *)klplat_Message_GetPayload},
    {"ovr_User_GetID",                         (void *)klplat_User_GetID},
    {"ovr_User_GetOculusID",                   (void *)klplat_User_GetOculusID},
    {"ovr_User_GetDisplayName",                (void *)klplat_User_GetDisplayName},
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
    {"ovr_Message_GetError",      (void *)klplat_Message_GetError},
    {"ovr_Error_GetCode",         (void *)klplat_Error_GetCode},
    {"ovr_Error_GetHttpCode",     (void *)klplat_Error_GetHttpCode},
    {"ovr_Error_GetMessage",      (void *)klplat_Error_GetMessage},
    {"ovr_Error_GetDisplayableMessage", (void *)klplat_Error_GetDisplayableMessage},
    {"ovr_IAP_LaunchCheckoutFlow", (void *)klplat_IAP_LaunchCheckoutFlow},
    // Language-pack status (batman). The AssetFile/AssetDetails names share the
    // course entries above and branch on plat_is_langpack_target(); these four
    // exist only for language packs.
    {"ovr_AssetDetails_GetLanguage",       (void *)klplat_AssetDetails_GetLanguage_any},
    {"ovr_LanguagePackInfo_GetEnglishName", (void *)klplat_LanguagePackInfo_tag},
    {"ovr_LanguagePackInfo_GetNativeName",  (void *)klplat_LanguagePackInfo_tag},
    {"ovr_LanguagePackInfo_GetTag",         (void *)klplat_LanguagePackInfo_tag},
    // The current pack (Asgard's Wrath 2). The two DownloadResult readers are
    // reached only where plat_is_langpack_select lets them past the refusal.
    {"ovr_LanguagePack_GetCurrent",             (void *)klplat_LanguagePack_GetCurrent},
    {"ovr_LanguagePack_SetCurrent",             (void *)klplat_LanguagePack_SetCurrent},
    {"ovr_Message_GetAssetFileDownloadResult",  (void *)klplat_Message_GetAssetFileDownloadResult},
    {"ovr_AssetFileDownloadResult_GetAssetId",  (void *)klplat_AssetFileDownloadResult_GetAssetId},
    {"ovr_AssetFileDownloadResult_GetFilepath", (void *)klplat_AssetFileDownloadResult_GetFilepath},
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

static void *klplat_sym(const char *name, int s);
void *kl_ovrplat_sym(const char *name) {
    if (!name) return NULL;
    int s = plat_slot(name);                    // resolved, whatever happens next
    void *fn = klplat_sym(name, s);
    if (strncmp(name, "ovr_", 4) == 0) {
        int impl = 0;
        for (size_t i = 0; i < sizeof g_plat_impl / sizeof g_plat_impl[0] && !impl; i++)
            impl = strcmp(g_plat_impl[i].name, name) == 0;
        klplat_trace("resolve %s -> %s", name,
                     plat_is_drm(name) ? "REFUSED (aborts if called)" :
                     impl ? "answered" : "generic stub (unimplemented ones abort if called)");
    }
    return fn;
}
static void *klplat_sym(const char *name, int s) {
    // Unity IL2CPP P/Invoke probes the "W" (wide) variant first for entry points
    // with CharSet.Auto. Android's libovrplatformloader.so does not export "W"
    // entry points. Returning NULL lets IL2CPP fall back to the un-suffixed symbol.
    size_t n = strlen(name);
    if (n > 1 && name[n - 1] == 'W')
        return NULL;

    // Classified at *resolve* time as well as call time, so kl_ovrplat_report can
    // show what the guest went looking for even on a run where nothing aborted.
    // Resolving is still only a measurement — the refusal is at the call.
    if (plat_is_drm(name)) {
        if (s >= 0) __atomic_store_n(&g_plat[s].drm, 1, __ATOMIC_RELAXED);
        return kl_named_stub(name, (void *)klplat_drm);
    }
    for (size_t i = 0; i < sizeof g_plat_impl / sizeof g_plat_impl[0]; i++)
        if (strcmp(g_plat_impl[i].name, name) == 0) return g_plat_impl[i].fn;
    // The SDK's other enum-to-text helpers (ovr<Enum>_ToString): a name for a
    // log line, nothing a guest branches on. One answer serves them all rather
    // than each ending a run the first time something is logged.
    {
        size_t n = strlen(name);
        if (n > 9 && strcmp(name + n - 9, "_ToString") == 0 && strncmp(name, "ovr_", 4) != 0)
            return kl_named_stub(name, (void *)klplat_enum_to_string);
    }
    if (plat_is_unity_hook(name))
        return kl_named_stub(name, (void *)klplat_void);
    if (plat_is_drop(name))
        return kl_named_stub(name, (void *)klplat_void);
    if (strcmp(name, "ovr_Microphone_Create") == 0)
        return kl_named_stub(name, (void *)klplat_microphone_create);
    if (plat_is_microphone(name))
        return kl_named_stub(name, (void *)klplat_void);
    if (plat_is_options_create(name))
        return kl_named_stub(name, (void *)klplat_options_create);
    if (plat_is_launch_details(name))
        return kl_named_stub(name, (void *)klplat_launch_details);
    if (plat_is_launch_field(name))
        return kl_named_stub(name, (void *)klplat_launch_field);
    if (plat_is_empty_array(name))
        return kl_named_stub(name, (void *)klplat_empty_array);
    if (plat_is_room_field(name))
        return kl_named_stub(name, (void *)klplat_room_field);
    if (plat_is_options_sink(name))
        return kl_named_stub(name, (void *)klplat_void);
    if (plat_is_init_async(name))
        return kl_named_stub(name, (void *)klplat_init_async);
    if (plat_is_absent_ok(name) || plat_is_init(name) || plat_is_request(name) ||
        plat_is_user_field(name))
        return kl_named_stub(name, (void *)klplat_init_fails);
    return kl_named_stub(name, (void *)klplat_called);
}

// ---- self-test (tests/t_ovrplat.c) ------------------------------------------
#define KLPLAT_ST_TYPE      0x7E570000u      // the self-test's own message types
#define KLPLAT_ST_PRODUCERS 4
#define KLPLAT_ST_PER       250
#define KLPLAT_ST_HITTERS   8
#define KLPLAT_ST_NAMES     64
#define KLPLAT_ST_ROUNDS    50

static void *klplat_st_produce(void *arg) {
    int who = (int)(intptr_t)arg;
    for (int i = 0; i < KLPLAT_ST_PER; i++)
        if (!klplat_request_kind("selftest", KLPLAT_ST_TYPE | (uint32_t)who, i, KLPLAT_KIND_NONE))
            return (void *)1;
    return NULL;
}

static void *klplat_st_hit(void *arg) {
    int who = (int)(intptr_t)arg;
    char name[48];
    for (int r = 0; r < KLPLAT_ST_ROUNDS; r++)
        for (int i = 0; i < KLPLAT_ST_NAMES; i++) {
            // Each thread walks the names from a different start, so first
            // inserts of one name race between threads.
            snprintf(name, sizeof name, "kl_selftest_%d", (i + who * 7) % KLPLAT_ST_NAMES);
            plat_hit(name);
        }
    return NULL;
}

int kl_ovrplat_queue_selftest(FILE *f) {
    int ok = 1;
#define KLPLAT_ST_CHECK(c, ...) do { if (!(c)) { ok = 0; fprintf(f, "  FAIL: " __VA_ARGS__); \
                                                 fprintf(f, "\n"); } } while (0)
    // 1. Three blocks' worth outstanding at once: every request gets a real id,
    //    and every one arrives, in order, with its own payload.
    enum { N = 3 * KLPLAT_MSGQ };
    uint64_t ids[N];
    klplat_msg *held[N];
    for (int i = 0; i < N; i++) {
        ids[i] = klplat_request_kind("selftest", KLPLAT_ST_TYPE, i, KLPLAT_KIND_NONE);
        KLPLAT_ST_CHECK(ids[i] != 0 && (i == 0 || ids[i] > ids[i - 1]),
                        "request %d got id %llu", i, (unsigned long long)ids[i]);
    }
    for (int i = 0; i < N; i++) {
        held[i] = klplat_PopMessage();
        KLPLAT_ST_CHECK(held[i] && held[i]->request == ids[i] && held[i]->payload == i,
                        "completion %d missing or out of order", i);
        if (!held[i]) return 0;
    }
    KLPLAT_ST_CHECK(klplat_PopMessage() == NULL, "the queue holds more than was asked for");
    fprintf(f, "  %d requests outstanding: %d completions, in order\n", N, N);

    // 2. Handles the guest holds are not rewritten by requests queued after them.
    uint64_t more[N];
    for (int i = 0; i < N; i++)
        more[i] = klplat_request_str("selftest", KLPLAT_ST_TYPE + 1, -i, "x");
    for (int i = 0; i < N; i++)
        KLPLAT_ST_CHECK(held[i]->request == ids[i] && held[i]->payload == i &&
                        held[i]->type == KLPLAT_ST_TYPE && held[i]->str == NULL &&
                        held[i]->state == KLPLAT_SLOT_POPPED,
                        "held handle %d was rewritten", i);
    for (int i = 0; i < N; i++) {
        klplat_msg *m = klplat_PopMessage();
        KLPLAT_ST_CHECK(m && m->request == more[i] && m->payload == -i,
                        "second batch completion %d missing or out of order", i);
        if (m) klplat_FreeMessage(m);
    }
    fprintf(f, "  %d held handles intact across %d further requests\n", N, N);

    // 3. FreeMessage misuse: a double free, a foreign pointer and a message
    //    still queued are all ignored, so no slot is ever handed out twice.
    klplat_FreeMessage(held[0]);
    klplat_FreeMessage(held[0]);
    int foreign = 0;
    klplat_FreeMessage((klplat_msg *)&foreign);
    klplat_FreeMessage((klplat_msg *)((char *)held[1] + 1));
    uint64_t a = klplat_request("selftest", KLPLAT_ST_TYPE + 2, 1);
    pthread_mutex_lock(&g_msg_mu);
    klplat_msg *queued = g_msg_head;
    pthread_mutex_unlock(&g_msg_mu);
    klplat_FreeMessage(queued);
    uint64_t b = klplat_request("selftest", KLPLAT_ST_TYPE + 2, 2);
    klplat_msg *ma = klplat_PopMessage(), *mb = klplat_PopMessage();
    KLPLAT_ST_CHECK(ma && mb && ma != mb && ma->request == a && mb->request == b &&
                    ma->payload == 1 && mb->payload == 2,
                    "a misused FreeMessage put a slot into circulation twice");
    KLPLAT_ST_CHECK(held[1]->request == ids[1], "a misaligned free touched a held handle");
    if (ma) klplat_FreeMessage(ma);
    if (mb) klplat_FreeMessage(mb);
    for (int i = 1; i < N; i++) klplat_FreeMessage(held[i]);
    fprintf(f, "  double free, foreign and queued frees ignored\n");

    // 4. Language-pack details are the message's own copy.
    klplat_asset src;
    memset(&src, 0, sizeof src);
    src.id = 42;
    snprintf(src.path, sizeof src.path, "/selftest/fr.lang");
    snprintf(src.tag, sizeof src.tag, "fr");
    src.status = "installed";
    uint64_t c = klplat_request_asset("selftest", KLPLAT_ST_TYPE + 3, KLPLAT_KIND_LANGPACK_SET, &src);
    snprintf(src.tag, sizeof src.tag, "de");
    klplat_msg *mc = klplat_PopMessage();
    const klplat_asset *det = mc ? (const klplat_asset *)(uintptr_t)
                                   klplat_Message_GetAssetFileDownloadResult(mc) : NULL;
    KLPLAT_ST_CHECK(mc && mc->request == c && det == &mc->asset && det->id == 42 &&
                    strcmp(det->tag, "fr") == 0 && strcmp(det->path, "/selftest/fr.lang") == 0,
                    "language-pack details did not ride in the message");
    if (mc) klplat_FreeMessage(mc);
    fprintf(f, "  language-pack details copied into their message\n");

    // 4b. ...and so is a LIST of them: the elements ovr_AssetDetailsArray_GetElement
    //     hands out point into the message, and a rescan that rewrites or
    //     shrinks the shared list after the request changes none of them.
    klplat_asset saved[KLPLAT_LANG_MAX];
    pthread_mutex_lock(&g_msg_mu);
    int saved_n = g_nlang;
    memcpy(saved, g_lang, sizeof saved);
    g_lang[0] = src;                                  // "de", id 42
    g_lang[1] = src; g_lang[1].id = 43;
    snprintf(g_lang[1].tag, sizeof g_lang[1].tag, "it");
    g_nlang = 2;
    pthread_mutex_unlock(&g_msg_mu);
    uint64_t dl = klplat_request_lang_list("selftest", KLPLAT_ST_TYPE + 6);
    pthread_mutex_lock(&g_msg_mu);                    // the rescan
    snprintf(g_lang[0].tag, sizeof g_lang[0].tag, "xx");
    g_lang[0].id = 99;
    g_nlang = 0;
    pthread_mutex_unlock(&g_msg_mu);
    klplat_msg *mlist = klplat_PopMessage();
    const klplat_asset *e0 = mlist ? klplat_AssetDetailsArray_GetElement(mlist, 0) : NULL;
    const klplat_asset *e1 = mlist ? klplat_AssetDetailsArray_GetElement(mlist, 1) : NULL;
    const char *lo = mlist && mlist->list ? (const char *)mlist->list : NULL;
    int inside = lo && (const char *)e0 >= lo &&
                 (const char *)(e1 + 1) <= lo + KLPLAT_LANG_MAX * sizeof *mlist->list;
    KLPLAT_ST_CHECK(mlist && mlist->request == dl &&
                    klplat_AssetDetailsArray_GetSize(mlist) == 2 && e0 && e1 && inside &&
                    e0->id == 42 && strcmp(e0->tag, "de") == 0 &&
                    e1->id == 43 && strcmp(e1->tag, "it") == 0 &&
                    klplat_AssetDetailsArray_GetElement(mlist, 2) == NULL &&
                    klplat_AssetDetailsArray_GetElement(mlist, -1) == NULL,
                    "a language-pack list did not answer out of its own copy");
    if (mlist) klplat_FreeMessage(mlist);
    pthread_mutex_lock(&g_msg_mu);
    memcpy(g_lang, saved, sizeof saved);
    g_nlang = saved_n;
    pthread_mutex_unlock(&g_msg_mu);
    fprintf(f, "  language-pack list elements are the message's own, past a rescan\n");

    // 5. Producers on four threads, the pump on this one: every id arrives once.
    enum { T = KLPLAT_ST_PRODUCERS * KLPLAT_ST_PER };
    pthread_mutex_lock(&g_msg_mu);
    uint64_t first = g_next_request;
    pthread_mutex_unlock(&g_msg_mu);
    unsigned char *seen = calloc(T, 1);
    if (!seen) return 0;
    pthread_t th[KLPLAT_ST_HITTERS];
    for (int i = 0; i < KLPLAT_ST_PRODUCERS; i++)
        pthread_create(&th[i], NULL, klplat_st_produce, (void *)(intptr_t)i);
    int got = 0, dup = 0, stray = 0, spins = 0;
    while (got < T && spins < 50000000) {
        klplat_msg *m = klplat_PopMessage();
        if (!m) { spins++; continue; }
        uint64_t k = m->request - first;
        if ((m->type & 0xFFFF0000u) != KLPLAT_ST_TYPE || k >= T) stray++;
        else if (seen[k]++) dup++;
        else got++;
        klplat_FreeMessage(m);
    }
    int failed = 0;
    for (int i = 0; i < KLPLAT_ST_PRODUCERS; i++) {
        void *r = NULL;
        pthread_join(th[i], &r);
        failed += r != NULL;
    }
    free(seen);
    KLPLAT_ST_CHECK(got == T && !dup && !stray && !failed,
                    "threaded: %d of %d completions, %d duplicate, %d stray, %d producer(s) refused",
                    got, T, dup, stray, failed);
    KLPLAT_ST_CHECK(klplat_PopMessage() == NULL, "threaded: completions left over");
    fprintf(f, "  %d threads x %d requests: %d completions, each once\n",
            KLPLAT_ST_PRODUCERS, KLPLAT_ST_PER, got);

    // 6. The call-surface table under eight threads racing on first inserts.
    for (int i = 0; i < KLPLAT_ST_HITTERS; i++)
        pthread_create(&th[i], NULL, klplat_st_hit, (void *)(intptr_t)i);
    for (int i = 0; i < KLPLAT_ST_HITTERS; i++) pthread_join(th[i], NULL);
    unsigned n = __atomic_load_n(&g_nplat, __ATOMIC_ACQUIRE), names = 0, nulls = 0, wrong = 0;
    for (unsigned i = 0; i < n; i++) {
        if (!g_plat[i].name) { nulls++; continue; }
        if (strncmp(g_plat[i].name, "kl_selftest_", 12) != 0) continue;
        names++;
        if (g_plat[i].calls != KLPLAT_ST_HITTERS * KLPLAT_ST_ROUNDS) wrong++;
    }
    KLPLAT_ST_CHECK(!nulls && names == KLPLAT_ST_NAMES && !wrong,
                    "surface table: %u names (want %d), %u NULL, %u miscounted",
                    names, KLPLAT_ST_NAMES, nulls, wrong);
    fprintf(f, "  %d threads x %d names: %u slots, none NULL, counts exact\n",
            KLPLAT_ST_HITTERS, KLPLAT_ST_NAMES, names);

    // 7. The pool's limit: with nothing outstanding, exactly
    //    KLPLAT_MSG_MAX_BLOCKS * KLPLAT_MSGQ requests get an id, the next one
    //    is refused with 0 and leaves no completion behind, the growth warning
    //    was said, and once the guest drains the queue requests succeed again.
    enum { CAP = KLPLAT_MSG_MAX_BLOCKS * KLPLAT_MSGQ };
    int issued = 0;
    while (issued <= CAP && klplat_request("selftest", KLPLAT_ST_TYPE + 4, issued)) issued++;
    pthread_mutex_lock(&g_msg_mu);
    int nblocks = g_msg_nblocks, warned = g_msg_warned;
    pthread_mutex_unlock(&g_msg_mu);
    int drained = 0, inorder = 1;
    for (klplat_msg *m; (m = klplat_PopMessage()) != NULL; drained++) {
        inorder &= m->payload == drained;
        klplat_FreeMessage(m);
    }
    uint64_t after = klplat_request("selftest", KLPLAT_ST_TYPE + 5, 0);
    klplat_msg *ml = klplat_PopMessage();
    KLPLAT_ST_CHECK(issued == CAP && nblocks == KLPLAT_MSG_MAX_BLOCKS && warned &&
                    drained == CAP && inorder && after && ml && ml->request == after,
                    "pool limit: %d of %d issued, %d blocks, warned %d, %d drained%s, "
                    "%s afterwards", issued, CAP, nblocks, warned, drained,
                    inorder ? "" : " OUT OF ORDER", after ? "served" : "REFUSED");
    if (ml) klplat_FreeMessage(ml);
    fprintf(f, "  pool limit: %d requests served, the next refused with 0, "
               "served again once drained\n", issued);
#undef KLPLAT_ST_CHECK
    return ok;
}

void kl_ovrplat_report(FILE *f) {
    // No lock (see g_plat_mu): this runs on the way to abort(). The count is
    // acquired once, so every slot below it is complete.
    static int done;
    unsigned n = __atomic_load_n(&g_nplat, __ATOMIC_ACQUIRE);
    if (!n || __atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL)) return;
#define KLPLAT_CALLS(i) __atomic_load_n(&g_plat[i].calls, __ATOMIC_RELAXED)
#define KLPLAT_DRM(i)   __atomic_load_n(&g_plat[i].drm, __ATOMIC_RELAXED)
    unsigned called = 0, drm = 0;
    for (unsigned i = 0; i < n; i++) {
        if (KLPLAT_CALLS(i)) called++;
        if (KLPLAT_DRM(i))   drm++;
    }
    fprintf(f, "\n=== Oculus Platform surface ===\n");
    fprintf(f, "  resolved: %u, of which called: %u\n", n, called);
    if (drm) {
        unsigned refused = __atomic_load_n(&g_drm_calls, __ATOMIC_RELAXED);
        fprintf(f, "  --- entitlement / ownership (REFUSED by policy) ---\n");
        for (unsigned i = 0; i < n; i++)
            if (KLPLAT_DRM(i))
                fprintf(f, "    %-52s %s\n", g_plat[i].name,
                        KLPLAT_CALLS(i) ? "CALLED" : "resolved only");
        fprintf(f, "  (%u DRM call%s refused)\n", refused, refused == 1 ? "" : "s");
    }
    fprintf(f, "  --- called ---\n");
    for (unsigned i = 0; i < n; i++)
        if (KLPLAT_CALLS(i) && !KLPLAT_DRM(i))
            fprintf(f, "    %-52s x%u\n", g_plat[i].name, KLPLAT_CALLS(i));
    fprintf(f, "  --- resolved but never called ---\n");
    for (unsigned i = 0; i < n; i++)
        if (!KLPLAT_CALLS(i) && !KLPLAT_DRM(i)) fprintf(f, "    %s\n", g_plat[i].name);
#undef KLPLAT_CALLS
#undef KLPLAT_DRM
}
