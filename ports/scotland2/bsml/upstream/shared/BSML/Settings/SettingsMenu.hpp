#pragma once

#include "custom-types/shared/macros.hpp"
#include "HMUI/FlowCoordinator.hpp"
#include "HMUI/ViewController.hpp"
#include "../Components/CustomListTableData.hpp"
#include "../Parsing/BSMLParserParams.hpp"
#include "../MenuSource.hpp"

DECLARE_CLASS_CUSTOM(BSML, SettingsMenu, CustomCellInfo) {
    public:
        DECLARE_INSTANCE_FIELD(StringW, name);
        DECLARE_INSTANCE_FIELD(StringW, content_key);
        DECLARE_INSTANCE_FIELD(System::Object*, host);
        DECLARE_INSTANCE_FIELD(bool, enableExtraButtons);
        DECLARE_INSTANCE_FIELD(HMUI::ViewController*, viewController);
        DECLARE_INSTANCE_FIELD(HMUI::FlowCoordinator*, flowCoordinator);
        DECLARE_INSTANCE_FIELD(System::Type*, csType);
        DECLARE_INSTANCE_FIELD(MenuSource, menuSource);

        DECLARE_INSTANCE_METHOD(HMUI::ViewController*, get_viewController);

        DECLARE_INSTANCE_METHOD(void, Setup);
        DECLARE_INSTANCE_METHOD(bool, get_didSetup);

        DECLARE_DEFAULT_CTOR();
        DECLARE_OVERRIDE_METHOD_MATCH(void, Finalize, &System::Object::Finalize);

    public:
        static SettingsMenu* Make_new(std::string_view name, std::string_view content_key, System::Object* host, bool enableExtraButtons = false);
        static SettingsMenu* Make_new(std::string_view name, System::Type* viewControllerType, MenuSource menuSource, bool enableExtraButtons = false);
        static SettingsMenu* Make_new(std::string_view name, std::function<void(HMUI::ViewController*, bool, bool, bool)> viewControllerDidActivate, bool enableExtraButtons);

        std::string_view get_content();
        std::shared_ptr<BSMLParserParams> parserParams;
        std::function<void(HMUI::ViewController*, bool, bool, bool)> viewControllerDidActivate;

        static void SetupViewControllerTransform(HMUI::ViewController* viewController);
};