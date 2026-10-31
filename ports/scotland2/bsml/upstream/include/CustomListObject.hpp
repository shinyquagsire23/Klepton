#pragma once

#include "custom-types/shared/macros.hpp"
#include "BSML/Components/CustomCellTableCell.hpp"
#include "TMPro/TextMeshProUGUI.hpp"

DECLARE_CLASS_CODEGEN(BSML, CustomListObject, System::Object) {
    DECLARE_INSTANCE_FIELD(CustomCellTableCell*, cell);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, cellText);

    DECLARE_INSTANCE_METHOD(void, Select, CustomCellTableCell* cell);
    DECLARE_INSTANCE_METHOD(void, Setup, CustomCellTableCell* cell);
    DECLARE_INSTANCE_METHOD(void, Reused, CustomCellTableCell* cell);

    public:
        DECLARE_DEFAULT_CTOR();
};