#include "BSML/Tags/ButtonWithIconTag.hpp"
#include "BSML/Components/ExternalComponents.hpp"
#include "BSML/Components/ButtonIconImage.hpp"
#include "Helpers/getters.hpp"
#include "Helpers/utilities.hpp"
#include "logging.hpp"

#include "BGLib/Polyglot/LocalizedTextMeshProUGUI.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "UnityEngine/UI/LayoutGroup.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/UI/ContentSizeFitter.hpp"
#include "HMUI/HoverHint.hpp"
#include "HMUI/ImageView.hpp"
#include "GlobalNamespace/LocalizedHoverHint.hpp"

using namespace UnityEngine;
using namespace UnityEngine::UI;

namespace BSML {
    static BSMLNodeParser<ButtonWithIconTag> buttonWithIconTagParser({"button-with-icon", "icon-button"});

    Button* get_buttonWithIconTemplate() {
        static SafePtrUnity<Button> buttonWithIconTemplate;
        if (!buttonWithIconTemplate) {
            buttonWithIconTemplate = Resources::FindObjectsOfTypeAll<Button*>()->LastOrDefault([&](auto x){ return x->get_name() == "PracticeButton"; });
        }
        return buttonWithIconTemplate.ptr();
    }

    UnityEngine::GameObject* ButtonWithIconTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating Button with icon");

        auto button = Object::Instantiate(get_buttonWithIconTemplate(), parent, false);
        button->set_name("BSMLIconButton");
        button->set_interactable(true);
        auto transform = button->transform.cast<RectTransform>();
        auto gameObject = button->get_gameObject();

        Object::Destroy(button->GetComponent<HMUI::HoverHint*>());
        Object::Destroy(button->GetComponent<GlobalNamespace::LocalizedHoverHint*>());

        auto externalComponents = gameObject->AddComponent<ExternalComponents*>();
        externalComponents->Add(button);
        externalComponents->Add(transform);

        auto contentTransform = transform->Find("Content");
        Object::Destroy(contentTransform->GetComponent<LayoutElement*>());

        Object::Destroy(contentTransform->Find("Text")->get_gameObject());

        auto iconImage = GameObject::New_ctor("Icon")->AddComponent<HMUI::ImageView*>();
        DEBUG("Iconimage: {}", fmt::ptr(iconImage));
        auto mat = Helpers::GetUINoGlowMat();
        DEBUG("Material: {}", fmt::ptr(mat));
        iconImage->set_material(mat);
        auto iconRectTransform = iconImage->get_rectTransform();
        iconRectTransform->SetParent(contentTransform, false);
        iconRectTransform->set_anchoredPosition({0, 0});
        iconRectTransform->set_sizeDelta({20, 20});
        iconRectTransform->set_anchorMin({0.5f, 0.f});
        iconRectTransform->set_anchorMax({0.5f, 0.5f});
        iconImage->set_preserveAspect(true);
        iconImage->set_sprite(Utilities::FindSpriteCached("EditIcon"));

        auto btnIcon = button->get_gameObject()->AddComponent<ButtonIconImage*>();
        btnIcon->image = iconImage;
        externalComponents->Add(btnIcon);

        auto buttonSizeFitter = gameObject->AddComponent<ContentSizeFitter*>();
        buttonSizeFitter->set_verticalFit(ContentSizeFitter::FitMode::PreferredSize);
        buttonSizeFitter->set_horizontalFit(ContentSizeFitter::FitMode::PreferredSize);
        externalComponents->Add(buttonSizeFitter);

        auto stackLayoutGroup = button->GetComponentInChildren<LayoutGroup*>();
        if (stackLayoutGroup)
            externalComponents->Add(stackLayoutGroup);

        auto layoutElement = gameObject->AddComponent<LayoutElement*>();
        layoutElement->set_preferredWidth(30.0f);
        externalComponents->Add(layoutElement);

        gameObject->SetActive(true);
        return gameObject;
    }
}
