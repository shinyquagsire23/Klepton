#pragma once

#include "custom-types/shared/macros.hpp"
#include "SettingsMenuListViewController.hpp"
#include "../SettingsMenu.hpp"
#include "HMUI/ViewController.hpp"
#include "HMUI/NavigationController.hpp"
#include "HMUI/FlowCoordinator.hpp"
#include "System/Collections/Generic/Stack_1.hpp"

template<typename T>
using Stack = System::Collections::Generic::Stack_1<T>;

DECLARE_CLASS_CODEGEN(BSML, ModSettingsFlowCoordinator, HMUI::FlowCoordinator) {
    DECLARE_INSTANCE_FIELD(bool, isAnimating);
    DECLARE_INSTANCE_FIELD(bool, isPresenting);

    DECLARE_INSTANCE_FIELD(HMUI::ViewController*, activeController);
    DECLARE_INSTANCE_FIELD(HMUI::NavigationController*, navigationController);

    DECLARE_INSTANCE_FIELD(SettingsMenuListViewController*, settingsMenuListViewController);
    DECLARE_INSTANCE_FIELD(Stack<HMUI::ViewController*>*, submenuStack);
    DECLARE_INSTANCE_FIELD(UnityEngine::Transform*, bottomButtons);

    DECLARE_OVERRIDE_METHOD_MATCH(void, DidActivate, &HMUI::FlowCoordinator::DidActivate, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling);
    DECLARE_OVERRIDE_METHOD_MATCH(void, BackButtonWasPressed, &HMUI::FlowCoordinator::BackButtonWasPressed, HMUI::ViewController* topViewController);

    DECLARE_INSTANCE_METHOD(void, Ok);
    DECLARE_INSTANCE_METHOD(void, Cancel);
    DECLARE_INSTANCE_METHOD(void, Back);
    DECLARE_INSTANCE_METHOD(void, ShowInitial);

    DECLARE_CTOR(ctor);
    public:
        void OpenMenu(SettingsMenu* menu);
        void OpenMenu(HMUI::ViewController* viewController, bool isSubmenu, bool isBack);
    protected:
        std::shared_ptr<BSMLParserParams> parserParams;
        void EmitEventToAll(std::string_view ev);
};