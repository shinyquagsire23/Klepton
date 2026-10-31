#include "BSML/Components/ClickableText.hpp"
#include "logging.hpp"

#include "UnityEngine/Resources.hpp"
#include "GlobalNamespace/VRController.hpp"
#include "UnityEngine/XR/XRNode.hpp"

DEFINE_TYPE(BSML, ClickableText);

namespace BSML {
    void ClickableText::ctor() {
        INVOKE_CTOR();

        isHighlighted = false;
        highlightColor = UnityEngine::Color(0.60f, 0.80f, 1.0f, 1.0f);
        defaultColor = UnityEngine::Color(1.0f, 1.0f, 1.0f, 1.0f);

        onEnter = nullptr;
        onExit = nullptr;

        buttonClickedSignal = nullptr;
        hapticFeedbackManager = nullptr;
        hapticFeedbackPresetSO = nullptr;

        static auto base_ctor = il2cpp_functions::class_get_method_from_name(classof(HMUI::CurvedTextMeshPro*), ".ctor", 0);
        if (base_ctor) { il2cpp_utils::RunMethod(this, base_ctor); }
        else { ERROR("Could not run base ctor for ClickableText"); }
    }

    void ClickableText::UpdateHighlight() {
        set_color(get_isHighlighted() ? get_highlightColor() : get_defaultColor());
    }

    void ClickableText::OnPointerClick(UnityEngine::EventSystems::PointerEventData* eventData) {
        INFO("Click");
        if (buttonClickedSignal) buttonClickedSignal->Raise();
        set_isHighlighted(false);
        if (onClick.size() > 0) onClick.invoke();
    }

    void ClickableText::OnPointerEnter(UnityEngine::EventSystems::PointerEventData* eventData) {
        INFO("Enter");
        set_isHighlighted(true);
        if (onEnter) onEnter();
        if (!vrPointer || !vrPointer->m_CachedPtr.m_value || !vrPointer->lastSelectedVrController) {
            ERROR("VRPointer or lastSelectedVrController is null in ClickableText::OnPointerEnter");
            return;
        }
        Vibrate(vrPointer->lastSelectedVrController->node == UnityEngine::XR::XRNode::LeftHand);
    }

    void ClickableText::OnPointerExit(UnityEngine::EventSystems::PointerEventData* eventData) {
        INFO("Exit");
        set_isHighlighted(false);
        if (onExit) onExit();
    }

    void ClickableText::Vibrate(bool left)
    {
        UnityEngine::XR::XRNode node = left ? UnityEngine::XR::XRNode::LeftHand : UnityEngine::XR::XRNode::RightHand;
        if (hapticFeedbackManager && hapticFeedbackPresetSO) hapticFeedbackManager->PlayHapticFeedback(node, hapticFeedbackPresetSO);
    }

    void ClickableText::set_highlightColor(UnityEngine::Color color) {
        highlightColor = color;
        UpdateHighlight();
    }

    UnityEngine::Color ClickableText::get_highlightColor() {
        return highlightColor;
    }

    void ClickableText::set_defaultColor(UnityEngine::Color color) {
        defaultColor = color;
        UpdateHighlight();
    }

    UnityEngine::Color ClickableText::get_defaultColor() {
        return defaultColor;
    }

    bool ClickableText::get_isHighlighted() {
        return isHighlighted;
    }

    void ClickableText::set_isHighlighted(bool value) {
        isHighlighted = value;
        UpdateHighlight();
    }

    VRUIControls::VRPointer* ClickableText::get_vrPointer() {
        if (_vrPointer && _vrPointer->m_CachedPtr.m_value) {
            return _vrPointer;
        } else {
            _vrPointer = nullptr;
        }

        _vrPointer = UnityEngine::Resources::FindObjectsOfTypeAll<VRUIControls::VRPointer*>()->FirstOrDefault();
        return _vrPointer;
    }
}
