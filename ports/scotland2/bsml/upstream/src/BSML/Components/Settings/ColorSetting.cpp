#include "BSML/Components/Settings/ColorSetting.hpp"
#include "Helpers/delegates.hpp"
#include "logging.hpp"

#include "UnityEngine/Events/UnityAction.hpp"
#include "UnityEngine/Color.hpp"

DEFINE_TYPE(BSML, ColorSetting);

using namespace UnityEngine;
using namespace UnityEngine::UI;

namespace BSML {
    void ColorSetting::ctor() {
        genericSetting = GenericSettingWrapper::New_ctor();
        currentColor = {1.0f, 1.0f, 1.0f, 1.0f};
    }

    UnityEngine::Color ColorSetting::get_currentColor() {
        return currentColor;
    }

    void ColorSetting::set_currentColor(UnityEngine::Color value) {
        currentColor = value;
        if (colorImage)
            colorImage->set_color(currentColor);
    }

    bool ColorSetting::get_interactable() {
        return editButton ? editButton->get_interactable() : false;
    }

    void ColorSetting::set_interactable(bool value) {
        if (editButton) editButton->set_interactable(value);
    }

    void ColorSetting::Setup() {
        if (modalColorPicker) {
            modalColorPicker->host = this;
            modalColorPicker->onDoneInfo = il2cpp_functions::class_get_method_from_name(this->klass, "DonePressed", 1);
            modalColorPicker->onCancelInfo = il2cpp_functions::class_get_method_from_name(this->klass, "CancelPressed", 0);
        } else {
            ERROR("No modalColorPicker found!");
        }

        auto delegate = MakeUnityAction(std::bind(&ColorSetting::EditButtonPressed, this));
        editButton->set_onClick(Button::ButtonClickedEvent::New_ctor());
        editButton->get_onClick()->AddListener(delegate);
        ReceiveValue();
    }

    void ColorSetting::EditButtonPressed() {
        modalColorPicker->set_currentColor(get_currentColor());
        modalColorPicker->modalView->Show();
    }

    void ColorSetting::DonePressed(UnityEngine::Color color) {
        set_currentColor(color);
        if (genericSetting) {
            genericSetting->OnChange(color);
            if (genericSetting->applyOnChange) ApplyValue();
        }
    }

    void ColorSetting::CancelPressed() {
        if (genericSetting)
            genericSetting->OnChange(get_currentColor());
    }

    void ColorSetting::ApplyValue() {
        if (!genericSetting) return;
        genericSetting->SetValue(get_currentColor());
    }

    void ColorSetting::ReceiveValue() {
        if (!genericSetting) return;
        set_currentColor(genericSetting->GetValueOpt<UnityEngine::Color>().value_or(currentColor));
    }

}
