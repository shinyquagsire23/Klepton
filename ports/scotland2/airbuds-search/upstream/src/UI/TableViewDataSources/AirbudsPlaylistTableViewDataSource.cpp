#include "HMUI/Touchable.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"

#include "assets.hpp"
#include "Log.hpp"
#include "UI/TableViewCells/AirbudsPlaylistTableViewCell.hpp"
#include "UI/TableViewDataSources/AirbudsPlaylistTableViewDataSource.hpp"
#include "main.hpp"

DEFINE_TYPE(AirbudsSearch::UI, AirbudsPlaylistTableViewDataSource);

using namespace AirbudsSearch::UI;

HMUI::TableCell* AirbudsPlaylistTableViewDataSource::CellForIdx(HMUI::TableView* tableView, int idx) {
    auto tcd = tableView->DequeueReusableCellForIdentifier(AirbudsPlaylistTableViewCell::CELL_REUSE_ID);
    AirbudsPlaylistTableViewCell* playlistCell;
    if (!tcd) {
        auto tableCell = UnityEngine::GameObject::New_ctor("AirbudsPlaylistTableViewCell");
        playlistCell = tableCell->AddComponent<AirbudsPlaylistTableViewCell*>();
        playlistCell->set_interactable(true);

        playlistCell->set_reuseIdentifier(AirbudsPlaylistTableViewCell::CELL_REUSE_ID);
        BSML::parse_and_construct(IncludedAssets::AirbudsPlaylistTableViewCell_bsml, playlistCell->get_transform(), playlistCell);
        playlistCell->get_gameObject()->AddComponent<HMUI::Touchable*>();
    } else {
        playlistCell = tcd->GetComponent<AirbudsPlaylistTableViewCell*>();
    }

    const airbuds::Playlist& playlist = playlists_.at(idx);
    playlistCell->setPlaylist(playlist);

    return playlistCell;
}

int AirbudsPlaylistTableViewDataSource::NumberOfCells() {
    return playlists_.size();
}

float AirbudsPlaylistTableViewDataSource::CellSize() {
    return 8.0f;
}
