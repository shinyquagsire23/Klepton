#pragma once

#include "../../../_config.h"
#include "../Settings/IncDecSettingTag.hpp"
#include "../../Components/Settings/IncrementSetting.hpp"

namespace BSML {
    class BSML_EXPORT IncrementSettingTag : public IncDecSettingTag<BSML::IncrementSetting*> {
    private:
        using Base = IncDecSettingTag<BSML::IncrementSetting*>;
    public:
        IncrementSettingTag() : Base() {}
    private:
        virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override {
            return Base::CreateObject(parent);
        }
    };
}
