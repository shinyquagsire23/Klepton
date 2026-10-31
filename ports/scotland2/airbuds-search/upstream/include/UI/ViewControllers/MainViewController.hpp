#pragma once

#include <atomic>
#include <optional>
#include <queue>

#include "HMUI/ImageView.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "UnityEngine/UI/HorizontalLayoutGroup.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"
#include "bsml/shared/BSML/Components/CustomListTableData.hpp"
#include "custom-types/shared/macros.hpp"
#include "song-details/shared/SongDetails.hpp"

#include "CustomSongFilter.hpp"
#include "Airbuds/AirbudsClient.hpp"
#include "UI/TableViewDataSources/DownloadHistoryTableViewDataSource.hpp"

#if HOT_RELOAD
#include "bsml/shared/BSML/ViewControllers/HotReloadViewController.hpp"
using BaseViewController = BSML::HotReloadViewController;
#else
#include "HMUI/ViewController.hpp"
using BaseViewController = HMUI::ViewController;
#endif

namespace AirbudsSearch::Filter {

using SongFilterFunction = std::function<bool(const SongDetailsCache::Song* const song, const airbuds::Track& track)>;
using SongScoreFunction = std::function<int(const airbuds::Track& track, const SongDetailsCache::Song& song)>;

extern SongFilterFunction DEFAULT_SONG_FILTER_FUNCTION;
extern SongScoreFunction DEFAULT_SONG_SCORE_FUNCTION;

} // namespace AirbudsSearch::Filter

DECLARE_CLASS_CODEGEN_INTERFACES(AirbudsSearch::UI::ViewControllers, MainViewController, BaseViewController) {

    DECLARE_CTOR(ctor);

    DECLARE_OVERRIDE_METHOD_MATCH(void, DidActivate, &HMUI::ViewController::DidActivate, bool isFirstActivation, bool addedToHierarchy, bool screenSystemDisabling);

    DECLARE_INSTANCE_METHOD(void, PostParse);

    // Airbuds songs and playlists lists

    // Header
    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, airbudsColumnTitleTextView_);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, playlistsMenuButton_);
    DECLARE_INSTANCE_METHOD(void, onPlaylistsMenuButtonClicked);

    // Playlist list
    DECLARE_INSTANCE_FIELD(UnityW<BSML::CustomListTableData>, airbudsPlaylistListView_);
    DECLARE_INSTANCE_METHOD(void, onPlaylistSelected, UnityW<HMUI::TableView> table, int id);

    // Track list
    DECLARE_INSTANCE_FIELD(UnityW<BSML::CustomListTableData>, airbudsTrackListView_);
    DECLARE_INSTANCE_METHOD(void, onTrackSelected, UnityW<HMUI::TableView> table, int id);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, randomTrackButton_);
    DECLARE_INSTANCE_METHOD(void, onRandomTrackButtonClicked);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, randomScopeButton_);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::HorizontalLayoutGroup>, randomScopeToggleContainer_);
    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, randomScopeButtonTextView_);
    DECLARE_INSTANCE_METHOD(void, onRandomScopeToggleClicked);

    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::HorizontalLayoutGroup>, airbudsTrackListLoadingIndicatorContainer_);

    // Error message container
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::VerticalLayoutGroup>, airbudsListViewErrorContainer_);
    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, airbudsTrackListErrorMessageTextView_);
    DECLARE_INSTANCE_METHOD(void, onAirbudsTrackListRetryButtonClicked);

    // Status message container
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::VerticalLayoutGroup>, airbudsListViewStatusContainer_);
    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, airbudsTrackListStatusTextView_);

    // Search results
    DECLARE_INSTANCE_FIELD(UnityW<BSML::CustomListTableData>, searchResultsList_);
    DECLARE_INSTANCE_METHOD(void, onSearchResultSelected, UnityW<HMUI::TableView> table, int id);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::HorizontalLayoutGroup>, searchResultsListLoadingIndicatorContainer_);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::VerticalLayoutGroup>, searchResultsListViewErrorContainer_);
    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, searchResultsListStatusTextView_);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, showAllByArtistButton_);
    DECLARE_INSTANCE_METHOD(void, onShowAllByArtistButtonClicked);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, hideDownloadedMapsButton_);
    DECLARE_INSTANCE_METHOD(void, onHideDownloadedMapsButtonClicked);

    // Custom Song Preview
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, previewSongNameTextView_);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, previewSongAuthorTextView_);
    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, previewSongImage_);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, previewSongUploaderTextView_);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, previewSongLengthTextView_);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, previewSongNPSTextView_);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, previewSongNJSTextView_);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, downloadButton_);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, playButton_);
    DECLARE_INSTANCE_METHOD(void, onDownloadButtonClicked);
    DECLARE_INSTANCE_METHOD(void, onPlayButtonClicked);

    public:
    void setFilter(const CustomSongFilter& customSongFilter);
    void setHistoryFriend(const std::optional<airbuds::Friend>& friendUser);

    private:
    std::vector<const SongDetailsCache::Song*> searchResultItems_;
    const SongDetailsCache::Song* previewSong_;

    std::unique_ptr<airbuds::Playlist> selectedPlaylist_;
    std::unique_ptr<const airbuds::Track> selectedTrack_;
    std::optional<airbuds::Friend> selectedFriend_;

    std::queue<std::shared_ptr<DownloadHistoryItem>> pendingDownloads_;
    std::mutex pendingDownloadsMutex_;
    std::atomic<bool> isDownloadThreadRunning_;

    std::atomic<bool> isLoadingMoreAirbudsTracks_;
    std::atomic<bool> isLoadingMoreAirbudsPlaylists_;
    std::atomic<bool> isSearchInProgress_;

    std::atomic<bool> isShowingAllTracksByArtist_;
    std::atomic<bool> isShowingDownloadedMaps_{true};

    void setRandomScopeVisible(bool visible);
    void updateRandomScopeButtonLabel();
    void updateHistoryTitle();
    void forceLayoutRebuild();

    bool randomAcrossAllDays_;
    std::optional<airbuds::PlaylistTrack> pendingRandomTrack_;

    AirbudsSearch::Filter::SongFilterFunction currentSongFilter_;
    AirbudsSearch::Filter::SongScoreFunction currentSongScore_;

    void reloadAirbudsTrackListView();
    void reloadAirbudsPlaylistListView();
    bool selectPlaylistById(std::string_view playlistId);
    std::optional<std::string> getSelectedFriendId() const;
    bool historyContextMatches(const std::optional<std::string>& friendId) const;
    std::vector<airbuds::PlaylistTrack> getRecentlyPlayedCachedOnlyForCurrentUser();
    std::string getHistoryTitle() const;

    void setSelectedSongUi(const SongDetailsCache::Song* const song);

    void showAirbudsTrackLoadingIndicator();
    void showAirbudsTrackListView();
    void showAirbudsPlaylistListView();
    void onAirbudsTrackLoadingError(const std::string& message);

    void doSongSearch(const airbuds::Track& track);

    CustomSongFilter customSongFilter_;

    void startDownloadThread();

    void onTrackLoadError(const std::string& message);
    void resetListError();
};
