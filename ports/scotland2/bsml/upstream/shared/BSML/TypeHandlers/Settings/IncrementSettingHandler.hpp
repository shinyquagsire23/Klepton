#pragma once

#include "../../../_config.h"
#include "../TypeHandler.hpp"
#include "../../Components/Settings/IncrementSetting.hpp"

namespace BSML {
    class BSML_EXPORT IncrementSettingHandler : public TypeHandler<BSML::IncrementSetting*> {
        public:
            using Base = TypeHandler<BSML::IncrementSetting*>;
            IncrementSettingHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
    };
}
