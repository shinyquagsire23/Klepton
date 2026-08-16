// android framework: assets, time zone, Intent and the sticky
// battery query, WebView, WifiManager, NetworkCapabilities, storage
//
// One family of the synthetic JNIEnv's Java classes. The mechanism (registries,
// dispatch, id interning) is kl_jni.c; this file owns implementations and the
// binding table that names them. See runtime/jni/kl_jni_int.h for the seam.
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "klepton.h"
#include "kl_jni.h"
#include "kl_fault.h"
#include "kl_target.h"
#include "kl_env.h"
#include "kl_ovrp.h"
#include "kl_avdec.h"
#include "kl_egl.h"
#include "kl_ndk.h"
#include "kl_va.h"
#include <zlib.h>
#include "kl_jni_int.h"

// ---- assets ----
// This is the path the M3 measurement predicted: with no AAssetManager_* import,
// assets reach Unity over JNI instead — Context.getAssets() -> AssetManager.open()
// -> InputStream -> Scanner. We serve it from the unpacked APK on disk.
// Every path we hand the guest must be absolute. Android's are — getPackageCodePath
// returns /data/app/<pkg>/base.apk and getFilesDir /data/data/<pkg>/files — and
// Unity relies on it: it mounts the APK into its VFS under the path it was given
// and later resolves entries by concatenating onto that mount point. A relative
// mount point survives the mount and then fails to match, so the lookup falls
// through to a raw open() of "beatsaber.apk/assets/..." — which is not a
// directory, and Unity reports it as "Not enough storage space to install
// required resources."
//
// realpath() is not usable here: several of these name directories we have not
// created yet. Prefixing the cwd is enough, since that is what a relative path
// already meant.
 const char *klj_abspath(const char *p) {
    if (!p || p[0] == '/') return p;
    char cwd[1024];
    if (!getcwd(cwd, sizeof cwd)) return p;
    size_t n = strlen(cwd) + strlen(p) + 2;
    char  *out = malloc(n);
    snprintf(out, n, "%s/%s", cwd, p);
    return out;
}

 const char *g_assets_dir = "beatsaber/assets";
void kl_jni_set_assets_dir(const char *dir) {
    g_assets_dir = klj_abspath(dir);
    // There are TWO doors onto the same directory and they have to agree:
    // AssetManager.open() over JNI (this file) and AAssetManager_open() in the
    // NDK (kl_ndk.c, its own g_asset_root). Beat Saber only ever uses the first
    // and Steam Link's 2D half only ever uses the first, so the second sat at
    // its "assets" default with NO CALLER ANYWHERE — which is fine until a
    // guest uses the NDK door, and then it silently resolves against the
    // working directory. libvrlink_scene is that guest: its config/*.json loads
    // failed with "Failed to load file config/hmd_config.json" and nothing
    // pointing at a path at all. One setter now feeds both.
    kl_ndk_set_assets_dir(g_assets_dir);
}

 klj_val klj_singleton(const char *cls, void **slot) {
    if (!*slot) {
        *slot = kl_jni_new_object(cls);
        ((klj_object *)*slot)->pinned = 1;   // host-held: survives frame pops
    }
    return (klj_val){.l = *slot};
}

// Android's application context is a longer-lived object than the Activity, but
// every context here is the same synthetic bag of services, so one singleton
// answers both. It matters only that it IS a Context: the guest passes it to a
// WebView constructor, and everything it then asks of it lands on the
// Context bindings below.
static klj_val klj_Activity_getApplicationContext(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *appctx;
    return klj_singleton("android/content/Context", &appctx);
}

// ---- time zone and clock format ----
// Mono's TimeZoneInfo asks ICU rather than reading /usr/share/zoneinfo on
// Android, so these are the .NET time-zone database as this guest sees it.
//
// The host's zone is the honest answer and it is available: tzset() fills
// `tzname`, and TZ names it when it is set. Reporting a zone we are not in
// would put every timestamp the guest shows an hour or more out, which is the
// kind of wrong that looks like a bug in the guest.
//
// getAvailableIDs answers with the host's own zone and UTC and nothing else.
// The full IANA list is 600 names we would be inventing from a database we do
// not carry, and the only lookups a guest can make that we could then satisfy
// are exactly these two: its own zone, and the one every protocol falls back
// to. A name we do not list fails as "unknown zone", which is a real answer;
// a name we list and then describe wrongly is not.
static const char *klj_host_tzid(void) {
    static char id[128];
    if (id[0]) return id;
    const char *tz = getenv("TZ");
    if (tz && *tz) { snprintf(id, sizeof id, "%s", tz); return id; }
    // /etc/localtime is a symlink into the zoneinfo tree on Darwin, and its
    // tail IS the IANA id — tzname only carries the abbreviation ("PST"),
    // which is not an id and cannot be looked up.
    char buf[512];
    ssize_t k = readlink("/etc/localtime", buf, sizeof buf - 1);
    if (k > 0) {
        buf[k] = '\0';
        const char *p = strstr(buf, "zoneinfo/");
        if (p) { snprintf(id, sizeof id, "%s", p + 9); return id; }
    }
    snprintf(id, sizeof id, "UTC");
    return id;
}

static klj_val klj_TimeZone_getDefault(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *tz;
    if (!tz) {
        tz = kl_jni_new_object("android/icu/util/TimeZone");
        klj_as_object(tz)->pinned = 1;
        KLJ_LOG("TimeZone.getDefault() -> %s", klj_host_tzid());
    }
    return (klj_val){.l = tz};
}

static klj_val klj_TimeZone_getID(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string(klj_host_tzid())};
}

// getTimeZone(id) — the lookup the list above exists to be looked up in. Java
// answers GMT for an id it does not know rather than throwing, and that is the
// behaviour transcribed here: a guest asking for a zone we do not carry gets a
// real zone with a real offset, which is what its own runtime would have given
// it. The object is the same singleton either way, because everything anyone
// can then ask of it comes from getID.
static klj_val klj_TimeZone_getTimeZone(void *env, void *self, const klj_val *a, int n) {
    const char *id = n > 0 ? klj_str(a[0].l) : NULL;
    if (id && strcmp(id, klj_host_tzid()) != 0 && strcmp(id, "UTC") != 0 &&
        strcmp(id, "GMT") != 0)
        KLJ_LOG("TimeZone.getTimeZone(\"%s\"): not a zone this host carries — "
                "answering the default, as Java does", id);
    return klj_TimeZone_getDefault(env, self, NULL, 0);
}

// The offsets. These are a GROUP answer rather than three separate ones, and
// deliberately so: getOffset, getRawOffset and useDaylightTime have to agree
// with each other and with getID, and a guest that computes a local time from
// one and checks it against another would see the disagreement rather than the
// gap. All three come from the host's own zone database through localtime_r,
// which is the same database getID named.
//
// getOffset takes UTC milliseconds and returns milliseconds INCLUDING any DST
// in force at that instant, which is exactly tm_gmtoff at that instant.
static klj_val klj_TimeZone_getOffset(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    time_t when = n > 0 ? (time_t)((int64_t)a[0].j / 1000) : time(NULL);
    struct tm tm;
    if (!localtime_r(&when, &tm)) return (klj_val){.j = 0};
    return (klj_val){.j = (uint64_t)(int64_t)(tm.tm_gmtoff * 1000)};
}

// ...and the same without DST: January and July, and the smaller one is the
// standard offset. Asking the database twice is how you get "what is this
// zone's base offset" out of an API that only reports a moment.
static void klj_tz_offsets(long *raw, int *has_dst) {
    time_t now = time(NULL);
    struct tm tm;
    long a_off = 0, b_off = 0;
    time_t jan = now - 182L * 86400, jul = now + 182L * 86400;
    if (localtime_r(&jan, &tm)) a_off = tm.tm_gmtoff;
    if (localtime_r(&jul, &tm)) b_off = tm.tm_gmtoff;
    *raw     = a_off < b_off ? a_off : b_off;
    *has_dst = a_off != b_off;
}

static klj_val klj_TimeZone_getRawOffset(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    long raw; int dst;
    klj_tz_offsets(&raw, &dst);
    return (klj_val){.j = (uint64_t)(int64_t)(raw * 1000)};
}

static klj_val klj_TimeZone_useDaylightTime(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    long raw; int dst;
    klj_tz_offsets(&raw, &dst);
    return (klj_val){.j = (uint64_t)dst};
}

// getDisplayName(daylight, style). The host has abbreviations and not long
// names: tzname[] is "PST"/"PDT" and there is no "Pacific Standard Time"
// anywhere in libc. So SHORT gets the abbreviation and LONG gets the id, which
// is a name the guest can round-trip through getTimeZone rather than a phrase
// it can only print. Style is ICU's: 1 is SHORT, everything else reads as long.
static klj_val klj_TimeZone_getDisplayName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int daylight = n > 0 ? (int)a[0].j : 0;
    int style    = n > 1 ? (int)a[1].j : 0;
    tzset();
    const char *abbr = tzname[daylight && tzname[1] && tzname[1][0] ? 1 : 0];
    return (klj_val){.l = kl_jni_new_string(style == 1 && abbr && *abbr
                                            ? abbr : klj_host_tzid())};
}

