#pragma once
#include "register.hpp"
#include "util.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"

namespace custom_types {
    template<typename T, typename R, Il2CppTypeEnum typeEnum_, size_t baseSize>
    struct TypeWrapperParent {
        using ___TargetType = T;
        using ___TypeRegistration = R;
        constexpr static auto ___Base__Size = baseSize;
        constexpr static bool __IL2CPP_IS_VALUE_TYPE = typeEnum_ != Il2CppTypeEnum::IL2CPP_TYPE_CLASS;
        static inline constexpr const int __IL2CPP_REFERENCE_TYPE_SIZE = sizeof(T);
        uint8_t _baseFields[baseSize];
        TypeWrapperParent(TypeWrapperParent&&) = delete;
        TypeWrapperParent(TypeWrapperParent const&) = delete;
        protected:
        TypeWrapperParent() {};
    };

    template<typename T, typename R, Il2CppTypeEnum typeEnum_, typename baseT>
    struct TypeWrapperInheritanceParent : public baseT {
        using ___TargetType = T;
        using ___TypeRegistration = R;
        constexpr static auto ___Base__Size = sizeof(baseT);
        constexpr static bool __IL2CPP_IS_VALUE_TYPE = typeEnum_ != Il2CppTypeEnum::IL2CPP_TYPE_CLASS;
        static inline constexpr const int __IL2CPP_REFERENCE_TYPE_SIZE = sizeof(T);
        TypeWrapperInheritanceParent(TypeWrapperInheritanceParent&&) = delete;
        TypeWrapperInheritanceParent(TypeWrapperInheritanceParent const&) = delete;
        protected:
        TypeWrapperInheritanceParent() {};
    };
}
