#pragma once

#include "../../../_config.h"
#include "../TypeHandler.hpp"
#include "../../Components/Settings/ListSliderSetting.hpp"

namespace BSML {
    class BSML_EXPORT ListSliderSettingHandler : public TypeHandler<BSML::ListSliderSetting*> {
        public:
            using Base = TypeHandler<BSML::ListSliderSetting*>;
            ListSliderSettingHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;

            virtual void HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) override;
    };
}
