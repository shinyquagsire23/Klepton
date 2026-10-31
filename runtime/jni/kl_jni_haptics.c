// bHaptics — com.bhaptics.bhapticsunity.BhapticsManagerWrapper.
//
// The bHaptics vest/glove SDK, shipped inside Unity titles such as Paint the
// Town Red VR and driven from C# by BhapticsLibrary. There is no bHaptics hardware on an Apple Vision Pro, no
// Bluetooth LE pairing to it, and no com.bhaptics.player service to bind to —
// the manifest `queries` that package and it does not exist here. So this whole
// family has exactly one honest answer: **no device, nothing playing, ever**.
//
// It is bound WHOLE rather than one method per launch, which is the point of
// this file. The class's public surface was read out of that APK's own
// BhapticsManagerWrapper.smali, all 40 entries, and the guest's il2cpp
// metadata confirms it is reached: `BhapticsLibrary -
// Initialize`, `isBhapticsUser`, `getDeviceListString` and
// `initializeWithPermissionOption` are string literals in the metadata, and the
// identifier heap carries about a hundred `BHaptics_*` event names — this title
// is wired for it throughout, from the menu to every hit.
//
// The answers are chosen so the SDK's own "no hardware" branch is the one that
// runs, rather than a branch that waits:
//
//   - isBhapticsUser() is FALSE. It is the gate the C# checks before offering
//     bHaptics UI at all, and false is what a device with no paired hardware
//     and no player app reports.
//   - every is*() is false, so nothing is ever reported as playing, registered
//     or streaming, and nothing waits for a completion.
//   - the play*() family returns 0. A real one returns a request id; 0 means "no
//     request", which is consistent with isPlayingByRequestId(0) answering
//     false. A non-zero id would promise a playback that will never end.
//   - getDeviceListString() is "[]" — the SDK parses this as JSON — and
//     getDeviceList()/getStreamingHosts() are zero-length arrays. Empty, not
//     null: a caller reading .Length on null is an exception, and reading it on
//     an empty array is the no-devices case it already handles.
//   - getPositionStatus() is a zero-length byte[] for the same reason: it is the
//     per-motor state of a device that is not attached.
//   - everything void records and returns. There is no thread to start, no
//     socket to open, and no callback owed to anyone.
//
// The one thing this family must NOT do is model a wait. bHaptics' real
// initialize() binds a service and blocks; nothing here can complete such a
// bind, and a modelled wait would be a hang on the caller's thread with no cause
// anywhere near it — the same rule as UnityPlayer.requestUserAuthorization in
// kl_jni_display.c.
//
// Deliberately NOT bound: com.bhaptics.core.CoreWrapper. Every one of its
// methods is `public native`, implemented in libbhaptics-native-lib.so, which
// translates and loads — those calls belong to the translated library through
// RegisterNatives, and a host binding here would shadow real code with a stub.
#include <string.h>
#include "kl_jni_int.h"

// The wrapper object itself needs an identity and nothing else — its whole
// state, on a host with no hardware, is "there is no hardware". Local rather
// than shared with kl_jni_android.c's identical constructor because each family
// file keeps its own implementations static, per runtime/jni/kl_jni_int.h.
static klj_val klj_bh_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    KLJ_LOG("bHaptics: BhapticsManagerWrapper constructed — no hardware, no player app");
    return (klj_val){.l = kl_jni_new_object(klj_class_name(clazz))};
}

static klj_val klj_bh_void(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// The calls worth naming in the log: the ones a launch would otherwise leave no
// trace of, and the ones whose answer decides whether bHaptics appears in the
// game's UI at all.
static klj_val klj_bh_initialize(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("bHaptics initialize(\"%s\") — no bHaptics hardware, no player app, "
            "and no service to bind on this host; returning immediately",
            n > 0 ? klj_str(a[0].l) : "");
    return (klj_val){.j = 0};
}

static klj_val klj_bh_false(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

static klj_val klj_bh_isBhapticsUser(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("bHaptics isBhapticsUser() -> false (no paired device, no player app)");
    return (klj_val){.j = 0};
}

// 0, not -1: a request id of 0 is "no request", and isPlayingByRequestId(0)
// answers false below, so the two agree. -1 is an error code in this SDK and
// would have the caller log a failure for every hit in the game.
static klj_val klj_bh_zero(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

static klj_val klj_bh_empty_string(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string("[]")};
}

static klj_val klj_bh_empty_strings(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_array('L', "java/lang/String", 0)};
}

static klj_val klj_bh_empty_bytes(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_array('B', NULL, 0)};
}

