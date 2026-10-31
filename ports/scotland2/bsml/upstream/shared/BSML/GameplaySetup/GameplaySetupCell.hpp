#pragma once

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/GameObject.hpp"
#include "HMUI/TableCell.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "GameplaySetupMenu.hpp"
#include "../Components/Settings/ToggleSetting.hpp"

DECLARE_CLASS_CODEGEN(BSML, GameplaySetupCell, HMUI::TableCell) {
    DECLARE_INSTANCE_FIELD(GameplaySetupMenu*, tab);
    DECLARE_INSTANCE_FIELD(ToggleSetting*, toggle);

    DECLARE_INSTANCE_METHOD(GameplaySetupCell*, PopulateCell, GameplaySetupMenu* menu);
    DECLARE_INSTANCE_METHOD(StringW, get_name);
    DECLARE_INSTANCE_METHOD(bool, get_visible);
    DECLARE_INSTANCE_METHOD(void, set_visible, bool value);

    DECLARE_DEFAULT_CTOR();
};