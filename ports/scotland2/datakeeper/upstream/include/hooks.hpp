#pragma once

#include "main.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"

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

template <auto C, class T>
struct FunctionToMethod {
    using type = T;
};
template <auto C, class R, class T, class... Ts>
requires(C((R(T::*)(Ts...)) nullptr))
struct FunctionToMethod<C, R (*)(T*, Ts...)> {
    using type = R (T::*)(Ts...);
};
template <auto C, class R, class T, class... Ts>
requires(C((R(T::*)(Ts...)) nullptr))
struct FunctionToMethod<C, R (*)(T, Ts...)> {
    using type = R (T::*)(Ts...);
};

#define AUTO_HOOK_FUNCTION(name_)                                        \
    namespace {                                                          \
        struct Auto_Install_##name_ {                                    \
            static void Install();                                       \
            Auto_Install_##name_() { ::Hooks::AddInstallFunc(Install); } \
        };                                                               \
    }                                                                    \
    static Auto_Install_##name_ Auto_Install_Instance_##name_;           \
    void Auto_Install_##name_::Install()

#define AUTO_INSTALL_ORIG(name_) \
    AUTO_HOOK_FUNCTION(Hook_##name_) { ::Hooking::InstallOrigHook<Hook_##name_>(logger); }

#define AUTO_INSTALL(name_) \
    AUTO_HOOK_FUNCTION(Hook_##name_) { ::Hooking::InstallHook<Hook_##name_>(logger); }

#ifdef SC2_AOT_HOOKS
struct SC2BuildHookTarget { uint64_t address; uint32_t originalHook,reserved; char name[128]; };
static_assert(sizeof(SC2BuildHookTarget)==144);
#define SC2_RECORD_HOOK(name_, mPtr, orig_) \
    static inline constexpr SC2BuildHookTarget sc2_target __attribute__((used,section(".sc2_hooks"))) = { \
        ::il2cpp_utils::il2cpp_type_check::MetadataGetter<mPtr>::addrs, orig_, 0, #name_ };
#else
#define SC2_RECORD_HOOK(name_, mPtr, orig_)
#endif

#define MAKE_AUTO_HOOK_MATCH(name_, mPtr, retval, ...)                                                                                           \
    struct Hook_##name_ {                                                                                                                        \
        static constexpr auto matchTest = []<class T>(T t) { return requires { static_cast<T>(mPtr); }; };                                       \
        using funcType = retval (*)(__VA_ARGS__);                                                                                                \
        using mType = FunctionToMethod<matchTest, funcType>::type;                                                                               \
        static constexpr auto matches = matchTest((mType) nullptr);                                                                              \
        SC2_RECORD_HOOK(name_, static_cast<mType>(mPtr), 0) \
        static_assert(MATCH_HOOKABLE_ASSERT(static_cast<mType>(mPtr)));                                                                          \
        constexpr static const char* name() { return #name_; }                                                                                   \
        static const MethodInfo* getInfo() { return ::il2cpp_utils::il2cpp_type_check::MetadataGetter<static_cast<mType>(mPtr)>::methodInfo(); } \
        static funcType* trampoline() { return &name_; }                                                                                         \
        static inline retval (*name_)(__VA_ARGS__) = nullptr;                                                                                    \
        static funcType hook() { return &::Hooking::HookCatchWrapper<&hook_##name_, funcType>::wrapper; }                                        \
        static retval hook_##name_(__VA_ARGS__);                                                                                                 \
    };                                                                                                                                           \
    static_assert(Hook_##name_::matches, "Hook method signature does not match!");                                                               \
    AUTO_INSTALL(name_)                                                                                                                          \
    retval Hook_##name_::hook_##name_(__VA_ARGS__)

#define MAKE_AUTO_ORIG_HOOK_MATCH(name_, mPtr, retval, ...)                                                                                      \
    struct Hook_##name_ {                                                                                                                        \
        static constexpr auto matchTest = []<class T>(T t) { return requires { static_cast<T>(mPtr); }; };                                       \
        using funcType = retval (*)(__VA_ARGS__);                                                                                                \
        using mType = FunctionToMethod<matchTest, funcType>::type;                                                                               \
        static constexpr auto matches = matchTest((mType) nullptr);                                                                              \
        SC2_RECORD_HOOK(name_, static_cast<mType>(mPtr), 1) \
        static_assert(MATCH_HOOKABLE_ASSERT(static_cast<mType>(mPtr)));                                                                          \
        constexpr static const char* name() { return #name_; }                                                                                   \
        static const MethodInfo* getInfo() { return ::il2cpp_utils::il2cpp_type_check::MetadataGetter<static_cast<mType>(mPtr)>::methodInfo(); } \
        static funcType* trampoline() { return &name_; }                                                                                         \
        static inline retval (*name_)(__VA_ARGS__) = nullptr;                                                                                    \
        static funcType hook() { return &::Hooking::HookCatchWrapper<&hook_##name_, funcType>::wrapper; }                                        \
        static retval hook_##name_(__VA_ARGS__);                                                                                                 \
    };                                                                                                                                           \
    static_assert(Hook_##name_::matches, "Hook method signature does not match!");                                                               \
    AUTO_INSTALL_ORIG(name_)                                                                                                                     \
    retval Hook_##name_::hook_##name_(__VA_ARGS__)

void Camera_Pause();
void Camera_Unpause();

namespace UnityEngine {
    class Camera;
}
extern UnityEngine::Camera* mainCamera;
