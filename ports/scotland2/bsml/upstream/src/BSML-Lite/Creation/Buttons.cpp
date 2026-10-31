#include "BSML-Lite/Creation/Buttons.hpp"
#include "logging.hpp"

#include "TMPro/TextMeshProUGUI.hpp"
#include "BGLib/Polyglot/LocalizedTextMeshProUGUI.hpp"
#include "UnityEngine/UI/Image.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/UI/ContentSizeFitter.hpp"
#include "UnityEngine/UI/LayoutGroup.hpp"
#include "UnityEngine/Sprite.hpp"
#include "UnityEngine/TextureWrapMode.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Events/UnityAction.hpp"
#include "HMUI/ButtonSpriteSwap.hpp"

#define protected public
#include "BSML/Tags/ButtonTag.hpp"
#include "BSML/Tags/PrimaryButtonTag.hpp"
#undef protected

#include "BSML/Components/ExternalComponents.hpp"

namespace BSML::Lite {
    UnityEngine::UI::Button* CreateUIButton(const TransformWrapper& parent, StringW buttonText, const std::string_view& buttonTemplate, UnityEngine::Vector2 anchoredPosition, UnityEngine::Vector2 sizeDelta, std::function<void()> onClick) {
        UnityEngine::UI::Button* button = nullptr;
        if (buttonTemplate == DEFAULT_BUTTONTEMPLATE) {
            auto go = BSML::ButtonTag{}.CreateObject(parent);
            button = go->GetComponent<UnityEngine::UI::Button*>();
        } else if (buttonTemplate == "PlayButton") {
            auto go = BSML::PrimaryButtonTag{}.CreateObject(parent);
            button = go->GetComponent<UnityEngine::UI::Button*>();
        } else {
            static std::unordered_map<std::string, SafePtrUnity<UnityEngine::UI::Button>> buttonCopyMap;
            auto& buttonCopy = buttonCopyMap[std::string(buttonTemplate)];
            if (!buttonCopy) {
                buttonCopy = UnityEngine::Resources::FindObjectsOfTypeAll<UnityEngine::UI::Button*>()->LastOrDefault([&buttonTemplate](auto* x) { return x->get_name() == buttonTemplate; });
            }

            if (!buttonCopy) {
                ERROR("Could not find button template {}, returning nullptr", buttonTemplate);
                return nullptr;
            }

            // it was none of the BSML provided buttons, let's just make it manually
            button = UnityEngine::Object::Instantiate(buttonCopy.ptr(), parent, false);
            button->set_name("BSMLButton");
            button->set_interactable(true);

            auto transform = button->transform.cast<UnityEngine::RectTransform>();
            auto gameObject = button->get_gameObject();
            gameObject->SetActive(true);
            auto externalComponents = gameObject->AddComponent<BSML::ExternalComponents*>();
            externalComponents->Add(button);
            externalComponents->Add(transform);

            auto textT = button->transform->Find("Content/Text");
            if (textT) {
                auto textObject = textT->gameObject;
                auto localizer = textObject->GetComponent<BGLib::Polyglot::LocalizedTextMeshProUGUI*>();
                if (localizer) UnityEngine::Object::Destroy(localizer);

                auto textMesh = textObject->GetComponent<TMPro::TextMeshProUGUI*>();
                if (textMesh) {
                    textMesh->set_text("BSMLButton");
                    textMesh->set_richText(true);
                    externalComponents->Add(textMesh);
                }
            }

            auto content = transform->Find("Content");
            if (content) {
                auto layout = content->GetComponent<UnityEngine::UI::LayoutElement*>();
                if (layout) UnityEngine::Object::Destroy(layout);
            }
            auto buttonSizeFitter = gameObject->AddComponent<UnityEngine::UI::ContentSizeFitter*>();
            buttonSizeFitter->set_verticalFit(UnityEngine::UI::ContentSizeFitter::FitMode::PreferredSize);
            buttonSizeFitter->set_horizontalFit(UnityEngine::UI::ContentSizeFitter::FitMode::PreferredSize);
            externalComponents->Add(buttonSizeFitter);

            auto stackLayoutGroup = button->GetComponentInChildren<UnityEngine::UI::LayoutGroup*>();
            if (stackLayoutGroup)
                externalComponents->Add(stackLayoutGroup);

            auto layoutElement = gameObject->GetComponent<UnityEngine::UI::LayoutElement*>();
            if (!layoutElement) layoutElement = gameObject->AddComponent<UnityEngine::UI::LayoutElement*>();

            layoutElement->set_preferredWidth(30.0f);
            externalComponents->Add(layoutElement);
        }

        auto localizedText = button->GetComponentInChildren<BGLib::Polyglot::LocalizedTextMeshProUGUI*>();
        if (localizedText && localizedText->m_CachedPtr.m_value) {
            UnityEngine::Object::Destroy(localizedText);
        }
        SetButtonText(button, buttonText);

        auto rect = button->transform.cast<UnityEngine::RectTransform>();
        rect->set_anchoredPosition(anchoredPosition);
        rect->set_sizeDelta(sizeDelta);
        auto layoutElement = button->GetComponent<UnityEngine::UI::LayoutElement*>();
        if (layoutElement) {
            if (sizeDelta.x != 0) layoutElement->preferredWidth = sizeDelta.x;
            if (sizeDelta.y != 0) layoutElement->preferredHeight = sizeDelta.y;
        }

        button->set_onClick(UnityEngine::UI::Button::ButtonClickedEvent::New_ctor());
        if (onClick) {
            button->get_onClick()->AddListener(custom_types::MakeDelegate<UnityEngine::Events::UnityAction*>(onClick));
        }
        return button;
    }

