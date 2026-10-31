#include "UI/ModalView.hpp"

#include <bsml/shared/BSML.hpp>

#include "assets.hpp"
#include "Log.hpp"

DEFINE_TYPE(AirbudsSearch::UI, ModalView);

namespace AirbudsSearch::UI {

void ModalView::ctor() {
    isInitialized_ = false;
    text_ = "";
    primaryButtonData_ = {
        true,
        "OK",
        nullptr
    };
    secondaryButtonData_ = {
        true,
        "Cancel",
        nullptr
    };
}

void ModalView::OnDisable() {
    hide(false);
}

void ModalView::PostParse() {
    modalWrapper_ = std::make_unique<Modal>(modal_);
}

void ModalView::show() {
    if (!isInitialized_) {
        isInitialized_ = true;
        BSML::parse_and_construct(IncludedAssets::BasicModal_bsml, this->get_transform(), this);
    }

    auto updateButtonProperties = [this](UnityW<UnityEngine::UI::Button> button, ButtonData properties) {
        button->get_gameObject()->set_active(properties.isVisible_);

        button->get_transform()->Find("Content/Text")->GetComponent<HMUI::CurvedTextMeshPro*>()->set_text(properties.label_);

        button->set_onClick(UnityEngine::UI::Button_ButtonClickedEvent::New_ctor());
        if (properties.onClicked_) {
            button->get_onClick()->AddListener(BSML::MakeUnityAction(properties.onClicked_));
        } else {
            button->get_onClick()->AddListener(BSML::MakeUnityAction(std::bind(&ModalView::hide, this, true)));
        }
    };

    // Update message
    textView_->set_text(text_);

    // Update buttons
    updateButtonProperties(primaryButton_, primaryButtonData_);
    updateButtonProperties(secondaryButton_, secondaryButtonData_);

    modalWrapper_->show();
}

void ModalView::hide(const bool animate) {
    if (!modalWrapper_) {
        return;
    }
    modalWrapper_->hide(animate);
}

void ModalView::setMessage(std::string_view message) {
    text_ = message;
}

void ModalView::setPrimaryButton(bool isEnabled, std::string_view label, std::function<void()> onClick) {
    primaryButtonData_ = {
        isEnabled,
        std::string(label),
        onClick
    };
}

void ModalView::setSecondaryButton(bool isEnabled, std::string_view label, std::function<void()> onClick) {
    secondaryButtonData_ = {
        isEnabled,
        std::string(label),
        onClick
    };
}

}
