// Vivox's unavailable Android backend must raise a managed-visible exception,
// never silently succeed or abort the process. No SDK, game or network needed.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "kl_jni.h"
#include "kl_jni_slots.h"
#include "kl_va.h"
#include "guest/kl_driver.h"

#define SLOT(n, name) JNI_##name = n,
enum { KL_JNI_SLOTS(SLOT) };
#undef SLOT
#define JNI(name, type) ((type)jni[JNI_##name])
#define VIVOX "com/vivox/vivoxnative/VivoxNative"
static void *env;
static void **jni;

static void call(const char *name, const char *sig, int expect_error) {
    kl_jni_local_frame_push();
    void *cls = kl_jni_class(VIVOX);
    void *m = JNI(GetStaticMethodID, void *(*)(void *, void *, const char *, const char *))
        (env, cls, name, sig);
    uint64_t arg = (uintptr_t)kl_jni_new_object("android/content/Context");
    kl_va a = {.gr_top = &arg + 1, .gr_offs = -8};
    JNI(CallStaticVoidMethodV, void (*)(void *, void *, void *, kl_va *))(env, cls, m, &a);
    void *e = JNI(ExceptionOccurred, void *(*)(void *))(env);
    assert((e != NULL) == expect_error);
    if (e) {
        assert(JNI(IsInstanceOf, uint8_t (*)(void *, void *, void *))
            (env, e, kl_jni_class("java/lang/UnsupportedOperationException")));
        JNI(ExceptionClear, void (*)(void *))(env);
        assert(!JNI(ExceptionOccurred, void *(*)(void *))(env));
    }
    assert(!kl_jni_pending_ui_tasks());
    kl_jni_local_frame_pop();
}
int main(void) {
    env = kl_jni_env(); jni = *(void ***)env;
    kl_jni_set_permissive(1); // Observe unhandled calls without process aborts.
    call("init", "(Landroid/content/Context;)V", 0);
    const kl_target other = {.name = "jni-vivox-test-other"};
    const kl_target target = {.name = "x9386827918099701"};
    kl_driver_init(&other, NULL, 0);
    call("init", "(Landroid/content/Context;)V", 0);
    call("init", "(Landroid.content.Context;)V", 0);
    kl_driver_init(&target, NULL, 0);
    call("init", "(Ljava/lang/String;)V", 0); // Different overload stays unhandled.
    call("shutdown", "(Landroid/content/Context;)V", 0);
    kl_jni_set_permissive(0);
    for (int i = 0; i < 3; i++) {
        call("init", "(Landroid/content/Context;)V", 1);
        call("init", "(Landroid.content.Context;)V", 1);
    }
    puts("PASS: Vivox init reports an explicit exception, survives retries, and remains limited to BattleGroupVR2 and the observed signatures.");
    return 0;
}
