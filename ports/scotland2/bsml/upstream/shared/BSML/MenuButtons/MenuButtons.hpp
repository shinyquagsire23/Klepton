#pragma once

#include "custom-types/shared/macros.hpp"
#include "custom-types/shared/coroutine.hpp"
#include "HMUI/Screen.hpp"
#include "MenuButton.hpp"
#include "MenuButtonsViewController.hpp"
#include "UnityEngine/Coroutine.hpp"

DECLARE_CLASS_CODEGEN(BSML, MenuButtons, System::Object) {
    DECLARE_INSTANCE_FIELD(MenuButtonsViewController*, menuButtonsViewController);
    DECLARE_INSTANCE_FIELD(HMUI::Screen*, leftScreen);
    DECLARE_INSTANCE_FIELD(ListW<System::Object*>, _buttons);
    DECLARE_INSTANCE_FIELD(UnityEngine::Coroutine*, presentViewCoroutine);

    DECLARE_INSTANCE_METHOD(ListW<System::Object*>, get_buttons);
    DECLARE_INSTANCE_METHOD(bool, Registerbutton, MenuButton* button);
    DECLARE_INSTANCE_METHOD(bool, UnRegisterbutton, MenuButton* button);
    DECLARE_INSTANCE_METHOD(void, Setup);
    DECLARE_INSTANCE_METHOD(void, Refresh);
    DECLARE_INSTANCE_METHOD(void, ShowView, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling);

    DECLARE_DEFAULT_CTOR();
    public:
        static MenuButtons* get_instance();
        custom_types::Helpers::Coroutine PresentView();
    protected:
        static SafePtr<MenuButtons> instance;
};