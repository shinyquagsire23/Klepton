// Unreal Engine 4: MessageBox01, the GetMetaData family, the three
// BroadcastReceivers, MediaPlayer14
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
#include "kl_jni_int.h"

// ---- UE4's MessageBox01 ----
// `FPlatformMisc::MessageBoxExt` on Android: the engine news up one of these,
// sets a caption and a body, adds one button per choice, calls show() and BLOCKS
// on the answer. It is the only way a UE4 guest can say something to a person,
// and in a Shipping build with logging stripped it is very nearly the only way
// it can say anything at all — so this prints the whole box rather than
// swallowing it. A refusal here would have been an abort naming `setText`, which
// says nothing about what the engine was trying to tell us.
//
// show() answers 0, the FIRST button, which is UE4's own convention for the
// default/affirmative choice (`EAppReturnType::Ok` is the first thing added by
// every caller in the engine). There is no person here to press anything, and a
// modal that never returns is a hang.
static char g_ue4_msg_caption[512];
static char g_ue4_msg_text[4096];
static char g_ue4_msg_buttons[512];

// ---- UE4's GetMetaData* family ----
// `FAndroidMisc::GetConfigRulesVariable`'s neighbours: the engine asks its
// activity for a named value and the activity answers from one of THREE places,
// which is the whole reason these cannot be a plain Bundle read. Transcribed
// from this APK's GameActivity: a handful of `ue4.`-prefixed keys are synthetic
// and answered from the Display or from a system property, and everything else
// falls through to the manifest <meta-data> Bundle.
//
// Routing the synthetic ones at the same Display handlers Unity reads is the
// display-panel group answer again — two engines asking one device the same
// question must not get two numbers.
static klj_val klj_GA_hasMetaDataKey(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *k = n > 0 ? klj_str(a[0].l) : NULL;
    return (klj_val){.j = (uint64_t)(k && kl_jni_manifest_meta(k) != NULL)};
}
static klj_val klj_GA_getMetaDataBoolean(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *k = n > 0 ? klj_str(a[0].l) : NULL;
    const char *v = k ? kl_jni_manifest_meta(k) : NULL;
    return (klj_val){.j = (uint64_t)(v && strcmp(v, "true") == 0)};
}
static klj_val klj_GA_getMetaDataInt(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *k = n > 0 ? klj_str(a[0].l) : "";
    // The graphics API decision, and it is made HERE rather than by the RHI:
    // UE4 offers Vulkan only when the platform reports the vulkan.version
    // feature, and the activity reads that out of
    // PackageManager.getSystemAvailableFeatures().
    //
    // RE4 is packaged VULKAN-ONLY and says so itself — presenting no Vulkan
    // feature gets "This device does not support Vulkan but the app was not
    // packaged with ES 3.1 support" out of its own message box, which is the
    // engine reporting that the alternative does not exist in this build rather
    // than a preference. So the answer is Vulkan 1.0.3 (`VK_MAKE_VERSION(1,0,3)`,
    // what a Quest 2 reports) and level 1, and the guest reaches kl_vulkan.c —
    // the path BONELAB already drives. `KL_UE4_VULKAN=0` restores the refusal
    // exactly, which is the A/B for anything that suspects the API choice.
    // The two audio keys, and answering them is not optional: GameActivity
    // resolves them through `AudioManager.getProperty`, and
    // `FMixerPlatformAndroid::GetPlatformSettings` rounds its callback size up
    // to a multiple of the frames-per-buffer answer with
    // `while (n < want) n += framesPerBuffer;` — no zero check. A 0 there is an
    // INFINITE LOOP inside the engine, on the game thread, in
    // `UEngine::InitializeAudioDeviceManager`, holding the audio device
    // manager's lock; the process stays alive with every counter healthy and
    // the boot never completes. Routed at kl_jni's own AudioManager handler so
    // the number a UE4 guest is told and the number a Unity guest is told are
    // the same number.
    if (strcmp(k, "audiomanager.framesPerBuffer") == 0)
        return (klj_val){.j = (uint64_t)KLJ_AUDIO_FRAMES};
    if (strcmp(k, "audiomanager.optimalSampleRate") == 0)
        return (klj_val){.j = (uint64_t)KLJ_AUDIO_RATE};
    if (strcmp(k, "android.hardware.vulkan.version") == 0
        || strcmp(k, "android.hardware.vulkan.level") == 0) {
        int level = strcmp(k, "android.hardware.vulkan.level") == 0;
        const char *env_v = getenv("KL_UE4_VULKAN");
        int on = env_v ? (int)strtol(env_v, NULL, 0) != 0 : 1;
        int v = !on ? 0 : level ? 1 : ((1 << 22) | 3);
        KLJ_LOG("GetMetaDataInt(\"%s\") -> 0x%x%s", k, v,
                on ? "" : " (no Vulkan feature presented — KL_UE4_VULKAN=0)");
        return (klj_val){.j = (uint64_t)(int64_t)v};
    }
    const char *v = kl_jni_manifest_meta(k);
    return (klj_val){.j = (uint64_t)(int64_t)(v ? strtol(v, NULL, 10) : 0)};
}

