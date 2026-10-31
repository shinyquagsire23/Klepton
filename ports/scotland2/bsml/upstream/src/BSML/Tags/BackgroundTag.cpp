#include "BSML/Tags/BackgroundTag.hpp"
#include "logging.hpp"

#include "BSML/Components/Backgroundable.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/UI/ContentSizeFitter.hpp"

using namespace UnityEngine;
using namespace UnityEngine::UI;

namespace BSML {
    static BSMLNodeParser<BackgroundTag> backgroundTagParser({"background", "bg", "div"});
    UnityEngine::GameObject* BackgroundTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating Background");
        auto gameObject = GameObject::New_ctor("BSMLBackground");
        gameObject->get_transform()->SetParent(parent, false);
        gameObject->AddComponent<ContentSizeFitter*>();
        gameObject->AddComponent<Backgroundable*>();

        auto rectTransform = gameObject->transform.cast<RectTransform>();
        rectTransform->set_anchorMin({0, 0});
        rectTransform->set_anchorMax({1, 1});
        rectTransform->set_sizeDelta({0, 0});

        return gameObject;
    }
}
