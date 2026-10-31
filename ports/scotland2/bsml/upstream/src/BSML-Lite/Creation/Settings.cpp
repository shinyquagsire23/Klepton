#include "BSML-Lite/Creation/Settings.hpp"

#include "BSML/Components/ExternalComponents.hpp"
#include "custom-types/shared/delegate.hpp"

#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Sprite.hpp"
#include "UnityEngine/Events/UnityAction_1.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "HMUI/ImageView.hpp"

#define protected public
#define private public
#include "BSML/Tags/TextFieldTag.hpp"
#include "BSML/Tags/ModifierTag.hpp"
#include "BSML/Tags/Settings/ToggleSettingTag.hpp"
#include "BSML/Tags/Settings/IncrementSettingTag.hpp"
#include "BSML/Tags/Settings/SliderSettingTag.hpp"
#include "BSML/Tags/Settings/DropdownListSettingTag.hpp"
#include "BSML/Tags/Settings/ColorSettingTag.hpp"
#include "BSML/Tags/ModalColorPickerTag.hpp"
#undef protected
#undef private

namespace BSML::Lite {
    HMUI::InputFieldView* CreateStringSetting(const TransformWrapper& parent, StringW settingsName, StringW currentValue, UnityEngine::Vector2 anchoredPosition, UnityEngine::Vector3 keyboardPositionOffset, std::function<void(StringW)> onValueChange) {
        auto go = BSML::TextFieldTag{}.CreateObject(parent);
        auto textField = go->GetComponent<HMUI::InputFieldView*>();
        textField->_keyboardPositionOffset = keyboardPositionOffset;
        auto rect = go->transform.cast<UnityEngine::RectTransform>();
        rect->set_anchoredPosition(anchoredPosition);

        textField->onValueChanged = HMUI::InputFieldView::InputFieldChanged::New_ctor();
        if (onValueChange) {
            textField->onValueChanged->AddListener(
                    custom_types::MakeDelegate<UnityEngine::Events::UnityAction_1<UnityW<HMUI::InputFieldView>>*>(
                        std::function<void(HMUI::InputFieldView*)>(
                            [onValueChange](auto fieldView){
                                onValueChange(fieldView->get_text());
                            }
                        )
                    )
                );
        }

        textField->set_text(currentValue);

        textField->_placeholderText->GetComponent<TMPro::TextMeshProUGUI*>()->set_text(settingsName);

        return textField;
    }

    UnityEngine::UI::Toggle* CreateModifierButton(const TransformWrapper& parent, StringW buttonText, bool currentValue, UnityEngine::Sprite* iconSprite, std::function<void(bool)> onClick, UnityEngine::Vector2 anchoredPosition) {
        auto go = BSML::ModifierTag{}.CreateObject(parent);
        auto toggle = go->GetComponent<UnityEngine::UI::Toggle*>();
        auto externalComponents = go->GetComponent<BSML::ExternalComponents*>();
        auto text = externalComponents->Get<TMPro::TextMeshProUGUI*>();
        text->set_text(buttonText);

        if (iconSprite && iconSprite->m_CachedPtr.m_value) {
            auto img = externalComponents->Get<HMUI::ImageView*>();
            img->set_sprite(iconSprite);
        }

        auto rect = go->transform.cast<UnityEngine::RectTransform>();
        rect->set_anchoredPosition(anchoredPosition);
        if (onClick) {
            toggle->onValueChanged = UnityEngine::UI::Toggle::ToggleEvent::New_ctor();
            toggle->onValueChanged->AddListener(
                custom_types::MakeDelegate<UnityEngine::Events::UnityAction_1<bool>*>(onClick)
            );
        }

        toggle->set_isOn(currentValue);

        return toggle;
    }

    BSML::IncrementSetting* CreateIncrementSetting(const TransformWrapper& parent, StringW label, int decimals, float increment, float currentValue, bool hasMin, bool hasMax, float minValue, float maxValue, UnityEngine::Vector2 anchoredPosition, std::function<void(float)> onValueChange) {
        auto go = BSML::IncrementSettingTag{}.CreateObject(parent);
        auto incrementSetting = go->GetComponent<BSML::IncrementSetting*>();
        auto externalComponents = go->GetComponent<BSML::ExternalComponents*>();

        incrementSetting->digits = decimals;
        incrementSetting->isInt = std::abs(increment - 1.0f) < 0.00001f;
        incrementSetting->increments = increment;

        auto rect = go->transform.cast<UnityEngine::RectTransform>();
        rect->set_anchoredPosition(anchoredPosition);

        if (hasMin) incrementSetting->minValue = minValue;
        if (hasMax) incrementSetting->maxValue = maxValue;
        auto text = externalComponents->Get<TMPro::TextMeshProUGUI*>();
        text->set_text(label);

        incrementSetting->BaseSetup();
        incrementSetting->Setup();

        incrementSetting->set_Value(currentValue);

        incrementSetting->onChange = onValueChange;

        incrementSetting->set_Value(currentValue);

        return incrementSetting;
    }

    BSML::SliderSetting* CreateSliderSetting(const TransformWrapper& parent, StringW label, float increment, float currentValue, float minValue, float maxValue, float applyValueTime, UnityEngine::Vector2 anchoredPosition, std::function<void(float)> onValueChange) {
        return CreateSliderSetting(parent, label, increment, currentValue, minValue, maxValue, applyValueTime, false, anchoredPosition, onValueChange);
    }

