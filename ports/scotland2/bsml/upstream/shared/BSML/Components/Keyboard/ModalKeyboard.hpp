#pragma once

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "../ModalView.hpp"
#include "../../GenericSettingWrapper.hpp"

#include "Key.hpp"
#include "Keyboard.hpp"


DECLARE_CLASS_CODEGEN(BSML, ModalKeyboard, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_FIELD(GenericSettingWrapper*, genericSetting);
    DECLARE_INSTANCE_FIELD(ModalView*, modalView);
    DECLARE_INSTANCE_FIELD(Keyboard*, keyboard);
    DECLARE_INSTANCE_FIELD(bool, clearOnOpen);
    
    DECLARE_INSTANCE_METHOD(void, OnEnable);
    DECLARE_INSTANCE_METHOD(void, OnEnter, StringW value);
    DECLARE_INSTANCE_METHOD(void, SetText, StringW value);

    DECLARE_CTOR(ctor);

    public:
        std::function<void(StringW)> onEnter = nullptr;
};