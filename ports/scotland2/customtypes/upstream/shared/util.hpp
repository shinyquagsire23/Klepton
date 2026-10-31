#pragma once
#include <type_traits>
#include "beatsaber-hook/shared/utils/type-concepts.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-functions.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils-exceptions.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"

namespace custom_types {
    template<class F>
    struct Decomposer;

    template<class R, class T, class... TArgs>
    struct Decomposer<R (T::*)(TArgs...)> {
        template<class... TArgs2>
        static constexpr bool equal() {
            if constexpr (sizeof...(TArgs) != sizeof...(TArgs2)) {
                return false;
            } else {
                return (std::is_same_v<TArgs, TArgs2> && ...);
            }
        }
        template<class... TArgs2>
        static constexpr bool convertible() {
            if constexpr (sizeof...(TArgs) != sizeof...(TArgs2)) {
                return false;
            } else {
                // Convertible from the args specified TO the function pointer provided
                return (std::is_convertible_v<TArgs2, TArgs> && ...);
            }
        }
    };

    template<size_t N>
    struct string_literal {
        constexpr string_literal(const char (&str)[N]) {
            std::copy_n(str, N, value);
        }
        char value[N];
    };

    template<string_literal namespaze_, string_literal name_>
    struct interface_helper {
        static auto constexpr namespaze = namespaze_;
        static auto constexpr name = name_;
    };

    struct CUSTOM_TYPES_EXPORT NullAccessException : il2cpp_utils::exceptions::StackTraceException {
        NullAccessException() : il2cpp_utils::exceptions::StackTraceException("Null instance access on a custom type field!") {}
    };

    template<typename T>
    Il2CppClass* ExtractClass() {
        return ::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_class<T>::get();
    }
    template<typename T>
    requires requires () { ::il2cpp_utils::GetClassFromName(T::namespaze.value, T::name.value); }
    Il2CppClass* ExtractClass() {
        return ::il2cpp_utils::GetClassFromName(T::namespaze.value, T::name.value);
    }

    template<typename... Ts>
    std::vector<Il2CppClass*> ExtractClasses() {
        return {ExtractClass<Ts>()...};
    }

#if __has_feature(cxx_exceptions)
    #define CT_FIELD_ACCESS_CHECK(inst) if (!static_cast<const void*>(inst)) throw ::custom_types::NullAccessException()
#else
    #define CT_FIELD_ACCESS_CHECK(inst) if (!static_cast<const void*>(inst)) SAFE_ABORT_MSG("Null instance access on a custom type field!")
#endif

    template<typename T>
    struct field_accessor {
        inline constexpr T const* field_addr(const void* instance, std::size_t offset) const noexcept {
            return static_cast<T const*>(static_cast<void const*>(static_cast<uint8_t const*>(instance) + offset));
        }

        inline constexpr T* field_addr(void* instance, std::size_t offset) const noexcept {
            return static_cast<T*>(static_cast<void*>(static_cast<uint8_t*>(instance) + offset));
        }

        inline constexpr T& read(void* instance, std::size_t offset) const noexcept {
            return *field_addr(instance, offset);
        }

        inline constexpr T const& read(void const* instance, std::size_t offset) const noexcept {
            return *field_addr(instance, offset);
        }

        inline constexpr void write(void* instance, std::size_t offset, T&& v) const noexcept {
            *field_addr(instance, offset) = v;
        }
    };

    template<il2cpp_utils::il2cpp_reference_type T>
    struct field_accessor<T> {
        inline constexpr void* field_addr(void* instance, std::size_t offset) const noexcept {
            return static_cast<void*>(static_cast<uint8_t*>(instance) + offset);
        }

        inline constexpr void const* field_addr(void const* instance, std::size_t offset) const noexcept {
            return static_cast<void const*>(static_cast<uint8_t const*>(instance) + offset);
        }

        inline constexpr T& read(void* instance, std::size_t offset) const noexcept {
            return *static_cast<T*>(field_addr(instance, offset));
        }

        inline constexpr T const& read(void const* instance, std::size_t offset) const noexcept {
            return *static_cast<T const*>(field_addr(instance, offset));
        }

        inline void write(void* instance, std::size_t offset, T&& v) const noexcept {
            il2cpp_functions::gc_wbarrier_set_field(
                static_cast<Il2CppObject*>(instance),
                static_cast<void**>(field_addr(instance, offset)),
                il2cpp_utils::il2cpp_reference_type_value<T>(std::forward<T>(v))
            );
        }
    };
}
