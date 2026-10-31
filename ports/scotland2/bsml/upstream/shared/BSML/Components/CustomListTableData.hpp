#pragma once

#include "custom-types/shared/macros.hpp"
#include "HMUI/TableView.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/Sprite.hpp"

#include "BoxTableCell.hpp"
#include "GlobalNamespace/LevelListTableCell.hpp"
#include "GlobalNamespace/AnnotatedBeatmapLevelCollectionCell.hpp"
#include "GlobalNamespace/SimpleTextTableCell.hpp"

#include <string>

namespace GlobalNamespace {
    using LevelPackCell = AnnotatedBeatmapLevelCollectionCell;
}

DECLARE_CLASS_CODEGEN(BSML, CustomCellInfo, System::Object) {
        DECLARE_INSTANCE_FIELD(StringW, text);
        DECLARE_INSTANCE_FIELD(StringW, subText);
        DECLARE_INSTANCE_FIELD(UnityEngine::Sprite*, icon);

        DECLARE_DEFAULT_CTOR();

    public:
        static CustomCellInfo* construct(StringW text, StringW subText = nullptr, UnityEngine::Sprite* icon = nullptr);
};

DECLARE_CLASS_CODEGEN_INTERFACES(BSML, CustomListTableData, UnityEngine::MonoBehaviour, HMUI::TableView::IDataSource*) {

    /* pls no touchy raw listStyle if you value your sanity */
    DECLARE_INSTANCE_FIELD(int, _listStyle);
    DECLARE_INSTANCE_FIELD(ListW<CustomCellInfo*>, data);
    DECLARE_INSTANCE_FIELD(float, cellSize);
    DECLARE_INSTANCE_FIELD(StringW, reuseIdentifier);
    DECLARE_INSTANCE_FIELD(HMUI::TableView*, tableView);
    DECLARE_INSTANCE_FIELD(bool, expandCell);

    DECLARE_INSTANCE_FIELD(UnityEngine::Sprite*, blackSprite);
    DECLARE_INSTANCE_FIELD(GlobalNamespace::LevelListTableCell*, levelListTableCell);
    DECLARE_INSTANCE_FIELD(GlobalNamespace::LevelPackCell*, levelPackCell);
    DECLARE_INSTANCE_FIELD(GlobalNamespace::SimpleTextTableCell*, simpleTextTableCell);

    DECLARE_INSTANCE_METHOD(GlobalNamespace::LevelListTableCell*, GetTableCell);
    DECLARE_INSTANCE_METHOD(GlobalNamespace::SimpleTextTableCell*, GetSimpleTextTableCell);
    DECLARE_INSTANCE_METHOD(BSML::BoxTableCell*, GetBoxTableCell);
    DECLARE_INSTANCE_METHOD(void, Awake);

    DECLARE_OVERRIDE_METHOD_MATCH(float, CellSize, &HMUI::TableView::IDataSource::CellSize);
    DECLARE_OVERRIDE_METHOD_MATCH(int, NumberOfCells, &HMUI::TableView::IDataSource::NumberOfCells);
    DECLARE_OVERRIDE_METHOD_MATCH(HMUI::TableCell*, CellForIdx, &HMUI::TableView::IDataSource::CellForIdx, HMUI::TableView* tableView, int idx);

    DECLARE_CTOR(ctor);

    public:
        enum class ListStyle {
            List, Box, Simple
        };

        static ListStyle stringToListStyle(const std::string& str);
        ListStyle get_listStyle();
        void set_listStyle(ListStyle value);
        __declspec(property(get=get_listStyle, put=set_listStyle)) ListStyle listStyle;
        HMUI::TableView::IDataSource* i_IDataSource();
};