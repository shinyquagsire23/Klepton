#pragma once

#include <functional>
#include "./_config.h"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"
#include "macros.hpp"


namespace custom_types {

CUSTOM_TYPES_EXPORT int get_delegate_count();

template <typename T>
using make_boxed_t = std::conditional_t<il2cpp_utils::il2cpp_type_check::need_box<T>::value, T*, T>;

template <typename T>
auto unbox_arg(T arg) {
    if constexpr (std::is_pointer_v<T> && il2cpp_utils::il2cpp_type_check::need_box<std::remove_pointer_t<T>>::value) {
        return *arg;
    } else {
        return arg;
    }
}

/// @brief The wrapper for an invokable delegate without an existing context.
/// DO NOT ATTEMPT TO CREATE THIS YOURSELF!
/// @tparam R The return type of the function being called.
/// @tparam TArgs The argument types of the function being called.
template <class RI, class... TArgsI>
struct DelegateWrapperStatic : Il2CppObject {
    // CT Boilerplate
    using ___TargetType = DelegateWrapperStatic<RI, TArgsI...>;
    constexpr static auto ___Base__Size = sizeof(Il2CppObject);
    friend ::custom_types::Register;

   public:
    struct ___TypeRegistration : ::custom_types::TypeRegistration {
        ___TypeRegistration() {
            ::custom_types::Register::AddType(this);
            ::custom_types::logger.debug("Adding delegate type: {}", name());
            instance = this;
        }
        std::vector<::custom_types::FieldRegistrator*> const getFields() const override {
            return {};
        }
        std::vector<::custom_types::StaticFieldRegistrator*> const getStaticFields() const override {
            return {};
        }
        std::vector<::custom_types::MethodRegistrator*> const getMethods() const override {
            return { &___Invoke_MethodRegistrator, &___dtor_MethodRegistrator, &___InvokeUnboxed_MethodRegistrator };
        }
        static inline char* st_fields;
        char*& static_fields() override {
            return st_fields;
        }
        size_t static_fields_size() const override {
            return 0;
        }
        const char* name() const override {
            if constexpr (!std::is_same_v<void, RI>) {
                static std::string nm{ fmt::format("DelegateWrapper({} ret, {} args, {} sz) ({})", sizeof(RI), sizeof...(TArgsI), sizeof(___TargetType), custom_types::get_delegate_count()) };
                return nm.c_str();
            } else {
                static std::string nm{ fmt::format("DelegateWrapper(void ret, {} args, {} sz) ({})", sizeof...(TArgsI), sizeof(___TargetType), custom_types::get_delegate_count()) };
                return nm.c_str();
            }
        }
        constexpr const char* namespaze() const override {
            return "CustomTypes";
        }
        constexpr const char* dllName() const override {
            return "CustomTypes.dll";
        }
        Il2CppClass* baseType() const override {
            ::il2cpp_functions::Init();
            return ::il2cpp_functions::defaults->object_class;
        }
        std::vector<Il2CppClass*> const interfaces() const override {
            return {};
        }
        constexpr Il2CppTypeEnum typeEnum() const override {
            return IL2CPP_TYPE_CLASS;
        }
        constexpr uint32_t typeFlags() const override {
            return 0;
        }
        static inline Il2CppClass* klass_ptr;
        Il2CppClass*& klass() const override {
            return klass_ptr;
        }
        constexpr size_t size() const override {
            return sizeof(___TargetType);
        }
        TypeRegistration* customBase() const override {
            return nullptr;
        }
        bool initialized() const override {
            return init;
        }
        void setInitialized() const override {
            init = true;
        }
        static inline bool init = false;
        static inline TypeRegistration* instance;
        static TypeRegistration* get() {
            return instance;
        }
    };

   public:
    static inline ___TypeRegistration __registration_instance_DelegateWrapperStatic;
    // Disallow creation
    DelegateWrapperStatic() = delete;
    // The (potentially capturing) function to invoke
    std::function<RI(TArgsI...)> wrappedFunc;

