#include "BSML/Tags/VerticalTag.hpp"
#include "logging.hpp"

#include "BSML/Components/Backgroundable.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"
#include "UnityEngine/UI/ContentSizeFitter.hpp"

using namespace UnityEngine;
using namespace UnityEngine::UI;

namespace BSML {
    static BSMLNodeParser<VerticalTag> verticalTagParser({"vertical"});
    UnityEngine::GameObject* VerticalTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating Vertical");
        auto gameObject = GameObject::New_ctor("BSMLVerticalLayoutGroup");
        gameObject->get_transform()->SetParent(parent, false);
        auto horizontal = gameObject->AddComponent<VerticalLayoutGroup*>();

        auto contentSizeFitter = gameObject->AddComponent<ContentSizeFitter*>();
        contentSizeFitter->set_horizontalFit(ContentSizeFitter::FitMode::PreferredSize);
        gameObject->AddComponent<Backgroundable*>();

        auto rectTransform = horizontal->get_rectTransform();
        rectTransform->set_anchorMin({0, 0});
        rectTransform->set_anchorMax({1, 1});
        rectTransform->set_sizeDelta({0, 0});

        gameObject->AddComponent<LayoutElement*>();
        return gameObject;
    }
}
