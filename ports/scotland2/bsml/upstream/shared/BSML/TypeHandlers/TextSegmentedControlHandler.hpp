#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "HMUI/TextSegmentedControl.hpp"

namespace BSML {
    class BSML_EXPORT TextSegmentedControlHandler : public TypeHandler<HMUI::TextSegmentedControl*> {
        public:
            using Base = TypeHandler<HMUI::TextSegmentedControl*>;
            TextSegmentedControlHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
            virtual void HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) override;
    };
}
