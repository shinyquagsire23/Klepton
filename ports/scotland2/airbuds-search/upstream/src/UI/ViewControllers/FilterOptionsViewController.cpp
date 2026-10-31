#include <string>
#include <thread>
#include <vector>

#include "bsml/shared/BSML.hpp"
#include "UnityEngine/UI/ContentSizeFitter.hpp"
#include "HMUI/CurvedCanvasSettings.hpp"
#include "bsml/shared/BSML/Components/Backgroundable.hpp"
#include "UnityEngine/Resources.hpp"
#include "bsml/shared/Helpers/getters.hpp"

#include "assets.hpp"
#include "UI/TableViewDataSources/FriendListTableViewDataSource.hpp"
#include "UI/ViewControllers/FilterOptionsViewController.hpp"
#include "Log.hpp"
#include "Utils.hpp"
#include "UI/FlowCoordinators/AirbudsSearchFlowCoordinator.hpp"
#include "UI/ViewControllers/MainViewController.hpp"
#include "main.hpp"

DEFINE_TYPE(AirbudsSearch::UI::ViewControllers, FilterOptionsViewController);

using namespace AirbudsSearch::UI::ViewControllers;

void FilterOptionsViewController::DidActivate(bool firstActivation, bool addedToHierarchy, bool screenSystemDisabling) {

    if (firstActivation) {
        BSML::parse_and_construct(IncludedAssets::FilterOptionsViewController_bsml, this->get_transform(), this);

#if HOT_RELOAD
        fileWatcher->filePath = "/sdcard/FilterOptionsViewController.bsml";
        fileWatcher->checkInterval = 1.0f;
#endif
    }

    if (addedToHierarchy && friendsListView_) {
        reloadFriendList();
    }
}

void FilterOptionsViewController::PostParse() {
    // Allow the dropdown to show all the difficulties at once without needing to scroll
    difficultyDropdown_->dropdown->_numberOfVisibleCells = get_dropdownOptionsDifficulties_().size();
    difficultyDropdown_->dropdown->ReloadData();

    // Check the saved filter
    selectedDifficultyString_ = getConfig().config["filter"]["difficulty"].GetString();

    auto* friendsDataSource = gameObject->GetComponent<FriendListTableViewDataSource*>();
    if (!friendsDataSource) {
        friendsDataSource = gameObject->AddComponent<FriendListTableViewDataSource*>();
    }
    friendsListView_->tableView->SetDataSource(reinterpret_cast<HMUI::TableView::IDataSource*>(friendsDataSource), true);

    if (friendsStatusTextView_) {
        friendsStatusTextView_->set_text("Loading...");
        friendsStatusTextView_->get_gameObject()->set_active(false);
    }
    if (backToMyHistoryButton_) {
        backToMyHistoryButton_->get_gameObject()->set_active(selectedFriend_.has_value());
    }
}

void FilterOptionsViewController::onFilterChanged() {
    // There seems to be a bug that causes this method to get called before the member variables are actually updated
    // with the new values. They seem to get set on the next frame, so this is an attempt to delay reading them until
    // then.
    std::thread([this](){
        BSML::MainThreadScheduler::Schedule([this](){
            // Save to config
            getConfig().config["filter"]["difficulty"].SetString(selectedDifficultyString_, getConfig().config.GetAllocator());
            getConfig().Write();

            // Difficulty
            customSongFilter_.difficulties_.clear();
            if (selectedDifficultyString_ != "Any") {
                customSongFilter_.difficulties_.push_back(Utils::getMapDifficultyFromString(selectedDifficultyString_));
            }

            // Notify the main view controller
            UnityW<HMUI::FlowCoordinator> parentFlow = BSML::Helpers::GetMainFlowCoordinator()->YoungestChildFlowCoordinatorOrSelf();
            auto flow = parentFlow.cast<AirbudsSearch::UI::FlowCoordinators::AirbudsSearchFlowCoordinator>();
            auto mainViewController = flow->mainViewController_;
            mainViewController->setFilter(customSongFilter_);
        });
    }).detach();
}

