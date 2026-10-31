#pragma once

#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/utils/logging.hpp"
#include <vector>

class Hooks
{
private:
    inline static std::vector<void (*)()> installFuncs;

public:
    static void AddInstallFunc(void (*installFunc)())
    {
        installFuncs.push_back(installFunc);
    }

    static inline void InstallHooks()
    {
        for (auto& func : installFuncs)
            func();
    }
};

#define AUTO_INSTALL_ORIG(name_)                                                                    \
    struct Auto_Hook_##name_                                                                        \
    {                                                                                               \
        static void Auto_Hook_##name_##_Install() {                                                 \
            static constexpr auto logger = Paper::ConstLoggerContext(MOD_ID "_Install_" #name_);    \
            ::Hooking::InstallOrigHook<Hook_##name_>(logger);                                       \
        }                                                                                           \
        Auto_Hook_##name_() { ::Hooks::AddInstallFunc(Auto_Hook_##name_##_Install); }               \
    };                                                                                              \
    static Auto_Hook_##name_ Auto_Hook_Instance_##name_;

#define AUTO_INSTALL(name_)                                                                         \
    struct Auto_Hook_##name_                                                                        \
    {                                                                                               \
        static void Auto_Hook_##name_##_Install() {                                                 \
            static constexpr auto logger = Paper::ConstLoggerContext(MOD_ID "_Install_" #name_);    \
            ::Hooking::InstallHook<Hook_##name_>(logger);                                           \
        }                                                                                           \
        Auto_Hook_##name_() { ::Hooks::AddInstallFunc(Auto_Hook_##name_##_Install); }               \
    };                                                                                              \
    static Auto_Hook_##name_ Auto_Hook_Instance_##name_;

#ifdef SC2_AOT_HOOKS
struct SC2BuildHookTarget {
    uint64_t address;
    uint32_t originalHook, reserved;
    char name[128];
};
static_assert(sizeof(SC2BuildHookTarget)==144);
#define SC2_RECORD_HOOK(name_, mPtr, orig_) \
    static const SC2BuildHookTarget sc2_target_##name_ __attribute__((used,section(".sc2_hooks"))) = { \
        ::il2cpp_utils::il2cpp_type_check::MetadataGetter<mPtr>::addrs, orig_, 0, #name_ };
#else
#define SC2_RECORD_HOOK(name_, mPtr, orig_)
#endif

#define MAKE_AUTO_HOOK_MATCH(name_, mPtr, retval, ...)                                                                                              \
    SC2_RECORD_HOOK(name_, mPtr, 0) \
    struct Hook_##name_                                                                                                                             \
    {                                                                                                                                               \
        using funcType = retval (*)(__VA_ARGS__);                                                                                                   \
        static_assert(std::is_same_v<funcType, ::Hooking::InternalMethodCheck<decltype(mPtr)>::funcType>, "Hook method signature does not match!"); \
        constexpr static const char* name() { return #name_; }                                                                                      \
        static const MethodInfo* getInfo() { return ::il2cpp_utils::il2cpp_type_check::MetadataGetter<mPtr>::methodInfo(); }                        \
        static funcType* trampoline() { return &name_; }                                                                                            \
        static inline retval (*name_)(__VA_ARGS__) = nullptr;                                                                                       \
        static funcType hook() { return &::Hooking::HookCatchWrapper<&hook_##name_, funcType>::wrapper; }                                           \
        static retval hook_##name_(__VA_ARGS__);                                                                                                    \
    };                                                                                                                                              \
    AUTO_INSTALL(name_)                                                                                                                             \
    retval Hook_##name_::hook_##name_(__VA_ARGS__)

#define MAKE_AUTO_HOOK_ORIG_MATCH(name_, mPtr, retval, ...)                                                                                         \
    SC2_RECORD_HOOK(name_, mPtr, 1) \
    struct Hook_##name_                                                                                                                             \
    {                                                                                                                                               \
        using funcType = retval (*)(__VA_ARGS__);                                                                                                   \
        static_assert(std::is_same_v<funcType, ::Hooking::InternalMethodCheck<decltype(mPtr)>::funcType>, "Hook method signature does not match!"); \
        constexpr static const char* name() { return #name_; }                                                                                      \
        static const MethodInfo* getInfo() { return ::il2cpp_utils::il2cpp_type_check::MetadataGetter<mPtr>::methodInfo(); }                        \
        static funcType* trampoline() { return &name_; }                                                                                            \
        static inline retval (*name_)(__VA_ARGS__) = nullptr;                                                                                       \
        static funcType hook() { return &::Hooking::HookCatchWrapper<&hook_##name_, funcType>::wrapper; }                                           \
        static retval hook_##name_(__VA_ARGS__);                                                                                                    \
    };                                                                                                                                              \
    AUTO_INSTALL_ORIG(name_)                                                                                                                        \
    retval Hook_##name_::hook_##name_(__VA_ARGS__)
