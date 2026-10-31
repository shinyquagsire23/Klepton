// What a UE 4.21 guest (Robo Recall: Unplugged) asks of the runtime before its
// first frame, answered without the game. `make ue421`, in `make check`.
//
//   1. native_app_glue's print_cur_config(): every AConfiguration getter it
//      reads resolves, the "not specified" ones answer 0 (ACONFIGURATION_*_ANY),
//      and the SDK version is the one Build.VERSION.SDK_INT answers.
//   2. zlib's one-shot compress() resolves to a working forward.
//   3. MediaPlayer14's two-flag constructor, (ZZ)V, builds the runtime's player:
//      a later setLooping/isLooping round-trip only holds on that object.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include "klepton.h"
#include "kl_jni.h"
#include "kl_jni_slots.h"
#include "guest/kl_ndk.h"

#define SLOT(n, name) JNI_##name = n,
enum { KL_JNI_SLOTS(SLOT) };
#undef SLOT
typedef union { uint8_t z; int32_t i; int64_t j; void *l; double d; } jvalue;
static void *env;
static void **jni;
#define JNI(name, type) ((type)jni[JNI_##name])

static int g_fail;
static void ck(int ok, const char *what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail = 1;
}

static void *method(const char *cls, const char *name, const char *sig) {
    return JNI(GetMethodID, void *(*)(void *, void *, const char *, const char *))
        (env, kl_jni_class(cls), name, sig);
}

int main(void) {
    printf("=== UE 4.21 boot surface ===\n");

    // ---- 1. AConfiguration ----
    static const char *any[] = {
        "AConfiguration_getMcc", "AConfiguration_getMnc", "AConfiguration_getOrientation",
        "AConfiguration_getTouchscreen", "AConfiguration_getDensity",
        "AConfiguration_getKeyboard", "AConfiguration_getNavigation",
        "AConfiguration_getKeysHidden", "AConfiguration_getNavHidden",
        "AConfiguration_getScreenSize", "AConfiguration_getScreenLong",
        "AConfiguration_getUiModeType", "AConfiguration_getUiModeNight",
    };
    char cfg[256] = {0};   // what AConfiguration_new hands out is never read by these
    int resolved = 0, zero = 0;
    for (size_t i = 0; i < sizeof any / sizeof any[0]; i++) {
        int32_t (*fn)(void *) = (int32_t (*)(void *))kl_ndk_lookup(any[i]);
        if (!fn) { printf("  FAIL %s does not resolve\n", any[i]); g_fail = 1; continue; }
        resolved++;
        if (fn(cfg) == 0) zero++;
        else { printf("  FAIL %s is not ACONFIGURATION_*_ANY\n", any[i]); g_fail = 1; }
    }
    printf("  %d of %zu \"not specified\" getters resolve, %d answer 0\n",
           resolved, sizeof any / sizeof any[0], zero);
    int32_t (*sdk)(void *) = (int32_t (*)(void *))kl_ndk_lookup("AConfiguration_getSdkVersion");
    ck(sdk && sdk(cfg) == kl_jni_build_int("SDK_INT", 29),
       "AConfiguration_getSdkVersion is Build.VERSION.SDK_INT");

    // ---- 2. compress() ----
    int (*comp)(Bytef *, uLongf *, const Bytef *, uLong) =
        (int (*)(Bytef *, uLongf *, const Bytef *, uLong))kl_shim_lookup("compress");
    static const char text[] = "Robo Recall: Unplugged, UE 4.21.2, compress() round trip";
    Bytef packed[256], back[256];
    uLongf plen = sizeof packed, blen = sizeof back;
    int ok = comp && comp(packed, &plen, (const Bytef *)text, sizeof text) == Z_OK &&
             uncompress(back, &blen, packed, plen) == Z_OK &&
             blen == sizeof text && memcmp(back, text, sizeof text) == 0;
    ck(ok, "compress resolves and round-trips through uncompress");

    // ---- 3. MediaPlayer14(ZZ)V ----
    env = kl_jni_env(); jni = *(void ***)env;
    kl_jni_local_frame_push();
    const char *mp = "com/epicgames/ue4/MediaPlayer14";
    void *player = JNI(NewObjectA, void *(*)(void *, void *, void *, const jvalue *))
        (env, kl_jni_class(mp), method(mp, "<init>", "(ZZ)V"),
         (jvalue[]){{.z = 0}, {.z = 0}});
    JNI(CallVoidMethodA, void (*)(void *, void *, void *, const jvalue *))
        (env, player, method(mp, "setLooping", "(Z)V"), (jvalue[]){{.z = 1}});
    uint8_t looping = player ? JNI(CallBooleanMethodA,
        uint8_t (*)(void *, void *, void *, const jvalue *))
        (env, player, method(mp, "isLooping", "()Z"), NULL) : 0;
    ck(player && looping, "MediaPlayer14(ZZ)V builds the runtime's player");

    printf("%s: UE 4.21 boot surface\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
