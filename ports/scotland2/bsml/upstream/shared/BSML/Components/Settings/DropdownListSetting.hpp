#pragma once

#include "custom-types/shared/macros.hpp"
#include "BaseSetting.hpp"
#include "../../GenericSettingWrapper.hpp"
#include "HMUI/DropdownWithTableView.hpp"
#include "HMUI/SimpleTextDropdown.hpp"

DECLARE_CLASS_CUSTOM(BSML, DropdownListSetting, BSML::BaseSetting) {
    DECLARE_INSTANCE_FIELD(GenericSettingWrapper*, genericSetting);
    DECLARE_INSTANCE_FIELD(HMUI::SimpleTextDropdown*, dropdown);
    DECLARE_INSTANCE_FIELD(ListW<System::Object*>, values);
    DECLARE_INSTANCE_FIELD(int, index);                             /* default: 0; */

    DECLARE_INSTANCE_METHOD(System::Object*, get_Value);
    DECLARE_INSTANCE_METHOD(void, set_Value, System::Object* value);

    DECLARE_INSTANCE_METHOD(bool, get_interactable);
    DECLARE_INSTANCE_METHOD(void, set_interactable, bool value);

    DECLARE_INSTANCE_METHOD(void, Setup);
    DECLARE_INSTANCE_METHOD(void, UpdateChoices);
    DECLARE_INSTANCE_METHOD(void, OnSelectIndex, HMUI::DropdownWithTableView* tableView, int index);
    DECLARE_INSTANCE_METHOD(void, ReceiveValue);
    DECLARE_INSTANCE_METHOD(void, ApplyValue);
    DECLARE_INSTANCE_METHOD(void, ValidateRange);
    DECLARE_INSTANCE_METHOD(void, UpdateState);

    DECLARE_CTOR(ctor);

    public:
        std::function<void(System::Object*)> onChange = nullptr;
        std::function<StringW(System::Object*)> formatter = nullptr;
};