static klj_val klj_GA_getMetaDataLong(void *env, void *self, const klj_val *a, int n) {
    const char *k = n > 0 ? klj_str(a[0].l) : "";
    if (strcmp(k, "ue4.display.PresentationDeadlineNanos") == 0)
        return klj_Display_getPresentationDeadlineNanos(env, self, a, n);
    if (strcmp(k, "ue4.display.AppVsyncOffsetNanos") == 0)
        return klj_Display_getAppVsyncOffsetNanos(env, self, a, n);
    const char *v = kl_jni_manifest_meta(k);
    return (klj_val){.j = (uint64_t)(v ? strtoll(v, NULL, 10) : 0)};
}
static klj_val klj_GA_getMetaDataFloat(void *env, void *self, const klj_val *a, int n) {
    const char *k = n > 0 ? klj_str(a[0].l) : "";
    if (strcmp(k, "ue4.display.getRefreshRate") == 0)
        return klj_Display_getRefreshRate(env, self, a, n);
    const char *v = kl_jni_manifest_meta(k);
    return (klj_val){.d = v ? strtod(v, NULL) : 0.0};
}
static klj_val klj_GA_getMetaDataString(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *k = n > 0 ? klj_str(a[0].l) : "";
    // `getprop:<name>` is a system property read wearing a meta-data key's
    // clothes, and the properties are the ones kl_libc's sysprop table already
    // answers — so this goes through that rather than growing a second table.
    if (strncmp(k, "getprop:", 8) == 0) {
        extern int klb_sysprop_get(const char *name, char *value);   // kl_libc.c
        static char val[512];
        val[0] = '\0';
        klb_sysprop_get(k + 8, val);
        KLJ_LOG("GetMetaDataString(\"%s\") -> \"%s\"", k, val);
        return (klj_val){.l = kl_jni_new_string(val)};
    }
    // `ue4.displaymetrics.dpi` is the DisplayMetrics triple as one comma
    // separated string — xdpi, ydpi, densityDpi — which the engine splits with
    // `FString::ParseIntoArray` and then indexes WITHOUT checking the count.
    // So an empty answer here is not a missing setting, it is a null
    // dereference inside `FAndroidApplicationMisc::ComputePhysicalScreenDensity`
    // eight instructions later, on a guest worker thread, naming nothing.
    // Built from the same three constants DisplayMetrics' own fields are, so
    // the two doors onto the panel cannot disagree.
    if (strcmp(k, "ue4.displaymetrics.dpi") == 0) {
        static char dpi[64];
        snprintf(dpi, sizeof dpi, "%.2f,%.2f,%d",
                 (double)KLJ_DISPLAY_XDPI, (double)KLJ_DISPLAY_YDPI, KLJ_DISPLAY_DPI);
        KLJ_LOG("GetMetaDataString(\"%s\") -> \"%s\"", k, dpi);
        return (klj_val){.l = kl_jni_new_string(dpi)};
    }
    const char *v = kl_jni_manifest_meta(k);
    return (klj_val){.l = kl_jni_new_string(v ? v : "")};
}

// How the JNI surface reaches back into the guest. See kl_jni.h.
static void *(*g_guest_native)(const char *symbol);
void kl_jni_set_guest_native_resolver(void *(*resolve)(const char *symbol)) {
    g_guest_native = resolve;
}

// `AndroidThunkJava_InitHMDs` — the activity posts a Runnable to the UI thread
// whose entire body is `nativeInitHMDs()`, which is the engine giving every HMD
// module its PreInit. Nothing else starts the XR path, so a no-op here is a
// guest with no head-mounted display and no error anywhere.
//
// Called INLINE rather than queued, and the difference is stated because it is
// real: on Android this crosses from the game thread to the UI thread and comes
// back later, and here it runs on the caller. The engine's own code is what
// runs either way, and running it late is the failure that has no symptom — the
// module list is consumed by the very next thing the game thread does.
static klj_val klj_GA_initHMDs(void *env, void *self, const klj_val *a, int n) {
    (void)a; (void)n;
    void (*fn)(void *, void *) = g_guest_native
        ? (void (*)(void *, void *))g_guest_native(
              "Java_com_epicgames_ue4_GameActivity_nativeInitHMDs")
        : NULL;
    if (!fn) {
        KLJ_LOG("AndroidThunkJava_InitHMDs: no guest nativeInitHMDs to call back "
                "into — the HMD modules never get PreInit");
        return (klj_val){.j = 0};
    }
    KLJ_LOG("AndroidThunkJava_InitHMDs -> nativeInitHMDs()");
    fn(env, self);
    return (klj_val){.j = 0};
}

