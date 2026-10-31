#pragma once

#include "HMUI/ViewController.hpp"
#include "bsml/shared/macros.hpp"
#include "bsml/shared/BSML.hpp"
#include "HMUI/ModalView.hpp"
#include "HMUI/TableView.hpp"
#include "bsml/shared/BSML/Components/Settings/SliderSetting.hpp"
#include "bsml/shared/BSML/Components/ModalView.hpp"
#include "bsml/shared/BSML/ViewControllers/HotReloadViewController.hpp"
#include "custom-types/shared/coroutine.hpp"
#include "custom-types/shared/macros.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "bsml/shared/BSML/Components/CustomListTableData.hpp"


namespace BetterSongSearch::UI::Modals {
    enum class GenreCellStatus {
        None,
        Include,
        Exclude
    };

    struct GenreCellState {
        std::string tag;
        uint64_t mask;
        GenreCellStatus status;
        uint32_t songCount;
    };
}


DECLARE_CLASS_CODEGEN_INTERFACES(BetterSongSearch::UI::Modals, GenrePicker, UnityEngine::MonoBehaviour, HMUI::TableView::IDataSource*) {
    DECLARE_OVERRIDE_METHOD_MATCH(HMUI::TableCell*, CellForIdx, &HMUI::TableView::IDataSource::CellForIdx, HMUI::TableView* tableView, int idx);
    DECLARE_OVERRIDE_METHOD_MATCH(float, CellSize, &HMUI::TableView::IDataSource::CellSize);
    DECLARE_OVERRIDE_METHOD_MATCH(int, NumberOfCells, &HMUI::TableView::IDataSource::NumberOfCells);

    DECLARE_INSTANCE_METHOD(void, OnEnable);
    DECLARE_CTOR(ctor);
    DECLARE_INSTANCE_METHOD(void, OnDestroy);

    DECLARE_INSTANCE_METHOD(void, PostParse);

    DECLARE_INSTANCE_METHOD(void, OpenModal);
    DECLARE_INSTANCE_METHOD(void, CloseModal);

    DECLARE_INSTANCE_FIELD(UnityW<BSML::CustomListTableData>, genresTableData);

    DECLARE_INSTANCE_METHOD(void, ClearGenre);
    DECLARE_INSTANCE_METHOD(void, SelectGenre);
    DECLARE_INSTANCE_METHOD(void, RefreshGenreList);

    DECLARE_INSTANCE_FIELD(bool, initialized);

    // Modals
    DECLARE_INSTANCE_FIELD(UnityW<BSML::ModalView>, genrePickerModal);

    private:
        std::vector<BetterSongSearch::UI::Modals::GenreCellState> genres;
};
