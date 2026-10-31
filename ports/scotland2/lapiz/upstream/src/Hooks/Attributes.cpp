#include "utilities/hooking.hpp"
#include "utilities/logging.hpp"
#include "utilities/typeutil.hpp"

#include "Hooks/Attributes.hpp"
#include "Zenject/InjectAttributeBase.hpp"
#include "AttributeRegistration_internal.hpp"

#include "System/Reflection/RuntimeFieldInfo.hpp"
#include "System/Reflection/RuntimeMethodInfo.hpp"

MAKE_AUTO_HOOK_MATCH(
        RuntimeFieldInfo_GetCustomAttributes,
        static_cast<::ArrayW<::System::Object*> (::System::Reflection::RuntimeFieldInfo::*)(::System::Type*, bool)>(&System::Reflection::RuntimeFieldInfo::GetCustomAttributes),
        ArrayW<::System::Object*>,
        System::Reflection::RuntimeFieldInfo* self,
        System::Type* attributeType,
        bool inherit
) {
    auto retval = RuntimeFieldInfo_GetCustomAttributes(self, attributeType, inherit);
    Lapiz::Zenject::Internal::Attributes::FieldInfo_GetCustomAttributes(retval, self, attributeType, inherit);
    return retval;
}

MAKE_AUTO_HOOK_MATCH(
        RuntimeMethodInfo_GetCustomAttributes,
        static_cast<::ArrayW<::System::Object*> (::System::Reflection::RuntimeMethodInfo::*)(::System::Type*, bool)>(&System::Reflection::RuntimeMethodInfo::GetCustomAttributes),
        ArrayW<::System::Object*>,
        System::Reflection::RuntimeMethodInfo* self,
        System::Type* attributeType,
        bool inherit
) {
    auto retval = RuntimeMethodInfo_GetCustomAttributes(self, attributeType, inherit);
    Lapiz::Zenject::Internal::Attributes::MethodInfo_GetCustomAttributes(retval, self, attributeType, inherit);
    return retval;
}

namespace Lapiz::Zenject::Internal {
    void Attributes::FieldInfo_GetCustomAttributes(ArrayW<::System::Object*>& retval, System::Reflection::FieldInfo* self, System::Type* attributeType, bool inherit) {
        if (!DerivesFromInjectAttributeBase(attributeType)) return;
        auto attribute = GetInjectAttribute(self);
        if (!attribute) return;
        retval = InsertCustomAttribute(retval, attribute);
    }

    void Attributes::MethodInfo_GetCustomAttributes(ArrayW<::System::Object*>& retval, System::Reflection::MethodInfo* self, System::Type* attributeType, bool inherit) {
        if (!DerivesFromInjectAttributeBase(attributeType)) return;
        auto attribute = GetInjectAttribute(self);
        if (!attribute) return;
        retval = InsertCustomAttribute(retval, attribute);
    }

    bool Attributes::DerivesFromInjectAttributeBase(System::Type* type) {
        auto klass = il2cpp_functions::class_from_system_type((Il2CppReflectionType*)type);
        static auto ancestor = classof(::Zenject::InjectAttributeBase*);
        return TypeUtil::hasAncestor(klass, ancestor);
    }

    ArrayW<System::Object*> Attributes::InsertCustomAttribute(::ArrayW<System::Object*> arr, System::Attribute* attribute) {
        auto newArr = ArrayW<System::Object*>(arr.size() + 1);
        memcpy(newArr.begin(), arr.begin(), arr.size() * sizeof(System::Object*));
        newArr[arr.size()] = attribute;
        return newArr;
    }

    System::Attribute* Attributes::GetInjectAttribute(System::Reflection::MemberInfo* member) {
        if (!member) return nullptr;
        // declaring type is the type of the class that has this member
        auto declaringType = member->get_DeclaringType();
        if (!declaringType) return nullptr;
        auto declaringClass = il2cpp_functions::class_from_system_type((Il2CppReflectionType*)declaringType);

        auto attrReg = Lapiz::Attributes::Get(declaringClass, member->get_Name());
        return attrReg ? attrReg->get_attribute() : nullptr;
    }
}
