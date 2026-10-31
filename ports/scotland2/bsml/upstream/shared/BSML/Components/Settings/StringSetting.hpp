#pragma once

#include "custom-types/shared/macros.hpp"
#include "BaseSetting.hpp"
#include "../Keyboard/ModalKeyboard.hpp"
#include "../../GenericSettingWrapper.hpp"

#include "TMPro/TextMeshProUGUI.hpp"
#include "UnityEngine/UI/Button.hpp"

DECLARE_CLASS_CUSTOM(BSML, StringSetting, BSML::BaseSetting) {
    DECLARE_INSTANCE_FIELD(GenericSettingWrapper*, genericSetting);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, text);
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::Button*, editButton);

    DECLARE_INSTANCE_FIELD(UnityEngine::RectTransform*, boundingBox);
    DECLARE_INSTANCE_FIELD(ModalKeyboard*, modalKeyboard);
    
    DECLARE_INSTANCE_FIELD(StringW, currentValue);

    DECLARE_INSTANCE_METHOD(bool, get_interactable);
    DECLARE_INSTANCE_METHOD(void, set_interactable, bool value);

    DECLARE_INSTANCE_METHOD(StringW, get_text);
    DECLARE_INSTANCE_METHOD(void, set_text, StringW);

    DECLARE_INSTANCE_METHOD(void, BaseSetup);
    DECLARE_INSTANCE_METHOD(void, Setup);
    DECLARE_INSTANCE_METHOD(void, EditButtonPressed);
    DECLARE_INSTANCE_METHOD(void, EnterPressed, StringW value);
    DECLARE_INSTANCE_METHOD(void, ApplyValue);
    DECLARE_INSTANCE_METHOD(void, ReceiveValue);

    DECLARE_CTOR(ctor);

    public:
        std::function<StringW(StringW)> formatter = nullptr;
};