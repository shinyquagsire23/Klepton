#pragma once

#include "custom-types/shared/macros.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "../../GenericSettingWrapper.hpp"
#include "BaseSetting.hpp"

DECLARE_CLASS_CUSTOM(BSML, IncDecSetting, BSML::BaseSetting) {
    DECLARE_INSTANCE_FIELD(GenericSettingWrapper*, genericSetting);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, text);
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::Button*, decButton);
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::Button*, incButton);

    DECLARE_INSTANCE_FIELD(bool, interactable);
    DECLARE_INSTANCE_FIELD(bool, enableDec);
    DECLARE_INSTANCE_FIELD(bool, enableInc);

    DECLARE_INSTANCE_METHOD(bool, get_interactable);
    DECLARE_INSTANCE_METHOD(void, set_interactable, bool value);

    DECLARE_INSTANCE_METHOD(void, set_enableDec, bool value);
    DECLARE_INSTANCE_METHOD(void, set_enableInc, bool value);

    DECLARE_INSTANCE_METHOD(void, set_text, StringW value);

    DECLARE_INSTANCE_METHOD(void, BaseSetup);

    DECLARE_CTOR(construct);
};