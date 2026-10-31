#include "beatsaber-hook/shared/utils/hooking.hpp"
#include <cstdint>
extern "C" int sc2_aot_restore(void *);
namespace {
MethodInfo info;
#define SDK_HOOK(TYPE,AMOUNT) \
struct TYPE { \
    using funcType=uint64_t (*)(uint64_t); \
    static inline funcType original; \
    static const char *name() { return #TYPE; } \
    static const MethodInfo *getInfo() { return &info; } \
    static funcType *trampoline() { return &original; } \
    static funcType hook() { return replacement; } \
    static uint64_t replacement(uint64_t value) { return original(value)+AMOUNT; } \
};
SDK_HOOK(Outer,1000)
SDK_HOOK(Beneath1,100)
SDK_HOOK(Beneath2,10)
}
extern "C" __attribute__((visibility("default")))
int sc2_test_sdk_original_hooks(void *target) {
    auto call=reinterpret_cast<Outer::funcType>(target);
    if (call(0)!=42) return 1;
    info.methodPointer=reinterpret_cast<Il2CppMethodPointer>(target);
    auto logger=Paper::ConstLoggerContext("Scotland2SdkHooks");
    Hooking::InstallHook<Outer>(logger);
    if (call(0)!=1042 || HookTracker::GetOrig(target)!=reinterpret_cast<void *>(Outer::original)) return 2;
    Hooking::InstallOrigHook<Beneath1>(logger);
    if (call(0)!=1142 || HookTracker::GetOrig(target)!=reinterpret_cast<void *>(Beneath1::original)) return 3;
    Hooking::InstallOrigHook<Beneath2>(logger);
    if (call(0)!=1152 || HookTracker::GetOrig(target)!=reinterpret_cast<void *>(Beneath2::original)) return 4;
    if (sc2_aot_restore(reinterpret_cast<void *>(Beneath1::original)) ||
        sc2_aot_restore(reinterpret_cast<void *>(Outer::original)) || sc2_aot_restore(target)) return 5;
    HookTracker::RemoveHooks(target);
    return call(0)==42 && HookTracker::GetOrig(target)==target?0:6;
}
