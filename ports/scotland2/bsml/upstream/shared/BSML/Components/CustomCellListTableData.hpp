#pragma once

#include "custom-types/shared/macros.hpp"
#include "CustomCellTableCell.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/TableCell.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/Sprite.hpp"

#include <string>

DECLARE_CLASS_CODEGEN_INTERFACES(BSML, CustomCellListTableData, UnityEngine::MonoBehaviour, HMUI::TableView::IDataSource*) {
    DECLARE_INSTANCE_FIELD(ListW<System::Object*>, data);
    DECLARE_INSTANCE_FIELD(float, cellSize);
    DECLARE_INSTANCE_FIELD(bool, clickableCells);
    DECLARE_INSTANCE_FIELD(StringW, reuseIdentifier);
    DECLARE_INSTANCE_FIELD(HMUI::TableView*, tableView);

    DECLARE_OVERRIDE_METHOD_MATCH(float, CellSize, &HMUI::TableView::IDataSource::CellSize);
    DECLARE_OVERRIDE_METHOD_MATCH(int, NumberOfCells, &HMUI::TableView::IDataSource::NumberOfCells);
    DECLARE_OVERRIDE_METHOD_MATCH(HMUI::TableCell*, CellForIdx, &HMUI::TableView::IDataSource::CellForIdx, HMUI::TableView* tableView, int idx);
    DECLARE_INSTANCE_METHOD(CustomCellTableCell*, NewCellForData, System::Object* data);

    DECLARE_CTOR(ctor);

    public:
        std::string bsmlString;
        HMUI::TableView::IDataSource* i_IDataSource();
};