    // The invoke method that wraps the delegate call
    static RI Invoke(DelegateWrapperStatic<RI, TArgsI...>* instance, make_boxed_t<TArgsI>... args);
    static RI InvokeUnboxed(DelegateWrapperStatic<RI, TArgsI...>* instance, TArgsI... args);

   private:
    struct ___MethodRegistrator_Invoke : ::custom_types::MethodRegistrator {
        constexpr const char* name() const override {
            return "Invoke";
        }
        constexpr const char* csharpName() const override {
            return "Invoke";
        }
        int flags() const override {
            return METHOD_ATTRIBUTE_STATIC;
        }
        const MethodInfo* virtualMethod() const override {
            return nullptr;
        }
        const Il2CppType* returnType() const override {
            return ::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_type<RI>::get();
        }
        std::vector<const Il2CppType*> params() const override {
            il2cpp_functions::Init();
            return { ::il2cpp_functions::class_get_type(___TypeRegistration::klass_ptr), (::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_type<TArgsI>::get())... };
        }
        uint8_t params_size() const override {
            return sizeof...(TArgsI) + 1;
        }
        Il2CppMethodPointer methodPointer() const override {
            return reinterpret_cast<Il2CppMethodPointer>(&___TargetType::Invoke);
        }
        InvokerMethod invoker() const override {
            return &::custom_types::invoker_creator<decltype(&___TargetType::Invoke)>::invoke;
        }
    };

    struct ___MethodRegistrator_InvokeUnboxed : ::custom_types::MethodRegistrator {
        constexpr const char* name() const override {
            return "InvokeUnboxed";
        }
        constexpr const char* csharpName() const override {
            return "InvokeUnboxed";
        }
        int flags() const override {
            return METHOD_ATTRIBUTE_STATIC;
        }
        const MethodInfo* virtualMethod() const override {
            return nullptr;
        }
        const Il2CppType* returnType() const override {
            ::il2cpp_functions::Init();
            return ::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_type<RI>::get();
        }
        std::vector<const Il2CppType*> params() const override {
            il2cpp_functions::Init();
            return { ::il2cpp_functions::class_get_type(___TypeRegistration::klass_ptr), (::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_type<TArgsI>::get())... };
        }
        uint8_t params_size() const override {
            return sizeof...(TArgsI) + 1;
        }
        Il2CppMethodPointer methodPointer() const override {
            return reinterpret_cast<Il2CppMethodPointer>(&___TargetType::InvokeUnboxed);
        }
        InvokerMethod invoker() const override {
            return &::custom_types::invoker_creator<decltype(&___TargetType::InvokeUnboxed)>::invoke;
        }
    };

   public:
    static inline ___MethodRegistrator_Invoke ___Invoke_MethodRegistrator;
    static inline ___MethodRegistrator_InvokeUnboxed ___InvokeUnboxed_MethodRegistrator;

   public:
    void dtor();

   private:
    template <typename T>
    struct ___MethodRegistrator_dtor;
    template <typename R, typename T, typename... TArgs>
    struct ___MethodRegistrator_dtor<R (T::*)(TArgs...)> : ::custom_types::MethodRegistrator {
        constexpr const char* name() const override {
            return "dtor";
        }
        constexpr const char* csharpName() const override {
            return "dtor";
        }
        int flags() const override {
            return METHOD_ATTRIBUTE_VIRTUAL | METHOD_ATTRIBUTE_PUBLIC | METHOD_ATTRIBUTE_HIDE_BY_SIG;
        }
        const MethodInfo* virtualMethod() const override {
            return ::il2cpp_utils::FindMethodUnsafe("System", "Object", "Finalize", 0);
        }
        const Il2CppType* returnType() const override {
            ::il2cpp_functions::Init();
            return &il2cpp_functions::defaults->void_class->byval_arg;
        }
        std::vector<const Il2CppType*> params() const override {
            return {};
        }
        uint8_t params_size() const override {
            return 0;
        }
        Il2CppMethodPointer methodPointer() const override {
            static auto mptr = &DelegateWrapperStatic<RI, TArgsI...>::dtor;
            return reinterpret_cast<Il2CppMethodPointer>(+[](DelegateWrapperStatic<RI, TArgsI...>* inst) { (inst->*mptr)(); });
        }
        InvokerMethod invoker() const override {
            return &::custom_types::invoker_creator<decltype(&___TargetType::dtor)>::invoke;
        }
    };

