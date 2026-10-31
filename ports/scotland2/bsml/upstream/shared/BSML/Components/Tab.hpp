#pragma once

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/MonoBehaviour.hpp"

namespace BSML {
    class TabSelector;
}

DECLARE_CLASS_CODEGEN(BSML, Tab, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_FIELD(System::Object*, _selector);
    DECLARE_INSTANCE_FIELD(StringW, tabName);
    DECLARE_INSTANCE_FIELD(StringW, tabKey);
    DECLARE_INSTANCE_FIELD(bool, isVisible);

    TabSelector* get_selector();
    TabSelector const* get_selector() const;
    void set_selector(TabSelector* value);
    __declspec(property(get=get_selector, put=set_selector)) TabSelector* selector;

    DECLARE_INSTANCE_METHOD(bool, get_isVisible);
    DECLARE_INSTANCE_METHOD(void, set_isVisible, bool value);

    DECLARE_INSTANCE_METHOD(StringW, get_tabName);
    DECLARE_INSTANCE_METHOD(void, set_tabName, StringW value);

    DECLARE_INSTANCE_METHOD(StringW, get_tabKey);
    DECLARE_INSTANCE_METHOD(void, set_tabKey, StringW value);

    DECLARE_CTOR(ctor);
};