// ---- UE4's three BroadcastReceivers ----
//
// `VolumeReceiver`, `BatteryReceiver` and `HeadsetReceiver`: three tiny Java
// classes UE4 ships, each one a BroadcastReceiver plus a `startReceiver` /
// `stopReceiver` pair the engine calls statically, and each one carrying a
// private native the engine registers on it. GameActivity calls all three
// during onResume.
//
// Their shape is the same and it is worth naming, because it is not the shape
// of a subscription: `startReceiver` registers the filter AND THEN READS THE
// CURRENT STATE AND DISPATCHES IT IMMEDIATELY, by calling the class's own
// native. That initial dispatch is the only one that will ever happen here —
// nothing on this host broadcasts a volume change, a battery change or a
// headset plug — so refusing these calls is not "no events", it is the engine
// never learning the state at all, and each one has a consequence
// (UE4's `FAndroidMisc::GetBatteryState` and `AreHeadPhonesPluggedIn` answer
// out of exactly these).
//
// Transcribed from this APK's own smali, which is what fixes the values that
// are not ours to choose: the volume stream is STREAM_MUSIC (3) and the level
// is `AudioManager.getStreamVolume(3)`, the battery triple is
// `(status, level*100/scale, temperature)` and is dispatched only when one of
// the three CHANGED, and the headset state is the `state` extra of a
// HEADSET_PLUG Intent, which for an Intent that is not one is 0.
//
// Every value comes from the seam that already answers the same question
// elsewhere — kl_ovrp's battery, kl_jni's own stream volume — rather than a
// constant restated here, for the display-panel reason: two numbers describing
// one battery is a level that disagrees with its own charging flag.
//
// The natives are `RegisterNatives` bindings (unlike GameActivity's, which are
// static exports), so they are looked up through kl_jni_native and NOT cached:
// a cache filled by a lookup that beat RegisterNatives would pin a NULL and
// leave the seam silently dead for the run.
static void klj_ue4_receiver_dispatch(void *env, const char *cls, const char *name,
                                      const char *sig, const klj_val *args, int nargs) {
    void *fn = kl_jni_native(cls, name, sig);
    if (!fn) {
        KLJ_LOG("%s.%s: no registered native — the engine never learns this state",
                cls, name);
        return;
    }
    // Static natives take (JNIEnv *, jclass, ...). The class argument is unread
    // by all three of these bodies (they are one-line forwards into the engine),
    // and NULL is what our own jclass handles amount to anyway.
    if (nargs == 1)
        ((void (*)(void *, void *, int32_t))fn)(env, NULL, (int32_t)args[0].j);
    else
        ((void (*)(void *, void *, int32_t, int32_t, int32_t))fn)(
            env, NULL, (int32_t)args[0].j, (int32_t)args[1].j, (int32_t)args[2].j);
}

static klj_val klj_VolumeReceiver_start(void *env, void *self, const klj_val *a, int n) {
    (void)self; (void)a; (void)n;
    klj_val v = klj_AudioManager_getStreamVolume(env, NULL,
                                                 &(klj_val){.j = 3}, 1);
    KLJ_LOG("VolumeReceiver.startReceiver -> volumeChanged(%d)", (int)v.j);
    klj_ue4_receiver_dispatch(env, "com/epicgames/ue4/VolumeReceiver",
                              "volumeChanged", "(I)V", &v, 1);
    return (klj_val){.j = 0};
}

static klj_val klj_BatteryReceiver_start(void *env, void *self, const klj_val *a, int n) {
    (void)self; (void)a; (void)n;
    // BatteryManager.BATTERY_STATUS_*: CHARGING 2, DISCHARGING 3, FULL 5 — the
    // same three-way split klj_Intent_getIntExtra already makes on the sticky
    // battery Intent, from the same seam.
    int charging = kl_ovrp_battery_charging();
    int level    = kl_ovrp_battery_level();
    klj_val args[3] = {
        {.j = (uint64_t)(charging ? (level >= 100 ? 5 : 2) : 3)},
        {.j = (uint64_t)level},
        // Tenths of a degree Celsius, which is what the `temperature` extra
        // carries. kl_ovrp answers the same number to
        // ovrp_GetSystemBatteryTemperature2 in whole degrees.
        {.j = (uint64_t)(int)(kl_ovrp_battery_temperature() * 10.0f)},
    };
    KLJ_LOG("BatteryReceiver.startReceiver -> dispatchEvent(%d, %d, %d)",
            (int)args[0].j, (int)args[1].j, (int)args[2].j);
    klj_ue4_receiver_dispatch(env, "com/epicgames/ue4/BatteryReceiver",
                              "dispatchEvent", "(III)V", args, 3);
    return (klj_val){.j = 0};
}

static klj_val klj_HeadsetReceiver_start(void *env, void *self, const klj_val *a, int n) {
    (void)self; (void)a; (void)n;
    // `getIntExtra("state", 0)` on the launch Intent, which is not a
    // HEADSET_PLUG Intent — so 0, "unplugged". That is also true of this host:
    // the audio output is whatever CoreAudio has, and nothing here presents a
    // headphone jack whose plug state could change.
    klj_val v = {.j = 0};
    KLJ_LOG("HeadsetReceiver.startReceiver -> stateChanged(0) (no headphone jack)");
    klj_ue4_receiver_dispatch(env, "com/epicgames/ue4/HeadsetReceiver",
                              "stateChanged", "(I)V", &v, 1);
    return (klj_val){.j = 0};
}

