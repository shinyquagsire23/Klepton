#pragma once

#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "main.hpp"

class Hooks {
   private:
    static inline std::vector<void (*)()> installFuncs;

   public:
    static inline void AddInstallFunc(void (*installFunc)()) { installFuncs.push_back(installFunc); }

    static inline void Install() {
        logger.info("Installing hooks...");
        for (auto& func : installFuncs)
            func();
        logger.info("Installed all hooks!");
    }
};

#define AUTO_INSTALL_FUNCTION(name_)                                     \
    namespace {                                                          \
        struct Auto_Install_##name_ {                                    \
            static void Install();                                       \
            Auto_Install_##name_() { ::Hooks::AddInstallFunc(Install); } \
        };                                                               \
    }                                                                    \
    static Auto_Install_##name_ Auto_Install_Instance_##name_;           \
    void Auto_Install_##name_::Install()

#define AUTO_INSTALL_ORIG(name_) \
    AUTO_INSTALL_FUNCTION(Hook_##name_) { ::Hooking::InstallOrigHook<Hook_##name_>(logger); }

#define AUTO_INSTALL(name_) \
    AUTO_INSTALL_FUNCTION(Hook_##name_) { ::Hooking::InstallHook<Hook_##name_>(logger); }

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

#define MAKE_AUTO_HOOK_MATCH(name_, mPtr, retval, ...) \
    SC2_RECORD_HOOK(name_, mPtr, 0)                                                                                              \
    struct Hook_##name_ {                                                                                                                           \
        using funcType = retval (*)(__VA_ARGS__);                                                                                                   \
        static_assert(MATCH_HOOKABLE_ASSERT(mPtr));                                                                                                 \
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

#define MAKE_AUTO_ORIG_HOOK_MATCH(name_, mPtr, retval, ...) \
    SC2_RECORD_HOOK(name_, mPtr, 1)                                                                                         \
    struct Hook_##name_ {                                                                                                                           \
        using funcType = retval (*)(__VA_ARGS__);                                                                                                   \
        static_assert(MATCH_HOOKABLE_ASSERT(mPtr));                                                                                                 \
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
