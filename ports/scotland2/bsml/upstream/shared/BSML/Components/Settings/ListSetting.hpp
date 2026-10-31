#pragma once

#include "custom-types/shared/macros.hpp"
#include "IncDecSetting.hpp"

DECLARE_CLASS_CUSTOM(BSML, ListSetting, BSML::IncDecSetting) {
    DECLARE_INSTANCE_METHOD(void, Setup);
    DECLARE_INSTANCE_METHOD(void, IncButtonPressed);
    DECLARE_INSTANCE_METHOD(void, DecButtonPressed);
    DECLARE_INSTANCE_METHOD(void, EitherPressed);
    DECLARE_INSTANCE_METHOD(void, ReceiveValue);
    DECLARE_INSTANCE_METHOD(void, ApplyValue);
    DECLARE_INSTANCE_METHOD(void, ValidateRange);
    DECLARE_INSTANCE_METHOD(void, UpdateState);

    DECLARE_INSTANCE_METHOD(System::Object*, get_Value);
    DECLARE_INSTANCE_METHOD(void, set_Value, System::Object* value);

    DECLARE_INSTANCE_FIELD(ListW<System::Object*>, values);
    DECLARE_INSTANCE_FIELD(int, index); /* default: 0; */

    DECLARE_CTOR(ctor);

    public:
        /* set a formatter if you want to give your own format to the setting */
        std::function<StringW(System::Object*)> formatter = nullptr;
};