   public:
    static inline ___MethodRegistrator_dtor<decltype(&___TargetType::dtor)> ___dtor_MethodRegistrator;
};

}  // namespace custom_types

template <class R, class... TArgs>
struct ::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_class<custom_types::DelegateWrapperStatic<R, TArgs...>*> {
    static inline Il2CppClass* get() {
        return custom_types::DelegateWrapperStatic<R, TArgs...>::___TypeRegistration::klass_ptr;
    }
};
template <class R, class... TArgs>
struct ::il2cpp_utils::il2cpp_type_check::need_box<custom_types::DelegateWrapperStatic<R, TArgs...>> {
    constexpr static bool value = false;
};

namespace custom_types {
// Reopen it here

/// @brief The invoker function for a delegate with a wrapped type.
/// @tparam R The return type of the function.
/// @tparam TArgs The argument types of the function.
/// @param instance The wrapped instance of this context function.
/// @param args The arguments to pass to this function.
/// @return The return from the wrapped function.
template <class RI, class... TArgsI>
RI __attribute__((noinline)) DelegateWrapperStatic<RI, TArgsI...>::Invoke(DelegateWrapperStatic<RI, TArgsI...>* instance, make_boxed_t<TArgsI>... args) {
    IL2CPP_CATCH_HANDLER(
        if (!instance || !instance->wrappedFunc) {
            custom_types::logger.critical("Attempting to invoke delegate on a null or destroyed instance: {}, class: {} ({})", fmt::ptr(instance), fmt::ptr(___TypeRegistration::klass_ptr),
                                          ___TypeRegistration::get()->name());
        } if constexpr (std::is_same_v<RI, void>) { instance->wrappedFunc(unbox_arg(args)...); } else { return instance->wrappedFunc(unbox_arg(args)...); })
}

/// @brief The unboxed invoker function for a delegate with a wrapped type.
/// @tparam R The return type of the function.
/// @tparam TArgs The argument types of the function.
/// @param instance The wrapped instance of this context function.
/// @param args The arguments to pass to this function.
/// @return The return from the wrapped function.
template <class RI, class... TArgsI>
RI __attribute__((noinline)) DelegateWrapperStatic<RI, TArgsI...>::InvokeUnboxed(DelegateWrapperStatic<RI, TArgsI...>* instance, TArgsI... args) {
    IL2CPP_CATCH_HANDLER(
        if (!instance || !instance->wrappedFunc) {
            custom_types::logger.critical("Attempting to invoke delegate on a null or destroyed instance: {}, class: {} ({})", fmt::ptr(instance), fmt::ptr(___TypeRegistration::klass_ptr),
                                          ___TypeRegistration::get()->name());
        } if constexpr (std::is_same_v<RI, void>) { instance->wrappedFunc(args...); } else { return instance->wrappedFunc(args...); })
}

template <class RI, class... TArgsI>
void DelegateWrapperStatic<RI, TArgsI...>::dtor() {
    custom_types::logger.debug("Destroying delegate: {}, class: {} ({})", fmt::ptr(this), fmt::ptr(___TypeRegistration::klass_ptr), ___TypeRegistration::get()->name());
    this->~DelegateWrapperStatic();
}

/// @brief The wrapper for an invokable delegate without an existing context.
/// DO NOT ATTEMPT TO CREATE THIS YOURSELF!
/// @tparam R The return type of the function being called.
/// @tparam TArgs The argument types of the function being called.
template <class RI, class TI, class... TArgsI>
struct DelegateWrapperInstance : Il2CppObject {
    // CT Boilerplate
    using ___TargetType = DelegateWrapperInstance<RI, TI, TArgsI...>;
    constexpr static auto ___Base__Size = sizeof(Il2CppObject);
    friend ::custom_types::Register;

