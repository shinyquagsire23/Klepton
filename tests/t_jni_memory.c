// Memory-pressure delivery through registered guest natives, and
// BattleGroupVR2's own memory defaults. No game content.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "kl_jni.h"
#include "kl_jni_slots.h"
#include "klepton.h"
#include "guest/kl_driver.h"

#define SLOT(n, name) JNI_##name = n,
enum { KL_JNI_SLOTS(SLOT) };
#undef SLOT
#define JNI(name, type) ((type)jni[JNI_##name])
#define PLAYER "com/unity3d/player/UnityPlayer"
#define MODERN PLAYER "ForActivityOrService"
static void *env, *player;
static void **jni;
static int legacy_calls, modern_calls;
static const kl_target other = {.name = "jni-memory-test-other"};
static const kl_target target = {.name = "x9386827918099701"};

static void init(void) { env = kl_jni_env(); jni = *(void ***)env; }
static void reg(const char *cls, const char *name, const char *sig, void *fn) {
    kl_jni_method m = {name, sig, fn};
    assert(JNI(RegisterNatives, kl_jint (*)(void *, void *, const kl_jni_method *, kl_jint))
        (env, kl_jni_class(cls), &m, 1) == 0);
}
static void wrong(void) { assert(!"incompatible native must never be invoked"); }
static void legacy(void *e, void *cls) {
    assert(e == env && cls == kl_jni_class(PLAYER));
    legacy_calls++;
}
static void modern(void *e, void *receiver, kl_jint level) {
    assert(e == env && receiver == player && level == 4);
    // JNI re-entry must work: the callback cannot run under the registry lock.
    assert(JNI(GetObjectClass, void *(*)(void *, void *))(e, receiver) == kl_jni_class(PLAYER));
    modern_calls++;
}
// A game with nativeLowMemory is told through it, as before — also where the
// newer callback is registered beside it, and for the title the newer one is
// switched on for.
static void legacy_test(void) {
    init();
    assert(!kl_jni_low_memory());
    reg(PLAYER, "nativeLowMemory", "()V", legacy);
    assert(kl_jni_low_memory() && legacy_calls == 1);
    reg(MODERN, "nativeMemoryUsageChanged", "(I)V", wrong);
    kl_driver_init(&target, NULL, 0);
    player = kl_jni_unity_player();
    assert(kl_jni_low_memory() && legacy_calls == 2);
    kl_mem_pressure_fire("test");
    assert(legacy_calls == 3);
}
int main(void) {
    // Separate native registries: UnregisterNatives is intentionally inert here.
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) { legacy_test(); _exit(0); }
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));

    init();
    unsetenv("KL_LOWMEM_USAGE_CHANGED");
    assert(!kl_jni_low_memory());
    reg("test/Other", "nativeMemoryUsageChanged", "(I)V", wrong);
    reg(MODERN, "nativeMemoryUsageChanged", "()V", wrong);
    kl_driver_init(&target, NULL, 0);
    assert(!kl_jni_low_memory());
    reg(MODERN, "nativeMemoryUsageChanged", "(I)V", modern);
    assert(!kl_jni_low_memory()); // Registered, but the player is not made yet.
    kl_jni_local_frame_push();
    player = kl_jni_unity_player();
    kl_jni_local_frame_pop(); // The player must survive the frame it was made in.
    assert(kl_jni_low_memory() && modern_calls == 1);
    kl_mem_pressure_fire("test");
    assert(modern_calls == 2);
    // The switch, both ways; and no other title is told through this callback.
    setenv("KL_LOWMEM_USAGE_CHANGED", "0", 1);
    assert(!kl_jni_low_memory() && modern_calls == 2);
    unsetenv("KL_LOWMEM_USAGE_CHANGED");
    kl_driver_init(&other, NULL, 0);
    assert(!kl_jni_low_memory() && modern_calls == 2);
    setenv("KL_LOWMEM_USAGE_CHANGED", "1", 1);
    assert(kl_jni_low_memory() && modern_calls == 3);
    unsetenv("KL_LOWMEM_USAGE_CHANGED");
    // The lifecycle natives' class is a UnityPlayer.
    kl_jni_local_frame_push();
    assert(JNI(IsInstanceOf, uint8_t (*)(void *, void *, void *))
        (env, kl_jni_new_object(MODERN), kl_jni_class(PLAYER)));
    kl_jni_local_frame_pop();

    // The title's memory defaults: its alone, and the user's value stands.
    const char *keys[] = {"MVK_CONFIG_USE_MTLHEAP", "KL_LOWMEM_FREE_MB"};
    const char *defaults[] = {"0", "1536"};
    const unsigned nkeys = sizeof keys / sizeof *keys;
    unsetenv("KL_OVRP_EYE_MAX");
    for (unsigned i = 0; i < nkeys; i++) unsetenv(keys[i]);
    kl_driver_init(&other, NULL, 0);
    for (unsigned i = 0; i < nkeys; i++) assert(!getenv(keys[i]));
    kl_driver_init(&target, NULL, 0);
    assert(!getenv("KL_OVRP_EYE_MAX")); // The eye size remains the user's choice.
    for (unsigned i = 0; i < nkeys; i++) {
        assert(!strcmp(getenv(keys[i]), defaults[i]));
        setenv(keys[i], "42", 1);
    }
    kl_driver_init(&target, NULL, 0);
    for (unsigned i = 0; i < nkeys; i++) assert(!strcmp(getenv(keys[i]), "42"));
    puts("PASS: Unity memory callbacks (nativeLowMemory first, Unity 6.2's for BattleGroupVR2 only), the title's memory defaults and user overrides.");
    return 0;
}
