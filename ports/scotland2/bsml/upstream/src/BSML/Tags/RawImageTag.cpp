#include "BSML/Tags/RawImageTag.hpp"
#include "Helpers/getters.hpp"
#include "logging.hpp"

#include "UnityEngine/UI/RawImage.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Vector2.hpp"

using namespace UnityEngine;

namespace BSML {
    static BSMLNodeParser<RawImageTag> rawImageTagParser({"raw-image", "raw-img"});

    UnityEngine::GameObject* RawImageTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating RawImage");
        auto gameObject = GameObject::New_ctor("BSMLRawImage");

        auto image = gameObject->AddComponent<UnityEngine::UI::RawImage*>();
        image->set_material(Helpers::GetUINoGlowMat());
        auto rectTransform = image->get_rectTransform();
        rectTransform->set_sizeDelta({20, 20});
        rectTransform->SetParent(parent, false);

        gameObject->AddComponent<UI::LayoutElement*>();
        return gameObject;
    }
}