   public:
    struct ___TypeRegistration : ::custom_types::TypeRegistration {
        ___TypeRegistration() {
            ::custom_types::Register::AddType(this);
            ::custom_types::logger.debug("Adding instance delegate type: {}", name());
            instance = this;
        }
        std::vector<::custom_types::FieldRegistrator*> const getFields() const override {
            return { &___obj_FieldRegistrator };
        }
        std::vector<::custom_types::StaticFieldRegistrator*> const getStaticFields() const override {
            return {};
        }
        std::vector<::custom_types::MethodRegistrator*> const getMethods() const override {
            return { &___Invoke_MethodRegistrator, &___dtor_MethodRegistrator, &___InvokeUnboxed_MethodRegistrator };
        }
        static inline char* st_fields;
        char*& static_fields() override {
            return st_fields;
        }
        size_t static_fields_size() const override {
            return 0;
        }
        const char* name() const override {
            if constexpr (!std::is_same_v<void, RI>) {
                static std::string nm{ fmt::format("DelegateWrapper({} ret, {} args, {} sz) ({})", sizeof(RI), sizeof...(TArgsI), sizeof(___TargetType), custom_types::get_delegate_count()) };
                return nm.c_str();
            } else {
                static std::string nm{ fmt::format("DelegateWrapper(void ret, {} args, {} sz) ({})", sizeof...(TArgsI), sizeof(___TargetType), custom_types::get_delegate_count()) };
                return nm.c_str();
            }
        }
        constexpr const char* namespaze() const override {
            return "CustomTypes";
        }
        constexpr const char* dllName() const override {
            return "CustomTypes.dll";
        }
        Il2CppClass* baseType() const override {
            ::il2cpp_functions::Init();
            return ::il2cpp_functions::defaults->object_class;
        }
        std::vector<Il2CppClass*> const interfaces() const override {
            return {};
        }
        constexpr Il2CppTypeEnum typeEnum() const override {
            return IL2CPP_TYPE_CLASS;
        }
        constexpr uint32_t typeFlags() const override {
            return 0;
        }
        static inline Il2CppClass* klass_ptr;
        Il2CppClass*& klass() const override {
            return klass_ptr;
        }
        constexpr std::size_t size() const override {
            return sizeof(___TargetType);
        }
        constexpr TypeRegistration* customBase() const override {
            return nullptr;
        }
        bool initialized() const override {
            return init;
        }
        void setInitialized() const override {
            init = true;
        }
        static inline bool init = false;
        static inline TypeRegistration* instance;
        static TypeRegistration* get() {
            return instance;
        }
    };

   public:
    static inline ___TypeRegistration __registration_instance_DelegateWrapperInstance;
    // Disallow creation
    DelegateWrapperInstance() = delete;

    // The invoke method that wraps the delegate call
    static RI Invoke(DelegateWrapperInstance<RI, TI, TArgsI...>* instance, make_boxed_t<TArgsI>... args);
    static RI InvokeUnboxed(DelegateWrapperInstance<RI, TI, TArgsI...>* instance, TArgsI... args);

   private:
    struct ___MethodRegistrator_Invoke : ::custom_types::MethodRegistrator {
        constexpr const char* name() const override {
            return "Invoke";
        }
        constexpr const char* csharpName() const override {
            return "Invoke";
        }
        int flags() const override {
            return METHOD_ATTRIBUTE_STATIC;
        }
        const MethodInfo* virtualMethod() const override {
            return nullptr;
        }
        const Il2CppType* returnType() const override {
            ::il2cpp_functions::Init();
            return ::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_type<RI>::get();
        }
        std::vector<const Il2CppType*> params() const override {
            il2cpp_functions::Init();
            return { ::il2cpp_functions::class_get_type(___TypeRegistration::klass_ptr), (::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_type<TArgsI>::get())... };
        }
        uint8_t params_size() const override {
            return sizeof...(TArgsI) + 1;
        }
        Il2CppMethodPointer methodPointer() const override {
            return reinterpret_cast<Il2CppMethodPointer>(&___TargetType::Invoke);
        }
        InvokerMethod invoker() const override {
            return &::custom_types::invoker_creator<decltype(&___TargetType::Invoke)>::invoke;
        }
    };

