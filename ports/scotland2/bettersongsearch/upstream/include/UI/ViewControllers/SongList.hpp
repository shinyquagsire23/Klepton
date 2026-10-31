#pragma once
#include <fmt/chrono.h>

#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/Components/CustomListTableData.hpp"
#include "bsml/shared/BSML/Components/ModalView.hpp"
#include "bsml/shared/BSML/Components/Settings/DropdownListSetting.hpp"
#include "bsml/shared/BSML/ViewControllers/HotReloadViewController.hpp"
#include "bsml/shared/macros.hpp"
#include "custom-types/shared/coroutine.hpp"
#include "custom-types/shared/macros.hpp"
#include "FilterOptions.hpp"
#include "GlobalNamespace/LevelSelectionFlowCoordinator.hpp"
#include "GlobalNamespace/MultiplayerLevelSelectionFlowCoordinator.hpp"
#include "GlobalNamespace/SoloFreePlayFlowCoordinator.hpp"
#include "HMUI/CurvedTextMeshPro.hpp"
#include "HMUI/ImageView.hpp"
#include "HMUI/TableCell.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/TextPageScrollView.hpp"
#include "HMUI/ViewController.hpp"
#include "song-details/shared/Data/MapCharacteristic.hpp"
#include "song-details/shared/Data/Song.hpp"
#include "song-details/shared/Data/SongDifficulty.hpp"
#include "song-details/shared/SongDetails.hpp"
#include "songcore/shared/SongCore.hpp"
#include "songcore/shared/SongLoader/CustomBeatmapLevel.hpp"
#include "System/Object.hpp"
#include "System/Threading/CancellationToken.hpp"
#include "System/Threading/CancellationTokenSource.hpp"
#include "TMPro/TextAlignmentOptions.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "UI/Modals/MultiDL.hpp"
#include "UI/Modals/Settings.hpp"
#include "UI/Modals/UploadDetails.hpp"
#include "UI/ViewControllers/SongListCellTableData.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/UI/HorizontalOrVerticalLayoutGroup.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"
#include "Util/RatelimitCoroutine.hpp"

#ifndef DECLARE_OVERRIDE_METHOD_MATCH
#define DECLARE_OVERRIDE_METHOD_MATCH(retval, method, mptr, ...) \
    DECLARE_OVERRIDE_METHOD(retval, method, il2cpp_utils::il2cpp_type_check::MetadataGetter<mptr>::get(), __VA_ARGS__)
#endif

namespace BetterSongSearch::UI {
    inline bool fromBSS = false;
    inline bool openToCustom = false;
}  // namespace BetterSongSearch::UI

