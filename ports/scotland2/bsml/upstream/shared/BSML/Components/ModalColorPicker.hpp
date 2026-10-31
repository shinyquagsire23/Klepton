#pragma once

#include "custom-types/shared/macros.hpp"
#include "ModalView.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "GlobalNamespace/RGBPanelController.hpp"
#include "GlobalNamespace/HSVPanelController.hpp"
#include "GlobalNamespace/ColorChangeUIEventType.hpp"
#include "UnityEngine/UI/Image.hpp"
#include "../GenericSettingWrapper.hpp"

DECLARE_CLASS_CODEGEN(BSML, ModalColorPicker, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_FIELD(GenericSettingWrapper*, genericSetting);
    DECLARE_INSTANCE_FIELD(System::Object*, host);
    DECLARE_INSTANCE_FIELD(BSML::ModalView*, modalView);
    DECLARE_INSTANCE_FIELD(GlobalNamespace::RGBPanelController*, rgbPanel);
    DECLARE_INSTANCE_FIELD(GlobalNamespace::HSVPanelController*, hsvPanel);
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::Image*, colorImage);
    DECLARE_INSTANCE_FIELD(UnityEngine::Color, currentColor);

    DECLARE_INSTANCE_METHOD(UnityEngine::Color, get_currentColor);
    DECLARE_INSTANCE_METHOD(void, set_currentColor, UnityEngine::Color value);
    
    DECLARE_INSTANCE_METHOD(void, OnEnable);
    DECLARE_INSTANCE_METHOD(void, CancelPressed);
    DECLARE_INSTANCE_METHOD(void, DonePressed);
    DECLARE_INSTANCE_METHOD(void, OnChange, UnityEngine::Color value, GlobalNamespace::ColorChangeUIEventType type);

    DECLARE_CTOR(ctor);
    public:
        std::function<void(UnityEngine::Color)> onChange = nullptr;
        std::function<void(UnityEngine::Color)> done = nullptr;
        std::function<void(void)> cancel = nullptr;
    protected:
        /* protected because these are for internal use */
        friend class ModalColorPickerData;
        friend class ColorSetting;
        friend class ModalColorPickerHandler;

        /* TODO replace with std::function? */
        const MethodInfo* onCancelInfo = nullptr;
        const MethodInfo* onDoneInfo = nullptr;
        const MethodInfo* colorChangeInfo = nullptr;
};