// `AndroidThunkJava_PushSensorEvents` — the engine draining the activity's
// accumulated accelerometer / gyro / magnetometer / gravity readings into
// `nativeHandleSensorEvents([F[F[F[F)`, once per game tick.
//
// A no-op, and it is the guest's OWN no-op arm rather than a refusal: the body
// is `if (!sensorsRegistered) return;` and then `if (!havePending) return;`,
// and nothing here registers a sensor. A VR guest's head pose comes from
// OVRPlugin, not from these — this is the phone-orientation path — so there is
// no reading to deliver and delivering a fabricated one would be four arrays of
// invented physics arriving every frame.
static klj_val klj_GA_pushSensorEvents(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// ...and the other end of all three. Unregistering a receiver that will never
// fire is a no-op with nothing to record, but it is answered rather than
// refused because the engine calls it on every pause.
static klj_val klj_ue4_receiver_stop(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// The engine's request for a render surface size. On Android the activity only
// acts on it when the SurfaceView sizing workaround is on (a device quirk we do
// not have); everything else about it is a record. Worth printing rather than
// dropping — it is the engine stating the resolution it intends to render at,
// which is the first number a graphics arc wants.
static klj_val klj_GA_setDesiredViewSize(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("AndroidThunkJava_SetDesiredViewSize(%d, %d) — recorded; the window "
            "size is kl_ndk's",
            n > 0 ? (int)(int64_t)a[0].j : 0, n > 1 ? (int)(int64_t)a[1].j : 0);
    return (klj_val){.j = 0};
}

// The system font directory, as the Java finds it: the first of /system/fonts,
// /system/font, /data/fonts that exists, with a trailing slash. A real Android
// device answers the first. Nothing of the guest's is under it here, so a font
// the engine then fails to open is a miss it reports itself — which is a better
// place to find out than a directory name we invented.
static klj_val klj_GA_getFontDirectory(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string("/system/fonts/")};
}

// UE4's own VR gate, and the same question Unity's is: GameActivity.onCreate
// sets `PackagedForOculusMobile` from `queryIntentActivities()` for its own
// package under `com.oculus.intent.category.VR`, OR the presence of the Samsung
// VR mode <meta-data> key. Answered by asking those two, not by a constant —
// the manifest declares the category, so a match is the truth (see "Settled
// decisions"), and a guest told otherwise disables the whole path under test.
static klj_val klj_GA_isOculusMobile(void *env, void *self, const klj_val *a, int n) {
    void *list = klj_PM_queryIntentActivities(env, self, a, n).l;
    int vr = list && klj_List_size(env, list, NULL, 0).j > 0;
    if (!vr) vr = kl_jni_manifest_meta("com.samsung.android.vr.application.mode") != NULL;
    KLJ_LOG("AndroidThunkJava_IsOculusMobileApplication() -> %s", vr ? "true" : "false");
    return (klj_val){.j = (uint64_t)(vr != 0)};
}

static klj_val klj_MsgBox_new(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_object("com/epicgames/ue4/MessageBox01")};
}
static klj_val klj_MsgBox_setCaption(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    snprintf(g_ue4_msg_caption, sizeof g_ue4_msg_caption, "%s",
             n > 0 ? klj_str(a[0].l) : "");
    return (klj_val){.j = 0};
}
static klj_val klj_MsgBox_setText(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    snprintf(g_ue4_msg_text, sizeof g_ue4_msg_text, "%s",
             n > 0 ? klj_str(a[0].l) : "");
    return (klj_val){.j = 0};
}
static klj_val klj_MsgBox_addButton(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    size_t at = strlen(g_ue4_msg_buttons);
    snprintf(g_ue4_msg_buttons + at, sizeof g_ue4_msg_buttons - at, "%s[%s]",
             at ? " " : "", n > 0 ? klj_str(a[0].l) : "");
    return (klj_val){.j = 0};
}
static klj_val klj_MsgBox_clear(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    g_ue4_msg_caption[0] = g_ue4_msg_text[0] = g_ue4_msg_buttons[0] = '\0';
    return (klj_val){.j = 0};
}
static klj_val klj_MsgBox_show(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("MESSAGE BOX from the guest — \"%s\": %s   buttons: %s "
            "(answering 0, the first)",
            g_ue4_msg_caption, g_ue4_msg_text,
            g_ue4_msg_buttons[0] ? g_ue4_msg_buttons : "(none)");
    return (klj_val){.j = 0};
}

// ---- Unreal Engine 4: com.epicgames.ue4.MediaPlayer14 ----------------------
//
// UE4's Java media player, and the thing standing between RE4 and its opening
// cutscene: `AVR4CutscenePlayerPawn::StreamLoadTheatreBox` streams a level
// whose actor's BeginPlay runs `UMediaPlayer::OpenSourceLatent`, which reaches
// `FAndroidMediaPlayer` -> `FJavaAndroidMediaPlayer(bool,bool,bool)` -> `new
// MediaPlayer14(ZZZ)`. The whole surface is 33 methods, read out of that
// constructor's own GetClassMethod calls rather than from a version of UE4's
// sources — it resolves every id up front, so the ctor IS the contract.
//
// The media is real and it is ours to decode: `main.203.com.Armature.VR4.obb`
// carries four H.265 MP4s, `Stored` (so a plain byte range), the big one being
// `VR4/Content/Movies/Andy/bio4_opening_h265.mp4`. Everything else in Movies/
// is `.bk2` and goes through the guest's own Bink libraries, which is why only
// this handful ever arrives here. kl_avdec is the decoder; this file is the
// contract and knows nothing about how a frame is produced.
//
// The three constructor booleans are `swizzlePixels`, `vulkanRenderer` and
// `needTrackInfo`, and the second one decides the whole output path: a Vulkan
// guest reads frames through `getVideoLastFrameData()Ljava/nio/Buffer;` — a
// direct ByteBuffer of packed pixels — rather than through the GLES external
// texture `getExternalTextureId` names. RE4 is Vulkan, so that is the path
// implemented; `getExternalTextureId` answers 0, which is "no external texture"
// rather than a name that would be sampled and would be someone else's.
typedef struct {
    int        swizzle, vulkan, need_track_info;
    char       source[1024];     // what the guest asked for, verbatim
    kl_avdec  *dec;
    int        video_enabled, audio_enabled;
    float      volume;
    int        looping;
    int        started;          // start() seen; distinct from the decoder running
    void      *buffer;           // the java.nio.Buffer handed out, cached
    unsigned long long buf_serial;
    int        last_w, last_h;   // for didResolutionChange
    int        res_changed;
} klj_media;