// How much DST adds when it is in force — the difference between the two
// samples klj_tz_offsets already takes, so it cannot disagree with
// useDaylightTime or with getOffset in July.
static klj_val klj_TimeZone_getDSTSavings(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    time_t now = time(NULL);
    struct tm tm;
    long lo = 0, hi = 0;
    time_t jan = now - 182L * 86400, jul = now + 182L * 86400;
    if (localtime_r(&jan, &tm)) lo = tm.tm_gmtoff;
    if (localtime_r(&jul, &tm)) hi = tm.tm_gmtoff;
    long d = hi > lo ? hi - lo : lo - hi;
    return (klj_val){.j = (uint64_t)(int64_t)(d * 1000)};
}

static klj_val klj_TimeZone_getAvailableIDs(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    const char *host = klj_host_tzid();
    int utc = strcmp(host, "UTC") == 0;
    void *arr = klj_new_array('L', KLJ_CLASS_STRING, utc ? 1 : 2);
    void **d = klj_arr(arr)->data;
    d[0] = kl_jni_new_string(host);
    if (!utc) d[1] = kl_jni_new_string("UTC");
    KLJ_LOG("TimeZone.getAvailableIDs() -> %s%s", host, utc ? "" : " + UTC");
    return (klj_val){.l = arr};
}

// Android reads Settings.System.TIME_12_24; there is no such setting here, so
// it comes from the host's locale — strftime's own %X for an unambiguous hour.
// Picking one would be a guess, and this is the same question asked of the
// machine actually running.
static klj_val klj_DateFormat_is24HourFormat(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static int cached = -1;
    if (cached < 0) {
        struct tm tm = {0};
        tm.tm_hour = 13; tm.tm_min = 0; tm.tm_mday = 1; tm.tm_year = 100;
        char buf[64] = {0};
        strftime(buf, sizeof buf, "%X", &tm);
        cached = !(strstr(buf, "PM") || strstr(buf, "pm") || strchr(buf, '\xef'));
        KLJ_LOG("DateFormat.is24HourFormat() -> %d (the host formats 13:00 as \"%s\")",
                cached, buf);
    }
    return (klj_val){.j = (uint64_t)cached};
}

// ActivityManager.getMemoryInfo(MemoryInfo) — Android's other memory question,
// and the one a title asks when it wants to size a cache or decide whether to
// drop one. It fills the caller's object in place, so the answers go through
// the same per-object write table an ordinary Set*Field would use and the
// guest reads them back by the ordinary path.
//
// Every number comes from kl_mem_* — the SAME budget /proc/meminfo reports and
// the same footprint the low-memory notification fires on. Two sources here
// would be two answers to one question: a guest that reads `availMem` and then
// polls /proc would see them disagree, and `lowMemory` would say no while the
// notification said yes. `threshold` is the level at which Android would start
// killing background processes, which here is exactly KL_LOWMEM_AT.
static klj_val klj_ActivityManager_getMemoryInfo(void *env, void *self,
                                                 const klj_val *a, int n) {
    (void)env; (void)self;
    void *mi = n > 0 ? a[0].l : NULL;
    if (!mi) return (klj_val){.j = 0};
    uint64_t total = kl_mem_budget_bytes(), avail = kl_mem_available_bytes();
    uint64_t thresh = total / 100 * (uint64_t)(100 - kl_env_int("KL_LOWMEM_AT", 80));
    void *cls = klj_class_object("android/app/ActivityManager$MemoryInfo");
    klj_field_store(mi, klj_want(cls, "availMem",  "J", 'f'), (klj_val){.j = avail});
    klj_field_store(mi, klj_want(cls, "totalMem",  "J", 'f'), (klj_val){.j = total});
    klj_field_store(mi, klj_want(cls, "threshold", "J", 'f'), (klj_val){.j = thresh});
    klj_field_store(mi, klj_want(cls, "lowMemory", "Z", 'f'),
                    (klj_val){.j = avail < thresh});
    KLJ_LOG("ActivityManager.getMemoryInfo -> avail %llu MiB of %llu, threshold %llu, low=%d",
            (unsigned long long)(avail >> 20), (unsigned long long)(total >> 20),
            (unsigned long long)(thresh >> 20), (int)(avail < thresh));
    return (klj_val){.j = 0};
}

// ---- com.vrchat.android.plugin.Info ----
// The title's own Java half, and it is entirely transcribed from
// smali_classes2/com/vrchat/android/plugin/Info.smali rather than guessed:
// instance() is `Class.forName(...).getConstructor().newInstance()`, i.e. a
// plain `new Info()`; init() reads PackageInfo.versionName/versionCode off the
// context; and the two getters hand those back. Every one of those already has
// a real answer here (kl_jni_guest_version, through PackageManager), so this
// class is a pass-through to the answer the guest would have got anyway — which
// is why it is served rather than refused.
static klj_val klj_VRCInfo_instance(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *inst;
    return klj_singleton("com/vrchat/android/plugin/Info", &inst);
}

// Answered from the same place PackageInfo is, rather than from a field of our
// own: `Info.init()` copies them OUT of PackageInfo, so two sources here would
// be two answers to one question and only one of them would be the guest's.
static klj_val klj_VRCInfo_getVersionCode(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return klj_PackageInfo_versionCode();
}
static klj_val klj_VRCInfo_getVersionName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return klj_PackageInfo_versionName();
}

static klj_val klj_Activity_getIntent(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *intent;
    return klj_singleton("android/content/Intent", &intent);
}
// The launch extras — i.e. the SL-6 handoff, arriving.
//
// SL-6 measured the 2D shell building an explicit Intent for
// `com.valvesoftware.steamlinkvr/android.app.NativeActivity` carrying four
// string extras, and refused it because the VR activity did not exist. It does
// now, and it reads them back through exactly this call: getIntent().getExtras()
// then Bundle.getString("sArgs"). Without them libvrlink_scene prints
// "No sArgs and release build panic. Aborting back to SteamLink." and exits
// before its first frame — so this is the join between the two halves of the
// Steam Link arc, not a convenience.
//
// The values come from the environment rather than from a live shell, because
// the two halves do not yet run in one process: KL_SLINK_SARGS is pasted from a
// pairing run (notes/STEAMLINK.md has the format,
// "<ip>~10400~10400~0,0,1~~~~<token>"). Wiring the shell's startVRLink straight
// into this table is what removes the paste step.
//
// **Unset means unset.** With no sArgs the whole Bundle is absent and getExtras
// answers null, which is what a normally-launched activity sees and what Unity
// on the other target relies on. An empty-but-present Bundle would be a
// different claim — "launched with arguments, none of them set" — and this
// guest distinguishes them ("No extras bundle was present" is its own log line).
static klj_val klj_Intent_getExtras(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *extras;
    static int   built;
    if (!built) {
        built = 1;
        static klj_kv kv[5];
        int k = 0;
        const struct { const char *env, *key; } want[] = {
            {"KL_SLINK_SARGS",         "sArgs"},
            {"KL_SLINK_START_INFO",    "sStartInfo"},
            {"KL_SLINK_ORIG_PACKAGE",  "sOriginalPackage"},
            {"KL_SLINK_ORIG_ACTIVITY", "sOriginalActivity"},
        };
        for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
            const char *v = getenv(want[i].env);
            if (v && *v) { kv[k].key = want[i].key; kv[k].val = v; k++; }
        }
        // sStartInfo is not an independent value: the shell derives it from
        // sArgs and we are standing in for the shell, so deriving it here is
        // transcription rather than invention. SteamLink.startVRLink does
        //     String[] f = sArgs.split("~");
        //     if (f.length > 3) intent.putExtra("sStartInfo", f[3]);
        // and field 3 is the "0,0,1" in the middle of a real handoff string.
        // The guest reads it back with the same GetExtrasKey call it uses for
        // everything else, so an absent one is simply an empty string to it —
        // which is why this went unnoticed, and why it is worth closing: the
        // whole point of the synthesized Intent is to be the one the shell
        // would have sent.
        if (!getenv("KL_SLINK_START_INFO")) {
            const char *args = getenv("KL_SLINK_SARGS");
            if (args && *args) {
                const char *p = args;
                int field = 0;
                while (field < 3 && (p = strchr(p, '~')) != NULL) { p++; field++; }
                if (field == 3) {
                    const char *end = strchr(p, '~');
                    size_t len = end ? (size_t)(end - p) : strlen(p);
                    if (len) {
                        char *si = malloc(len + 1);
                        if (si) {
                            memcpy(si, p, len);
                            si[len] = '\0';
                            kv[k].key = "sStartInfo"; kv[k].val = si; k++;
                        }
                    }
                }
            }
        }
        kv[k].key = kv[k].val = NULL;
        if (k) {
            extras = klj_new_object_data("android/os/Bundle", kv);
            ((klj_object *)extras)->pinned = 1;   // host-held across frames
            for (int i = 0; i < k; i++)
                KLJ_LOG("launch extra %s = \"%s\"", kv[i].key, kv[i].val);
        }
    }
    return (klj_val){.l = extras};
}
// new Intent(action). The action string is the only part anything reads so far.
static klj_val klj_Intent_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)clazz;
    const char *action = n > 0 ? klj_str(a[0].l) : NULL;
    void       *obj    = kl_jni_new_object("android/content/Intent");
    klj_as_object(obj)->data = action ? strdup(action) : NULL;
    KLJ_LOG("new Intent(\"%s\")", action ? action : "(null)");
    return (klj_val){.l = obj};
}
// Intent's builder methods all return `this` — that is the Java contract, not a
// convenience. Nothing reads the categories back, so they are logged, not stored.
static klj_val klj_Intent_addCategory(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    const char *c = n > 0 ? klj_str(a[0].l) : NULL;
    KLJ_LOG("Intent.addCategory(\"%s\")", c ? c : "(null)");
    return (klj_val){.l = self};
}

