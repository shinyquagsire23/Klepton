#pragma once

#include "custom-types/shared/macros.hpp"

#include "HMUI/FlowCoordinator.hpp"
#include "HMUI/ViewController.hpp"

#include "DownloadSongsOptionsViewController.hpp"
#include "DownloadSongsSearchViewController.hpp"
#include "DownloadSongsPlaylistViewController.hpp"

DECLARE_CLASS_CODEGEN(SongDownloader, DownloadSongsFlowCoordinator, HMUI::FlowCoordinator) {
    public:
        void GoToSongSelect();
        void Close(bool immediately);

    DECLARE_INSTANCE_FIELD(SongDownloader::DownloadSongsOptionsViewController*, DownloadSongsOptionsViewController);
    DECLARE_INSTANCE_FIELD(SongDownloader::DownloadSongsSearchViewController*, DownloadSongsSearchViewController);
    DECLARE_INSTANCE_FIELD(SongDownloader::DownloadSongsPlaylistViewController*, DownloadSongsPlaylistViewController);

    DECLARE_INSTANCE_METHOD(void, Awake);

    DECLARE_OVERRIDE_METHOD_MATCH(void, DidActivate, &HMUI::FlowCoordinator::DidActivate, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling);

    DECLARE_OVERRIDE_METHOD_MATCH(void, BackButtonWasPressed, &HMUI::FlowCoordinator::BackButtonWasPressed, HMUI::ViewController* topViewController);

};

inline UnityW<SongDownloader::DownloadSongsFlowCoordinator> fcInstance;