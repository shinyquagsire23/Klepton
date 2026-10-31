#include "CustomTypes/DownloadSongsPlaylistViewController.hpp"

#include "CustomLogger.hpp"
#include "HMUI/Touchable.hpp"
#include "HMUI/TableView.hpp"
#include "UnityEngine/UI/ContentSizeFitter.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"

#include "playlistcore/shared/CustomTypes/CoverTableCell.hpp"
#include "bsml/shared/BSML-Lite.hpp"

DEFINE_TYPE(SongDownloader, DownloadSongsPlaylistViewController);

using namespace SongDownloader;

bool DownloadSongsPlaylistViewController::downloadToPlaylistEnabled = true;
PlaylistCore::Playlist* DownloadSongsPlaylistViewController::selectedPlaylist = nullptr;

void DownloadSongsPlaylistViewController::RefreshPlaylistList() {
    if(!list) return;

    loadedPlaylists = PlaylistCore::GetLoadedPlaylists();
    std::vector<UnityEngine::Sprite*> newCovers;
    std::vector<std::string> newHovers;
    for(auto& playlist : loadedPlaylists) {
        newCovers.emplace_back(PlaylistCore::GetCoverImage(playlist));
        newHovers.emplace_back(playlist->name);
    }
    list->replaceSprites(newCovers);
    list->replaceTexts(newHovers);
    list->tableView->ReloadDataKeepingPosition();
}

void DownloadSongsPlaylistViewController::DidActivate(bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {

    RefreshPlaylistList();
    if(!firstActivation) return;

    gameObject->AddComponent<HMUI::Touchable*>();

    list = BSML::Lite::CreateScrollableCustomSourceList<PlaylistCore::CustomListSource*>(transform, {-50, -35}, {15, 70}, [this](int idx) {
        if (idx < loadedPlaylists.size())
            selectedPlaylist = loadedPlaylists[idx];
    });
    list->setType(csTypeOf(PlaylistCore::CoverTableCell*));

    RefreshPlaylistList();

    auto toggle = BSML::Lite::CreateToggle(transform, "Download To Playlist", downloadToPlaylistEnabled, {-50, -70}, [](bool enabled) {
        downloadToPlaylistEnabled = enabled;
    });
    toggle->GetComponent<UnityEngine::UI::LayoutElement*>()->preferredWidth = 45;
    auto sizeFitter = toggle->gameObject->AddComponent<UnityEngine::UI::ContentSizeFitter*>();
    sizeFitter->horizontalFit = UnityEngine::UI::ContentSizeFitter::FitMode::PreferredSize;
}

PlaylistCore::Playlist* DownloadSongsPlaylistViewController::GetSelectedPlaylist() {
    if (!downloadToPlaylistEnabled)
        return nullptr;
    return selectedPlaylist;
}
