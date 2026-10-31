#include "UI/TableViewDataSources/CustomSongTableViewDataSource.hpp"
#include "assets.hpp"
#include "Log.hpp"
#include "UI/TableViewCells/CustomSongTableViewCell.hpp"

#include "HMUI/Touchable.hpp"

DEFINE_TYPE(AirbudsSearch::UI, CustomSongTableViewDataSource);

using namespace AirbudsSearch::UI;

HMUI::TableCell* CustomSongTableViewDataSource::CellForIdx(HMUI::TableView* tableView, int idx) {
    auto tcd = tableView->DequeueReusableCellForIdentifier("CustomSongListTableCellReuseIdentifier");
    CustomSongTableViewCell* cell;
    if (!tcd) {
        auto tableCell = UnityEngine::GameObject::New_ctor("CustomSongListTableCell");
        cell = tableCell->AddComponent<CustomSongTableViewCell*>();
        cell->get_gameObject()->AddComponent<HMUI::Touchable*>();
        cell->set_interactable(true);

        cell->set_reuseIdentifier("CustomSongListTableCellReuseIdentifier");
        BSML::parse_and_construct(IncludedAssets::CustomSongTableViewCell_bsml, cell->get_transform(), cell);
    } else {
        cell = tcd->GetComponent<CustomSongTableViewCell*>();
    }

    const SongDetailsCache::Song* const song = customSongs_.at(idx);
    cell->setSong(song);

    return cell;
}

int CustomSongTableViewDataSource::NumberOfCells() {
    return customSongs_.size();
}

float CustomSongTableViewDataSource::CellSize() {
    return 12.0f;
}

void CustomSongTableViewDataSource::setSource(const std::vector<const SongDetailsCache::Song*>& source) {
    customSongs_ = source;
}
