#pragma once

#include "../../../_config.h"
#include "../TypeHandler.hpp"
#include "../../Components/Settings/SliderSetting.hpp"

namespace BSML {
    class BSML_EXPORT SliderSettingHandler : public TypeHandler<BSML::SliderSetting*> {
        public:
            using Base = TypeHandler<BSML::SliderSetting*>;
            SliderSettingHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
    };
}
