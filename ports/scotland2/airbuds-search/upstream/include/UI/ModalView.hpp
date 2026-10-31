#pragma once

#include <HMUI/ModalView.hpp>
#include <TMPro/TextMeshProUGUI.hpp>
#include <UnityEngine/MonoBehaviour.hpp>
#include <UnityEngine/UI/Button.hpp>
#include <custom-types/shared/macros.hpp>

#include "Modal.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(AirbudsSearch::UI, ModalView, UnityEngine::MonoBehaviour) {
    DECLARE_CTOR(ctor);
    DECLARE_INSTANCE_METHOD(void, OnDisable);

    DECLARE_INSTANCE_METHOD(void, PostParse);

    DECLARE_INSTANCE_FIELD(UnityW<HMUI::ModalView>, modal_);
    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, textView_);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, primaryButton_);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, secondaryButton_);

    void show();
    void hide(bool animate);

    void setMessage(std::string_view message);
    void setPrimaryButton(bool isEnabled, std::string_view label, std::function<void()> onClick);
    void setSecondaryButton(bool isEnabled, std::string_view label, std::function<void()> onClick);

    private:
    bool isInitialized_;
    std::unique_ptr<Modal> modalWrapper_;

    struct ButtonData {
        bool isVisible_;
        std::string label_;
        std::function<void()> onClicked_;
    };

    std::string text_;
    ButtonData primaryButtonData_;
    ButtonData secondaryButtonData_;
};
