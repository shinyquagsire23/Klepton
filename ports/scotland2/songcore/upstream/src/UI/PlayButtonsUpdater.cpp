#include "UI/PlayButtonsUpdater.hpp"
#include "CustomJSONData.hpp"
#include "LevelSelect.hpp"
#include "SongLoader/RuntimeSongLoader.hpp"
#include "logging.hpp"
#include "hooking.hpp"

#include "bsml/shared/Helpers/delegates.hpp"
#include <string_view>

DEFINE_TYPE(SongCore::UI, PlayButtonsUpdater);

bool IsPracticeButtonInteractable = true;
bool IsPlayButtonInteractable = true;

namespace SongCore::UI {
    void PlayButtonsUpdater::ctor(SongLoader::RuntimeSongLoader* runtimeSongLoader, GlobalNamespace::StandardLevelDetailViewController* levelDetailViewController, PlayButtonInteractable* playButtonInteractable, Capabilities* capabilities, LevelSelect* levelSelect) {
        _runtimeSongLoader = runtimeSongLoader;
        _levelDetailViewController = levelDetailViewController;
        _playButtonInteractable = playButtonInteractable;
        _capabilities = capabilities;
        _levelSelect = levelSelect;
    }

    void PlayButtonsUpdater::Initialize() {
        _playButton = _levelDetailViewController->_standardLevelDetailView->actionButton;
        _practiceButton = _levelDetailViewController->_standardLevelDetailView->practiceButton;

        _anyDisablingModInfos = !_playButtonInteractable->PlayButtonDisablingModInfos.empty();
        _isRefreshing = _runtimeSongLoader->AreSongsRefreshing;

        _runtimeSongLoader->SongsWillRefresh += {&PlayButtonsUpdater::SongsWillRefresh, this};
        _runtimeSongLoader->SongsLoaded += {&PlayButtonsUpdater::SongsLoaded, this};
        _playButtonInteractable->PlayButtonDisablingModsChanged += {&PlayButtonsUpdater::HandleDisablingModInfosChanged, this};
        _levelSelect->LevelWasSelected += {&PlayButtonsUpdater::LevelWasSelected, this};

        HandleDisablingModInfosChanged(_playButtonInteractable->PlayButtonDisablingModInfos);
    }

    void PlayButtonsUpdater::Dispose() {
        _runtimeSongLoader->SongsWillRefresh += {&PlayButtonsUpdater::SongsWillRefresh, this};
        _runtimeSongLoader->SongsLoaded += {&PlayButtonsUpdater::SongsLoaded, this};
        _playButtonInteractable->PlayButtonDisablingModsChanged -= {&PlayButtonsUpdater::HandleDisablingModInfosChanged, this};
        _levelSelect->LevelWasSelected -= {&PlayButtonsUpdater::LevelWasSelected, this};

        IsPracticeButtonInteractable = true;
        IsPlayButtonInteractable = true;
    }

    bool PlayButtonsUpdater::IsPlayerAllowedToStart() {
        if (_isRefreshing) return false;
        if (_anyDisablingModInfos) return false;
        if (!_levelIsCustom) return true;
        if (_missingRequirements) return false;

        return true;
    }

    void PlayButtonsUpdater::UpdatePlayButtonsState() {
        auto interactable = IsPlayerAllowedToStart();

        IsPracticeButtonInteractable = interactable;
        IsPlayButtonInteractable = interactable && !_levelIsWIP;

        // if we allow player to start at all, practice button is enabled
        _practiceButton->set_interactable(IsPracticeButtonInteractable);
        // we want it to be "allow to start" and "not wip"
        _playButton->set_interactable(IsPlayButtonInteractable);
    }

    void PlayButtonsUpdater::SongsWillRefresh() {
        _isRefreshing = true;
        UpdatePlayButtonsState();
    }

    void PlayButtonsUpdater::SongsLoaded(std::span<SongLoader::CustomBeatmapLevel* const> levels) {
        _isRefreshing = false;
        UpdatePlayButtonsState();
    }

    void PlayButtonsUpdater::LevelWasSelected(LevelSelect::LevelWasSelectedEventArgs const& eventArgs) {
        _levelIsCustom = eventArgs.isCustom;
        _levelIsWIP = eventArgs.isWIP;

        _missingRequirements = false;
        if (eventArgs.customLevelDetails.has_value()) {
            for (auto& requirement : eventArgs.customLevelDetails->difficultyDetails.requirements) {
                if (!_capabilities->IsCapabilityRegistered(requirement)) {
                    _missingRequirements = true;
                    break;
                }
            }
        }

        UpdatePlayButtonsState();
    }

    void PlayButtonsUpdater::HandleDisablingModInfosChanged(std::span<PlayButtonInteractable::PlayButtonDisablingModInfo const> disablingModInfos) {
        _anyDisablingModInfos = !disablingModInfos.empty();

        UpdatePlayButtonsState();
    }
}

// since the game decided to update button interactability more often, we also have to hook things
MAKE_AUTO_HOOK_MATCH(
    StandardLevelDetailView_ShowContent,
    &GlobalNamespace::StandardLevelDetailView::ShowContent,
    void,
    GlobalNamespace::StandardLevelDetailView* self,
    GlobalNamespace::StandardLevelDetailViewController::ContentType contentType,
    float_t progress
) {
    StandardLevelDetailView_ShowContent(self, contentType, progress);

    if (contentType == GlobalNamespace::StandardLevelDetailViewController::ContentType::OwnedAndReady) {
        self->practiceButton->set_interactable(IsPracticeButtonInteractable);
        self->actionButton->set_interactable(IsPlayButtonInteractable);
    }
}
