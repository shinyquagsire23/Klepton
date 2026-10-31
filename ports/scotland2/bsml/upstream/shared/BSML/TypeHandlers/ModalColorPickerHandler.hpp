#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "../Components/ModalColorPicker.hpp"

namespace BSML {
    class BSML_EXPORT ModalColorPickerHandler : public TypeHandler<BSML::ModalColorPicker*> {
        public:
            using Base = TypeHandler<BSML::ModalColorPicker*>;
            ModalColorPickerHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;

            virtual void HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) override;
            virtual void HandleTypeAfterParse(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) override;
    };
}