static klj_media *klj_media_of(void *self) {
    klj_object *o = klj_as_object(self);
    return (o && strcmp(o->cls, "com/epicgames/ue4/MediaPlayer14") == 0) ? o->data : NULL;
}

// Resolve what the guest named to a host file plus a byte range.
//
// Three of the four setDataSource forms carry an explicit (offset, length) into
// a container, because on Android the movie lives inside the OBB; the URL form
// carries a path on its own. Both end up here so there is one place that
// decides what file is opened — two would be two chances to disagree about
// which container an offset is into.
static void klj_media_open(klj_media *m, const char *what, long long off,
                           long long len) {
    if (!m) return;
    if (m->dec) { kl_avdec_close(m->dec); m->dec = NULL; }
    snprintf(m->source, sizeof m->source, "%s", what ? what : "");

    // `file://` and a bare path are the same thing to us; UE4 hands over both
    // depending on which MediaSource built the URL.
    const char *p = m->source;
    if (strncmp(p, "file://", 7) == 0) p += 7;
    m->dec = kl_avdec_open(p, off, len);
    if (!m->dec)
        KLJ_LOG("MediaPlayer14: could not open %s (offset %lld, %lld bytes) — "
                "setDataSource answers false and the guest's own no-media path "
                "runs", p, off, len);
}

static klj_val klj_MP14_ctor(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    klj_media *m = calloc(1, sizeof *m);
    if (!m) return (klj_val){.l = NULL};
    m->swizzle         = n > 0 && a[0].j != 0;
    m->vulkan          = n > 1 && a[1].j != 0;
    m->need_track_info = n > 2 && a[2].j != 0;
    m->video_enabled = m->audio_enabled = 1;
    m->volume = 1.0f;
    // Printed once per player and worth it: `vulkan` selects between two
    // completely different ways of handing frames back, and getting it wrong
    // is a black movie with every call succeeding.
    KLJ_LOG("MediaPlayer14(swizzlePixels=%d, vulkanRenderer=%d, needTrackInfo=%d)",
            m->swizzle, m->vulkan, m->need_track_info);
    return (klj_val){.l = klj_new_object_data("com/epicgames/ue4/MediaPlayer14", m)};
}

// The four data sources. Each answers Z — true only if a decoder really opened.
static klj_val klj_MP14_setDataSourceURL(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    klj_media_open(m, n > 0 ? klj_str(a[0].l) : NULL, 0, 0);
    return (klj_val){.j = m && m->dec ? 1 : 0};
}
// (String path, long offset, long size) — the OBB form with the container named.
static klj_val klj_MP14_setDataSourceRange(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    klj_media_open(m, n > 0 ? klj_str(a[0].l) : NULL,
                   n > 1 ? (long long)a[1].j : 0, n > 2 ? (long long)a[2].j : 0);
    return (klj_val){.j = m && m->dec ? 1 : 0};
}
// (AssetManager, String name, long offset, long size) — the same, addressed
// through the asset manager. The AssetManager argument is dropped deliberately:
// kl_jni resolves an asset name against the unpacked tree already, and a second
// resolution here would be a second answer to "where do assets live".
static klj_val klj_MP14_setDataSourceAsset(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    const char *name = n > 1 ? klj_str(a[1].l) : NULL;
    char path[1024];
    if (name && name[0] != '/') {
        snprintf(path, sizeof path, "%s/%s", g_assets_dir, name);
        name = path;
    }
    klj_media_open(m, name, n > 2 ? (long long)a[2].j : 0,
                   n > 3 ? (long long)a[3].j : 0);
    return (klj_val){.j = m && m->dec ? 1 : 0};
}
// (long, long) — a range in an archive the guest already has open, handed over
// as raw numbers with no container named at all. There is nothing here to
// resolve them against, so it is refused BY NAME rather than guessed at: an
// offset into an unknown file is the one input that cannot be honoured.
static klj_val klj_MP14_setDataSourceArchive(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("MediaPlayer14.setDataSourceArchive(%lld, %lld) — refused: the guest "
            "names an offset and a length with no container, and this shim has "
            "no handle on the archive it means",
            n > 0 ? (long long)a[0].j : 0, n > 1 ? (long long)a[1].j : 0);
    return (klj_val){.j = 0};
}