// ---- IntentFilter + registerReceiver: the STICKY battery query --------------
//
// libunity reads the battery exactly as Android documents it: build an
// IntentFilter for ACTION_BATTERY_CHANGED, call registerReceiver with a NULL
// receiver, and read the Intent that comes back — `level`, `scale` and
// `status`, which are the three extras its own .text names.
//
// ACTION_BATTERY_CHANGED is a STICKY broadcast, and that is what makes this
// answerable at all: the call is a QUERY, returning the value the system last
// published, so it works with nothing ever being delivered. That is the whole
// difference from JavaBroadcastReceiver.setReceiver further down, which
// subscribes to a bus with no publisher and correctly models nothing — here
// there is no delivery to model, only a current value.
//
// The value is the kl_ovrp battery seam, the same source
// BatteryManager.isCharging and getIntProperty already answer from. Two sources
// for one battery is a level that disagrees with its own charging flag, and
// nothing anywhere would report it.
#define KLJ_IF_MAX_ACTIONS 8
#define KLJ_ACTION_BATTERY "android.intent.action.BATTERY_CHANGED"

typedef struct { char *act[KLJ_IF_MAX_ACTIONS]; int n; } klj_intent_filter;

static void klj_intent_filter_free(void *p) {
    klj_intent_filter *f = p;
    if (!f) return;
    for (int i = 0; i < f->n; i++) free(f->act[i]);
    free(f);
}

static void klj_if_add(void *self, const char *action) {
    klj_object *o = klj_as_object(self);
    klj_intent_filter *f = o ? o->data : NULL;
    if (!f || !action) return;
    if (f->n >= KLJ_IF_MAX_ACTIONS) {
        // Bounded, and it says so: a filter that silently drops the action the
        // caller cares about is a registration that matches nothing.
        KLJ_LOG("IntentFilter.addAction(\"%s\") — DROPPED, filter is full (%d)",
                action, KLJ_IF_MAX_ACTIONS);
        return;
    }
    f->act[f->n++] = strdup(action);
}

// Serves both `new IntentFilter()` and `new IntentFilter(action)` — the one-arg
// form is just the empty one with its first addAction already done.
static klj_val klj_IntentFilter_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)clazz;
    void *obj = klj_new_object_data("android/content/IntentFilter",
                                    calloc(1, sizeof(klj_intent_filter)));
    klj_own(obj, klj_intent_filter_free);
    if (n > 0) klj_if_add(obj, klj_str(a[0].l));
    return (klj_val){.l = obj};
}

static klj_val klj_IntentFilter_addAction(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_if_add(self, n > 0 ? klj_str(a[0].l) : NULL);
    return (klj_val){0};
}

static klj_val klj_Context_registerReceiver(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    klj_object        *fo = n > 1 ? klj_as_object(a[1].l) : NULL;
    klj_intent_filter *f  = fo ? fo->data : NULL;

    int sticky_battery = 0;
    for (int i = 0; f && i < f->n; i++)
        if (f->act[i] && strcmp(f->act[i], KLJ_ACTION_BATTERY) == 0) sticky_battery = 1;

    if (!sticky_battery) {
        // Registration for a non-sticky filter. It must not FAIL — the caller
        // treats registering as unconditional — and null is the correct Android
        // answer, not a stub: there is no sticky value to return. Named once,
        // because this list is the only statement of what the guest expected to
        // hear about, and nothing here can ever deliver it.
        static int said;
        if (!said++) {
            char buf[256]; buf[0] = 0;
            for (int i = 0; f && i < f->n; i++) {
                if (buf[0]) strlcat(buf, " ", sizeof buf);
                strlcat(buf, f->act[i] ? f->act[i] : "(null)", sizeof buf);
            }
            KLJ_LOG("Context.registerReceiver for [%s] — accepted, but nothing "
                    "here broadcasts, so no onReceive can ever arrive", buf);
        }
        return (klj_val){.l = NULL};
    }

    void *obj = kl_jni_new_object("android/content/Intent");
    klj_as_object(obj)->data = strdup(KLJ_ACTION_BATTERY);
    klj_own(obj, free);
    return (klj_val){.l = obj};
}

// Intent.getIntExtra(key, default). Only the battery Intent above carries any,
// and only the three libunity actually reads; everything else gets the caller's
// own default, which is exactly what Android returns for an absent extra.
static klj_val klj_Intent_getIntExtra(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_object *o      = klj_as_object(self);
    const char *action = o ? o->data : NULL;
    const char *key    = n > 0 ? klj_str(a[0].l) : NULL;
    int         dflt   = n > 1 ? (int)(int32_t)a[1].j : -1;

    if (!action || !key || strcmp(action, KLJ_ACTION_BATTERY) != 0)
        return (klj_val){.j = (uint32_t)dflt};

    // level/scale rather than a percentage: the caller divides one by the
    // other, so the pair has to be internally consistent and the seam's 0..100
    // is already a percentage — scale 100 makes level its own numerator.
    if (strcmp(key, "level") == 0) return (klj_val){.j = (uint32_t)kl_ovrp_battery_level()};
    if (strcmp(key, "scale") == 0) return (klj_val){.j = 100};
    if (strcmp(key, "status") == 0) {
        // BatteryManager.BATTERY_STATUS_*: CHARGING 2, DISCHARGING 3, FULL 5.
        // FULL at 100% is the documented mapping, and Unity's own
        // SystemInfo.batteryStatus distinguishes it from Charging.
        int charging = kl_ovrp_battery_charging();
        int level    = kl_ovrp_battery_level();
        return (klj_val){.j = (uint32_t)(charging ? (level >= 100 ? 5 : 2) : 3)};
    }
    KLJ_LOG("Intent.getIntExtra(\"%s\") on the battery Intent — not one of "
            "level/scale/status, answering the caller's default %d", key, dflt);
    return (klj_val){.j = (uint32_t)dflt};
}

static klj_val klj_Intent_getAction(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    const char *action = o ? o->data : NULL;
    return (klj_val){.l = action ? kl_jni_new_string(action) : NULL};
}

// The other two extra getters, and the answer is the same one getIntExtra gives
// for a key it does not carry: the caller's own default, and null for a String.
// That is not a shrug — the LAUNCH Intent here genuinely carries no extras.
// Extras arrive from a launcher shortcut, from another app, or from `adb
// shell am start -e`, and this process was started by none of those; there is
// no Android launcher in it to attach any.
//
// Open Brush reads three (Config.cs, the UNITY_ANDROID arm): EnableMonoscopicMode
// and DisableXrMode, both defaulting false, and OpenBrushArgs, a command line.
// Answering the defaults is what selects the ordinary stereo XR path — inventing
// either boolean would silently turn the headset off, and inventing an argument
// string would hand the guest a command line nobody typed.
static klj_val klj_Intent_getBooleanExtra(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    return (klj_val){.j = n > 1 ? (a[1].j & 1) : 0};
}
static klj_val klj_Intent_getStringExtra(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = NULL};
}

