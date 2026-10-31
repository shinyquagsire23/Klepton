#pragma once

#include "bsml/shared/BSML/Components/CustomListTableData.hpp"
#include "custom-types/shared/macros.hpp"
#include "song-details/shared/SongDetails.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(AirbudsSearch::UI, CustomSongTableViewDataSource, UnityEngine::MonoBehaviour, HMUI::TableView::IDataSource*) {

    DECLARE_OVERRIDE_METHOD_MATCH(HMUI::TableCell*, CellForIdx, &HMUI::TableView::IDataSource::CellForIdx, HMUI::TableView * tableView, int idx);
    DECLARE_OVERRIDE_METHOD_MATCH(float, CellSize, &HMUI::TableView::IDataSource::CellSize);
    DECLARE_OVERRIDE_METHOD_MATCH(int, NumberOfCells, &HMUI::TableView::IDataSource::NumberOfCells);

    private:
    std::vector<const SongDetailsCache::Song*> customSongs_;

    public:
    void setSource(const std::vector<const SongDetailsCache::Song*>& source);
};
