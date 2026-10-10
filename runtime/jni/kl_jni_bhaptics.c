// com.bhaptics.bhapticsunity.BhapticsManagerWrapper — the bHaptics SDK2 Unity
// plugin's Java side.
//
// bHaptics makes haptic vests, gloves and sleeves that pair with the headset
// over Bluetooth through the bHaptics Player app. Games built with the SDK2
// Unity package (Ancient Dungeon is one) construct this wrapper on startup with
// `new AndroidJavaObject("com.bhaptics.bhapticsunity.BhapticsManagerWrapper",
// currentActivity)`, hand it the app's id and key through initialize(), and
// then route every haptic event through it. With no host implementation the
// constructor call aborted before the first frame.
//
// There is no bHaptics Player and no vest here, and nothing of the real
// wrapper (a Bluetooth service client plus libbhaptics-native-lib) is wanted.
// So this answers the class as a device that is paired with nothing:
// isBhapticsUser() false, no devices, nothing playing, play requests refused
// with -1 — the same answers the real wrapper gives on a Quest with the Player
// app not installed, which is a state every SDK2 game already handles.
#include <stdint.h>
#include "kl_jni.h"
#include "kl_jni_int.h"

#define BHCLS "com/bhaptics/bhapticsunity/BhapticsManagerWrapper"

static klj_val klb_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    KLJ_LOG_ONCE("new BhapticsManagerWrapper(activity) — no bHaptics Player here; answering as unpaired");
    return (klj_val){.l = kl_jni_new_object(klj_class_name(clazz))};
}
static klj_val klb_void(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n; return (klj_val){.j = 0};
}
static klj_val klb_false(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n; return (klj_val){.j = 0};
}
// play*() return a request id; the SDK treats -1 as "not played".
static klj_val klb_minus1(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n; return (klj_val){.j = (uint64_t)(int64_t)-1};
}
static klj_val klb_noStrings(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_array('L', "java/lang/String", 0)};
}
// A JSON list of paired devices — empty, as a string rather than null so that
// the C# side's JSON parse of it succeeds.
static klj_val klb_emptyJsonList(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string("[]")};
}
static klj_val klb_noBytes(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_array('B', NULL, 0)};
}

const klj_binding klj_bind_bhaptics[] = {
    // Unity spells the constructor's argument as the activity's RUNTIME class,
    // with dots (AndroidJavaObject builds the signature from the object it was
    // handed), so that spelling is bound beside the declared one.
    {BHCLS, "<init>", "(Landroid/app/Activity;)V",                   klb_init},
    {BHCLS, "<init>", "(Lcom/unity3d/player/UnityPlayerActivity;)V", klb_init},
    {BHCLS, "<init>", "(Lcom.unity3d.player.UnityPlayerActivity;)V", klb_init},

    // Lifecycle and configuration — nothing to set up or tear down.
    {BHCLS, "initialize",                     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",  klb_void},
    {BHCLS, "initializeWithPermissionOption", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V", klb_void},
    {BHCLS, "quit",                 "()V",                                     klb_void},
    {BHCLS, "refreshPairing",       "()V",                                     klb_void},
    {BHCLS, "showBluetoothSetting", "()V",                                     klb_void},
    {BHCLS, "register",             "(Ljava/lang/String;Ljava/lang/String;)V", klb_void},
    {BHCLS, "registerReflected",    "(Ljava/lang/String;Ljava/lang/String;)V", klb_void},
    {BHCLS, "enableDevice",         "(Ljava/lang/String;I)V",                  klb_void},
    {BHCLS, "togglePosition",       "(Ljava/lang/String;)V",                   klb_void},
    {BHCLS, "toggleStreamingEnable","()V",                                     klb_void},
    {BHCLS, "ping",                 "(Ljava/lang/String;)V",                   klb_void},
    {BHCLS, "pingAll",              "()V",                                     klb_void},
    {BHCLS, "turnOff",              "(Ljava/lang/String;)V",                   klb_void},
    {BHCLS, "turnOffAll",           "()V",                                     klb_void},
    {BHCLS, "pause",                "(Ljava/lang/String;)V",                   klb_void},
    {BHCLS, "resume",               "(Ljava/lang/String;)V",                   klb_void},

    // State queries — unpaired, nothing playing.
    {BHCLS, "isBhapticsUser",       "()Z",                   klb_false},
    {BHCLS, "isStreamingEnable",    "()Z",                   klb_false},
    {BHCLS, "isAnythingPlaying",    "()Z",                   klb_false},
    {BHCLS, "isPlaying",            "(Ljava/lang/String;)Z", klb_false},
    {BHCLS, "isPlayingByEventId",   "(I)Z",                  klb_false},
    {BHCLS, "isPlayingByEventId",   "(Ljava/lang/String;)Z", klb_false},
    {BHCLS, "isPlayingByRequestId", "(I)Z",                  klb_false},
    {BHCLS, "isRegistered",         "(Ljava/lang/String;)Z", klb_false},
    {BHCLS, "stopAll",              "()Z",                   klb_false},
    {BHCLS, "stopByEventId",        "(I)Z",                  klb_false},
    {BHCLS, "stopByEventId",        "(Ljava/lang/String;)Z", klb_false},
    {BHCLS, "stopInt",              "(I)Z",                  klb_false},
    {BHCLS, "getDeviceList",        "()[Ljava/lang/String;", klb_noStrings},
    {BHCLS, "getStreamingHosts",    "()[Ljava/lang/String;", klb_noStrings},
    {BHCLS, "getDeviceListString",  "()Ljava/lang/String;",  klb_emptyJsonList},
    {BHCLS, "getPositionStatus",    "(Ljava/lang/String;)[B", klb_noBytes},
    {BHCLS, "getEventId",           "(Ljava/lang/String;)I", klb_minus1},

    // Playback — every request is refused; there is nothing to play it on.
    {BHCLS, "play",              "(Ljava/lang/String;)I",       klb_minus1},
    {BHCLS, "playEvent",         "(IIFFFF)I",                   klb_minus1},
    {BHCLS, "playWithStartTime", "(IIIFFFF)I",                  klb_minus1},
    {BHCLS, "playParam",         "(Ljava/lang/String;FFFF)I",   klb_minus1},
    {BHCLS, "playPos",           "(Ljava/lang/String;I)I",      klb_minus1},
    {BHCLS, "playPosParam",      "(Ljava/lang/String;IFFFF)I",  klb_minus1},
    {BHCLS, "playLoop",          "(IIFFFFII)I",                 klb_minus1},
    {BHCLS, "playLoop",          "(Ljava/lang/String;FFFFII)I", klb_minus1},
    {BHCLS, "playMotors",        "(II[I)I",                     klb_minus1},
    {BHCLS, "playPath",          "(II[F[F[I)I",                 klb_minus1},
    {BHCLS, "playGlove",         "(I[I[I[II)I",                 klb_minus1},
    {BHCLS, "playGlove",         "(I[I[I[III)I",                klb_minus1},
    {BHCLS, "submitDot",         "(Ljava/lang/String;Ljava/lang/String;[I[II)V",  klb_void},
    {BHCLS, "submitPath",        "(Ljava/lang/String;Ljava/lang/String;[F[F[II)V", klb_void},
    {BHCLS, "submitRegistered",  "(Ljava/lang/String;Ljava/lang/String;FFFF)V",   klb_void},
    {BHCLS, "submitRegisteredWithTime", "(Ljava/lang/String;I)V",                 klb_void},
    {0}
};
