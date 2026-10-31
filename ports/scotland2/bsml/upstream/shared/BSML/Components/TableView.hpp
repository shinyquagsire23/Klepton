#pragma once

#include "custom-types/shared/macros.hpp"
#include "HMUI/TableView.hpp"

DECLARE_CLASS_CODEGEN(BSML, TableView, HMUI::TableView) {
    DECLARE_CTOR(ctor);
    DECLARE_OVERRIDE_METHOD_MATCH(void, ReloadData, &HMUI::TableView::ReloadData);
    DECLARE_OVERRIDE_METHOD_MATCH(void, DidSelectCellWithIdx, &HMUI::TableView::DidSelectCellWithIdx, int idx);
};