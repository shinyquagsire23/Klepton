#include "UI/TableViewDataSources/FriendListTableViewDataSource.hpp"

#include "HMUI/Touchable.hpp"
#include "bsml/shared/BSML.hpp"

#include "assets.hpp"
#include "UI/TableViewCells/FriendListTableViewCell.hpp"

DEFINE_TYPE(AirbudsSearch::UI, FriendListTableViewDataSource);

using namespace AirbudsSearch::UI;

HMUI::TableCell* FriendListTableViewDataSource::CellForIdx(HMUI::TableView* tableView, int idx) {
    auto tcd = tableView->DequeueReusableCellForIdentifier(FriendListTableViewCell::CELL_REUSE_ID);
    FriendListTableViewCell* friendCell = nullptr;
    if (!tcd) {
        auto tableCell = UnityEngine::GameObject::New_ctor("FriendListTableViewCell");
        friendCell = tableCell->AddComponent<FriendListTableViewCell*>();
        friendCell->set_interactable(true);

        friendCell->set_reuseIdentifier(FriendListTableViewCell::CELL_REUSE_ID);
        BSML::parse_and_construct(IncludedAssets::FriendListTableViewCell_bsml, friendCell->get_transform(), friendCell);
        friendCell->get_gameObject()->AddComponent<HMUI::Touchable*>();
    } else {
        friendCell = tcd->GetComponent<FriendListTableViewCell*>();
    }

    if (idx >= 0 && static_cast<size_t>(idx) < friends_.size()) {
        friendCell->setFriend(friends_.at(static_cast<size_t>(idx)));
    }

    return friendCell;
}

int FriendListTableViewDataSource::NumberOfCells() {
    return friends_.size();
}

float FriendListTableViewDataSource::CellSize() {
    return 7.0f;
}

int FriendListTableViewDataSource::getIndexForFriendId(std::string_view friendId) const {
    if (friendId.empty()) {
        return -1;
    }
    for (size_t i = 0; i < friends_.size(); ++i) {
        if (friends_[i].id == friendId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
