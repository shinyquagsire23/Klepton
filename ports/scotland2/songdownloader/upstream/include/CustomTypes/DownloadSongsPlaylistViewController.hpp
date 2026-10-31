#pragma once

#include "custom-types/shared/macros.hpp"

#include "HMUI/ViewController.hpp"

#include "playlistcore/shared/CustomTypes/CustomListSource.hpp"
#include "playlistcore/shared/PlaylistCore.hpp"

DECLARE_CLASS_CODEGEN(SongDownloader, DownloadSongsPlaylistViewController, HMUI::ViewController) {

    DECLARE_INSTANCE_FIELD_DEFAULT(PlaylistCore::CustomListSource*, list, nullptr);

    DECLARE_INSTANCE_METHOD(void, RefreshPlaylistList);

    DECLARE_OVERRIDE_METHOD_MATCH(void, DidActivate, &HMUI::ViewController::DidActivate, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling);

    DECLARE_DEFAULT_CTOR();

    std::vector<PlaylistCore::Playlist*> loadedPlaylists;

    static bool downloadToPlaylistEnabled;
    static PlaylistCore::Playlist* selectedPlaylist;

    public:
    static PlaylistCore::Playlist* GetSelectedPlaylist();
};
