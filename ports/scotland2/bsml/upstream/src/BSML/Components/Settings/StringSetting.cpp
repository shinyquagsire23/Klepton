#include "BSML/Components/Settings/StringSetting.hpp"
#include "Helpers/delegates.hpp"
#include "logging.hpp"

#include "UnityEngine/Events/UnityAction.hpp"

DEFINE_TYPE(BSML, StringSetting);

using namespace UnityEngine;
using namespace UnityEngine::UI;

namespace BSML {
    void StringSetting::ctor() {
        genericSetting = GenericSettingWrapper::New_ctor();
        currentValue = "";
    }

    bool StringSetting::get_interactable() {
        return editButton ? editButton->get_interactable() : false;
    }

    void StringSetting::set_interactable(bool value) {
        if (editButton) editButton->set_interactable(value);
    }

    StringW StringSetting::get_text() {
        return currentValue;
    }

    void StringSetting::set_text(StringW value) {
        currentValue = value;
        if (text) 
            text->set_text(formatter ? formatter(value) : value);
    }

    void StringSetting::BaseSetup() {
        modalKeyboard->onEnter = std::bind(&StringSetting::EnterPressed, this, std::placeholders::_1);

        editButton->set_onClick(Button::ButtonClickedEvent::New_ctor());
        auto delegate = MakeUnityAction(std::bind(&StringSetting::EditButtonPressed, this));
        editButton->get_onClick()->AddListener(delegate);
    }

    void StringSetting::Setup() {
        modalKeyboard->clearOnOpen = false;
        ReceiveValue();
    }

    void StringSetting::EditButtonPressed() {
        modalKeyboard->modalView->Show();
        modalKeyboard->SetText(get_text());
    }

    void StringSetting::EnterPressed(StringW value) {
        set_text(value);
        if (genericSetting) {
            auto val = get_text();
            genericSetting->OnChange(val);
            if (genericSetting->applyOnChange) ApplyValue();
        }
    }

    void StringSetting::ApplyValue() {
        if (genericSetting) 
            genericSetting->SetValue(get_text());
    }

    void StringSetting::ReceiveValue() {
        if (!genericSetting) return; 
        set_text(genericSetting->GetValue<Il2CppString*>());
    }

}