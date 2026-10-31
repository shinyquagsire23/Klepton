#pragma once

#include "../../../_config.h"
#include "../TypeHandler.hpp"
#include "../../Components/Settings/DropdownListSetting.hpp"

namespace BSML {
    class BSML_EXPORT DropdownListSettingHandler : public TypeHandler<BSML::DropdownListSetting*> {
        public:
            using Base = TypeHandler<BSML::DropdownListSetting*>;
            DropdownListSettingHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;

            virtual void HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) override;
    };
}
