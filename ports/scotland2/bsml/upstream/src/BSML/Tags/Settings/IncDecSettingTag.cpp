#include "BSML/Tags/Settings/IncDecSettingTag.hpp"
#include "logging.hpp"

#include "BSML/Components/ExternalComponents.hpp"

#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "BGLib/Polyglot/LocalizedTextMeshProUGUI.hpp"
#include "GlobalNamespace/FormattedFloatListSettingsController.hpp"
#include "UnityEngine/UI/Button.hpp"

using namespace UnityEngine;
using namespace UnityEngine::UI;

namespace BSML {
    GlobalNamespace::FormattedFloatListSettingsController* get_incdecValueControllerTemplate() {
        static SafePtrUnity<GlobalNamespace::FormattedFloatListSettingsController> incdecValueControllerTemplate;
        if (!incdecValueControllerTemplate) {
            incdecValueControllerTemplate = Resources::FindObjectsOfTypeAll<GlobalNamespace::FormattedFloatListSettingsController*>()->First([](auto x){ return x->get_name() == "VRRenderingScale";});
        }
        return incdecValueControllerTemplate.ptr();
    }

    UnityEngine::GameObject* IncDecSettingTagBase::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating IncDecSetting");

        auto baseSetting = Object::Instantiate(get_incdecValueControllerTemplate(), parent, false);
        auto gameObject = baseSetting->get_gameObject();
        Object::Destroy(baseSetting);
        gameObject->SetActive(false);

        auto transform = gameObject->transform.cast<RectTransform>();
        static ConstString name{"BSMLIncDecSetting"};
        gameObject->set_name(name);

        // since we require the tag to be made with a type that inherits IncDecSetting, we can use that as the holder for the value;
        IncDecSetting* setting = gameObject->AddComponent(get_type()).cast<IncDecSetting>();
        auto firstChild = transform->GetChild(1)->get_gameObject();

        setting->text = firstChild->GetComponentsInChildren<TMPro::TextMeshProUGUI*>()->First();
        setting->text->set_richText(true);
        setting->text->set_overflowMode(TMPro::TextOverflowModes::Ellipsis);

        auto buttons = firstChild->GetComponentsInChildren<Button*>();
        setting->decButton = buttons->First();
        setting->incButton = buttons->Last();
        firstChild->transform.cast<RectTransform>()->set_sizeDelta({40, 0});

        // text stuff
        auto nameText = transform->Find("NameText")->get_gameObject();
        Object::Destroy(nameText->GetComponent<BGLib::Polyglot::LocalizedTextMeshProUGUI*>());

        TMPro::TextMeshProUGUI* text = nameText->GetComponent<TMPro::TextMeshProUGUI*>();
        text->set_richText(true);
        text->set_text("BSMLIncDecSetting");

        // layout stuff
        auto layoutElement = gameObject->GetComponent<LayoutElement*>();
        layoutElement->set_preferredWidth(90);

        auto externalComponents = gameObject->AddComponent<ExternalComponents*>();
        externalComponents->Add(text);

        gameObject->SetActive(true);

        return gameObject;
    }
}
