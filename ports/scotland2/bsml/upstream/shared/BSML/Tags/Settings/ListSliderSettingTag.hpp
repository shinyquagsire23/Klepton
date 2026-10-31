#pragma once

#include "../../../_config.h"
#include "../Settings/GenericSliderSettingTag.hpp"
#include "../../Components/Settings/ListSliderSetting.hpp"

namespace BSML {
    class BSML_EXPORT ListSliderSettingTag : public GenericSliderSettingTag<BSML::ListSliderSetting*> {
        private:
            using Base = GenericSliderSettingTag<BSML::ListSliderSetting*>;
        public:
            ListSliderSettingTag() : Base() {}
        private:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override {
                return Base::CreateObject(parent);
            }
    };
}