// Intent.getComponent() — for the launch Intent, the activity that was started.
// Non-null, because this activity was started EXPLICITLY: the manifest names one
// launcher activity and kl_jni_activity() instantiates that exact class, so
// there is a component and we know its name. Null is Android's answer for an
// IMPLICIT intent resolved by action, which this is not.
//
// It is read to detect an activity-ALIAS launch: Open Brush declares
// MonoscopicModeActivity and DisableXrModeActivity as aliases onto the same
// activity, and a launch through one of those is how a user asks for flat mode.
// getComponent() returning the alias is the only way that choice is visible,
// which is precisely why answering the real class is the right answer here —
// nobody picked an alias, and the guest's own suffix test then says so.
static klj_val klj_Intent_getComponent(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *component;
    return klj_singleton("android/content/ComponentName", &component);
}
// ComponentName.getClassName() is the DOTTED binary name, like Class.getName().
static klj_val klj_ComponentName_getClassName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    char dotted[256];
    const char *internal = klj_activity_class();
    size_t i = 0;
    for (; internal[i] && i < sizeof dotted - 1; i++)
        dotted[i] = internal[i] == '/' ? '.' : internal[i];
    dotted[i] = 0;
    return (klj_val){.l = kl_jni_new_string(dotted)};
}
static klj_val klj_ComponentName_getPackageName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string(klj_guest_package())};
}
// ---- WebView, and what it honestly is here ----------------------------------
//
// The VR client's in-headset UI is an Android WebView rendered to a texture:
// it constructs one, sizes it, draws it into a Canvas backed by a Bitmap, and
// copies that Bitmap's pixels into a direct ByteBuffer it uploads as a panel.
// There is no browser in this process and no plan to embed one, so the answer
// is the platform-absent answer the rest of the shim already gives: the object
// exists, every call is accepted, and it draws NOTHING — the guest's pixel
// buffer comes back exactly as it went in, which is a transparent panel.
//
// That is a deliberate cosmetic gap and not a fabrication: it grants nothing,
// asserts nothing about content, and the video panel is a different surface
// (SVLDecoder -> AImageReader -> EGLImage). If the UI ever becomes the thing
// under test, this is the seam to grow — a real WKWebView drawn into the same
// buffer would slot in here without the guest noticing.
//
// Loading is a different question from rendering, though, and it is one we can
// answer honestly. Every URL this guest loads is `file:///android_asset/...`,
// i.e. a file inside the APK we already serve through AssetManager.open(); so
// "did the document load" is a stat() away, and getProgress() below reports it
// rather than guessing. See that function for why the distinction is
// load-bearing.
static int g_webview_draws;

// Per-WebView state. There are three of them (streampreflight, streamloading,
// streamanimation) and they load different documents, so this hangs off the
// instance rather than the class — which is why <init> now mints a real object
// instead of handing back the jclass. The guest NewGlobalRef's what it gets
// (libvrlink_scene+0x149848), so it survives the local frame.

 klj_webdoc *klj_webdoc_of(void *self) {
    klj_object *o = klj_as_object(self);
    if (!o || !o->cls || strcmp(o->cls, "android/webkit/WebView") != 0) return NULL;
    return o->data;
}

static klj_val klj_WebView_init(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    void *obj = klj_new_object_data("android/webkit/WebView", calloc(1, sizeof(klj_webdoc)));
    KLJ_LOG("WebView.<init> — no browser is embedded; this panel draws nothing");
    return (klj_val){.l = obj};
}

// loadUrl: resolve it against the same assets root AssetManager.open() uses and
// record whether the document is really there. Nothing is parsed and nothing is
// rendered — this is the fetch, and only the fetch.
static klj_val klj_WebView_loadUrl(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    const char *u = n > 0 ? klj_str(a[0].l) : NULL;
    klj_webdoc *d = klj_webdoc_of(self);
    if (!d || !u) {
        KLJ_LOG("WebView.loadUrl(\"%s\") — accepted and dropped", u ? u : "(null)");
        return (klj_val){.j = 0};
    }
    static const char kAsset[] = "file:///android_asset/";
    free(d->url); free(d->path);
    d->url = strdup(u); d->path = NULL; d->found = 0; d->logged = 0;
    if (strncmp(u, kAsset, sizeof kAsset - 1) == 0) {
        const char *rel = u + sizeof kAsset - 1;
        size_t need = strlen(g_assets_dir) + strlen(rel) + 2;
        d->path = malloc(need);
        snprintf(d->path, need, "%s/%s", g_assets_dir, rel);
        struct stat st;
        d->found = (stat(d->path, &st) == 0 && S_ISREG(st.st_mode));
    }
    KLJ_LOG("WebView.loadUrl(\"%s\") — %s", u,
            d->found  ? "document found; nothing renders it"
          : d->path   ? "NOT FOUND under the assets root"
                      : "not an asset URL; accepted and dropped");
    return (klj_val){.j = 0};
}
static klj_val klj_WebView_getSettings(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *settings;
    return klj_singleton("android/webkit/WebSettings", &settings);
}
// draw(Canvas): the one call that would produce pixels. It does not, and it
// says so once rather than every frame.
static klj_val klj_WebView_draw(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    if (!g_webview_draws++)
        KLJ_LOG("WebView.draw — nothing to draw; the panel stays transparent");
    return (klj_val){.j = 0};
}
// ByteBuffer.rewind() — the second half of the panel readback:
// bitmap.copyPixelsToBuffer(buf) leaves the buffer's position at the end, and
// the guest rewinds it before reading. A Buffer here is an address and a
// capacity with no position (klj_direct_buffer), and everything that reads one
// does so from its base, so rewinding is already the state it is in. Returning
// the same buffer is what Buffer.rewind() does.
static klj_val klj_Buffer_rewind(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    return (klj_val){.l = self};
}
// klj_void_noop is further down — it is the shared void handler, and these
// bindings use it rather than adding a second one.
static klj_val klj_false(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}
// Bitmap/Canvas: handles with no backing store, because nothing ever writes to
// them. copyPixelsToBuffer therefore leaves the guest's buffer untouched —
// which is the correct consequence of a WebView that drew nothing, not a
// separate decision.
static klj_val klj_Bitmap_createBitmap(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("Bitmap.createBitmap(%d, %d) — handle only, no pixel store",
            n > 0 ? (int)a[0].j : 0, n > 1 ? (int)a[1].j : 0);
    static void *bmp;
    return klj_singleton("android/graphics/Bitmap", &bmp);
}
static klj_val klj_BitmapConfig_valueOf(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *cfg;
    return klj_singleton("android/graphics/Bitmap$Config", &cfg);
}
static klj_val klj_Canvas_init(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    return (klj_val){.l = self};
}

static klj_val klj_Intent_setPackage(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    const char *p = n > 0 ? klj_str(a[0].l) : NULL;
    KLJ_LOG("Intent.setPackage(\"%s\")", p ? p : "(null)");
    return (klj_val){.l = self};
}
static klj_val klj_Intent_addFlags(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    KLJ_LOG("Intent.addFlags(0x%llx)", n > 0 ? (unsigned long long)a[0].j : 0ULL);
    return (klj_val){.l = self};
}
// A minimal java/util/List. Fixed contents — nothing mutates one of ours.

 void *klj_new_list(void **items, int count) {
    klj_list *l = calloc(1, sizeof *l);
    l->items = items;
    l->count = count;
    void *obj = kl_jni_new_object("java/util/List");
    klj_as_object(obj)->data = l;
    return obj;
}
 klj_val klj_List_size(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    klj_list   *l = o ? o->data : NULL;
    return (klj_val){.j = l ? (uint64_t)l->count : 0};
}
static klj_val klj_List_isEmpty(void *env, void *self, const klj_val *a, int n) {
    return (klj_val){.j = klj_List_size(env, self, a, n).j == 0};
}
static klj_val klj_List_get(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_object *o = klj_as_object(self);
    klj_list   *l = o ? o->data : NULL;
    int         i = n > 0 ? (int)a[0].j : -1;
    if (!l || i < 0 || i >= l->count) return (klj_val){.l = NULL};
    return (klj_val){.l = l->items[i]};
}

// Unity asks whether its *own* activity is registered under the VR category —
// ACTION_MAIN + com.oculus.intent.category.VR, setPackage(our package). Our
// AndroidManifest.xml does declare exactly that on UnityPlayerActivity, so the
// truthful answer is one match. An empty list would read as "this is not a VR
// app" and is the kind of convenient lie that disables the path under test.
 klj_val klj_PM_queryIntentActivities(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void  *ri;
    static void  *items[1];
    static void  *list;
    if (!list) {
        ri       = kl_jni_new_object("android/content/pm/ResolveInfo");
        items[0] = ri;
        list     = klj_new_list(items, 1);
    }
    return (klj_val){.l = list};
}

// Object.getClass() is GetObjectClass reached the other way round — the guest
// calls it as a Java method on classes it has no jclass for yet.
static klj_val klj_Object_getClass(void *env, void *self, const klj_val *a, int n) {
    (void)a; (void)n;
    return (klj_val){.l = klj_GetObjectClass(env, self)};
}

// A constructor whose only job is to produce an identity. `self` is the jclass
// NewObject was given, so this stays correct for any class that needs no state —
// unlike the hardcoded class name a per-type <init> would use.
static klj_val klj_generic_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_object(klj_class_name(clazz))};
}

// Unity's JNIBridge builds a java.lang.reflect.Proxy implementing the given
// interfaces, backed by a native pointer; when Android later calls a method on
// it, JNIBridge.invoke(ptr, Class, Method, Object[]) forwards into native code.
// We are the Java side, so the proxy is just an object remembering the pointer.
// Nothing calls back into it until we start synthesising Android events, but the
// interface list is worth logging — it names every callback the engine expects.
// `disabled` is the guest telling us the native object behind this proxy is
// gone. It is not bookkeeping: klj_proxy_invoke calls THROUGH native_ptr, so
// invoking a disabled proxy is a call into freed guest memory, and the callback
// that would do it (a Choreographer frame, a posted Runnable) can be sitting in
// a queue at the moment the guest disables it.

