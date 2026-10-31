#pragma once

#include "./_config.h"
#include "beatsaber-hook/shared/utils/typedefs.h"
#include "System/Attribute.hpp"

namespace Lapiz::Attributes {
    class AttributeRegistration;

    /// @brief PreRegister an attribute registration
    /// @param registration the registration to register
    LAPIZ_EXPORT_FUNC void PreRegister(const AttributeRegistration* registration);

    /// @brief register any current pre registered attribute registrations
    LAPIZ_EXPORT_FUNC void AutoRegister();

    class LAPIZ_EXPORT AttributeRegistration {
        public:
            AttributeRegistration() { PreRegister(this); }
            virtual Il2CppClass* get_declaringType() const = 0;
            virtual const char* get_name() const = 0;
            virtual ::System::Attribute* get_attribute() const = 0;
            virtual bool isMethod() const { return false; }
            virtual bool isField() const { return false; }
        protected:
            mutable ::System::Attribute* attribute;
    };
}