#ifdef HotReload
DECLARE_CLASS_CUSTOM_INTERFACES(
    BetterSongSearch::UI::ViewControllers,
    SongListController,
    BSML::HotReloadViewController,
    std::vector<Il2CppClass*>({classof(HMUI::TableView::IDataSource*)})
) {
#else
DECLARE_CLASS_CODEGEN_INTERFACES(BetterSongSearch::UI::ViewControllers, SongListController, HMUI::ViewController, HMUI::TableView::IDataSource*)
{
#endif

    DECLARE_CTOR(ctor);
    DECLARE_INSTANCE_METHOD(void, OnDestroy);
    DECLARE_OVERRIDE_METHOD_MATCH(
        void, DidActivate, &HMUI::ViewController::DidActivate, bool firstActivation, bool addedToHeirarchy, bool screenSystemDisabling
    );
    DECLARE_INSTANCE_FIELD(UnityW<BSML::CustomListTableData>, songList);

    DECLARE_INSTANCE_METHOD(void, SelectSong, UnityW<HMUI::TableView> table, int id);
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::VerticalLayoutGroup*, scrollBarContainer);
    DECLARE_INSTANCE_FIELD(float, cellSize);

    DECLARE_OVERRIDE_METHOD_MATCH(HMUI::TableCell*, CellForIdx, &HMUI::TableView::IDataSource::CellForIdx, HMUI::TableView * tableView, int idx);
    DECLARE_OVERRIDE_METHOD_MATCH(float, CellSize, &HMUI::TableView::IDataSource::CellSize);
    DECLARE_OVERRIDE_METHOD_MATCH(int, NumberOfCells, &HMUI::TableView::IDataSource::NumberOfCells);

    DECLARE_INSTANCE_METHOD(void, SelectRandom);
    DECLARE_INSTANCE_METHOD(void, ShowMoreModal);
    DECLARE_INSTANCE_METHOD(void, HideMoreModal);
    DECLARE_INSTANCE_METHOD(void, UpdateDataAndFilters);

    DECLARE_INSTANCE_METHOD(void, ShowBatchDownload);
    DECLARE_INSTANCE_METHOD(void, ShowPlaylistCreation);
    DECLARE_INSTANCE_METHOD(void, ShowSettings);
    DECLARE_INSTANCE_METHOD(void, ShowCloseConfirmation);
    DECLARE_INSTANCE_METHOD(void, ForcedUIClose);
    DECLARE_INSTANCE_METHOD(void, ForcedUICloseCancel);

    // Play/DL
    DECLARE_INSTANCE_METHOD(void, Download);
    DECLARE_INSTANCE_METHOD(void, PostParse);
    DECLARE_INSTANCE_METHOD(void, Play);

    DECLARE_INSTANCE_METHOD(void, ShowSongDetails);
    DECLARE_INSTANCE_METHOD(void, FilterByUploader);
    DECLARE_INSTANCE_METHOD(void, UpdateSearchedSongsList);
    DECLARE_INSTANCE_METHOD(void, _UpdateSearchedSongsList);

    DECLARE_INSTANCE_FIELD(bool, IsSearching);

    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::HorizontalOrVerticalLayoutGroup>, searchBoxContainer);
    DECLARE_INSTANCE_FIELD(UnityW<HMUI::CurvedTextMeshPro>, songSearchPlaceholder);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::HorizontalOrVerticalLayoutGroup>, detailActions);
    DECLARE_INSTANCE_FIELD(UnityW<HMUI::ModalView>, rootModal);
    DECLARE_INSTANCE_FIELD(UnityW<HMUI::ModalView>, moreModal);
    DECLARE_INSTANCE_FIELD(UnityW<BSML::ModalView>, downloadCancelConfirmModal);
    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, selectedSongKey);
    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, selectedRating);
    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, selectedCharacteristics);
    DECLARE_INSTANCE_FIELD(UnityW<HMUI::ImageView>, songDetailsLoading);
    DECLARE_INSTANCE_FIELD(UnityW<HMUI::ImageView>, searchInProgress);

    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, selectedSongAuthor);
    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, selectedSongName);

    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::GameObject>, searchBox);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::Sprite>, defaultImage);
    DECLARE_INSTANCE_FIELD(UnityW<HMUI::ImageView>, coverImage);
    DECLARE_INSTANCE_FIELD(UnityW<HMUI::ImageView>, coverLoading);

    // Modals
    DECLARE_INSTANCE_FIELD(UnityW<Modals::MultiDL>, multiDlModal);
    DECLARE_INSTANCE_FIELD(UnityW<Modals::Settings>, settingsModal);
    DECLARE_INSTANCE_FIELD(UnityW<Modals::UploadDetails>, uploadDetailsModal);

    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, selectedSongDiffInfo);

    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, downloadButton);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, playButton);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, infoButton);
    BSML_OPTIONS_LIST_OBJECT(sortModeSelections, "Newest", "Oldest", "Latest Ranked", "Most Stars", "Least Stars", "Best rated", "Worst rated");

    DECLARE_INSTANCE_FIELD(UnityW<BSML::DropdownListSetting>, sortDropdown);
    DECLARE_INSTANCE_FIELD(StringW, selectedSortMode);

    DECLARE_INSTANCE_FIELD(UnityW<GlobalNamespace::SoloFreePlayFlowCoordinator>, soloFreePlayFlowCoordinator);
    DECLARE_INSTANCE_FIELD(UnityW<GlobalNamespace::MultiplayerLevelSelectionFlowCoordinator>, multiplayerLevelSelectionFlowCoordinator);

    DECLARE_INSTANCE_FIELD(System::Threading::CancellationTokenSource*, songAssetLoadCanceller);

   public:
    UnityW<HMUI::TableView> songListTable() {
        if (songList) {
            return songList->tableView;
        } else {
            return nullptr;
        }
    }

    void SelectSongByHash(std::string hash);
    void SetSelectedSong(SongDetailsCache::Song const* song);

    BetterSongSearch::Util::RatelimitCoroutine* limitedUpdateSearchedSongsList = nullptr;

    void SortAndFilterSongs(FilterTypes::SortMode sort, std::string_view search, bool resetTable);
    void ResetTable();

    void UpdateDetails();
    void SetIsDownloaded(bool isDownloaded, bool downloadable = true);

    void UpdateSearch();
    custom_types::Helpers::Coroutine UpdateDataAndFiltersCoro();

    void PlaySong(SongDetailsCache::Song const* song = nullptr);
    void DownloadSongList();
    void RetryDownloadSongList();

    void EnterSolo(GlobalNamespace::BeatmapLevel * level);

    // Event receivers
    void SearchDone();
    void SongDataDone();
    void SongDataError(std::string message);
    void PlayerDataLoaded();
    void OnSongsLoaded(std::span<SongCore::SongLoader::CustomBeatmapLevel* const> songs);

    SongDetailsCache::Song const* GetCurrentSong();
    void SetCurrentSong(SongDetailsCache::Song const* song);

   private:
    std::shared_mutex _currentSongMutex;
    SongDetailsCache::Song const* _currentSong = nullptr;
};