static klj_val klj_JNIBridge_newInterfaceProxy(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)clazz;
    klj_proxy *p = calloc(1, sizeof *p);
    p->native_ptr = n > 0 ? (int64_t)a[0].j : 0;
    p->classes    = n > 1 ? a[1].l : NULL;

    klj_array *ifaces = p->classes ? klj_arr(p->classes) : NULL;
    if (ifaces && ifaces->kind == 'L')
        for (int i = 0; i < ifaces->len; i++)
            KLJ_LOG("newInterfaceProxy: implements %s (native 0x%llx)",
                    klj_class_name(((void **)ifaces->data)[i]),
                    (unsigned long long)p->native_ptr);

    void *obj = kl_jni_new_object("bitter/jnibridge/JNIBridge$Proxy");
    klj_as_object(obj)->data = p;
    klj_as_object(obj)->pinned = 1;   // guest-held long-term via native_ptr
    return (klj_val){.l = obj};
}

// ...and the other end of it. Unity calls this when the native object behind a
// proxy is destroyed; every later call on that proxy must become a no-op rather
// than a call through a dangling pointer.
static klj_val klj_JNIBridge_disableInterfaceProxy(void *env, void *clazz,
                                                   const klj_val *a, int n) {
    (void)env; (void)clazz;
    klj_object *o = n > 0 ? klj_as_object(a[0].l) : NULL;
    klj_proxy  *p = (o && strcmp(o->cls, KLJ_CLASS_PROXY) == 0) ? o->data : NULL;
    if (p) {
        p->disabled = 1;
        KLJ_LOG("disableInterfaceProxy: proxy 0x%llx is dead",
                (unsigned long long)p->native_ptr);
    } else {
        KLJ_LOG("disableInterfaceProxy: %s is not a proxy — ignored",
                o ? o->cls : "(not an object)");
    }
    return (klj_val){0};
}

static klj_val klj_Context_getPackageManager(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *pm;
    return klj_singleton("android/content/pm/PackageManager", &pm);
}
// The APK's real package name, from AndroidManifest.xml. Unity uses it to look
// itself up through the PackageManager and to derive storage paths.
static klj_val klj_Context_getPackageName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string(klj_guest_package())};
}
// ---- WifiManager, for the VR half ----
//
// The 2D shell reads Wi-Fi through its own ShellWifiInfo (see the field table
// above, and the settled answer there: we model no WifiManager, so
// getConnectionInfo() is null, which is Android's own answer on a device not
// associated with a network). The VR half asks the framework directly, and gets
// the same answer for the same reason — claiming a network would be reporting a
// signal strength we have not measured.
static klj_val klj_WifiManager_getConnectionInfo(void *env, void *self,
                                                 const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("WifiManager.getConnectionInfo() -> null (not associated)");
    return (klj_val){.l = NULL};
}

// ConnectivityManager.getActiveNetwork() — and note what the guest does with it,
// because the name of its caller is misleading. BIsWiFiConnected()
// (libvrlink_scene+0x146130) is literally `return getActiveNetwork() != null`:
// it never asks which transport, so there is no Wi-Fi claim in it to get wrong.
// The question it really asks is "is this device on a network at all", and the
// answer is yes — the guest is streaming from a Steam host over it as it asks.
// Answering null would say the machine is offline while its own socket is
// connected, and the stream scene reads that as a reason there is no video.
static klj_val klj_ConnectivityManager_getActiveNetwork(void *env, void *self,
                                                        const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *net;
    return klj_singleton("android/net/Network", &net);
}

// ...and the older spelling of the same question, which is what a Unity 2018
// title reaches for. SUPERHOT asks it before it tries to sync achievements, and
// the honest answer is the one above's: this device is on a network. Note that
// this is a statement about the DEVICE, not about the Oculus platform — the
// achievement calls behind it still fail on their own terms, by their own
// request path, which is where that absence belongs.
//
// The NetworkInfo it hands back answers only what the guest actually asks of it;
// anything else stops the run by name, as everywhere else here.
static klj_val klj_ConnectivityManager_getActiveNetworkInfo(void *env, void *self,
                                                            const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *info;
    return klj_singleton("android/net/NetworkInfo", &info);
}

// A network that exists is connected: there is no "connecting" state to model
// here, because nothing about this host's networking is asynchronous from the
// guest's point of view. isAvailable is the same answer for the same reason.
static klj_val klj_NetworkInfo_isConnected(void *env, void *self,
                                           const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("NetworkInfo.isConnected() -> true");
    return (klj_val){.j = 1};
}

// ConnectivityManager.TYPE_WIFI. The pair isConnected()+getType() is the
// standard "is this a link I may use freely, or is it someone's mobile data"
// test, and the coherent answer is the one the rest of this file already
// gives: we present a Quest 2 (Build.MODEL, settled), a Quest 2 has no cellular
// radio at all, so WIFI is the only transport the device we describe can have.
// Answering MOBILE would claim hardware that device does not have; answering
// NONE would contradict isConnected() one line above.
//
// This does not reopen "we model no WifiManager" (getConnectionInfo -> null):
// that refuses to invent an SSID and a signal strength, which are measurements.
// Which KIND of link this is follows from the device we are already claiming to
// be, and the guest asks it as a policy question rather than a measurement.
#define KLJ_CONNECTIVITY_TYPE_WIFI 1
static klj_val klj_NetworkInfo_getType(void *env, void *self,
                                       const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("NetworkInfo.getType() -> TYPE_WIFI");
    return (klj_val){.j = KLJ_CONNECTIVITY_TYPE_WIFI};
}

// ---- NetworkCapabilities: the modern spelling of the same two answers -------
//
// getActiveNetworkInfo (above) is the Unity-2018 way to ask; a Unity 2022 title
// asks getNetworkCapabilities instead, and VRChat does. These MUST agree: a
// device that is on Wi-Fi through one API and on nothing through the other is a
// contradiction the guest is entitled to notice, and this file already answers
// the question once — connected, and Wi-Fi, because we present a Quest 2 and a
// Quest 2 has no cellular radio (see klj_NetworkInfo_getType for why that is a
// policy answer rather than an invented measurement).
//
// So this is a group, for the reason the display panel and the audio device are
// groups: the members cross-check each other, and answering them one at a time
// as each is forced is how they end up disagreeing.
static klj_val klj_ConnectivityManager_getNetworkCapabilities(void *env, void *self,
                                                              const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *caps;
    return klj_singleton("android/net/NetworkCapabilities", &caps);
}

static klj_val klj_NetworkCapabilities_hasTransport(void *env, void *self,
                                                    const klj_val *a, int n) {
    (void)env; (void)self;
    int t   = n > 0 ? (int)(int32_t)a[0].j : -1;
    int yes = (t == KLJ_NC_TRANSPORT_WIFI);
    KLJ_LOG("NetworkCapabilities.hasTransport(%d) -> %s", t, yes ? "true" : "false");
    return (klj_val){.j = (uint64_t)yes};
}

// The positive capabilities are the two that mean "this link works", and the
// NOT_* family are negative assertions about problems this host does not have —
// there is no metering, no roaming, no VPN and no captive portal on a desktop
// Ethernet/Wi-Fi link, so denying them would describe a worse network than the
// one the guest is demonstrably talking to us over. Anything outside the set is
// NAMED once and answered false, because a blanket true here would claim
// capabilities we have not thought about.
static klj_val klj_NetworkCapabilities_hasCapability(void *env, void *self,
                                                     const klj_val *a, int n) {
    (void)env; (void)self;
    int c = n > 0 ? (int)(int32_t)a[0].j : -1;
    int yes;
    switch (c) {
    case KLJ_NC_CAP_INTERNET:      case KLJ_NC_CAP_VALIDATED:
    case KLJ_NC_CAP_NOT_METERED:   case KLJ_NC_CAP_NOT_RESTRICTED:
    case KLJ_NC_CAP_TRUSTED:       case KLJ_NC_CAP_NOT_VPN:
    case KLJ_NC_CAP_NOT_ROAMING:   case KLJ_NC_CAP_NOT_CONGESTED:
    case KLJ_NC_CAP_NOT_SUSPENDED: yes = 1; break;
    default: {
        static int said;
        if (!said++)
            KLJ_LOG("NetworkCapabilities.hasCapability(%d) — outside the set this "
                    "host models; answering false. If a guest depends on it, "
                    "this line is where the work starts", c);
        yes = 0;
        break;
    }
    }
    KLJ_LOG("NetworkCapabilities.hasCapability(%d) -> %s", c, yes ? "true" : "false");
    return (klj_val){.j = (uint64_t)yes};
}

