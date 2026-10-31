#include "UI/FlowCoordinators/BetterSongSearchFlowCoordinator.hpp"

#include <bsml/shared/Helpers/creation.hpp>
#include <bsml/shared/Helpers/getters.hpp>
#include <UnityEngine/Resources.hpp>

#include "GlobalNamespace/SongPreviewPlayer.hpp"
#include "HMUI/ViewController.hpp"
#include "logging.hpp"
#include "System/Action.hpp"

using namespace GlobalNamespace;

DEFINE_TYPE(BetterSongSearch::UI::FlowCoordinators, BetterSongSearchFlowCoordinator);

void BetterSongSearch::UI::FlowCoordinators::BetterSongSearchFlowCoordinator::Awake() {
    fcInstance = this;
    if (!SongListController) {
        SongListController = BSML::Helpers::CreateViewController<ViewControllers::SongListController*>();
    }
    if (!FilterViewController) {
        FilterViewController = BSML::Helpers::CreateViewController<ViewControllers::FilterViewController*>();
    }
    if (!DownloadHistoryViewController) {
        DownloadHistoryViewController = BSML::Helpers::CreateViewController<ViewControllers::DownloadHistoryViewController*>();
    }

    // Make sure the flow coordinator is not destroyed when changing scenes
    UnityEngine::Object::DontDestroyOnLoad(this->get_gameObject());
}

void BetterSongSearch::UI::FlowCoordinators::BetterSongSearchFlowCoordinator::DidActivate(
    bool firstActivation, bool addedToHeirarchy, bool screenSystemEnabling
) {
    if (!firstActivation) {
        return;
    }

    SetTitle("Better Song Search", HMUI::ViewController::AnimationType::In);
    showBackButton = true;
    ProvideInitialViewControllers(SongListController, FilterViewController, DownloadHistoryViewController, nullptr, nullptr);
}

void BetterSongSearch::UI::FlowCoordinators::BetterSongSearchFlowCoordinator::BackButtonWasPressed(HMUI::ViewController* topViewController) {
    this->Close();
}

//  std::function<void(int)> cancelConfirmCallback;
void BetterSongSearch::UI::FlowCoordinators::BetterSongSearchFlowCoordinator::Close(bool immediately, bool downloadAbortConfim) {
    // Do nothing if there's no parent flow coordinator (in multiplayer if you never called it it crashed)

    if (downloadAbortConfim && ConfirmCancelOfPending([this, immediately]() {
            this->Close(immediately, false);
        })) {
        return;
    }

    cancelConfirmCallback = nullptr;

    // Stop song preview on exit
    UnityW<SongPreviewPlayer> songPreviewPlayer = BSML::Helpers::GetDiContainer()->Resolve<SongPreviewPlayer*>();
    if (songPreviewPlayer) {
        songPreviewPlayer->CrossfadeToDefault();
    }

    // Trigger refresh of songs only if needed
    if (DownloadHistoryViewController->hasUnloadedDownloads) {
        SongCore::API::Loading::RefreshSongs(false);
        DownloadHistoryViewController->hasUnloadedDownloads = false;
    }

    // Hide all modals
    for (auto modal : SongListController->GetComponentsInChildren<HMUI::ModalView*>()) {
        modal->Hide(false, nullptr);
    }

    if (fcInstance && fcInstance->get_isActiveAndEnabled() && fcInstance->get_isActivated()) {
        this->____parentFlowCoordinator->DismissFlowCoordinator(this, HMUI::ViewController::AnimationDirection::Horizontal, nullptr, immediately);
    }
};

bool BetterSongSearch::UI::FlowCoordinators::BetterSongSearchFlowCoordinator::ConfirmCancelOfPending(std::function<void()> callback) {
    if (DownloadHistoryViewController->HasPendingDownloads()) {
        cancelConfirmCallback = callback;
        SongListController->ShowCloseConfirmation();
        return true;
    } else {
        return false;
    }
};

void BetterSongSearch::UI::FlowCoordinators::BetterSongSearchFlowCoordinator::ConfirmCancelCallback(bool doCancel) {
    if (doCancel) {
        // Fail all dls
        for (auto entry : DownloadHistoryViewController->downloadEntryList) {
            if (entry->IsInAnyOfStates((DownloadHistoryEntry::DownloadStatus)(
                    DownloadHistoryEntry::DownloadStatus::Downloading | DownloadHistoryEntry::DownloadStatus::Queued
                ))) {
                entry->retries = 69;
                entry->status = DownloadHistoryEntry::DownloadStatus::Failed;
            }
        }

        // closeCancelSource?.Cancel();
        cancelConfirmCallback();
    }

    cancelConfirmCallback = nullptr;
}