    BSML::SliderSetting* CreateSliderSetting(const TransformWrapper& parent, StringW label, float increment, float currentValue, float minValue, float maxValue, float applyValueTime, bool showButtons, UnityEngine::Vector2 anchoredPosition, std::function<void(float)> onValueChange) {
        auto go = BSML::SliderSettingTag{}.CreateObject(parent);
        auto sliderSetting = go->GetComponent<BSML::SliderSetting*>();
        auto externalComponents = go->GetComponent<BSML::ExternalComponents*>();

        sliderSetting->digits = 2;
        sliderSetting->isInt = std::abs(increment - 1.0f) < 0.00001f;
        sliderSetting->increments = increment;

        auto rect = go->transform.cast<UnityEngine::RectTransform>();
        rect->set_anchoredPosition(anchoredPosition);

        sliderSetting->slider->set_minValue(minValue);
        sliderSetting->slider->set_maxValue(maxValue);
        auto text = externalComponents->Get<TMPro::TextMeshProUGUI*>();
        text->set_text(label);

        sliderSetting->showButtons = showButtons;
        sliderSetting->BaseSetup();
        sliderSetting->Setup();

        sliderSetting->set_Value(currentValue);

        sliderSetting->onChange = onValueChange;

        sliderSetting->set_Value(currentValue);

        return sliderSetting;
    }

    BSML::DropdownListSetting* CreateDropdown(const TransformWrapper& parent, StringW label, StringW currentValue, std::span<std::string_view> values, std::function<void(StringW)> onValueChange) {
        auto go = BSML::DropdownListSettingTag{}.CreateObject(parent);
        auto externalComponents = go->GetComponent<BSML::ExternalComponents*>();
        auto dropdownSetting = externalComponents->Get<BSML::DropdownListSetting*>();

        auto valuesList = ListW<System::Object*>::New();
        valuesList->EnsureCapacity(values.size());
        for (auto v : values) valuesList->Add(static_cast<System::Object*>(StringW(v).convert()));
        dropdownSetting->values = valuesList;

        dropdownSetting->Setup();

        // is this valid, this early?
        auto itr = std::find_if(values.begin(), values.end(), [currentValue](const auto& x){ return x == currentValue; });
        int idx = 0;
        if (itr != values.end()) {
            idx = itr - values.begin();
        }

        dropdownSetting->index = idx;
        dropdownSetting->dropdown->SelectCellWithIdx(idx);
        dropdownSetting->UpdateState();

        auto text = externalComponents->Get<TMPro::TextMeshProUGUI*>();
        text->set_text(label);

        dropdownSetting->onChange = onValueChange;

        return dropdownSetting;
    }

    BSML::ColorSetting* CreateColorPicker(const TransformWrapper& parent, StringW label, UnityEngine::Color defaultColor, std::function<void(UnityEngine::Color)> onDone, std::function<void()> onCancel, std::function<void(UnityEngine::Color)> onChange) {
        auto go = BSML::ColorSettingTag{}.CreateObject(parent);
        auto colorPicker = go->GetComponent<BSML::ColorSetting*>();
        auto externalComponents = go->GetComponent<BSML::ExternalComponents*>();
        auto text = externalComponents->Get<TMPro::TextMeshProUGUI*>();
        text->set_text(label);

        colorPicker->Setup();

        colorPicker->modalColorPicker->onChange = onChange;
        colorPicker->modalColorPicker->done = onDone;
        colorPicker->modalColorPicker->cancel = onCancel;

        colorPicker->set_currentColor(defaultColor);

        return colorPicker;
    }

    BSML::ModalColorPicker* CreateColorPickerModal(const TransformWrapper& parent, StringW name, UnityEngine::Color defaultColor, std::function<void(UnityEngine::Color)> onDone, std::function<void()> onCancel, std::function<void(UnityEngine::Color)> onChange) {
        auto go = BSML::ModalColorPickerTag{}.CreateObject(parent);
        auto colorPicker = go->GetComponent<BSML::ModalColorPicker*>();
        go->set_name(name);

        colorPicker->onChange = onChange;
        colorPicker->done = onDone;
        colorPicker->cancel = onCancel;

        colorPicker->currentColor = defaultColor;

        return colorPicker;
    }

    BSML::ToggleSetting* CreateToggle(const TransformWrapper& parent, StringW label, bool currentValue, UnityEngine::Vector2 anchoredPosition, std::function<void(bool)> onToggle) {
        auto go = BSML::ToggleSettingTag{}.CreateObject(parent);
        auto toggle = go->GetComponent<BSML::ToggleSetting*>();
        auto externalComponents = go->GetComponent<BSML::ExternalComponents*>();
        auto text = externalComponents->Get<TMPro::TextMeshProUGUI*>();
        text->set_text(label);

        auto rect = go->transform.cast<UnityEngine::RectTransform>();
        rect->set_anchoredPosition(anchoredPosition);

        toggle->set_Value(currentValue);

        if (onToggle) {
            toggle->toggle->onValueChanged = UnityEngine::UI::Toggle::ToggleEvent::New_ctor();
            toggle->toggle->onValueChanged->AddListener(
                custom_types::MakeDelegate<UnityEngine::Events::UnityAction_1<bool>*>(onToggle)
            );
        }

        return toggle;
    }
}
