#pragma once

#include "custom-types/shared/macros.hpp"
#include "HMUI/ViewController.hpp"
#include "UnityEngine/Transform.hpp"

DECLARE_CLASS_CODEGEN(BSML, MenuButtonsViewController, HMUI::ViewController) {
    DECLARE_INSTANCE_FIELD(ListW<System::Object*>, buttons);
    DECLARE_INSTANCE_FIELD(UnityEngine::Transform*, root);
    DECLARE_INSTANCE_METHOD(bool, get_anyButtons);
    DECLARE_INSTANCE_METHOD(void, RefreshView);

    DECLARE_OVERRIDE_METHOD_MATCH(void, DidActivate, &HMUI::ViewController::DidActivate, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling);
};