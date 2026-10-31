#pragma once

#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/Components/CustomListTableData.hpp"
#include "bsml/shared/BSML/Components/ModalView.hpp"
#include "bsml/shared/BSML/Components/Settings/SliderSetting.hpp"
#include "bsml/shared/BSML/Components/Settings/StringSetting.hpp"
#include "bsml/shared/BSML/ViewControllers/HotReloadViewController.hpp"
#include "bsml/shared/macros.hpp"
#include "custom-types/shared/coroutine.hpp"
#include "custom-types/shared/macros.hpp"
#include "HMUI/ModalView.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/ViewController.hpp"
#include "UnityEngine/MonoBehaviour.hpp"


DECLARE_CLASS_CODEGEN_INTERFACES(BetterSongSearch::UI::Modals, Presets, UnityEngine::
MonoBehaviour, HMUI::TableView::IDataSource*) {
    DECLARE_INSTANCE_METHOD(void, OnEnable);
    DECLARE_CTOR(ctor);

    DECLARE_OVERRIDE_METHOD_MATCH(HMUI::TableCell*, CellForIdx, &HMUI::TableView::IDataSource::CellForIdx, HMUI::TableView* tableView, int idx);
    DECLARE_OVERRIDE_METHOD_MATCH(float, CellSize
, &HMUI::TableView::IDataSource::CellSize);
    DECLARE_OVERRIDE_METHOD_MATCH(int, NumberOfCells, &HMUI::TableView::IDataSource::NumberOfCells);

    DECLARE_INSTANCE_FIELD(UnityW<BSML::CustomListTableData>, presetListTableData);

    DECLARE_INSTANCE_METHOD(void, PostParse);

    DECLARE_INSTANCE_METHOD(void, OpenModal);
    DECLARE_INSTANCE_METHOD(void, CloseModal);

    DECLARE_INSTANCE_METHOD(void, OpenSavePresetModal);
    DECLARE_INSTANCE_METHOD(void, CloseSavePresetModal);

    DECLARE_INSTANCE_METHOD(void, AddPreset);
    DECLARE_INSTANCE_METHOD(void, LoadPreset);
    DECLARE_INSTANCE_METHOD(void, DeletePreset);
    DECLARE_INSTANCE_METHOD(void, PresetSelected, UnityW<HMUI::TableView> table, int id);
    DECLARE_INSTANCE_METHOD(void, RefreshPresetsList);

    DECLARE_INSTANCE_FIELD(bool, initialized);

    DECLARE_INSTANCE_FIELD(UnityW<BSML::StringSetting>, newPresetNameSetting);
    // Modals
    DECLARE_INSTANCE_FIELD(UnityW<BSML::ModalView>, presetsModal);
    DECLARE_INSTANCE_FIELD(UnityW<BSML::ModalView>, savePresetModal);

    // Buttons
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, loadButton);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, deleteButton);

    public:
        UnityW<HMUI::TableView> presetsTable() {if(presetListTableData) {
            return presetListTableData->tableView;} else return nullptr;}

        std::vector<std::string> presets;
        std::string selectedPreset;
};