#define BH "com/bhaptics/bhapticsunity/BhapticsManagerWrapper"

const klj_binding klj_bind_haptics[] = {
    // Construction. Declared as the smali declares it — Activity — and reached
    // with a UnityPlayerActivity in hand; the assignability tier in kl_jni.c is
    // what bridges the two, the same way org.fmod.FMOD.init is reached.
    {BH, "<init>", "(Landroid/app/Activity;)V", klj_bh_init},

    // ---- the two gates the C# reads before doing anything else ----
    {BH, "isBhapticsUser",       "()Z", klj_bh_isBhapticsUser},
    {BH, "isStreamingEnable",    "()Z", klj_bh_false},

    // ---- setup ----
    {BH, "initialize",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", klj_bh_initialize},
    {BH, "initializeWithPermissionOption",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V", klj_bh_initialize},
    {BH, "quit",                 "()V", klj_bh_void},
    {BH, "refreshPairing",       "()V", klj_bh_void},
    {BH, "showBluetoothSetting", "()V", klj_bh_void},
    {BH, "toggleStreamingEnable","()V", klj_bh_void},
    {BH, "enableDevice",         "(Ljava/lang/String;I)V", klj_bh_void},
    {BH, "togglePosition",       "(Ljava/lang/String;)V",  klj_bh_void},

    // ---- what is attached: nothing ----
    {BH, "getDeviceList",       "()[Ljava/lang/String;", klj_bh_empty_strings},
    {BH, "getStreamingHosts",   "()[Ljava/lang/String;", klj_bh_empty_strings},
    {BH, "getDeviceListString", "()Ljava/lang/String;",  klj_bh_empty_string},
    {BH, "getPositionStatus",   "(Ljava/lang/String;)[B", klj_bh_empty_bytes},

    // ---- registration of patterns, and the ids they get ----
    {BH, "getEventId",       "(Ljava/lang/String;)I", klj_bh_zero},
    {BH, "register",         "(Ljava/lang/String;Ljava/lang/String;)V", klj_bh_void},
    {BH, "registerReflected","(Ljava/lang/String;Ljava/lang/String;)V", klj_bh_void},
    {BH, "isRegistered",     "(Ljava/lang/String;)Z", klj_bh_false},

    // ---- playback: every entry point returns "no request" ----
    {BH, "play",         "(Ljava/lang/String;)I",           klj_bh_zero},
    {BH, "playParam",    "(Ljava/lang/String;FFFF)I",       klj_bh_zero},
    {BH, "playPos",      "(Ljava/lang/String;I)I",          klj_bh_zero},
    {BH, "playPosParam", "(Ljava/lang/String;IFFFF)I",      klj_bh_zero},
    {BH, "playEvent",    "(IIFFFF)I",                       klj_bh_zero},
    {BH, "playGlove",    "(I[I[I[II)I",                     klj_bh_zero},
    {BH, "playLoop",     "(IIFFFFII)I",                     klj_bh_zero},
    {BH, "playLoop",     "(Ljava/lang/String;FFFFII)I",     klj_bh_zero},
    {BH, "playMotors",   "(II[I)I",                         klj_bh_zero},
    {BH, "playPath",     "(II[F[F[I)I",                     klj_bh_zero},
    {BH, "submitRegistered",
     "(Ljava/lang/String;Ljava/lang/String;FFFF)V", klj_bh_void},
    {BH, "submitRegisteredWithTime", "(Ljava/lang/String;I)V", klj_bh_void},

    // ---- "is it playing" — never ----
    {BH, "isAnythingPlaying",    "()Z",                  klj_bh_false},
    {BH, "isPlaying",            "(Ljava/lang/String;)Z",klj_bh_false},
    {BH, "isPlayingByEventId",   "(I)Z",                 klj_bh_false},
    {BH, "isPlayingByEventId",   "(Ljava/lang/String;)Z",klj_bh_false},
    {BH, "isPlayingByRequestId", "(I)Z",                 klj_bh_false},

    // ---- stopping something that is not running ----
    {BH, "turnOff",    "(Ljava/lang/String;)V", klj_bh_void},
    {BH, "turnOffAll", "()V",                   klj_bh_void},
    {BH, "ping",       "(Ljava/lang/String;)V", klj_bh_void},
    {BH, "pingAll",    "()V",                   klj_bh_void},
    {0}
};

#undef BH
