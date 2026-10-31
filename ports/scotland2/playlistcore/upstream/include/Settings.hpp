#pragma once

#include "HMUI/ViewController.hpp"
#include "custom-types/shared/macros.hpp"

DECLARE_CLASS_CODEGEN(PlaylistCore, SettingsViewController, HMUI::ViewController) {
    DECLARE_OVERRIDE_METHOD_MATCH(
        void, DidActivate, &HMUI::ViewController::DidActivate, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling
    );
};
