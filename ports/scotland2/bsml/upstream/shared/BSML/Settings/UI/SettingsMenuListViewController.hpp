#pragma once

#include "custom-types/shared/macros.hpp"
#include "HMUI/ViewController.hpp"
#include "../SettingsMenu.hpp"

DECLARE_CLASS_CODEGEN(BSML, SettingsMenuListViewController, HMUI::ViewController) {
    DECLARE_INSTANCE_FIELD(CustomListTableData*, list);
    DECLARE_OVERRIDE_METHOD_MATCH(void, DidActivate, &HMUI::ViewController::DidActivate, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling);
    DECLARE_INSTANCE_METHOD(void, SettingsClick, HMUI::TableView* tableView, int idx);

    public:
        std::function<void(SettingsMenu*)> clickedMenu;
};