// A WifiLock is not a claim about connectivity — it is a request to the power
// manager not to put the Wi-Fi radio to sleep. There is no radio here to put to
// sleep, so every guarantee the lock makes is already true and acquiring it is
// the work being *already done* rather than a stub standing in for it. That is
// why isHeld() answers true: the question is "did my acquire take effect", and
// it did, vacuously.
//
// The alternative was measured: answering false makes the app print
// "WiFi lock failed to acquire!" and carry on with a worse idea of its own
// network conditions than the truth warrants.
static klj_val klj_WifiManager_createWifiLock(void *env, void *self,
                                              const klj_val *a, int n) {
    (void)env; (void)self;
    const char *tag = n > 1 ? klj_str(a[1].l) : NULL;
    KLJ_LOG("WifiManager.createWifiLock(mode=%d, \"%s\")",
            n > 0 ? (int)a[0].j : 0, tag ? tag : "");
    static void *lock;
    return klj_singleton("android/net/wifi/WifiManager$WifiLock", &lock);
}
static klj_val klj_WifiLock_acquire(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){0};
}
static klj_val klj_WifiLock_isHeld(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 1};
}

// getSystemService returns the manager object for a service name. Returning null
// for an unknown one is legitimate Android — a device need not offer every
// service — so unknowns are logged rather than fabricated.
static klj_val klj_Context_getSystemService(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    static const struct { const char *svc, *cls; } services[] = {
        {"location",     "android/location/LocationManager"},
        {"audio",        "android/media/AudioManager"},
        {"window",       "android/view/WindowManager"},
        {"activity",     "android/app/ActivityManager"},
        {"sensor",       "android/hardware/SensorManager"},
        {"power",        "android/os/PowerManager"},
        {"vibrator",     "android/os/Vibrator"},
        {"connectivity", "android/net/ConnectivityManager"},
        {"wifi",         "android/net/wifi/WifiManager"},
        {"input",        "android/hardware/input/InputManager"},
        {"display",      "android/hardware/display/DisplayManager"},
        {"clipboard",    "android/content/ClipboardManager"},
        {"notification", "android/app/NotificationManager"},
        {"media_router", "android/media/MediaRouter"},
        {"batterymanager", "android/os/BatteryManager"},
        {NULL, NULL},
    };
    const char *want = n > 0 ? klj_str(a[0].l) : NULL;
    if (!want) return (klj_val){.l = NULL};
    // One instance per service, as Android does — callers compare identity.
    static void *cache[sizeof services / sizeof services[0]];
    for (unsigned i = 0; services[i].svc; i++) {
        if (strcmp(services[i].svc, want)) continue;
        if (!cache[i]) cache[i] = kl_jni_new_object(services[i].cls);
        KLJ_LOG("getSystemService(\"%s\") -> %s", want, services[i].cls);
        return (klj_val){.l = cache[i]};
    }
    KLJ_LOG("getSystemService(\"%s\") -> null (unknown service)", want);
    return (klj_val){.l = NULL};
}

// String.equals compares content, not identity — which matters, because our
// jstrings are freshly allocated per call and a constant read twice is not the
// same object. Anything comparing strings must come through here.
static klj_val klj_String_equals(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    const char *x = klj_str(self);
    const char *y = n > 0 ? klj_str(a[0].l) : NULL;
    return (klj_val){.j = (x && y && strcmp(x, y) == 0)};
}
static klj_val klj_String_length(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    const char *s = klj_str(self);
    return (klj_val){.j = s ? strlen(s) : 0};
}

// ---- storage ----
// Unity asks the Context where it may write: Application.persistentDataPath comes
// from getExternalFilesDir, and saves and player prefs land there. These must be
// real, writable directories — a stub path would make the first write fail deep
// inside the engine rather than here.

// Where the GUEST's own persistent state lives: saves, PlayerPrefs, Steam Link's
// pairing credentials. It used to be build/android-files, i.e. inside the
// directory `make clean` deletes — so clearing build artifacts silently cost a
// Beat Saber first-run setup and a Steam Link re-pairing. Guest state is not a
// build output and does not belong among them; ~/Library/Application Support is
// where macOS puts exactly this.
//
// Keyed on the GUEST, not on the APK, and that is the point rather than an
// approximation: swapping Beat Saber 1.28 for 1.6.0 is the case this exists to
// survive, so the two versions share one folder and neither run repeats first
// setup. Where two versions must NOT share — a save format that changed under
// them — KL_FILES_DIR overrides the path outright, which is also how a run gets
// a scratch profile without disturbing the real one.
//
// visionOS is unaffected: the app container is the only writable location there
// and kl_app.c passes it in explicitly, so this default is the host's.
const char *kl_userdata_dir(const char *guest) {
    char *env = kl_env_str("KL_FILES_DIR", NULL);
    if (env && *env) return klj_abspath(env);
    const char *home = getenv("HOME");
    size_t n = (home ? strlen(home) : 0) + strlen(guest) + 64;
    char *out = malloc(n);
    if (!out) return klj_abspath("userdata");
    // No HOME is not a case worth inventing a policy for, but it must not
    // produce a path at the filesystem root: fall back beside the build tree.
    if (home && *home)
        snprintf(out, n, "%s/Library/Application Support/Klepton/userdata/%s",
                 home, guest);
    else
        snprintf(out, n, "userdata/%s", guest);
    return klj_abspath(out);
}

// NULL until something asks or something sets it. Resolved lazily because
// kl_userdata_dir reads the environment, and a static initialiser cannot.
static const char *g_files_dir;
void kl_jni_set_files_dir(const char *dir) { g_files_dir = klj_abspath(dir); }
const char *kl_jni_files_dir(void) {
    // The DEFAULT target's key, not a literal: a driver that never called
    // kl_target_apply_host still gets the guest the rest of this file defaults
    // to, and the two cannot drift apart.
    if (!g_files_dir) g_files_dir = kl_userdata_dir(kl_target_default()->userdata);
    return g_files_dir;
}

// The APK itself. Unity opens this as a zip to read streaming assets, so it has
// to be a real file — the unpacked tree under beatsaber/ is not a substitute.
 const char *g_apk_path = "beatsaber.apk";
void kl_jni_set_apk_path(const char *path) { g_apk_path = klj_abspath(path); }
const char *kl_jni_apk_path(void) { return g_apk_path; }

static klj_val klj_Context_getPackageCodePath(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("getPackageCodePath() -> %s", g_apk_path);
    return (klj_val){.l = kl_jni_new_string(g_apk_path)};
}

// Where the guest's own .so files live. Unity reads this to dlopen further
// libraries, and our guest dlopen resolves against the same directory.
 const char *g_native_lib_dir = "beatsaber/lib/arm64-v8a";
void kl_jni_set_native_lib_dir(const char *dir) { g_native_lib_dir = klj_abspath(dir); }

// ApplicationInfo is a plain data holder — Unity reads its fields directly
// rather than calling accessors, so these are field getters, not methods.
 klj_val klj_appinfo_sourceDir(void)     { return (klj_val){.l = kl_jni_new_string(g_apk_path)}; }
 klj_val klj_appinfo_nativeLibraryDir(void) { return (klj_val){.l = kl_jni_new_string(g_native_lib_dir)}; }
 klj_val klj_appinfo_dataDir(void)       { return (klj_val){.l = kl_jni_new_string(kl_jni_files_dir())}; }
// This APK is not a split install, so there are no additional source dirs. An
// empty array says that unambiguously; Android would say null, and both read as
// "no splits" to a caller that checks length.
 klj_val klj_appinfo_splitSourceDirs(void) {
    static void *empty;
    if (!empty) empty = klj_new_array('L', "java/lang/String", 0);
    return (klj_val){.l = empty};
}

// ClassLoader.findLibrary(name) turns a System.loadLibrary() name into the
// absolute path of the .so inside the APK's native library directory, so a
// caller need not know the path layout. Null is Android's own answer for a
// library that is not there, so a miss needs nothing invented.
static klj_val klj_ClassLoader_findLibrary(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *name = n > 0 ? klj_str(a[0].l) : NULL;
    if (!name || !*name) return (klj_val){.l = NULL};
    char   path[1024];
    size_t len = strlen(name);
    if (strncmp(name, "lib", 3) == 0 && len > 3 && strcmp(name + len - 3, ".so") == 0)
        snprintf(path, sizeof path, "%s/%s", g_native_lib_dir, name);
    else
        snprintf(path, sizeof path, "%s/lib%s.so", g_native_lib_dir, name);
    // kl_can_dlopen, not stat and not kl_can_load: the question Unity is really
    // asking is "would the dlopen you are about to make succeed", and on this
    // platform a library can be loadable without being a file. Two ways that
    // happens, and each one cost a device run to learn:
    //
    //   - klepton-ld translations are embedded in the bundle and the ELF tree is
    //     not, so stat'ing the .so answers "absent" for a library that loads
    //     fine. That killed P5.4's first device lifecycle run: findLibrary
    //     ("il2cpp") returned null, Unity never attempted the dlopen, and the
    //     symptom was "Failed to load Il2CPP." nowhere near the stat.
    //   - synthetic libraries (OVRPlugin, the platform loader, GLES, OpenSL ES)
    //     have no file at all, anywhere. On the host the APK's own unused copy
    //     happened to sit on disk and hid this; in the bundle nothing does.
    //     findLibrary("OVRPlugin") -> null is what black-screened the device —
    //     see kl_can_dlopen in kl_dl.c for the whole chain.
    //
    // The answer stays the .so path: the guest hands it straight back to dlopen,
    // where kl_load_auto or the serving gateway resolves it, so there is one
    // resolver and not two.
    int found = kl_can_dlopen(path);
    KLJ_LOG("ClassLoader.findLibrary(\"%s\") -> %s", name, found ? path : "null");
    return (klj_val){.l = found ? kl_jni_new_string(path) : NULL};
}