static klj_val klj_MP14_prepare(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n; (void)self;
    return (klj_val){.j = 0};      // the open already did the work
}
static klj_val klj_MP14_start(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    if (m) { m->started = 1; kl_avdec_play(m->dec, 1); }
    return (klj_val){.j = 0};
}
static klj_val klj_MP14_pause(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    if (m) kl_avdec_play(m->dec, 0);
    return (klj_val){.j = 0};
}
static klj_val klj_MP14_stop(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    if (m) { m->started = 0; kl_avdec_play(m->dec, 0); kl_avdec_seek(m->dec, 0); }
    return (klj_val){.j = 0};
}
static klj_val klj_MP14_reset(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    if (m && m->dec) { kl_avdec_close(m->dec); m->dec = NULL; m->started = 0; }
    return (klj_val){.j = 0};
}
static klj_val klj_MP14_release(void *env, void *self, const klj_val *a, int n) {
    return klj_MP14_reset(env, self, a, n);
}
static klj_val klj_MP14_seekTo(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    if (m) kl_avdec_seek(m->dec, n > 0 ? (int32_t)a[0].j : 0);
    return (klj_val){.j = 0};
}
static klj_val klj_MP14_setLooping(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    if (m) { m->looping = n > 0 && a[0].j != 0; kl_avdec_set_looping(m->dec, m->looping); }
    return (klj_val){.j = 0};
}
static klj_val klj_MP14_isLooping(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = m && m->looping};
}
static klj_val klj_MP14_isPlaying(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = m && kl_avdec_playing(m->dec)};
}
// "Prepared" is "a decoder is open on real media", which is the same condition
// setDataSource answered — there is no asynchronous preparation here because
// kl_avdec_open has already read the asset by the time it returns.
static klj_val klj_MP14_isPrepared(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = m && m->dec != NULL};
}
static klj_val klj_MP14_didComplete(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = m && kl_avdec_complete(m->dec)};
}
static klj_val klj_MP14_getDuration(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = (uint64_t)(uint32_t)(m ? kl_avdec_duration_ms(m->dec) : 0)};
}
static klj_val klj_MP14_getCurrentPosition(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = (uint64_t)(uint32_t)(m ? kl_avdec_position_ms(m->dec) : 0)};
}
static klj_val klj_MP14_getVideoWidth(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = (uint64_t)(uint32_t)(m ? kl_avdec_width(m->dec) : 0)};
}
static klj_val klj_MP14_getVideoHeight(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = (uint64_t)(uint32_t)(m ? kl_avdec_height(m->dec) : 0)};
}
static klj_val klj_MP14_setVideoEnabled(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    if (m) m->video_enabled = n > 0 && a[0].j != 0;
    return (klj_val){.j = 0};
}
// Recorded rather than applied: nothing here decodes the audio track yet, so
// honouring the volume would be describing a mix that does not exist. Said
// once, so a silent cutscene is a known gap rather than a mystery.
static klj_val klj_MP14_setAudioEnabled(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    if (m) m->audio_enabled = n > 0 && a[0].j != 0;
    static int said;
    if (m && m->audio_enabled && !said) {
        said = 1;
        KLJ_LOG("MediaPlayer14: audio is enabled by the guest and this player "
                "decodes VIDEO only — the cutscene will be silent");
    }
    return (klj_val){.j = 0};
}
static klj_val klj_MP14_setAudioVolume(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    if (m && n > 0) memcpy(&m->volume, &a[0].j, sizeof m->volume);
    return (klj_val){.j = 0};
}

