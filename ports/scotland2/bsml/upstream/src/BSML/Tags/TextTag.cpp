#include "BSML/Tags/TextTag.hpp"
#include "Helpers/getters.hpp"
#include "Helpers/creation.hpp"
#include "logging.hpp"

#include "UnityEngine/RectTransform.hpp"
#include "HMUI/CurvedTextMeshPro.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Color.hpp"

using namespace UnityEngine;

namespace BSML {
    static BSMLNodeParser<TextTag> textTagParser({"text", "label"});
    UnityEngine::GameObject* TextTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating Text");
        auto gameObject = GameObject::New_ctor("BSMLText");
        gameObject->get_transform()->SetParent(parent, false);

        // on PC this is a FormattableText, but we're skipping that for now at least
        auto textMesh = gameObject->AddComponent<HMUI::CurvedTextMeshPro*>();
        textMesh->set_font(Helpers::GetMainTextFont());
        textMesh->set_fontSharedMaterial(Helpers::GetMainUIFontMaterial());
        textMesh->set_fontSize(4);
        textMesh->set_color({1.0f, 1.0f, 1.0f, 1.0f});
        textMesh->set_text("BSMLText");

        auto rectTransform = textMesh->get_rectTransform();
        rectTransform->set_anchorMin({0.5f, 0.5f});
        rectTransform->set_anchorMax({0.5f, 0.5f});

        return gameObject;
    }
}
