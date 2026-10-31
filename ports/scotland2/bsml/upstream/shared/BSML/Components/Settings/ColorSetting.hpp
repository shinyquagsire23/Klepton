#pragma once

#include "BaseSetting.hpp"
#include "../ModalColorPicker.hpp"
#include "../../GenericSettingWrapper.hpp"
#include "UnityEngine/UI/Button.hpp"

DECLARE_CLASS_CUSTOM(BSML, ColorSetting, BSML::BaseSetting) {
    DECLARE_INSTANCE_FIELD(GenericSettingWrapper*, genericSetting);
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::Button*, editButton);
    DECLARE_INSTANCE_FIELD(ModalColorPicker*, modalColorPicker);

    DECLARE_INSTANCE_FIELD(UnityEngine::UI::Image*, colorImage);
    DECLARE_INSTANCE_FIELD(UnityEngine::Color, currentColor);

    DECLARE_INSTANCE_METHOD(UnityEngine::Color, get_currentColor);
    DECLARE_INSTANCE_METHOD(void, set_currentColor, UnityEngine::Color value);

    DECLARE_INSTANCE_METHOD(bool, get_interactable);
    DECLARE_INSTANCE_METHOD(void, set_interactable, bool value);

    DECLARE_INSTANCE_METHOD(void, Setup);
    DECLARE_INSTANCE_METHOD(void, EditButtonPressed);
    DECLARE_INSTANCE_METHOD(void, DonePressed, UnityEngine::Color color);
    DECLARE_INSTANCE_METHOD(void, CancelPressed);
    DECLARE_INSTANCE_METHOD(void, ApplyValue);
    DECLARE_INSTANCE_METHOD(void, ReceiveValue);

    DECLARE_CTOR(ctor);

};