    struct ___MethodRegistrator_InvokeUnboxed : ::custom_types::MethodRegistrator {
        constexpr const char* name() const override {
            return "InvokeUnboxed";
        }
        constexpr const char* csharpName() const override {
            return "InvokeUnboxed";
        }
        int flags() const override {
            return METHOD_ATTRIBUTE_STATIC;
        }
        const MethodInfo* virtualMethod() const override {
            return nullptr;
        }
        const Il2CppType* returnType() const override {
            ::il2cpp_functions::Init();
            return ::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_type<RI>::get();
        }
        std::vector<const Il2CppType*> params() const override {
            il2cpp_functions::Init();
            return { ::il2cpp_functions::class_get_type(___TypeRegistration::klass_ptr), (::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_type<TArgsI>::get())... };
        }
        uint8_t params_size() const override {
            return sizeof...(TArgsI) + 1;
        }
        Il2CppMethodPointer methodPointer() const override {
            return reinterpret_cast<Il2CppMethodPointer>(&___TargetType::InvokeUnboxed);
        }
        InvokerMethod invoker() const override {
            return &::custom_types::invoker_creator<decltype(&___TargetType::InvokeUnboxed)>::invoke;
        }
    };

   public:
    static inline ___MethodRegistrator_Invoke ___Invoke_MethodRegistrator;
    static inline ___MethodRegistrator_InvokeUnboxed ___InvokeUnboxed_MethodRegistrator;

   private:
    struct ___FieldRegistrator_obj : ::custom_types::FieldRegistrator {
        constexpr const char* name() const override {
            return "obj";
        }
        const Il2CppType* type() const override {
            ::il2cpp_functions::Init();
            return ::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_type<TI>::get();
        }
        constexpr uint16_t fieldAttributes() const override {
            return FIELD_ATTRIBUTE_PUBLIC;
        }
        constexpr size_t size() const override {
            return sizeof(TI);
        }
        int32_t offset() const override {
            return offsetof(___TargetType, obj);
        }
    };
    static inline ___FieldRegistrator_obj ___obj_FieldRegistrator;

   public:
    // The instance object
    TI obj;
    // The (potentially capturing) function to invoke
    std::function<RI(TI, TArgsI...)> wrappedFunc;

    void dtor();

   private:
    template <typename T>
    struct ___MethodRegistrator_dtor;
    template <typename R, typename T, typename... TArgs>
    struct ___MethodRegistrator_dtor<R (T::*)(TArgs...)> : ::custom_types::MethodRegistrator {
        constexpr const char* name() const override {
            return "dtor";
        }
        constexpr const char* csharpName() const override {
            return "dtor";
        }
        int flags() const override {
            return METHOD_ATTRIBUTE_VIRTUAL | METHOD_ATTRIBUTE_PUBLIC | METHOD_ATTRIBUTE_HIDE_BY_SIG;
        }
        const MethodInfo* virtualMethod() const override {
            return ::il2cpp_utils::FindMethodUnsafe("System", "Object", "Finalize", 0);
        }
        const Il2CppType* returnType() const override {
            ::il2cpp_functions::Init();
            return &il2cpp_functions::defaults->void_class->byval_arg;
        }
        std::vector<const Il2CppType*> params() const override {
            return {};
        }
        uint8_t params_size() const override {
            return 0;
        }
        Il2CppMethodPointer methodPointer() const override {
            static auto mptr = &___TargetType::dtor;
            return reinterpret_cast<Il2CppMethodPointer>(+[](___TargetType* inst) { (inst->*mptr)(); });
        }
        InvokerMethod invoker() const override {
            return &::custom_types::invoker_creator<decltype(&___TargetType::dtor)>::invoke;
        }
    };

   public:
    static inline ___MethodRegistrator_dtor<decltype(&___TargetType::dtor)> ___dtor_MethodRegistrator;
};

}  // namespace custom_types

template <class R, class T, class... TArgs>
struct ::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_class<custom_types::DelegateWrapperInstance<R, T, TArgs...>*> {
    static inline Il2CppClass* get() {
        return custom_types::DelegateWrapperInstance<R, T, TArgs...>::___TypeRegistration::klass_ptr;
    }
};
template <class R, class T, class... TArgs>
struct ::il2cpp_utils::il2cpp_type_check::need_box<custom_types::DelegateWrapperInstance<R, T, TArgs...>> {
    constexpr static bool value = false;
};

