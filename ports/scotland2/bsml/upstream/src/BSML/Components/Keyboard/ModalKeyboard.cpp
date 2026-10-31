#include "BSML/Components/Keyboard/ModalKeyboard.hpp"

DEFINE_TYPE(BSML, ModalKeyboard);

namespace BSML {
    void ModalKeyboard::ctor() {
        genericSetting = GenericSettingWrapper::New_ctor();
    }

    void ModalKeyboard::OnEnable() {
        if (genericSetting && !clearOnOpen) {
            auto text = genericSetting->GetValue<Il2CppString*>();
            if (text) {
                SetText(text);
            }
        } else if (clearOnOpen) {
            SetText("");
        }
    }

    void ModalKeyboard::OnEnter(StringW value) {
        if (genericSetting) {
            genericSetting->SetValue(value);
        }

        if (onEnter) onEnter(value);
        modalView->Hide();
    }

    void ModalKeyboard::SetText(StringW value) {
        keyboard->keyboardText->set_text(value);
        keyboard->DrawCursor();
    }
}