// The frame, as a direct ByteBuffer over the decoder's own storage.
//
// The buffer object is cached per player: UE4 calls this every frame it wants a
// picture, and a fresh jobject per call is garbage the guest's local frame has
// to retire. The ADDRESS behind it is stable for the life of the decoder — the
// frame buffer is reallocated only when the dimensions change, which is why the
// cache is dropped when they do.
static klj_val klj_MP14_getVideoLastFrameData(void *env, void *self,
                                              const klj_val *a, int n) {
    (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    if (!m || !m->dec) return (klj_val){.l = NULL};
    int w = 0, h = 0;
    size_t bytes = 0;
    unsigned long long serial = 0;
    const void *px = kl_avdec_frame(m->dec, &w, &h, &bytes, &serial);
    if (!px || !bytes) return (klj_val){.l = NULL};
    if (w != m->last_w || h != m->last_h) {
        m->res_changed = 1;
        m->last_w = w; m->last_h = h;
        m->buffer = NULL;
    }
    if (!m->buffer)
        m->buffer = klj_own(klj_NewDirectByteBuffer(env, (void *)px, (int64_t)bytes),
                            NULL);
    m->buf_serial = serial;
    return (klj_val){.l = m->buffer};
}

// (int) -> Z: "is there a frame at all". The int is the external texture the
// GLES path would have rendered into and is not one here.
static klj_val klj_MP14_getVideoLastFrame(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    if (!m || !m->dec) return (klj_val){.j = 0};
    unsigned long long serial = 0;
    const void *px = kl_avdec_frame(m->dec, NULL, NULL, NULL, &serial);
    return (klj_val){.j = px != NULL && serial != 0};
}

// Latched and cleared by the read, which is what "did it change" means — a
// flag that stayed set would make UE4 rebuild its texture every frame.
static klj_val klj_MP14_didResolutionChange(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    if (!m) return (klj_val){.j = 0};
    int c = m->res_changed;
    m->res_changed = 0;
    return (klj_val){.j = c != 0};
}

// 0 = there is no external texture. The GLES path binds this name and samples
// it; answering anything else would name a texture belonging to the guest's own
// renderer, which is worse than having none.
static klj_val klj_MP14_getExternalTextureId(void *env, void *self,
                                             const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// NULL = "nothing new this tick", which is the answer the GLES path is built to
// handle. It is reached only by a guest that took the external-texture route,
// and RE4 does not.
static klj_val klj_MP14_updateVideoFrame(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static int said;
    if (!said) {
        said = 1;
        KLJ_LOG("MediaPlayer14.updateVideoFrame — the GLES external-texture path, "
                "which this player does not implement (frames come out of "
                "getVideoLastFrameData). Answering null.");
    }
    return (klj_val){.l = NULL};
}

// Empty track lists. `needTrackInfo` is the ctor's third bool and UE4 only asks
// when it is set; an empty array is "this player reports no selectable tracks",
// which is true — nothing here exposes alternates — and is distinguishable from
// the refusal an absent binding would be.
static klj_val klj_MP14_tracks_audio(void *env, void *self, const klj_val *a, int n) {
    (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_array('L',
        "com/epicgames/ue4/MediaPlayer14$AudioTrackInfo", 0)};
}
static klj_val klj_MP14_tracks_caption(void *env, void *self, const klj_val *a, int n) {
    (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_array('L',
        "com/epicgames/ue4/MediaPlayer14$CaptionTrackInfo", 0)};
}
static klj_val klj_MP14_tracks_video(void *env, void *self, const klj_val *a, int n) {
    (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_array('L',
        "com/epicgames/ue4/MediaPlayer14$VideoTrackInfo", 0)};
}
static klj_val klj_MP14_selectTrack(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

const klj_binding klj_bind_ue4[] = {
    {"android/view/Display", "getRefreshRate", "()F", klj_Display_getRefreshRate},
    {"android/view/Display", "getAppVsyncOffsetNanos",        "()J", klj_Display_getAppVsyncOffsetNanos},
    {"android/view/Display", "getPresentationDeadlineNanos",  "()J", klj_Display_getPresentationDeadlineNanos},

    // ---- Unreal Engine 4: com.epicgames.ue4.GameActivity ----
    //
    // UE4's engine-to-Java surface. Every one of these is a method the ENGINE
    // calls on its own activity — `AndroidThunkJava_*` is Epic's naming for
    // exactly that direction — so unlike the Android framework above, the
    // authority for what each one does is this APK's GameActivity.smali rather
    // than the platform.
    //
    // They are on `com/epicgames/ue4/GameActivity` and not on a superclass
    // because the guest looks the class up by that name (kl_ue4.c sets it as
    // the activity class for the same reason).
    // com.epicgames.ue4.MediaPlayer14 — UE4's Java media player. The names and
    // signatures are FJavaAndroidMediaPlayer's own GetClassMethod calls, in the
    // order that constructor resolves them.
    {"com/epicgames/ue4/MediaPlayer14", "<init>", "(ZZZ)V", klj_MP14_ctor},
    {"com/epicgames/ue4/MediaPlayer14", "getDuration", "()I", klj_MP14_getDuration},
    {"com/epicgames/ue4/MediaPlayer14", "reset", "()V", klj_MP14_reset},
    {"com/epicgames/ue4/MediaPlayer14", "stop", "()V", klj_MP14_stop},
    {"com/epicgames/ue4/MediaPlayer14", "getCurrentPosition", "()I",
     klj_MP14_getCurrentPosition},
    {"com/epicgames/ue4/MediaPlayer14", "isLooping", "()Z", klj_MP14_isLooping},
    {"com/epicgames/ue4/MediaPlayer14", "isPlaying", "()Z", klj_MP14_isPlaying},
    {"com/epicgames/ue4/MediaPlayer14", "isPrepared", "()Z", klj_MP14_isPrepared},
    {"com/epicgames/ue4/MediaPlayer14", "didComplete", "()Z", klj_MP14_didComplete},
    {"com/epicgames/ue4/MediaPlayer14", "setDataSourceURL",
     "(Ljava/lang/String;)Z", klj_MP14_setDataSourceURL},
    {"com/epicgames/ue4/MediaPlayer14", "setDataSourceArchive", "(JJ)Z",
     klj_MP14_setDataSourceArchive},
    {"com/epicgames/ue4/MediaPlayer14", "setDataSource", "(Ljava/lang/String;JJ)Z",
     klj_MP14_setDataSourceRange},
    {"com/epicgames/ue4/MediaPlayer14", "setDataSource",
     "(Landroid/content/res/AssetManager;Ljava/lang/String;JJ)Z",
     klj_MP14_setDataSourceAsset},
    {"com/epicgames/ue4/MediaPlayer14", "prepare", "()V", klj_MP14_prepare},
    {"com/epicgames/ue4/MediaPlayer14", "prepareAsync", "()V", klj_MP14_prepare},
    {"com/epicgames/ue4/MediaPlayer14", "seekTo", "(I)V", klj_MP14_seekTo},
    {"com/epicgames/ue4/MediaPlayer14", "setLooping", "(Z)V", klj_MP14_setLooping},
    {"com/epicgames/ue4/MediaPlayer14", "release", "()V", klj_MP14_release},
    {"com/epicgames/ue4/MediaPlayer14", "getVideoHeight", "()I", klj_MP14_getVideoHeight},
    {"com/epicgames/ue4/MediaPlayer14", "getVideoWidth", "()I", klj_MP14_getVideoWidth},
    {"com/epicgames/ue4/MediaPlayer14", "setVideoEnabled", "(Z)V", klj_MP14_setVideoEnabled},
    {"com/epicgames/ue4/MediaPlayer14", "setAudioEnabled", "(Z)V", klj_MP14_setAudioEnabled},
    {"com/epicgames/ue4/MediaPlayer14", "setAudioVolume", "(F)V", klj_MP14_setAudioVolume},
    {"com/epicgames/ue4/MediaPlayer14", "getVideoLastFrameData", "()Ljava/nio/Buffer;",
     klj_MP14_getVideoLastFrameData},
    {"com/epicgames/ue4/MediaPlayer14", "start", "()V", klj_MP14_start},
    {"com/epicgames/ue4/MediaPlayer14", "pause", "()V", klj_MP14_pause},
    {"com/epicgames/ue4/MediaPlayer14", "getVideoLastFrame", "(I)Z",
     klj_MP14_getVideoLastFrame},
    {"com/epicgames/ue4/MediaPlayer14", "GetAudioTracks",
     "()[Lcom/epicgames/ue4/MediaPlayer14$AudioTrackInfo;", klj_MP14_tracks_audio},
    {"com/epicgames/ue4/MediaPlayer14", "GetCaptionTracks",
     "()[Lcom/epicgames/ue4/MediaPlayer14$CaptionTrackInfo;", klj_MP14_tracks_caption},
    {"com/epicgames/ue4/MediaPlayer14", "GetVideoTracks",
     "()[Lcom/epicgames/ue4/MediaPlayer14$VideoTrackInfo;", klj_MP14_tracks_video},
    {"com/epicgames/ue4/MediaPlayer14", "didResolutionChange", "()Z",
     klj_MP14_didResolutionChange},
    {"com/epicgames/ue4/MediaPlayer14", "getExternalTextureId", "()I",
     klj_MP14_getExternalTextureId},
    {"com/epicgames/ue4/MediaPlayer14", "updateVideoFrame",
     "(I)Lcom/epicgames/ue4/MediaPlayer14$FrameUpdateInfo;", klj_MP14_updateVideoFrame},
    {"com/epicgames/ue4/MediaPlayer14", "selectTrack", "(I)V", klj_MP14_selectTrack},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IsOculusMobileApplication",
     "()Z", klj_GA_isOculusMobile},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_HasMetaDataKey",
     "(Ljava/lang/String;)Z", klj_GA_hasMetaDataKey},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetMetaDataBoolean",
     "(Ljava/lang/String;)Z", klj_GA_getMetaDataBoolean},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetMetaDataInt",
     "(Ljava/lang/String;)I", klj_GA_getMetaDataInt},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetMetaDataLong",
     "(Ljava/lang/String;)J", klj_GA_getMetaDataLong},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetMetaDataFloat",
     "(Ljava/lang/String;)F", klj_GA_getMetaDataFloat},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetMetaDataString",
     "(Ljava/lang/String;)Ljava/lang/String;", klj_GA_getMetaDataString},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_InitHMDs", "()V", klj_GA_initHMDs},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_SetDesiredViewSize", "(II)V",
     klj_GA_setDesiredViewSize},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetFontDirectory",
     "()Ljava/lang/String;", klj_GA_getFontDirectory},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_PushSensorEvents", "()V",
     klj_GA_pushSensorEvents},
    // UE4's three BroadcastReceivers. `startReceiver` is where the state is
    // actually delivered — see klj_ue4_receiver_dispatch.
    {"com/epicgames/ue4/VolumeReceiver", "startReceiver", "(Landroid/app/Activity;)V",
     klj_VolumeReceiver_start},
    {"com/epicgames/ue4/VolumeReceiver", "stopReceiver", "(Landroid/app/Activity;)V",
     klj_ue4_receiver_stop},
    {"com/epicgames/ue4/BatteryReceiver", "startReceiver", "(Landroid/app/Activity;)V",
     klj_BatteryReceiver_start},
    {"com/epicgames/ue4/BatteryReceiver", "stopReceiver", "(Landroid/app/Activity;)V",
     klj_ue4_receiver_stop},
    {"com/epicgames/ue4/HeadsetReceiver", "startReceiver", "(Landroid/app/Activity;)V",
     klj_HeadsetReceiver_start},
    {"com/epicgames/ue4/HeadsetReceiver", "stopReceiver", "(Landroid/app/Activity;)V",
     klj_ue4_receiver_stop},
    {"com/epicgames/ue4/MessageBox01", "<init>",     "()V", klj_MsgBox_new},
    {"com/epicgames/ue4/MessageBox01", "setCaption", "(Ljava/lang/String;)V", klj_MsgBox_setCaption},
    {"com/epicgames/ue4/MessageBox01", "setText",    "(Ljava/lang/String;)V", klj_MsgBox_setText},
    {"com/epicgames/ue4/MessageBox01", "addButton",  "(Ljava/lang/String;)V", klj_MsgBox_addButton},
    {"com/epicgames/ue4/MessageBox01", "clear",      "()V", klj_MsgBox_clear},
    {"com/epicgames/ue4/MessageBox01", "show",       "()I", klj_MsgBox_show},
    {0}
};
