#include "BSML/Components/Settings/SliderSetting.hpp"
#include "Helpers/delegates.hpp"
#include "logging.hpp"

#include "UnityEngine/GameObject.hpp"
#include "TMPro/TextMeshProUGUI.hpp"

#include <limits>

DEFINE_TYPE(BSML, SliderSetting);

namespace BSML {
    std::map<HMUI::RangeValuesTextSlider*, BSML::SliderSetting*> SliderSetting::remappers = {};

    void SliderSetting::ctor() {
        SliderSettingBase::construct();
        isInt = false;
        increments = 1;
        lastValue = -std::numeric_limits<float>::infinity();
        digits = 2;
    }
    void SliderSetting::OnDestroy() {
        remappers.erase(slider);
    }

    float SliderSetting::get_Value() {
        return slider ? slider->get_value() : 0.0f;
    }

    void SliderSetting::set_Value(float value) {
        if (slider) {
            slider->set_value(value);
        }
    }

    void SliderSetting::Setup() {
        if (slider) {
            remappers[slider] = this;
            text = slider->gameObject->GetComponentInChildren<TMPro::TextMeshProUGUI*>();

            // steps is range(max - min) divided by increments;
            int steps = (slider->maxValue - slider->minValue) / increments;
            slider->set_numberOfSteps(steps + 1);
            ReceiveValue();

            auto onChangeInfo = il2cpp_functions::class_get_method_from_name(this->klass, "OnChange", 2);
            auto delegate = MakeSystemAction<UnityW<HMUI::RangeValuesTextSlider>, float>(this, onChangeInfo);

            slider->add_valueDidChangeEvent(delegate);
        }
    }

    void SliderSetting::OnChange(HMUI::RangeValuesTextSlider* _, float value) {
        if (isInt)
            value = (int)value;

        if (lastValue == value)
            return;

        lastValue = value;

        if (genericSetting) {
            if (isInt) {
                genericSetting->OnChange<int>(value);
            } else {
                genericSetting->OnChange<float>(value);
            }

            if (onChange) onChange(value);
            if (genericSetting->applyOnChange) ApplyValue();
        }
    }

    void SliderSetting::ApplyValue() {
        if (!genericSetting) return;

        if (genericSetting) {
            if (isInt) {
                genericSetting->SetValue<int>(slider->get_value());
            } else {
                genericSetting->SetValue(slider->get_value());
            }
        }
    }

    void SliderSetting::ReceiveValue() {
        if (genericSetting) {
            if (isInt) {
                set_Value(genericSetting->GetValue<int>());
            } else {
                set_Value(genericSetting->GetValue<float>());
            }
        }
    }

    StringW SliderSetting::TextForValue(float value) {
        // since the formatter can't differentiate between int and float, we just call it
        if (formatter) return formatter(value);

        if (isInt) {
            return fmt::format("{}", (int)value);
        } else {
            return fmt::format("{:.{}f}", value, digits);
        }
    }
}
