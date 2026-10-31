#pragma once

#include "custom-types/shared/macros.hpp"
#include "../Components/HotReloadFileWatcher.hpp"
#include "HMUI/ViewController.hpp"

DECLARE_CLASS_CODEGEN(BSML, HotReloadViewController, HMUI::ViewController) {
    DECLARE_INSTANCE_FIELD(HotReloadFileWatcher*, fileWatcher);
    DECLARE_INSTANCE_METHOD(void, Awake);
};