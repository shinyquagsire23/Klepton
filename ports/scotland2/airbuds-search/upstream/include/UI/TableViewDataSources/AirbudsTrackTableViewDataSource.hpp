#pragma once

#include <bsml/shared/BSML/Components/CustomListTableData.hpp>
#include <custom-types/shared/macros.hpp>
#include <song-details/shared/SongDetails.hpp>

#include <string>

#include "Airbuds/Track.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(AirbudsSearch::UI, AirbudsTrackTableViewDataSource, UnityEngine::MonoBehaviour, HMUI::TableView::IDataSource*) {

    DECLARE_OVERRIDE_METHOD_MATCH(HMUI::TableCell*, CellForIdx, &HMUI::TableView::IDataSource::CellForIdx, HMUI::TableView * tableView, int idx);
    DECLARE_OVERRIDE_METHOD_MATCH(float, CellSize, &HMUI::TableView::IDataSource::CellSize);
    DECLARE_OVERRIDE_METHOD_MATCH(int, NumberOfCells, &HMUI::TableView::IDataSource::NumberOfCells);

    public:
    enum class RowType {
        Header,
        Track
    };

    enum class Grouping {
        None,
        ByDay,
        ByHour
    };

    struct Row {
        RowType type = RowType::Track;
        std::string title;
        airbuds::PlaylistTrack track;
    };

    void setTracks(std::vector<airbuds::PlaylistTrack> tracks, Grouping grouping);
    void clearTracks();
    const airbuds::PlaylistTrack* getTrackForRow(int idx) const;
    size_t trackCount() const;
    const airbuds::PlaylistTrack& getTrackAtIndex(size_t idx) const;
    int getRowIndexForTrackIndex(size_t trackIndex) const;
    int getRowIndexForTrack(const airbuds::PlaylistTrack& track) const;

    private:
    std::vector<airbuds::PlaylistTrack> tracks_;
    std::vector<Row> rows_;
};
