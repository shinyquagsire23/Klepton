#include "BSML/Components/Settings/IncDecSetting.hpp"
#include "Helpers/delegates.hpp"

#include "UnityEngine/Events/UnityAction.hpp"

DEFINE_TYPE(BSML, IncDecSetting);

namespace BSML {
    void IncDecSetting::construct() {
        genericSetting = GenericSettingWrapper::New_ctor();
        interactable = true;
        enableDec = true;
        enableInc = true;
    }

    void IncDecSetting::BaseSetup() {
        // we use reflection since we can't do proper virtual methods
        auto increaseInfo = il2cpp_functions::class_get_method_from_name(this->klass, "IncButtonPressed", 0);
        if (increaseInfo) {
            auto onClick = incButton->get_onClick();
            auto delegate = MakeUnityAction(this, increaseInfo);
            onClick->AddListener(delegate);
        }

        auto decreaseInfo = il2cpp_functions::class_get_method_from_name(this->klass, "DecButtonPressed", 0);
        if (decreaseInfo) {
            auto onClick = decButton->get_onClick();
            auto delegate = MakeUnityAction(this, decreaseInfo);
            onClick->AddListener(delegate);
        }
    }

    bool IncDecSetting::get_interactable() {
        return interactable;
    }

    void IncDecSetting::set_interactable(bool value) {
        if (interactable == value) return;
        interactable = value;
        set_enableDec(enableDec);
        set_enableInc(enableInc);
    }

    void IncDecSetting::set_enableDec(bool value) {
        enableDec = value;
        decButton->set_interactable(enableDec && interactable);
    }

    void IncDecSetting::set_enableInc(bool value) {
        enableInc = value;
        incButton->set_interactable(enableInc && interactable);
    }

    void IncDecSetting::set_text(StringW value) {
        text->set_text(value);
    }
}