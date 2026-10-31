#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "HMUI/IconSegmentedControl.hpp"

namespace BSML {
    class BSML_EXPORT IconSegmentedControlHandler : public TypeHandler<HMUI::IconSegmentedControl*> {
        public:
            using Base = TypeHandler<HMUI::IconSegmentedControl*>;
            IconSegmentedControlHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
            virtual void HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) override;
    };
}
