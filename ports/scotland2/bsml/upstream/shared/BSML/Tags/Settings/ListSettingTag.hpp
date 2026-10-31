#pragma once


#include "../../../_config.h"
#include "../Settings/IncDecSettingTag.hpp"
#include "../../Components/Settings/ListSetting.hpp"

namespace BSML {
    class BSML_EXPORT ListSettingTag : public IncDecSettingTag<BSML::ListSetting*> {
    private:
        using Base = IncDecSettingTag<BSML::ListSetting*>;
    public:
        ListSettingTag() : Base() {}
    private:
        virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override {
            return Base::CreateObject(parent);
        }
    };
}
