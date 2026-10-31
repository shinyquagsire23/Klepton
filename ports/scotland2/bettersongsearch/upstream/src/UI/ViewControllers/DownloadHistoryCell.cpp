#include "UI/ViewControllers/DownloadHistoryCell.hpp"

#include "UnityEngine/RectTransform.hpp"

DEFINE_TYPE(BetterSongSearch::UI::ViewControllers, CustomDownloadListTableCell)

namespace BetterSongSearch::UI::ViewControllers {
    CustomDownloadListTableCell* CustomDownloadListTableCell::PopulateWithSongData(DownloadHistoryEntry* entry) {
        songName->set_text(entry->songName);
        levelAuthorName->set_text(entry->levelAuthorName);
        statusLabel->set_text(entry->statusMessage());
        this->entry = entry;
        entry->UpdateProgressHandler = [this]() {
            UpdateProgress();
        };
        return this;
    }

    void CustomDownloadListTableCell::RefreshBgState() {
        bgContainer->set_color(UnityEngine::Color(0, 0, 0, highlighted ? 0.8f : 0.45f));
        RefreshBar();
    }

    void CustomDownloadListTableCell::RefreshBar() {
        if (!entry) {
            return;
        }
        auto clr = entry->status == DownloadHistoryEntry::Failed                 ? UnityEngine::Color::get_red()
                 : entry->status != DownloadHistoryEntry::DownloadStatus::Queued ? UnityEngine::Color::get_green()
                                                                                 : UnityEngine::Color::get_gray();
        clr.a = 0.5f + (entry->downloadProgress * 0.4f);
        bgProgress->set_color(clr);

        auto x = bgProgress->get_gameObject()->get_transform().cast<UnityEngine::RectTransform>();
        if (!x) {
            return;
        }
        x->set_anchorMax(UnityEngine::Vector2(entry->downloadProgress, 1));
        x->ForceUpdateRectTransforms();
    }

    void CustomDownloadListTableCell::UpdateProgress() {
        statusLabel->set_text(entry->statusMessage());
        RefreshBar();
    }

    void CustomDownloadListTableCell::SelectionDidChange(HMUI::SelectableCell::TransitionType transitionType) {
        RefreshBgState();
    }

    void CustomDownloadListTableCell::HighlightDidChange(HMUI::SelectableCell::TransitionType transitionType) {
        RefreshBgState();
    }

    void CustomDownloadListTableCell::WasPreparedForReuse() {
        if (!entry) {
            return;
        }
        entry->UpdateProgressHandler = nullptr;
    }
}  // namespace BetterSongSearch::UI::ViewControllers
