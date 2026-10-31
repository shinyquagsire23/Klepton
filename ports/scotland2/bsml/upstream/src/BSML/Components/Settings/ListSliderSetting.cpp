#include "BSML/Components/Settings/ListSliderSetting.hpp"
#include "Helpers/delegates.hpp"

#include "System/Action_2.hpp"
#include "System/Object.hpp"
#include "UnityEngine/GameObject.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include <limits>

DEFINE_TYPE(BSML, ListSliderSetting);

namespace BSML {
    std::map<HMUI::RangeValuesTextSlider*, BSML::ListSliderSetting*> ListSliderSetting::remappers = {};

    void ListSliderSetting::ctor() {
        SliderSettingBase::construct();
        values = ListW<System::Object*>::New();
    }

    void ListSliderSetting::OnDestroy() {
        remappers.erase(slider);
    }

    void ListSliderSetting::Setup() {
        if (slider) {
            remappers[slider] = this;

            // steps is range(max - min) divided by increments;
            int steps = values.size();
            slider->set_minValue(0);
            slider->set_maxValue(steps - 1);
            slider->set_numberOfSteps(steps);
            text = slider->get_gameObject()->GetComponentInChildren<TMPro::TextMeshProUGUI*>();
            ReceiveValue();

            auto onChangeInfo = il2cpp_functions::class_get_method_from_name(this->klass, "OnChange", 2);
            auto delegate = MakeSystemAction<UnityW<HMUI::RangeValuesTextSlider>, float>(this, onChangeInfo);
            slider->add_valueDidChangeEvent(delegate);
        }
    }

    void ListSliderSetting::OnChange(HMUI::RangeValuesTextSlider* _, float value) {
        auto actualValue = get_Value();
        if (genericSetting) {
            genericSetting->OnChange(actualValue);
            if (genericSetting->applyOnChange) ApplyValue();
        }
    }

    StringW ListSliderSetting::TextForValue(System::Object* value) {
        if (formatter)
            return formatter(value);
        else {
            return value ? value->ToString() : "NULL";
        }
    }


    void ListSliderSetting::ReceiveValue() {
        if (!genericSetting) return;
        set_Value(genericSetting->GetValue<System::Object*>());
    }

    void ListSliderSetting::ApplyValue() {
        if (!genericSetting) return;
        genericSetting->SetValue(get_Value());
    }

    System::Object* ListSliderSetting::get_Value() {
        if (values.size() > 0)
            return values[get_index()];
        return nullptr;
    }

    void ListSliderSetting::set_Value(System::Object* value) {
        int index = 0;
        for (auto& v : values) {
            // if both are the same, or v has a value and Equals the value
            if ((v == value) || (v && v->Equals(value)))
                break;
            index++;
        }

        if (index == values.size())
            index = 0;

        slider->set_value(index);
    }

    int ListSliderSetting::get_index() {
        return slider ? slider->get_value() : 0;
    }
}
