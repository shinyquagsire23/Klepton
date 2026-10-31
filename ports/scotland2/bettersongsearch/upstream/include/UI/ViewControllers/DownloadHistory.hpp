#pragma once

#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/ViewController.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/TableCell.hpp"
#include "System/Object.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
// #include "questui/shared/CustomTypes/Components/List/QuestUITableView.hpp"
// #include "questui/shared/CustomTypes/Components/Backgroundable.hpp"
#include "song-details/shared/Data/Song.hpp"
#include "song-details/shared/Data/SongDifficulty.hpp"
#include "song-details/shared/Data/MapCharacteristic.hpp"
#include "bsml/shared/macros.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/ViewControllers/HotReloadViewController.hpp"
#include "bsml/shared/BSML/Components/CustomListTableData.hpp"
#include "custom-types/shared/coroutine.hpp"
#include "custom-types/shared/macros.hpp"

#include "Util/RatelimitCoroutine.hpp"

#ifndef DECLARE_OVERRIDE_METHOD_MATCH
#define DECLARE_OVERRIDE_METHOD_MATCH(retval, method, mptr, ...) \
    DECLARE_OVERRIDE_METHOD(retval, method, il2cpp_utils::il2cpp_type_check::MetadataGetter<mptr>::get(), __VA_ARGS__)
#endif

inline static const int RETRY_COUNT = 3;


// Make sure to access it only from the main thread (not thread safe)
class DownloadHistoryEntry {
public:
    enum DownloadStatus {
        Downloading = 1,
        Preparing = 2,
        Extracting = 4,
        Queued = 8,
        Failed = 16,
        Downloaded = 32,
        Loaded = 64
    };
    DownloadStatus status = DownloadStatus::Queued;
    std::string statusDetails = "";
    std::string statusMessage() {return fmt::format("{} {}", StatusToString(status), statusDetails);}
    float downloadProgress = 1.0f;
    int retries = 0;
    bool isDownloading() { return status == DownloadStatus::Downloading || status == DownloadStatus::Preparing || status == DownloadStatus::Extracting;}
    bool isQueued() { return status == DownloadStatus::Queued || (status == DownloadStatus::Failed && retries < RETRY_COUNT);}
    std::string songName;
    std::string levelAuthorName;
    std::string key;
    std::string hash;

    // Last update used to limit progress updates cause I can't code in c++
    long long lastUpdate;

    int orderValue() { return ((int)status * 100) + retries; }

    bool IsInAnyOfStates(DownloadStatus states) {
        return (status & states) != 0;
    }

    static std::string StatusToString(DownloadStatus status){
        if (status == DownloadStatus::Downloading) {
            return "Downloading";
        }
        if (status == DownloadStatus::Preparing) {
            return "Preparing";
        }
        if (status == DownloadStatus::Extracting) {
            return "Extracting";
        }
        if (status == DownloadStatus::Queued) {
            return "Queued";
        }
        if (status == DownloadStatus::Failed) {
            return "Failed";
        }
        if (status == DownloadStatus::Downloaded) {
            return "Downloaded";
        }
        if (status == DownloadStatus::Loaded) {
            return "Loaded";
        }
        return "";

    }

    void ResetIfFailed() {
        if(status != DownloadStatus::Failed || retries < RETRY_COUNT)
            return;

        status = DownloadStatus::Queued;
        retries = 0;
    }

    DownloadHistoryEntry(const SongDetailsCache::Song* song) {
        songName = song->songName();
        levelAuthorName = song->levelAuthorName();
        key = song->key();
        hash = song->hash();
    }

    std::function<void()> UpdateProgressHandler;
};

#ifdef HotReload
DECLARE_CLASS_CUSTOM_INTERFACES(BetterSongSearch::UI::ViewControllers, DownloadHistoryViewController, BSML::HotReloadViewController, std::vector<Il2CppClass*>({classof(HMUI::TableView::IDataSource*)})) {
#else
DECLARE_CLASS_CODEGEN_INTERFACES(BetterSongSearch::UI::ViewControllers, DownloadHistoryViewController, HMUI::ViewController, HMUI::TableView::IDataSource*) {
#endif
    DECLARE_CTOR(ctor);
    DECLARE_SIMPLE_DTOR();
    DECLARE_OVERRIDE_METHOD_MATCH(void, DidActivate, &HMUI::ViewController::DidActivate, bool firstActivation, bool addedToHeirarchy, bool screenSystemDisabling);
    DECLARE_INSTANCE_FIELD(UnityW<BSML::CustomListTableData>, downloadList);
    DECLARE_INSTANCE_METHOD(void, SelectSong, HMUI::TableView* table, int id);
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::VerticalLayoutGroup*, scrollBarContainer);
    DECLARE_INSTANCE_FIELD(float, cellSize);
    DECLARE_OVERRIDE_METHOD_MATCH(HMUI::TableCell*, CellForIdx, &HMUI::TableView::IDataSource::CellForIdx, HMUI::TableView* tableView, int idx);
    DECLARE_OVERRIDE_METHOD_MATCH(float, CellSize, &HMUI::TableView::IDataSource::CellSize);
    DECLARE_OVERRIDE_METHOD_MATCH(int, NumberOfCells, &HMUI::TableView::IDataSource::NumberOfCells);



public:
    UnityW<HMUI::TableView> downloadHistoryTable() {if(downloadList) {return downloadList->tableView;} else return nullptr;}
    std::vector<DownloadHistoryEntry*> downloadEntryList;
    void ProcessDownloads(bool forceTableReload = false);
    void RefreshTable(bool fullReload = true);
    BetterSongSearch::Util::RatelimitCoroutine* limitedFullTableReload = nullptr;
    bool TryAddDownload(const SongDetailsCache::Song* song, bool isBatch = false);
    DownloadHistoryEntry* GetDownloadByHash(std::string hash);
    bool CheckIsDownloadedAndLoaded(std::string songHash);
    bool CheckIsDownloadable(std::string songHash);
    bool CheckIsDownloadable(DownloadHistoryEntry* entry);
    bool CheckIsDownloaded(std::string songHash);
    const int MAX_PARALLEL_DOWNLOADS = 3;
    bool hasUnloadedDownloads = false;
    bool HasPendingDownloads();
};
