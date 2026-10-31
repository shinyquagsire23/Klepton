#pragma once

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/EventSystems/IPointerClickHandler.hpp"
#include "UnityEngine/EventSystems/IPointerEnterHandler.hpp"
#include "UnityEngine/EventSystems/IPointerExitHandler.hpp"
#include "UnityEngine/EventSystems/PointerEventData.hpp"
#include "UnityEngine/Color.hpp"

#include "GlobalNamespace/HapticFeedbackManager.hpp"
#include "GlobalNamespace/Signal.hpp"
#include "Libraries/HM/HMLib/VR/HapticPresetSO.hpp"
#include "HMUI/ImageView.hpp"
#include "VRUIControls/VRPointer.hpp"

#include "beatsaber-hook/shared/utils/typedefs-wrappers.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(BSML, ClickableImage, HMUI::ImageView, UnityEngine::EventSystems::IPointerClickHandler*, UnityEngine::EventSystems::IPointerEnterHandler*, UnityEngine::EventSystems::IPointerExitHandler*, UnityEngine::EventSystems::IEventSystemHandler*) {
    DECLARE_INSTANCE_METHOD(void, set_highlightColor, UnityEngine::Color color);
    DECLARE_INSTANCE_METHOD(UnityEngine::Color, get_highlightColor);
    DECLARE_INSTANCE_METHOD(void, set_defaultColor, UnityEngine::Color color);
    DECLARE_INSTANCE_METHOD(UnityEngine::Color, get_defaultColor);
    DECLARE_INSTANCE_METHOD(void, UpdateHighlight);
    DECLARE_INSTANCE_METHOD(bool, get_isHighlighted);
    DECLARE_INSTANCE_METHOD(void, set_isHighlighted, bool value);
    DECLARE_INSTANCE_METHOD(void, Vibrate, bool left);

    DECLARE_OVERRIDE_METHOD_MATCH(void, OnPointerClick, &UnityEngine::EventSystems::IPointerClickHandler::OnPointerClick, UnityEngine::EventSystems::PointerEventData* eventData);
    DECLARE_OVERRIDE_METHOD_MATCH(void, OnPointerEnter, &UnityEngine::EventSystems::IPointerEnterHandler::OnPointerEnter, UnityEngine::EventSystems::PointerEventData* eventData);
    DECLARE_OVERRIDE_METHOD_MATCH(void, OnPointerExit, &UnityEngine::EventSystems::IPointerExitHandler::OnPointerExit, UnityEngine::EventSystems::PointerEventData* eventData);

    /* purely here because ImageView is larger than it says it is */
    DECLARE_INSTANCE_FIELD(void*, padding1);
    DECLARE_INSTANCE_FIELD(void*, padding2);

    DECLARE_INSTANCE_FIELD(GlobalNamespace::Signal*, buttonClickedSignal);
    DECLARE_INSTANCE_FIELD(GlobalNamespace::HapticFeedbackManager*, hapticFeedbackManager);
    DECLARE_INSTANCE_FIELD(Libraries::HM::HMLib::VR::HapticPresetSO*, hapticFeedbackPresetSO);
    DECLARE_INSTANCE_FIELD(VRUIControls::VRPointer*, _vrPointer);

    DECLARE_INSTANCE_FIELD(UnityEngine::Color, highlightColor);
    DECLARE_INSTANCE_FIELD(UnityEngine::Color, defaultColor);
    DECLARE_INSTANCE_FIELD(bool, isHighlighted);

    DECLARE_CTOR(ctor);

    public:
        UnorderedEventCallback<> onClick;
        std::function<void()> onEnter;
        std::function<void()> onExit;

        VRUIControls::VRPointer* get_vrPointer();
        __declspec(property(get=get_vrPointer)) VRUIControls::VRPointer* vrPointer;
};