// getObbDir() and getObbDirs() are ONE answer asked two ways — Android's plural
// form is the singular one followed by any adopted external volumes, and there
// are none here. They used to disagree: the plural returned an empty array
// under a comment asserting "this APK carries its assets inline", which was
// true of Beat Saber 1.28 and is FALSE of 1.40. 1.40 is a split application
// binary (assets/unity_obb_guid marks it), its data ships in
// main.<versionCode>.<package>.obb, and Unity looks for it through the PLURAL
// form — so an empty list reads as "this device has no OBB storage" and the
// asset pack is never found whatever getObbDir() says.
//
// The directory is created rather than merely named. Both callers are asking
// where to LOOK, and a path that does not exist is indistinguishable from one
// with no OBB in it, so creating it turns "somebody still has to make this
// directory" into "the file goes here".

// ...and the directory is READ once, here, for the one thing about it that
// nothing downstream can report: whether the OBB in it is the one the guest is
// about to ask for. The guest builds `main.<versionCode>.<package>.obb` itself
// out of the number klj_guest_version answers, so a wrong version code and a
// missing file are the same event from in here — Unity simply finds no asset
// pack, and what surfaces is `Unable to start Oculus XR Plugin` (the XR
// subsystem descriptors ship in the OBB, under bin/Data/UnitySubsystems/) and
// Addressables' `No Location found for Key=AppInit`, several layers away and
// naming neither the version nor the file.
//
// That is exactly what a device run did: nothing staged apktool.yml beside the
// staged assets, klj_guest_version fell back to 1.28's 545, and the 1.3 GB of
// 1.40 data sitting right here under main.1716... was never opened. So the
// mismatch is named, by both numbers, at the moment we hand the path over.
static void klj_obb_census(const char *dir) {
    long code = 0;
    klj_guest_version(&code, NULL);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    int match = 0, others = 0;
    char first_other[256] = {0};
    // ...and how big the matching one actually is. A NAME is not a file: the
    // host stages this directory as SYMLINKS into bonelab_obb/ (6.8 GB is not
    // worth duplicating), `devicectl device copy to` copies the links rather
    // than what they point at, and the device then holds two 174-byte files with
    // exactly the right names. Every check that reads the name passed, and the
    // failure surfaced a thousand lines later as Unity's `Application OBB has
    // mismatching GUID ... got ''` — which reads as a wrong OBB rather than an
    // empty one, and names nothing that would lead back to staging.
    //
    // Stat, not open: a dangling symlink fails here, which is the device case,
    // and a short-but-real file is the one worth printing a size for.
    long long bytes = -1;
    while ((e = readdir(d)) != NULL) {
        long got;
        if (sscanf(e->d_name, "main.%ld.", &got) != 1) continue;
        if (got == code) {
            char p[1024];
            struct stat st;
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            bytes = stat(p, &st) == 0 ? (long long)st.st_size : -1;
            match = 1;
            continue;
        }
        others++;
        if (!*first_other) snprintf(first_other, sizeof first_other, "%s", e->d_name);
    }
    closedir(d);
    if (match) {
        // 1 MB is not a threshold anyone has to tune: the smallest real OBB in
        // this project is hundreds of megabytes, and the failures this catches
        // are a dangling link (no size at all) and a copied link (~175 bytes).
        if (bytes < 0 || bytes < (1 << 20))
            KLJ_LOG("obb: main.%ld.*.obb in %s is %lld bytes — that is NOT the "
                    "archive. A staged SYMLINK copies as a link, so the name is "
                    "right and the data is absent; this run has no game content "
                    "and Unity will say the OBB's GUID is ''. Re-stage the obb "
                    "directory with the links RESOLVED",
                    code, dir, bytes);
        else
            KLJ_LOG("obb: main.%ld.*.obb is present in %s (%lld bytes)",
                    code, dir, bytes);
        return;
    }
    // No OBB at all is not an error here: 1.28 and Steam Link have none.
    if (!others) return;
    KLJ_LOG("obb: %s holds %s but this guest is versionCode %ld, so it will look "
            "for main.%ld.*.obb and find nothing. That is a MISSING GAME DATA "
            "run — check that apktool.yml was staged beside assets/",
            dir, first_other, code, code);
}

// Where the OBB is, as the TARGET states it — not "<files>/obb" spelled here.
//
// Unity asks Java for this (Context.getObbDirs()), so for five of the seven
// guests the two are the same string and always were. UE4 does not ask anyone:
// it builds Android's own <external>/Android/obb/<package>/ path itself, so a
// literal here would have been right for the guests that call this function and
// silently wrong for the one that never does — including for the census, which
// is the only instrument that says whether the game data is present at all.
const char *kl_jni_obb_dir(void) {
    static char path[1024];
    if (!*path) {
        const kl_target *t = kl_target_resolve(NULL);
        const char *rel = t && t->obb && *t->obb ? t->obb : "obb";
        snprintf(path, sizeof path, "%s/%s", kl_jni_files_dir(), rel);
        klj_mkdir_p(path);
        klj_obb_census(path);
    }
    return path;
}
static const char *klj_obb_dir(void) { return kl_jni_obb_dir(); }
static klj_val klj_Context_getObbDirs(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *dirs;
    if (!dirs) {
        dirs = klj_new_array('L', "java/io/File", 1);
        ((void **)klj_arr(dirs)->data)[0] = klj_new_file(klj_obb_dir());
    }
    return (klj_val){.l = dirs};
}
static klj_val klj_Context_getObbDir(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_file(klj_obb_dir())};
}

