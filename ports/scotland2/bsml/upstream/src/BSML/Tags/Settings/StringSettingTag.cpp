#include "BSML/Tags/Settings/StringSettingTag.hpp"
#include "Helpers/utilities.hpp"
#include "logging.hpp"

#include "BSML/Components/Settings/StringSetting.hpp"
#include "BSML/Components/ExternalComponents.hpp"

#include "UnityEngine/UI/Image.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/Object.hpp"
#include "BGLib/Polyglot/LocalizedTextMeshProUGUI.hpp"
#include "GlobalNamespace/FormattedFloatListSettingsController.hpp"
#include "GlobalNamespace/StepValuePicker.hpp"

using namespace UnityEngine;
using namespace UnityEngine::UI;

namespace BSML {
    static BSMLNodeParser<StringSettingTag> stringSettingTagParser({"string-setting"});
    GlobalNamespace::FormattedFloatListSettingsController* get_stringValueControllerTemplate() {
        static SafePtrUnity<GlobalNamespace::FormattedFloatListSettingsController> stringValueControllerTemplate;
        if (!stringValueControllerTemplate) {
            stringValueControllerTemplate = Resources::FindObjectsOfTypeAll<GlobalNamespace::FormattedFloatListSettingsController*>()->FirstOrDefault([](auto x){ return x->get_gameObject()->get_name() == "VRRenderingScale"; });
        }
        return stringValueControllerTemplate.ptr();
    }

    UnityEngine::GameObject* StringSettingTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating StringSetting");
        auto baseSetting = Object::Instantiate(get_stringValueControllerTemplate(), parent, false);
        baseSetting->set_name("BSMLStringSetting");

        auto gameObject = baseSetting->get_gameObject();
        gameObject->SetActive(false);
        Object::Destroy(baseSetting);

        auto stringSetting = gameObject->AddComponent<BSML::StringSetting*>();
        auto valuePick = gameObject->get_transform()->Find("ValuePicker");
        Object::Destroy(valuePick->GetComponent<GlobalNamespace::StepValuePicker*>());

        auto buttons = valuePick->GetComponentsInChildren<Button*>();
        auto decButton = buttons->FirstOrDefault();
        decButton->set_enabled(false);
        decButton->set_interactable(true);
        Object::Destroy(decButton->get_transform()->Find("Icon")->get_gameObject());
        stringSetting->text = valuePick->GetComponentsInChildren<TMPro::TextMeshProUGUI*>()->FirstOrDefault();
        stringSetting->text->set_richText(true);
        stringSetting->text->set_text("");
        stringSetting->editButton = buttons->LastOrDefault();
        stringSetting->boundingBox = valuePick.cast<RectTransform>();

        auto nameText = gameObject->get_transform()->Find("NameText")->get_gameObject();
        Object::Destroy(nameText->GetComponent<BGLib::Polyglot::LocalizedTextMeshProUGUI*>());

        auto text = nameText->GetComponent<TMPro::TextMeshProUGUI*>();
        text->set_text("BSMLStringSetting");

        auto layoutElement = gameObject->GetComponent<LayoutElement*>();
        layoutElement->set_preferredWidth(90.0f);
        stringSetting->text->set_alignment(TMPro::TextAlignmentOptions::MidlineRight);
        stringSetting->text->set_enableWordWrapping(false);
        auto textTransform = stringSetting->text->transform.cast<RectTransform>();
        textTransform->set_anchorMin({0, 0});
        textTransform->set_anchorMax({1, 1});
        textTransform->set_anchoredPosition({-6, 0});

        auto icon = stringSetting->editButton->get_transform()->Find("Icon")->GetComponent<Image*>();
        icon->set_name("EditIcon");
        icon->set_sprite(Utilities::FindSpriteCached("EditIcon"));
        icon->get_rectTransform()->set_sizeDelta({4, 4});
        stringSetting->editButton->set_interactable(true);
        stringSetting->editButton->transform.cast<RectTransform>()->set_anchorMin({0, 0});

        auto baseObject = Base::CreateObject(gameObject->get_transform());
        auto externalComponents = gameObject->AddComponent<ExternalComponents*>();
        stringSetting->modalKeyboard = baseObject->GetComponent<BSML::ModalKeyboard*>();
        externalComponents->Add(gameObject->get_transform());
        externalComponents->Add(baseObject->GetComponent<BSML::ModalView*>());
        externalComponents->Add(stringSetting->modalKeyboard);
        externalComponents->Add(stringSetting);
        externalComponents->Add(text);
        externalComponents->Add(layoutElement);

        gameObject->SetActive(true);
        return gameObject;
    }
}
