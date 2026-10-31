#include "BSML/Tags/ModifierContainerTag.hpp"
#include "logging.hpp"

#include "BSML/Components/Backgroundable.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/UI/ContentSizeFitter.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"
#include "UnityEngine/RectOffset.hpp"

using namespace UnityEngine;
using namespace UnityEngine::UI;

namespace BSML {
    static BSMLNodeParser<ModifierContainerTag> modifierContainerTagParser({"modifier-container"});

    UnityEngine::GameObject* ModifierContainerTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating ModifierContainer");

        auto gameObject = GameObject::New_ctor("BSMLModifierContainer");
        gameObject->get_transform()->SetParent(parent, false);

        auto vertical = gameObject->AddComponent<VerticalLayoutGroup*>();
        vertical->set_padding(RectOffset::New_ctor(3, 3, 2, 2));
        vertical->set_childControlHeight(false);
        vertical->set_childForceExpandHeight(false);

        gameObject->AddComponent<ContentSizeFitter*>()->set_verticalFit(ContentSizeFitter::FitMode::PreferredSize);
        // I saw that this one was not there on pc, but I want people to be able to use it anyways, it doesn't do anything unless you tell it to anyways
        gameObject->AddComponent<Backgroundable*>();

        auto rectTransform = vertical->get_rectTransform();
        rectTransform->set_anchoredPosition({0, 3});
        rectTransform->set_anchorMin({0.5f, 0.5f});
        rectTransform->set_anchorMax({0.5f, 0.5f});
        rectTransform->set_sizeDelta({54, 3});

        gameObject->AddComponent<LayoutElement*>();
        return gameObject;
    }
}
