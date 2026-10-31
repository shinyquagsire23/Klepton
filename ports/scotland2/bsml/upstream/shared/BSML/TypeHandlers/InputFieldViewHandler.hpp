#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "HMUI/InputFieldView.hpp"

namespace BSML {
    class BSML_EXPORT InputFieldViewHandler : public TypeHandler<HMUI::InputFieldView*> {
        public:
            using Base = TypeHandler<HMUI::InputFieldView*>;
            InputFieldViewHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
            virtual void HandleType(const BSML::ComponentTypeWithData& componentType, BSML::BSMLParserParams& parserParams) override;
    };
}
