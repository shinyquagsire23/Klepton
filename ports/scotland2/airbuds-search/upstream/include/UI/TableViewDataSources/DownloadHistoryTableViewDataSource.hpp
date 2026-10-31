#pragma once

#include "Log.hpp"
#include "bsml/shared/BSML/Components/CustomListTableData.hpp"
#include "custom-types/shared/macros.hpp"
#include "song-details/shared/SongDetails.hpp"

struct DownloadHistoryItem {
    const SongDetailsCache::Song* song = nullptr;

    bool isDownloadStarted_ = false;
    float downloadProgress_ = 0;
    bool isDownloadStopped_ = false;
    bool isDownloadError_ = false;

    std::mutex mutex_;

    void onDownloadStarted() {
        std::unique_lock<std::mutex> lock(mutex_);
        isDownloadStarted_ = true;
        if (onDownloadStarted_) {
            onDownloadStarted_();
        }
    }

    void onDownloadProgress(const float progress) {
        std::unique_lock<std::mutex> lock(mutex_);
        downloadProgress_ = progress;
        if (onDownloadProgress_) {
            onDownloadProgress_(progress);
        }
    }

    void onDownloadStopped(const bool isSuccessful) {
        std::unique_lock<std::mutex> lock(mutex_);
        isDownloadStopped_ = true;
        isDownloadError_ = !isSuccessful;
        if (onDownloadStopped_) {
            onDownloadStopped_(isSuccessful);
        }
    }

    std::function<void()> onDownloadStarted_;
    std::function<void(float)> onDownloadProgress_;
    std::function<void(bool)> onDownloadStopped_;
};

DECLARE_CLASS_CODEGEN_INTERFACES(AirbudsSearch::UI, DownloadHistoryTableViewDataSource, UnityEngine::MonoBehaviour, HMUI::TableView::IDataSource*) {

    DECLARE_OVERRIDE_METHOD_MATCH(HMUI::TableCell*, CellForIdx, &HMUI::TableView::IDataSource::CellForIdx, HMUI::TableView * tableView, int idx);
    DECLARE_OVERRIDE_METHOD_MATCH(float, CellSize, &HMUI::TableView::IDataSource::CellSize);
    DECLARE_OVERRIDE_METHOD_MATCH(int, NumberOfCells, &HMUI::TableView::IDataSource::NumberOfCells);

    public:
    std::vector<std::shared_ptr<DownloadHistoryItem>> downloadHistoryItems_;
};
