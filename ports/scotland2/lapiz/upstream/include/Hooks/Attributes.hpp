#pragma once

#include "beatsaber-hook/shared/utils/typedefs.h"

#include "System/Reflection/FieldInfo.hpp"
#include "System/Reflection/MethodInfo.hpp"

#include "System/Attribute.hpp"
#include "System/Type.hpp"

namespace Lapiz::Zenject::Internal {
    class Attributes {
        public:
            static void FieldInfo_GetCustomAttributes(ArrayW<::System::Object*>& retval, System::Reflection::FieldInfo* self, System::Type* attributeType, bool inherit);
            static void MethodInfo_GetCustomAttributes(ArrayW<::System::Object*>& retval, System::Reflection::MethodInfo* self, System::Type* attributeType, bool inherit);
        private:
            static bool DerivesFromInjectAttributeBase(System::Type* type);
            static ArrayW<System::Object*> InsertCustomAttribute(::ArrayW<System::Object*> arr, System::Attribute* attribute);
            static System::Attribute* GetInjectAttribute(System::Reflection::MemberInfo* member);
    };
}
