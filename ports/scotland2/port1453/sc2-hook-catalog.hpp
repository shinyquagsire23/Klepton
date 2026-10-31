#pragma once
#include <cstdint>

// Emitted from the same overload resolution used by the original hook macro.
// Flamingo keeps original/final ordering in its own priority graph.
struct SC2BuildHookTarget1453 {
    std::uint64_t address;
    std::uint32_t originalHook, reserved;
    char name[128];
};
static_assert(sizeof(SC2BuildHookTarget1453) == 144);

#ifdef SC2_AOT_HOOKS
#define SC2_RECORD_MATCH_1453(name_, method_, cast_) \
    static inline const SC2BuildHookTarget1453 sc2_build_target \
        __attribute__((used, section(".sc2_hooks"))) = { \
            ::i2c::metadata_getter<static_cast<cast_>(method_)>::addrs, 0, 0, #name_ };
#else
#define SC2_RECORD_MATCH_1453(name_, method_, cast_)
#endif