    UnityEngine::UI::Button* CreateUIButton(const TransformWrapper& parent, StringW buttonText, UnityEngine::Vector2 anchoredPosition, UnityEngine::Vector2 sizeDelta, std::function<void()> onClick)  {
        auto go = BSML::ButtonTag{}.CreateObject(parent);
        auto button = go->GetComponent<UnityEngine::UI::Button*>();

        SetButtonText(button, buttonText);

        auto rect = button->transform.cast<UnityEngine::RectTransform>();
        rect->set_anchoredPosition(anchoredPosition);
        rect->set_sizeDelta(sizeDelta);
        auto layoutElement = button->GetComponent<UnityEngine::UI::LayoutElement*>();
        if (layoutElement) {
            if (sizeDelta.x != 0) layoutElement->preferredWidth = sizeDelta.x;
            if (sizeDelta.y != 0) layoutElement->preferredHeight = sizeDelta.y;
        }

        button->set_onClick(UnityEngine::UI::Button::ButtonClickedEvent::New_ctor());
        if (onClick)
            button->get_onClick()->AddListener(custom_types::MakeDelegate<UnityEngine::Events::UnityAction*>(onClick));
        return button;
    }


    void SetButtonText(UnityEngine::UI::Button* button, StringW text) {
        if (!button) {
            ERROR("Can't set button text on nullptr button");
            return;
        }

        auto textMesh = button->GetComponentInChildren<TMPro::TextMeshProUGUI*>();
        if (textMesh)
            textMesh->set_text(text);
    }

    void SetButtonTextSize(UnityEngine::UI::Button* button, float fontSize) {
        auto textMesh = button->GetComponentInChildren<TMPro::TextMeshProUGUI*>();
        if (textMesh) textMesh->set_fontSize(fontSize);
    }

    void ToggleButtonWordWrapping(UnityEngine::UI::Button* button, bool enableWordWrapping) {
        auto textMesh = button->GetComponentInChildren<TMPro::TextMeshProUGUI*>();
        if (textMesh) textMesh->set_enableWordWrapping(enableWordWrapping);
    }

    void SetButtonIcon(UnityEngine::UI::Button* button, UnityEngine::Sprite* icon) {
        auto iconImage = button->GetComponentsInChildren<UnityEngine::UI::Image*>()->FirstOrDefault([](auto x){ return x->get_name() == "Icon"; });
        if (iconImage) iconImage->set_sprite(icon);
    }

    void SetButtonBackground(UnityEngine::UI::Button* button, UnityEngine::Sprite* background) {
        auto iconImage = button->GetComponentsInChildren<UnityEngine::UI::Image*>()->FirstOrDefault([](auto x){ return x->get_name() == "Background"; });
        if (iconImage) iconImage->set_sprite(background);
    }

    void SetButtonSprites(UnityEngine::UI::Button* button, UnityEngine::Sprite* inactive, UnityEngine::Sprite* active) {
        // make sure the textures are set to clamp
        inactive->get_texture()->set_wrapMode(UnityEngine::TextureWrapMode::Clamp);
        active->get_texture()->set_wrapMode(UnityEngine::TextureWrapMode::Clamp);

        auto spriteSwap = button->GetComponent<HMUI::ButtonSpriteSwap*>();

        // setting the sprites
        spriteSwap->_highlightStateSprite = active;
        spriteSwap->_pressedStateSprite = active;

        spriteSwap->_disabledStateSprite = inactive;
        spriteSwap->_normalStateSprite = inactive;
    }

}
