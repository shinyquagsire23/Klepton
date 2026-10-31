#pragma once

#include "custom-types/shared/macros.hpp"
#include "SliderSettingBase.hpp"
#include <map>

DECLARE_CLASS_CUSTOM(BSML, ListSliderSetting, BSML::SliderSettingBase) {
    DECLARE_INSTANCE_METHOD(void, Setup);
    DECLARE_INSTANCE_METHOD(void, OnDestroy);
    DECLARE_INSTANCE_METHOD(void, ReceiveValue);
    DECLARE_INSTANCE_METHOD(void, ApplyValue);

    DECLARE_INSTANCE_METHOD(System::Object*, get_Value);
    DECLARE_INSTANCE_METHOD(void, set_Value, System::Object* value);
    DECLARE_INSTANCE_METHOD(int, get_index);

    DECLARE_INSTANCE_FIELD(ListW<System::Object*>, values);

    DECLARE_INSTANCE_METHOD(void, OnChange, HMUI::RangeValuesTextSlider* _, float value);
    DECLARE_INSTANCE_METHOD(StringW, TextForValue, System::Object* value);

    DECLARE_CTOR(ctor);

    public:
        static std::map<HMUI::RangeValuesTextSlider*, BSML::ListSliderSetting*> remappers;
        /* set a formatter if you want to give your own format to the setting */
        std::function<StringW(System::Object*)> formatter = nullptr;

};