namespace custom_types {
// Reopen it here

/// @brief The invoker function for a delegate with a wrapped type.
/// @tparam R The return type of the function.
/// @tparam TArgs The argument types of the function.
/// @param instance The wrapped instance of this context function.
/// @param args The arguments to pass to this function.
/// @return The return from the wrapped function.
template <class RI, class TI, class... TArgsI>
RI __attribute__((noinline)) DelegateWrapperInstance<RI, TI, TArgsI...>::Invoke(DelegateWrapperInstance<RI, TI, TArgsI...>* instance, make_boxed_t<TArgsI>... args) {
    IL2CPP_CATCH_HANDLER(
        if (!instance || !instance->wrappedFunc) {
            custom_types::logger.critical("Attempting to invoke instance delegate that is null or has been destroyed! {}, class: {} ({})", fmt::ptr(instance), fmt::ptr(___TypeRegistration::klass_ptr),
                                          ___TypeRegistration::get()->name());
        } if constexpr (std::is_same_v<RI, void>) { instance->wrappedFunc(instance->obj, unbox_arg(args)...); } else { return instance->wrappedFunc(instance->obj, unbox_arg(args)...); })
}

/// @brief The unboxed invoker function for a delegate with a wrapped type.
/// @tparam R The return type of the function.
/// @tparam TArgs The argument types of the function.
/// @param instance The wrapped instance of this context function.
/// @param args The arguments to pass to this function.
/// @return The return from the wrapped function.
template <class RI, class TI, class... TArgsI>
RI __attribute__((noinline)) DelegateWrapperInstance<RI, TI, TArgsI...>::InvokeUnboxed(DelegateWrapperInstance<RI, TI, TArgsI...>* instance, TArgsI... args) {
    IL2CPP_CATCH_HANDLER(
        if (!instance || !instance->wrappedFunc) {
            custom_types::logger.critical("Attempting to invoke instance delegate that is null or has been destroyed! {}, class: {} ({})", fmt::ptr(instance), fmt::ptr(___TypeRegistration::klass_ptr),
                                          ___TypeRegistration::get()->name());
        } if constexpr (std::is_same_v<RI, void>) { instance->wrappedFunc(instance->obj, args...); } else { return instance->wrappedFunc(instance->obj, args...); })
}

template <class RI, class TI, class... TArgsI>
void DelegateWrapperInstance<RI, TI, TArgsI...>::dtor() {
    custom_types::logger.debug("Destroying delegate: {}, class: {} ({})", fmt::ptr(this), fmt::ptr(___TypeRegistration::klass_ptr), ___TypeRegistration::get()->name());
    this->~DelegateWrapperInstance();
}

CUSTOM_TYPES_EXPORT void log_delegate(Il2CppDelegate* d);

template <class T = MulticastDelegate*, class R, class... TArgs>
T MakeDelegate(const Il2CppClass* delegateClass, DelegateWrapperStatic<R, TArgs...>* inst) {
    // TODO: We could still make this a native method info, but for now it's much much easier to just wrap it
    // This will make delegates a little bit slower than just a pure native call, since it'll have to box the args
    // and then unbox them (as necessary).
    // But it should also make them much safer, hopefully solving all sorts of problems.
    // This gets the first method (which should be our Invoke static method)'s MethodInfo*.
    // It should already have been allocated, so get will cache it for us.
    // We need to ensure static initialization of both the dtor method registrator
    // and the invoke method registrator:
    custom_types::logger.debug("Delegate dtor registrator: {}", fmt::ptr(DelegateWrapperStatic<R, TArgs...>::___dtor_MethodRegistrator.get()));

    auto* delegate = reinterpret_cast<Il2CppDelegate*>(il2cpp_functions::object_new(delegateClass));
    // find the ctor method that takes object, intptr
    const MethodInfo* ctor_minfo = THROW_UNLESS(
        il2cpp_utils::FindMethod(const_cast<Il2CppClass*>(delegateClass), ".ctor", std::array<Il2CppClass*, 0>{},
                                 std::array<const Il2CppType*, 2>{ il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_type<Il2CppObject*>::get(), &il2cpp_functions::defaults->int_class->this_arg }));

    // find the invoke method
    const MethodInfo* invoke_minfo;
    auto methodSpan = std::span(delegateClass->methods, delegateClass->method_count);
    for (auto const& method : methodSpan) {
        // I hate C APIs
        if (strcmp(method->name, "Invoke") == 0) {
            custom_types::logger.debug("Found Invoke method: {}", fmt::ptr(method));
            invoke_minfo = method;
        }
    }

    MethodInfo* method;
    if (invoke_minfo->has_full_generic_sharing_signature) {
        // boxed
        method = DelegateWrapperStatic<R, TArgs...>::___Invoke_MethodRegistrator.get();
    } else {
        // unboxed
        method = DelegateWrapperStatic<R, TArgs...>::___InvokeUnboxed_MethodRegistrator.get();
    }

    CRASH_UNLESS(il2cpp_utils::RunMethodOpt<void, false>(delegate, ctor_minfo, inst, (void*)&method));

    if (invoke_minfo->has_full_generic_sharing_signature) {
        delegate->method_ptr = (void (*)())DelegateWrapperStatic<R, TArgs...>::Invoke;
        delegate->invoke_impl = (void (*)())DelegateWrapperStatic<R, TArgs...>::Invoke;
    } else {
        delegate->method_ptr = (void (*)())DelegateWrapperStatic<R, TArgs...>::InvokeUnboxed;
        delegate->invoke_impl = (void (*)())DelegateWrapperStatic<R, TArgs...>::InvokeUnboxed;
    }

    delegate->invoke_impl_this = (Il2CppObject*)inst;

    custom_types::logger.debug("Created delegate: {} ({}), for instance: {} with MethodInfo*: {}", fmt::ptr(delegate), fmt::ptr(delegateClass), fmt::ptr(inst), fmt::ptr(method));
    log_delegate(delegate);
    return reinterpret_cast<T>(delegate);
}

template <class T = MulticastDelegate*,  class R, class I, class... TArgs>
T MakeDelegate(const Il2CppClass* delegateClass, DelegateWrapperInstance<R, I, TArgs...>* inst) {
    custom_types::logger.debug("Delegate instance dtor registrator: {}", fmt::ptr(DelegateWrapperInstance<R, I, TArgs...>::___dtor_MethodRegistrator.get()));

    auto* delegate = reinterpret_cast<Il2CppDelegate*>(il2cpp_functions::object_new(delegateClass));
    // find the ctor method that takes object, intptr
    const MethodInfo* ctor_minfo = THROW_UNLESS(
        il2cpp_utils::FindMethod(const_cast<Il2CppClass*>(delegateClass), ".ctor", std::array<Il2CppClass*, 0>{},
                                 std::array<const Il2CppType*, 2>{ il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_type<Il2CppObject*>::get(), &il2cpp_functions::defaults->int_class->this_arg }));

    // find the invoke method
    const MethodInfo* invoke_minfo;
    auto methodSpan = std::span(delegateClass->methods, delegateClass->method_count);
    for (auto const& method : methodSpan) {
        // I hate C APIs
        if (strcmp(method->name, "Invoke") == 0) {
            custom_types::logger.debug("Found Invoke method: {}", fmt::ptr(method));
            invoke_minfo = method;
        }
    }

    MethodInfo* method;
    if (invoke_minfo->has_full_generic_sharing_signature) {
        // boxed
        method = DelegateWrapperInstance<R, I, TArgs...>::___Invoke_MethodRegistrator.get();
    } else {
        // unboxed
        method = DelegateWrapperInstance<R, I, TArgs...>::___InvokeUnboxed_MethodRegistrator.get();
    }

    CRASH_UNLESS(il2cpp_utils::RunMethodOpt<void, false>(delegate, ctor_minfo, inst, (void*)&method));

    if (invoke_minfo->has_full_generic_sharing_signature) {
        delegate->method_ptr = (void (*)())DelegateWrapperInstance<R, I, TArgs...>::Invoke;
        delegate->invoke_impl = (void (*)())DelegateWrapperInstance<R, I, TArgs...>::Invoke;
    } else {
        delegate->method_ptr = (void (*)())DelegateWrapperInstance<R, I, TArgs...>::InvokeUnboxed;
        delegate->invoke_impl = (void (*)())DelegateWrapperInstance<R, I, TArgs...>::InvokeUnboxed;
    }

    delegate->invoke_impl_this = (Il2CppObject*)inst;

    custom_types::logger.debug("Created instance delegate: {} ({}), for instance: {} with MethodInfo*: {}", fmt::ptr(delegate), fmt::ptr(delegateClass), fmt::ptr(inst), fmt::ptr(method));
    log_delegate(delegate);
    return reinterpret_cast<T>(delegate);
}

/// @brief Makes a delegate wrapping a context function (such as a context lambda).
/// @tparam T The type to return.
/// @tparam I The instance object to provide to this delegate.
/// @tparam R The return type of the delegate.
/// @tparam TArgs The arguments of the delegate.
/// @param delegateClass The Il2CppClass* of the delegate to create.
/// @param instance The (move constructible) instance reference to provide to the delegate. This instance is moved and will no longer be valid.
/// @param f The function to invoke with the delegate.
/// @return The created delegate.
template <typename T = MulticastDelegate*,  class R, class... TArgs>
T MakeDelegate(const Il2CppClass* delegateClass, std::function<R(TArgs...)> const& f) {
    il2cpp_functions::Init();
    // NOTE: This static field MUST be used in order for it to be instantiated within the generic, thus, it is important to NOT remove this log
    custom_types::logger.debug("Type registration for delegate being created (forcibly instantiated): {}", DelegateWrapperStatic<R, TArgs...>::__registration_instance_DelegateWrapperStatic.name());
    auto* wrapperInstance = reinterpret_cast<DelegateWrapperStatic<R, TArgs...>*>(il2cpp_functions::object_new(DelegateWrapperStatic<R, TArgs...>::___TypeRegistration::klass_ptr));
    wrapperInstance->wrappedFunc = f;
    return custom_types::MakeDelegate<T>(delegateClass, wrapperInstance);
}

/// @brief Makes a delegate wrapping a context function (such as a context lambda).
/// @tparam T The type to return.
/// @tparam I The instance object to provide to this delegate.
/// @tparam R The return type of the delegate.
/// @tparam TArgs The arguments of the delegate.
/// @param instance The (move constructible) instance reference to provide to the delegate. This instance is moved and will no longer be valid.
/// @param f The function to invoke with the delegate.
/// @return The created delegate.
template <typename T = MulticastDelegate*,  class R, class... TArgs>
T MakeDelegate(std::function<R(TArgs...)> const& f) {
    return MakeDelegate<T>(classof(T), f);
}

template <typename T = MulticastDelegate*,  class R, class I, class... TArgs>
// TODO: Requires that I has a classof
T MakeDelegate(const Il2CppClass* delegateClass, I obj, std::function<R(I, TArgs...)> const& f) {
    il2cpp_functions::Init();
    custom_types::logger.debug("Type registration for delegate being created (forcibly instantiated): {}",
                               DelegateWrapperInstance<R, I, TArgs...>::__registration_instance_DelegateWrapperInstance.name());
    auto* wrapperInstance = reinterpret_cast<DelegateWrapperInstance<R, I, TArgs...>*>(il2cpp_functions::object_new(DelegateWrapperInstance<R, I, TArgs...>::___TypeRegistration::klass_ptr));
    wrapperInstance->wrappedFunc = f;
    wrapperInstance->obj = obj;
    return custom_types::MakeDelegate<T>(delegateClass, wrapperInstance);
}

template <typename T = MulticastDelegate*,  class R, class I, class... TArgs>
T MakeDelegate(I obj, std::function<R(I, TArgs...)> const& f) {
    return MakeDelegate<T>(classof(T), obj, f);
}
}  // namespace custom_types
