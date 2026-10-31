#include "BSML/Tags/Settings/ToggleSettingTag.hpp"
#include "logging.hpp"

#include "BSML/Components/Settings/ToggleSetting.hpp"
#include "BSML/Components/ExternalComponents.hpp"

#include "UnityEngine/Resources.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Object.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/UI/Toggle.hpp"
#include "HMUI/AnimatedSwitchView.hpp"
#include "BGLib/Polyglot/LocalizedTextMeshProUGUI.hpp"
#include "GlobalNamespace/SwitchSettingsController.hpp"

using namespace UnityEngine;
using namespace UnityEngine::UI;

namespace BSML {
    static BSMLNodeParser<ToggleSettingTag> toggleSettingTagParser({"toggle-setting", "bool-setting", "checkbox-setting", "checkbox"});

    GameObject* get_toggleTemplate() {
        static SafePtrUnity<GameObject> toggleTemplate;
        if (!toggleTemplate) {
            auto foundToggle = Resources::FindObjectsOfTypeAll<Toggle*>()->FirstOrDefault([](auto x){
                if (!x) return false;
                auto parent = x->get_transform()->get_parent();
                if (!parent) return false;
                return parent->get_gameObject()->get_name() == "Fullscreen";
            });
            toggleTemplate = foundToggle ? foundToggle->get_transform()->get_parent()->get_gameObject() : nullptr;
        }
        return toggleTemplate.ptr();
    }

    UnityEngine::GameObject* ToggleSettingTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating ToggleSetting");

        auto go = Object::Instantiate(get_toggleTemplate(), parent, false);
        go->SetActive(false);
        auto transform = go->transform.cast<UnityEngine::RectTransform>();

        auto nameText = transform->Find("NameText")->get_gameObject();
        auto switchView = transform->Find("SwitchView")->get_gameObject();
        Object::Destroy(go->GetComponent<GlobalNamespace::SwitchSettingsController*>());
        
        go->set_name("BSMLToggle");
        auto toggleSetting = go->AddComponent<ToggleSetting*>();
        HMUI::AnimatedSwitchView* animatedSwitchView = switchView->GetComponent<HMUI::AnimatedSwitchView*>();

        Object::Destroy(nameText->GetComponent<BGLib::Polyglot::LocalizedTextMeshProUGUI*>());
        toggleSetting->toggle = switchView->GetComponent<Toggle*>();
        toggleSetting->toggle->onValueChanged = UnityEngine::UI::Toggle::ToggleEvent::New_ctor();
        toggleSetting->toggle->set_interactable(true);
        toggleSetting->toggle->set_isOn(false);

        toggleSetting->text = nameText->GetComponent<TMPro::TextMeshProUGUI*>();
        toggleSetting->text->set_text("BSML Toggle");
        toggleSetting->text->set_richText(true);
        toggleSetting->text->set_overflowMode(TMPro::TextOverflowModes::Ellipsis);

        auto layoutElement = go->GetComponent<LayoutElement*>();
        layoutElement->set_preferredWidth(90.0f);
        go->SetActive(true);

        auto externalComponents = go->AddComponent<ExternalComponents*>();
        externalComponents->Add(toggleSetting->text);

        return go;
    }
}
