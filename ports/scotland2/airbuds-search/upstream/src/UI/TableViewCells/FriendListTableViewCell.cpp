#include "UI/TableViewCells/FriendListTableViewCell.hpp"

#include <format>

#include "UnityEngine/GameObject.hpp"

DEFINE_TYPE(AirbudsSearch::UI, FriendListTableViewCell);

using namespace AirbudsSearch::UI;

void FriendListTableViewCell::ctor() {
    INVOKE_BASE_CTOR(classof(HMUI::TableCell*));
}

void FriendListTableViewCell::OnDestroy() {
}

void FriendListTableViewCell::SelectionDidChange(HMUI::SelectableCell::TransitionType transitionType) {
    updateBackground();
}

void FriendListTableViewCell::HighlightDidChange(HMUI::SelectableCell::TransitionType transitionType) {
    updateBackground();
}

void FriendListTableViewCell::WasPreparedForReuse() {
}

void FriendListTableViewCell::updateBackground() {
    if (!root_) {
        return;
    }
    root_->set_color(UnityEngine::Color(0, 0, 0, selected || highlighted ? 0.8f : 0.45f));
}

void FriendListTableViewCell::setFriend(const airbuds::Friend& friendUser) {
    friend_ = friendUser;

    std::string displayName = friendUser.displayName;
    if (displayName.empty()) {
        displayName = friendUser.identifier;
    }
    if (displayName.empty()) {
        displayName = friendUser.id;
    }
    friendNameTextView_->set_text(displayName);

    if (!friendUser.identifier.empty()) {
        friendHandleTextView_->set_text(std::format("@{}", friendUser.identifier));
    } else {
        friendHandleTextView_->set_text("");
    }

    updateBackground();
}
