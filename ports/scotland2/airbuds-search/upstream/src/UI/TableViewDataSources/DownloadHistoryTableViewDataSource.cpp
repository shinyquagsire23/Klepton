#include "UI/TableViewDataSources/DownloadHistoryTableViewDataSource.hpp"

#include <HMUI/Touchable.hpp>

#include "assets.hpp"
#include "Log.hpp"
#include "UI/TableViewCells/DownloadHistoryTableViewCell.hpp"

DEFINE_TYPE(AirbudsSearch::UI, DownloadHistoryTableViewDataSource);

using namespace AirbudsSearch::UI;

HMUI::TableCell* DownloadHistoryTableViewDataSource::CellForIdx(HMUI::TableView* tableView, int idx) {
    auto tcd = tableView->DequeueReusableCellForIdentifier("DownloadHistoryTableViewDataSource");
    DownloadHistoryTableViewCell* cell;
    if (!tcd) {
        auto tableCell = UnityEngine::GameObject::New_ctor();
        cell = tableCell->AddComponent<DownloadHistoryTableViewCell*>();
        cell->set_interactable(true);

        cell->set_reuseIdentifier("DownloadHistoryTableViewDataSource");
        BSML::parse_and_construct(IncludedAssets::DownloadHistoryTableViewCell_bsml, cell->get_transform(), cell);
    } else {
        cell = tcd->GetComponent<DownloadHistoryTableViewCell*>();
    }

    const std::shared_ptr<DownloadHistoryItem> downloadHistoryItem = downloadHistoryItems_.at(idx);

    cell->setDownloadHistoryItem(downloadHistoryItem);

    return cell;
}

int DownloadHistoryTableViewDataSource::NumberOfCells() {
    return downloadHistoryItems_.size();
}

float DownloadHistoryTableViewDataSource::CellSize() {
    return 8.0f;
}
