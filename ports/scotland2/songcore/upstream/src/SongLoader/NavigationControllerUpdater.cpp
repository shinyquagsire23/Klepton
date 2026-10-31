#include "SongLoader/NavigationControllerUpdater.hpp"

#include "GlobalNamespace/LevelCollectionTableView.hpp"
#include "GlobalNamespace/AnnotatedBeatmapLevelCollectionsViewController.hpp"
#include "SongLoader/CustomBeatmapLevelsRepository.hpp"
#include "System/Collections/Generic/IReadOnlyList_1.hpp"
#include "System/Action_1.hpp"
#include "UnityEngine/GameObject.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/ScrollView.hpp"
#include "logging.hpp"

#include "bsml/shared/BSML/MainThreadScheduler.hpp"

DEFINE_TYPE(SongCore::SongLoader, NavigationControllerUpdater);

namespace SongCore::SongLoader {
    void NavigationControllerUpdater::ctor(GlobalNamespace::BeatmapLevelsModel* beatmapLevelsModel, RuntimeSongLoader* runtimeSongLoader, GlobalNamespace::LevelFilteringNavigationController* levelFilteringNavigationController, GlobalNamespace::LevelCollectionNavigationController* levelCollectionNavigationController, GlobalNamespace::LevelCollectionViewController* levelCollectionViewController) {
        _runtimeSongLoader = runtimeSongLoader;
        _levelFilteringNavigationController = levelFilteringNavigationController;
        _levelCollectionNavigationController = levelCollectionNavigationController;
        _levelCollectionViewController = levelCollectionViewController;
        _beatmapLevelsModel = beatmapLevelsModel;
    }

    void NavigationControllerUpdater::Initialize() {
        _runtimeSongLoader->SongsWillRefresh += {&NavigationControllerUpdater::SongsWillRefresh, this};
        _runtimeSongLoader->SongsLoaded += {&NavigationControllerUpdater::SongsLoaded, this};
        if (_runtimeSongLoader->AreSongsLoaded) {
            SongsLoaded(_runtimeSongLoader->AllLevels);
        }
    }

    void NavigationControllerUpdater::Dispose() {
        _runtimeSongLoader->SongsWillRefresh -= {&NavigationControllerUpdater::SongsWillRefresh, this};
        _runtimeSongLoader->SongsLoaded -= {&NavigationControllerUpdater::SongsLoaded, this};
    }

    void NavigationControllerUpdater::SongsWillRefresh() {
        _levelFilteringNavigationController->_customLevelPacks = nullptr;
        _levelFilteringNavigationController->_annotatedBeatmapLevelCollectionsViewController->ShowLoading();
        _levelCollectionNavigationController->ShowLoading();

        _levelFilteringNavigationController->UpdateCustomSongs();
    }

    void NavigationControllerUpdater::SongsLoaded(std::span<SongLoader::CustomBeatmapLevel* const> levels) {
        auto levelCollectionTableView = _levelCollectionViewController->_levelCollectionTableView;
        auto level = levelCollectionTableView ? levelCollectionTableView->_selectedBeatmapLevel : nullptr;
        auto levelId = level ? level->levelID : "";

        auto tableView = levelCollectionTableView ? levelCollectionTableView->_tableView : nullptr;
        auto scrollView = tableView ? tableView->_scrollView : nullptr;
        float scrollPosition = scrollView ? scrollView->position : 0;

        auto selectedCategory = _levelFilteringNavigationController->selectedLevelCategory;
        if (selectedCategory == GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::MusicPacks
            || selectedCategory == GlobalNamespace::SelectLevelCategoryViewController::LevelCategory::CustomSongs) {
            auto pack = _levelFilteringNavigationController->selectedBeatmapLevelPack;
            if (pack) {
                // check whether the pack was still in the beatmap levels model
                if (_beatmapLevelsModel->GetLevelPack(pack->packID)) {
                    _levelFilteringNavigationController->_levelPackIdToBeSelectedAfterPresent = pack->packID;
                }
                // don't scroll back to the last position if the same pack won't be reselected
                else scrollPosition = 0;
            }
        }

        // thanks metalit for pointing out updatecustomsongs still starts an async thing, doing things this way lets us await the reload to be complete
        BSML::MainThreadScheduler::ScheduleUntil(
            [nav = this->_levelFilteringNavigationController](){
                return nav->_cancellationTokenSource == nullptr;
            },
            [this, levelId, scrollPosition](){
                INFO("Selecting level '{}'", levelId);
                // No saved selection is normal for the first empty custom-song scan.
                // Preserve scroll restoration while avoiding an empty-ID lookup.
                auto level = levelId && !static_cast<std::u16string_view>(levelId).empty()
                    ? _beatmapLevelsModel->GetBeatmapLevel(levelId) : nullptr;

                auto levelCollectionTableView = _levelCollectionViewController->_levelCollectionTableView;
                if (level && levelCollectionTableView) levelCollectionTableView->SelectLevel(level);

                auto tableView = levelCollectionTableView ? levelCollectionTableView->_tableView : nullptr;
                auto scrollView = tableView ? tableView->_scrollView : nullptr;

                if (scrollView) scrollView->ScrollTo(scrollPosition, false);
            }
        );
    }
}