void FilterOptionsViewController::reloadFriendList() {
    if (isLoadingFriends_) {
        return;
    }
    isLoadingFriends_ = true;

    if (friendsStatusTextView_) {
        friendsStatusTextView_->set_text("Loading...");
        friendsStatusTextView_->get_gameObject()->set_active(true);
    }

    auto* friendsDataSource = gameObject->GetComponent<FriendListTableViewDataSource*>();
    std::thread([this, friendsDataSource]() {
        std::vector<airbuds::Friend> friends;
        std::string status;

        try {
            if (!AirbudsSearch::airbudsClient) {
                status = "Airbuds client missing.";
            } else {
                friends = AirbudsSearch::airbudsClient->getFriends();
            }
        } catch (const std::exception& exception) {
            AirbudsSearch::Log.warn("Failed loading friends: {}", exception.what());
            status = "Failed to load friends.";
        }

        BSML::MainThreadScheduler::Schedule([this, friendsDataSource, friends, status]() {
            if (friendsDataSource) {
                friendsDataSource->friends_ = friends;
            }
            if (friendsListView_) {
                Utils::reloadDataKeepingPosition(friendsListView_->tableView);
            }

            if (friendsStatusTextView_) {
                if (!status.empty()) {
                    friendsStatusTextView_->set_text(status);
                    friendsStatusTextView_->get_gameObject()->set_active(true);
                } else if (friends.empty()) {
                    friendsStatusTextView_->set_text("No friends");
                    friendsStatusTextView_->get_gameObject()->set_active(true);
                } else {
                    friendsStatusTextView_->get_gameObject()->set_active(false);
                }
            }

            if (friendsDataSource && friendsListView_) {
                if (selectedFriend_ && !selectedFriend_->id.empty()) {
                    const int idx = friendsDataSource->getIndexForFriendId(selectedFriend_->id);
                    if (idx >= 0) {
                        friendsListView_->tableView->SelectCellWithIdx(idx, true);
                    } else {
                        selectedFriend_.reset();
                        if (backToMyHistoryButton_) {
                            backToMyHistoryButton_->get_gameObject()->set_active(false);
                        }
                        friendsListView_->tableView->ClearSelection();
                    }
                }
            }

            isLoadingFriends_ = false;
        });
    }).detach();
}

void FilterOptionsViewController::onFriendSelected(UnityW<HMUI::TableView> table, int id) {
    auto* friendsDataSource = gameObject->GetComponent<FriendListTableViewDataSource*>();
    if (!friendsDataSource) {
        return;
    }
    if (id < 0 || static_cast<size_t>(id) >= friendsDataSource->friends_.size()) {
        return;
    }

    const airbuds::Friend& friendUser = friendsDataSource->friends_.at(static_cast<size_t>(id));
    if (friendUser.id.empty()) {
        return;
    }

    selectedFriend_ = friendUser;
    if (backToMyHistoryButton_) {
        backToMyHistoryButton_->get_gameObject()->set_active(true);
    }

    UnityW<HMUI::FlowCoordinator> parentFlow = BSML::Helpers::GetMainFlowCoordinator()->YoungestChildFlowCoordinatorOrSelf();
    auto flow = parentFlow.cast<AirbudsSearch::UI::FlowCoordinators::AirbudsSearchFlowCoordinator>();
    if (flow && flow->mainViewController_) {
        flow->mainViewController_->setHistoryFriend(selectedFriend_);
    }
}

void FilterOptionsViewController::onBackToMyHistoryButtonClicked() {
    selectedFriend_.reset();
    if (backToMyHistoryButton_) {
        backToMyHistoryButton_->get_gameObject()->set_active(false);
    }
    if (friendsListView_) {
        friendsListView_->tableView->ClearSelection();
    }

    UnityW<HMUI::FlowCoordinator> parentFlow = BSML::Helpers::GetMainFlowCoordinator()->YoungestChildFlowCoordinatorOrSelf();
    auto flow = parentFlow.cast<AirbudsSearch::UI::FlowCoordinators::AirbudsSearchFlowCoordinator>();
    if (flow && flow->mainViewController_) {
        flow->mainViewController_->setHistoryFriend(std::nullopt);
    }
}
