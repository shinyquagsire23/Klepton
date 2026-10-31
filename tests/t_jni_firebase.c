// Exercise Firebase's startup/teardown and explicit backend failure via JNI.
// Synthetic SDK names only: no game, network, credentials, or filesystem.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "kl_jni.h"
#include "kl_jni_slots.h"
#include "kl_va.h"
#include "guest/kl_driver.h"
#include "jni/kl_jni_int.h"

#define SLOT(n, name) JNI_##name = n,
enum { KL_JNI_SLOTS(SLOT) };
#undef SLOT
#define JNI(name, type) ((type)jni[JNI_##name])
#define REG "com/google/firebase/platforminfo/GlobalLibraryVersionRegistrar"
#define APP "com/google/firebase/FirebaseApp"
#define OPTIONS "com/google/firebase/FirebaseOptions"
#define GOOGLE "com/google/android/gms/common/GoogleApiAvailability"
#define HELPER "com/google/firebase/app/internal/cpp/GoogleApiAvailabilityHelper"
#define GAMESTOTY "com/Gamestoty/GamestotyJava"
#define GS_INIT "(Ljava/lang/String;Ljava/lang/String;)V"
#define GS_EVENT "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V"
static void *env;
static void **jni;
static kl_va args(uint64_t *values, int count) {
    return (kl_va){.gr_top = values + count, .gr_offs = -8 * count};
}
static void *method(const char *cls, const char *name, const char *sig, int is_static) {
    int slot = is_static ? JNI_GetStaticMethodID : JNI_GetMethodID;
    return ((void *(*)(void *, void *, const char *, const char *))jni[slot])
        (env, kl_jni_class(cls), name, sig);
}
static void *get(const char *cls, const char *name, const char *sig, kl_va *values) {
    return JNI(CallStaticObjectMethodV, void *(*)(void *, void *, void *, kl_va *))
        (env, kl_jni_class(cls), method(cls, name, sig, 1), values);
}
static void exception(const char *cls) {
    void *e = JNI(ExceptionOccurred, void *(*)(void *))(env);
    assert((e != NULL) == (cls != NULL));
    if (!e) return;
    assert(JNI(IsInstanceOf, uint8_t (*)(void *, void *, void *))
        (env, e, kl_jni_class(cls)));
    JNI(ExceptionClear, void (*)(void *))(env);
    JNI(DeleteLocalRef, void (*)(void *, void *))(env, e);
}
static void shutdown_sdk(void) {
    const char *classes[] = {HELPER, "com/google/firebase/app/internal/cpp/Log"};
    const char *methods[] = {"stopCallbacks", "shutdown"};
    for (int i = 0; i < 2; i++) {
        kl_va none = {0};
        JNI(CallStaticVoidMethodV, void (*)(void *, void *, void *, kl_va *))
            (env, kl_jni_class(classes[i]), method(classes[i], methods[i], "()V", 1), &none);
        exception(NULL);
    }
}
int main(void) {
    env = kl_jni_env(); jni = *(void ***)env;
    kl_va none = {0};
    // Prove the targeted policy does not answer before a target is selected,
    // or for another title. Permissive mode lets us observe an unhandled call.
    kl_jni_set_permissive(1);
    assert(!get(REG, "getInstance", "()L" REG ";", &none)); exception(NULL);
    const kl_target other = {.name = "jni-firebase-test-other"};
    kl_driver_init(&other, NULL, 0);
    assert(!get(REG, "getInstance", "()L" REG ";", &none)); exception(NULL);
    assert(!get(GOOGLE, "getInstance", "()L" GOOGLE ";", &none)); exception(NULL);
    assert(!klj_firebase_binding(GAMESTOTY, "Init", GS_INIT));
    kl_jni_set_permissive(0);

    // BattleGroupVR2 and Shave & Stuff both reach Firebase's C++ registration
    // path before their first scene and get the same backend-unavailable
    // answers. Gamestoty, Shave & Stuff's own analytics bridge, is that title's
    // alone: BattleGroupVR2 must still see it unbound.
    const char *targets[] = {"x9386827918099701", "x7532973430077887"};
    void *identity = NULL;
    for (int t = 0; t < 2; t++) {
    const kl_target target = {.name = targets[t]};
    kl_driver_init(&target, NULL, 0);
    int shave = t == 1;
    assert(!klj_firebase_binding(GAMESTOTY, "Init", GS_INIT) == !shave);
    assert(!klj_firebase_binding(GAMESTOTY, "SendEvent", GS_EVENT) == !shave);
    if (shave) {
        kl_va init = args((uint64_t[]){(uintptr_t)kl_jni_new_string("game-key"),
                                      (uintptr_t)kl_jni_new_string("build")}, 2);
        JNI(CallStaticVoidMethodV, void (*)(void *, void *, void *, kl_va *))
            (env, kl_jni_class(GAMESTOTY), method(GAMESTOTY, "Init", GS_INIT, 1), &init);
        exception(NULL);
        kl_va event = args((uint64_t[]){(uintptr_t)kl_jni_new_string("scene-loaded"),
                                       (uintptr_t)kl_jni_new_string("{}"),
                                       (uintptr_t)kl_jni_new_string("[]")}, 3);
        JNI(CallStaticVoidMethodV, void (*)(void *, void *, void *, kl_va *))
            (env, kl_jni_class(GAMESTOTY), method(GAMESTOTY, "SendEvent", GS_EVENT, 1), &event);
        exception(NULL);
    }
    for (int cycle = 0; cycle < 3; cycle++) {
        kl_jni_local_frame_push();
        void *reg = get(REG, "getInstance", "()L" REG ";", &none);
        assert(reg && (!identity || identity == reg)); identity = reg;
        exception(NULL);
        kl_va v = args((uint64_t[]){(uintptr_t)kl_jni_new_string("test-sdk"),
                                   (uintptr_t)kl_jni_new_string("1.0")}, 2);
        JNI(CallVoidMethodV, void (*)(void *, void *, void *, kl_va *))
            (env, reg, method(REG, "registerVersion", "(Ljava/lang/String;Ljava/lang/String;)V", 0), &v);
        exception(NULL);
        v = args((uint64_t[]){0, 0}, 2);
        JNI(CallVoidMethodV, void (*)(void *, void *, void *, kl_va *))
            (env, reg, method(REG, "registerVersion", "(Ljava/lang/String;Ljava/lang/String;)V", 0), &v);
        exception("java/lang/NullPointerException");
        shutdown_sdk();

        v = args((uint64_t[]){(uintptr_t)kl_jni_new_object("android/content/Context")}, 1);
        assert(!get(OPTIONS, "fromResource", "(Landroid/content/Context;)L" OPTIONS ";", &v));
        exception("java/lang/UnsupportedOperationException");
        // Both the dependency check and repair paths request this singleton.
        for (int i = 0; i < 2; i++) {
            assert(!get(GOOGLE, "getInstance", "()L" GOOGLE ";", &none));
            exception("java/lang/UnsupportedOperationException");
        }
        assert(!get(APP, "getInstance", "()L" APP ";", &none));
        exception("java/lang/IllegalStateException");
        v = args((uint64_t[]){(uintptr_t)kl_jni_new_string("[DEFAULT]")}, 1);
        assert(!get(APP, "getInstance", "(Ljava/lang/String;)L" APP ";", &v));
        exception("java/lang/IllegalStateException");
        shutdown_sdk();
        assert(kl_jni_pending_ui_tasks() == 0);
        kl_jni_local_frame_pop();
    }
    }
    puts("PASS: Firebase startup/teardown survives repeated calls; unavailable backend returns "
         "errors only for BattleGroupVR2 and Shave & Stuff; Gamestoty is Shave & Stuff's alone.");
    return 0;
}
