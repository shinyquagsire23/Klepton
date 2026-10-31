#pragma once

#include "../_config.h"
#include "beatsaber-hook/shared/utils/typedefs.h"
#include "Zenject/InjectAttribute.hpp"
#include "../AttributeRegistration.hpp"

namespace Lapiz::Zenject {
    class LAPIZ_EXPORT InjectAttribute : public Lapiz::Attributes::AttributeRegistration {
        public:
            InjectAttribute() : Lapiz::Attributes::AttributeRegistration() {}
            virtual const char* get_id() const = 0;
            virtual bool optional() const = 0;

            ::System::Attribute* get_attribute() const override {
                if (!attribute) {
                    auto zenjectAttribute = *il2cpp_utils::New<::Zenject::InjectAttribute*, il2cpp_utils::CreationType::Manual>();
                    zenjectAttribute->set_Optional(optional());
                    if (get_id()) {
                        auto id = StringW(get_id());
                        zenjectAttribute->set_Id(static_cast<System::String*>(id.convert()));
                    }
                    attribute = zenjectAttribute;
                }
                return attribute;
            }
    };
}
