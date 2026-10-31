#include "GlobalNamespace/LevelSelectionFlowCoordinator.hpp"
#include "GlobalNamespace/SelectLevelCategoryViewController.hpp"
#include "HMUI/NoTransitionsButton.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "bsml/shared/Helpers/getters.hpp"
#include "songcore/shared/SongCore.hpp"

#include "Log.hpp"
#include "UI/TableViewCells/DownloadHistoryTableViewCell.hpp"
#include "Utils.hpp"

using namespace AirbudsSearch::UI;
using namespace GlobalNamespace;

DEFINE_TYPE(AirbudsSearch::UI, DownloadHistoryTableViewCell);

void DownloadHistoryTableViewCell::ctor() {
    INVOKE_BASE_CTOR(classof(HMUI::TableCell*));
    downloadHistoryItem_ = nullptr;
    guard_ = std::make_shared<bool>(true);
}

void DownloadHistoryTableViewCell::OnDestroy() {
}

void DownloadHistoryTableViewCell::SelectionDidChange(HMUI::SelectableCell::TransitionType transitionType) {
    updateBackground();
}

void DownloadHistoryTableViewCell::HighlightDidChange(HMUI::SelectableCell::TransitionType transitionType) {
    updateBackground();
}

void DownloadHistoryTableViewCell::WasPreparedForReuse() {
}

void DownloadHistoryTableViewCell::updateBackground() {
    root_->set_color(UnityEngine::Color(0, 0, 0, selected || highlighted ? 0.8f : 0.45f));
}

void DownloadHistoryTableViewCell::onPlayButtonClicked() {
    AirbudsSearch::Utils::goToLevelSelect(downloadHistoryItem_->song->hash());
}

void DownloadHistoryTableViewCell::setDownloadHistoryItem(const std::shared_ptr<DownloadHistoryItem>& downloadHistoryItem) {
    downloadHistoryItem_ = downloadHistoryItem;
    const SongDetailsCache::Song* const song = downloadHistoryItem_->song;

    // Song name
    songNameTextView_->set_text(song->songName());

    // Map author
    uploaderNameTextView_->set_text(song->uploaderName());

    // Loading sprite
    const UnityW<UnityEngine::Sprite> placeholderSprite = Utils::getAlbumPlaceholderSprite();
    image_->set_sprite(placeholderSprite);

    // Load cover image
    const std::string songHash = song->hash();
    const std::weak_ptr<DownloadHistoryItem> weakDownloadHistoryItem = downloadHistoryItem_;
    std::weak_ptr<bool> weakGuard = guard_;
    Utils::getCoverImageSprite(songHash, [this, weakGuard, songHash, weakDownloadHistoryItem](const UnityW<UnityEngine::Sprite> sprite) {
        if (!weakGuard.lock()) {
            AirbudsSearch::Log.warn("weakGuard was null!");
            return;
        }

        // Check if the selected song has changed
        std::shared_ptr<DownloadHistoryItem> item = weakDownloadHistoryItem.lock();
        if (!item || item->song->hash() != songHash) {
            AirbudsSearch::Log.warn("Cancelled sprite update");
            return;
        }

        // Update UI
        if (sprite) {
            image_->set_sprite(sprite);
        } else {
            AirbudsSearch::Log.warn("Failed loading cover image for song with hash: {}", songHash);
        }
    });

    std::unique_lock<std::mutex> lock(downloadHistoryItem_->mutex_);

    // Download progress
    if (downloadHistoryItem_->isDownloadStopped_) {
        if (downloadHistoryItem_->isDownloadError_) {
            downloadProgressTextView_->get_gameObject()->set_active(true);
            downloadProgressTextView_->set_text("Download Error!");
            playButton_->get_gameObject()->set_active(false);
        } else {
            SongCore::SongLoader::CustomBeatmapLevel* beatmap = SongCore::API::Loading::GetLevelByHash(songHash);
            if (beatmap) {
                downloadProgressTextView_->get_gameObject()->set_active(false);
                playButton_->get_gameObject()->set_active(true);
            } else {
                onDownloadProgress(1);
            }
        }
    } else {
        onDownloadProgress(0);
    }

    downloadHistoryItem_->onDownloadStarted_ = [this, songHash]() {
        BSML::MainThreadScheduler::Schedule([this, songHash]() {
            onDownloadProgress(0);
        });
    };
    downloadHistoryItem_->onDownloadProgress_ = [this, songHash](const float progress) {
        BSML::MainThreadScheduler::Schedule([this, progress, songHash]() {
            onDownloadProgress(progress);
        });
    };
    downloadHistoryItem_->onDownloadStopped_ = [this, songHash](const bool success) {
        BSML::MainThreadScheduler::Schedule([this, songHash]() {
            const SongCore::SongLoader::CustomBeatmapLevel* const beatmap = SongCore::API::Loading::GetLevelByHash(songHash);
            if (beatmap) {
                downloadProgressTextView_->get_gameObject()->set_active(false);
                playButton_->get_gameObject()->set_active(true);
            } else {
                onDownloadProgress(1);
            }
        });
    };
}

void DownloadHistoryTableViewCell::onDownloadProgress(const float progress) {
    playButton_->get_gameObject()->set_active(false);
    downloadProgressTextView_->get_gameObject()->set_active(true);
    downloadProgressTextView_->set_text(std::format("{}%", static_cast<int>(progress * 100.0f)));
}
