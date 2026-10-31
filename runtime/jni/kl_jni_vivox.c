// BattleGroupVR2 reaches Vivox, its voice chat, after drawing its menu (fix 38,
// sent in by a user, 2026-10-13). The Android services Vivox sets up are not
// here; its init says so to managed code instead of the process stopping on
// the name. No voice session, and none pretended. For that one title
// (x9386827918099701), and for this one call. Onward (x2677344882310094) makes
// the same call, with the same dotted signature, right after its first scene
// loads, behind the start logo.
#include <string.h>
#include "guest/kl_driver.h"
#include "kl_jni_int.h"

static klj_val init_unavailable(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // VivoxNative.init delegates to JniHelpers.init, which sets up Android
    // services. Merely loading libVivoxNative/libvivox-sdk is not sufficient.
    // VxClient.Start calls this before native Initialize and message-pump
    // registration: throwing keeps the SDK from entering a partially started
    // state. No voice session, service or successful initialization is faked.
    klj_raise("java/lang/UnsupportedOperationException",
              "Vivox voice chat is unavailable: Android services are not implemented on this host");
    return (klj_val){0};
}

const klj_binding *klj_vivox_binding(const char *cls, const char *name, const char *sig) {
    static const klj_binding binding = {
        "com/vivox/vivoxnative/VivoxNative", "init", "(Landroid/content/Context;)V", init_unavailable
    };
    const char *target = kl_driver_target_name();
    if (!target || (strcmp(target, "x9386827918099701") && strcmp(target, "x2677344882310094")) ||
        strcmp(cls, binding.cls) || strcmp(name, binding.name)) return NULL;
    // Unity's reflected call in this guest uses Context's dotted class name.
    // Accept just that observed spelling and the canonical JNI signature.
    if (strcmp(sig, binding.sig) && strcmp(sig, "(Landroid.content.Context;)V")) return NULL;
    return &binding;
}