const klj_binding klj_bind_android[] = {
    // ICU's time zones, which is where Mono's TimeZoneInfo looks on Android.
    {"android/icu/util/TimeZone", "getDefault", "()Ljava/lang/Object;", klj_TimeZone_getDefault},
    {"android/icu/util/TimeZone", "getID", "()Ljava/lang/String;", klj_TimeZone_getID},
    {"android/icu/util/TimeZone", "getAvailableIDs", "()[Ljava/lang/String;", klj_TimeZone_getAvailableIDs},
    {"android/icu/util/TimeZone", "getOffset", "(J)I", klj_TimeZone_getOffset},
    {"android/icu/util/TimeZone", "getRawOffset", "()I", klj_TimeZone_getRawOffset},
    {"android/icu/util/TimeZone", "useDaylightTime", "()Z", klj_TimeZone_useDaylightTime},
    // ICU's own name for the same question. It differs from useDaylightTime()
    // only for a zone that USED to observe DST and no longer does, which this
    // one-zone database cannot express and the host's tm_gmtoff cannot
    // distinguish — so they share an answer rather than one of them guessing.
    {"android/icu/util/TimeZone", "observesDaylightTime", "()Z", klj_TimeZone_useDaylightTime},
    {"android/icu/util/TimeZone", "getDisplayName", "(ZI)Ljava/lang/String;", klj_TimeZone_getDisplayName},
    {"android/icu/util/TimeZone", "getDSTSavings", "()I", klj_TimeZone_getDSTSavings},
    {"android/icu/util/TimeZone", "getTimeZone", "(Ljava/lang/String;)Ljava/lang/Object;", klj_TimeZone_getTimeZone},
    // The class name really does carry DOTS rather than slashes — that is the
    // guest's own string, built by C# from a type name, and a binding is
    // matched on it exactly. Same shape as UnityPermissions below.
    {"android/text/format/DateFormat", "is24HourFormat", "(Landroid/content/Context;)Z", klj_DateFormat_is24HourFormat},
    {"android/app/ActivityManager", "getMemoryInfo", "(Landroid/app/ActivityManager$MemoryInfo;)V", klj_ActivityManager_getMemoryInfo},
    // VRChat's own plugin class; see the transcription note at its implementation.
    {"com/vrchat/android/plugin/Info", "instance", "()Ljava/lang/Object;", klj_VRCInfo_instance},
    {"com/vrchat/android/plugin/Info", "getVersionCode", "()I", klj_VRCInfo_getVersionCode},
    {"com/vrchat/android/plugin/Info", "getVersionName", "()Ljava/lang/String;", klj_VRCInfo_getVersionName},
    {"android/media/AudioManager", "isStreamMute", "(I)Z", klj_false},
    {"android/media/AudioManager", "isMicrophoneMute", "()Z", klj_false},
    {"java/lang/ClassLoader", "findLibrary", "(Ljava/lang/String;)Ljava/lang/String;", klj_ClassLoader_findLibrary},

    {"android/app/Activity",   "getIntent",  "()Landroid/content/Intent;", klj_Activity_getIntent},
    {"android/app/Activity",   "getApplicationContext", "()Landroid/content/Context;",
     klj_Activity_getApplicationContext},
    {"android/util/DisplayMetrics", "<init>", "()V", klj_generic_init},
    // Unity's ReflectionHelper spells every reference return as Object, so it asks
    // for getClass with a signature real Java does not declare. Matching is on the
    // exact string, so both spellings are registered; the ()Ljava/lang/Class; one
    // is further down with the rest of the Object methods.
    {"java/lang/Object", "getClass", "()Ljava/lang/Object;", klj_Object_getClass},
    {"android/content/Intent", "getExtras",  "()Landroid/os/Bundle;",      klj_Intent_getExtras},
    {"android/content/Intent", "<init>",     "(Ljava/lang/String;)V",      klj_Intent_init},
    {"android/content/Intent", "addCategory", "(Ljava/lang/String;)Landroid/content/Intent;", klj_Intent_addCategory},
    {"android/content/Intent", "getIntExtra", "(Ljava/lang/String;I)I",     klj_Intent_getIntExtra},
    {"android/content/Intent", "getBooleanExtra", "(Ljava/lang/String;Z)Z", klj_Intent_getBooleanExtra},
    {"android/content/Intent", "getStringExtra",  "(Ljava/lang/String;)Ljava/lang/String;",
     klj_Intent_getStringExtra},
    {"android/content/Intent", "getComponent",    "()Landroid/content/ComponentName;",
     klj_Intent_getComponent},
    {"android/content/ComponentName", "getClassName",   "()Ljava/lang/String;",
     klj_ComponentName_getClassName},
    {"android/content/ComponentName", "getPackageName", "()Ljava/lang/String;",
     klj_ComponentName_getPackageName},
    {"android/content/Intent", "getAction",   "()Ljava/lang/String;",       klj_Intent_getAction},
    {"android/content/IntentFilter", "<init>",    "()V",                     klj_IntentFilter_init},
    {"android/content/IntentFilter", "<init>",    "(Ljava/lang/String;)V",   klj_IntentFilter_init},
    {"android/content/IntentFilter", "addAction", "(Ljava/lang/String;)V",   klj_IntentFilter_addAction},
    // registerReceiver is bound on Context, which every activity class here
    // reaches through g_supers. The three-arg form takes API 26+ flags that do
    // not change what a sticky query answers, so it shares the handler.
    {"android/content/Context", "registerReceiver",
     "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;",
     klj_Context_registerReceiver},
    {"android/content/Context", "registerReceiver",
     "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;I)Landroid/content/Intent;",
     klj_Context_registerReceiver},
    // The in-headset UI panel — see the WebView block above for why every one
    // of these is accepted and none of them produces a pixel.
    {"android/webkit/WebView", "<init>", "(Landroid/content/Context;)V", klj_WebView_init},
    {"android/webkit/WebView", "loadUrl", "(Ljava/lang/String;)V", klj_WebView_loadUrl},
    {"android/webkit/WebView", "getSettings", "()Landroid/webkit/WebSettings;", klj_WebView_getSettings},
    {"android/webkit/WebView", "draw", "(Landroid/graphics/Canvas;)V", klj_WebView_draw},
    {"android/webkit/WebView", "dispatchTouchEvent", "(Landroid/view/MotionEvent;)Z", klj_false},
    {"android/graphics/Bitmap", "createBitmap",
     "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;", klj_Bitmap_createBitmap},
    {"android/graphics/Bitmap$Config", "valueOf",
     "(Ljava/lang/String;)Landroid/graphics/Bitmap$Config;", klj_BitmapConfig_valueOf},
    {"java/nio/ByteBuffer", "rewind", "()Ljava/nio/Buffer;", klj_Buffer_rewind},
    {"android/graphics/Canvas", "<init>", "(Landroid/graphics/Bitmap;)V", klj_Canvas_init},

    // The view hierarchy the WebView is hung in. Three classes, and the guest
    // never reads anything back out of them — it builds the tree, hands it to
    // the Activity, and from then on only ever calls draw(). So a constructor
    // that returns the object it was given is the whole of it.
    {"android/widget/RelativeLayout", "<init>", "(Landroid/content/Context;)V", klj_Canvas_init},
    {"android/widget/RelativeLayout$LayoutParams", "<init>", "(II)V", klj_Canvas_init},

    {"android/content/Intent", "setPackage",  "(Ljava/lang/String;)Landroid/content/Intent;", klj_Intent_setPackage},
    {"android/content/Intent", "addFlags",    "(I)Landroid/content/Intent;",                 klj_Intent_addFlags},
    {"android/content/Context", "getPackageManager", "()Landroid/content/pm/PackageManager;", klj_Context_getPackageManager},
    {"android/content/Context", "getPackageName", "()Ljava/lang/String;", klj_Context_getPackageName},
    {"android/content/Context", "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", klj_Context_getSystemService},
    {"android/net/wifi/WifiManager", "getConnectionInfo", "()Landroid/net/wifi/WifiInfo;",
     klj_WifiManager_getConnectionInfo},
    {"android/net/ConnectivityManager", "getActiveNetwork", "()Landroid/net/Network;",
     klj_ConnectivityManager_getActiveNetwork},
    {"android/net/ConnectivityManager", "getActiveNetworkInfo", "()Landroid/net/NetworkInfo;",
     klj_ConnectivityManager_getActiveNetworkInfo},
    {"android/net/NetworkInfo", "isConnected",   "()Z", klj_NetworkInfo_isConnected},
    {"android/net/NetworkInfo", "isAvailable",   "()Z", klj_NetworkInfo_isConnected},
    {"android/net/NetworkInfo", "isConnectedOrConnecting", "()Z", klj_NetworkInfo_isConnected},
    {"android/net/NetworkInfo", "getType",       "()I", klj_NetworkInfo_getType},
    {"android/net/ConnectivityManager", "getNetworkCapabilities",
     "(Landroid/net/Network;)Landroid/net/NetworkCapabilities;",
     klj_ConnectivityManager_getNetworkCapabilities},
    {"android/net/NetworkCapabilities", "hasTransport",  "(I)Z", klj_NetworkCapabilities_hasTransport},
    {"android/net/NetworkCapabilities", "hasCapability", "(I)Z", klj_NetworkCapabilities_hasCapability},
    {"android/net/wifi/WifiManager", "createWifiLock", "(ILjava/lang/String;)Landroid/net/wifi/WifiManager$WifiLock;",
     klj_WifiManager_createWifiLock},
    {"android/net/wifi/WifiManager$WifiLock", "acquire", "()V", klj_WifiLock_acquire},
    {"android/net/wifi/WifiManager$WifiLock", "isHeld",  "()Z", klj_WifiLock_isHeld},
    {"android/content/Context", "getPackageCodePath", "()Ljava/lang/String;", klj_Context_getPackageCodePath},
    {"android/content/Context", "getObbDirs", "()[Ljava/io/File;", klj_Context_getObbDirs},
    {"android/content/Context", "getObbDir",  "()Ljava/io/File;",  klj_Context_getObbDir},
    {"android/content/pm/PackageManager", "queryIntentActivities",
     "(Landroid/content/Intent;I)Ljava/util/List;", klj_PM_queryIntentActivities},
    {"java/lang/Object", "<init>",   "()V",                   klj_generic_init},
    {"java/lang/Object", "getClass", "()Ljava/lang/Class;", klj_Object_getClass},
    {"java/lang/String", "equals", "(Ljava/lang/Object;)Z", klj_String_equals},
    {"java/lang/String", "length", "()I",                   klj_String_length},
    // Standalone-launched (UnityPlayerActivity is the LAUNCHER in this
    // manifest), not embedded in a host app — so the Unity-as-a-Library
    // predicate is false, which is what the real Android would compute.
    {"com/unity3d/player/UnityPlayer", "isUaaLUseCase", "()Z", klj_false},

    {"bitter/jnibridge/JNIBridge", "newInterfaceProxy",
     "(J[Ljava/lang/Class;)Ljava/lang/Object;", klj_JNIBridge_newInterfaceProxy},
    {"bitter/jnibridge/JNIBridge", "disableInterfaceProxy",
     "(Ljava/lang/Object;)V", klj_JNIBridge_disableInterfaceProxy},

    {"java/util/List", "size",    "()I",                 klj_List_size},
    {"java/util/List", "isEmpty", "()Z",                 klj_List_isEmpty},
    {"java/util/List", "get",  "(I)Ljava/lang/Object;",  klj_List_get},
    // No screensaver policy is set, no other app is playing audio, and no
    // InputDevice with a gamepad source is presented — controllers reach this
    // guest through the XR seam, not through android.view.InputDevice.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IsScreensaverEnabled", "()Z", klj_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IsMusicActive", "()Z", klj_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IsGamepadAttached", "()Z", klj_false},
    {0}
};
