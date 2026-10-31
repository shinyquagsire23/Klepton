#pragma once

#include "../_config.h"
#include "beatsaber-hook/shared/utils/typedefs.h"
#include "System/SerializableAttribute.hpp"
#include "../AttributeRegistration.hpp"

namespace Lapiz::Objects {
    class LAPIZ_EXPORT SerializableAttribute : public Lapiz::Attributes::AttributeRegistration {
        public:
            SerializableAttribute() : Lapiz::Attributes::AttributeRegistration() {}
            ::System::Attribute* get_attribute() const override {
                if (!attribute) {
                    attribute = *il2cpp_utils::New<::System::SerializableAttribute*, il2cpp_utils::CreationType::Manual>();
                }
                return attribute;
            }
    };
}
