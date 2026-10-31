#include "include/UI/DeleteLevelButton.hpp"
#include "logging.hpp"
#include "assets.hpp"

#include "UnityEngine/UI/Button.hpp"
#include "UnityEngine/Transform.hpp"
#include "GlobalNamespace/StandardLevelDetailView.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/UI/ContentSizeFitter.hpp"

#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/Components/ButtonIconImage.hpp"

#include <chrono>
#include <future>

DEFINE_TYPE(SongCore::UI, DeleteLevelButton);

namespace SongCore::UI {
    void DeleteLevelButton::ctor(SongLoader::RuntimeSongLoader* runtimeSongLoader, GlobalNamespace::StandardLevelDetailViewController* standardLevelDetailViewController, LevelSelect* levelSelect, IconCache* iconCache, ProgressBar* progressBar) {
        INVOKE_CTOR();
        _runtimeSongLoader = runtimeSongLoader;
        _levelDetailViewController = standardLevelDetailViewController;
        _levelSelect = levelSelect;
        _iconCache = iconCache;
        _progressBar = progressBar;
    }

    void DeleteLevelButton::Initialize() {
        _levelSelect->LevelWasSelected += {&DeleteLevelButton::LevelWasSelected, this};

        auto detailView = _levelDetailViewController->_standardLevelDetailView;
        auto parent = detailView->practiceButton->transform->parent;

        // create button
        BSML::parse_and_construct("<icon-button id='_deleteButton' pref-width='10' pref-height='10' on-click='AttemptDeleteCurrentlySelectedLevel'/>", parent, this);
        _deleteButtonRoot = _deleteButton->gameObject;
        /// make sure it's the leftmost button
        _deleteButtonRoot->transform->SetAsFirstSibling();
        // sprite, scale, skew fixing
        auto buttonImg = _deleteButtonRoot->GetComponent<BSML::ButtonIconImage*>();
        buttonImg->SetIcon(_iconCache->DeleteIcon);
        float scale = 1.7f;
        buttonImg->image->transform->localScale = {scale, scale, scale};
        auto imageView = il2cpp_utils::try_cast<HMUI::ImageView>(buttonImg->image).value_or(nullptr);
        if (imageView) {
            imageView->_skew = 0.18f;
        }

        BSML::parse_and_construct(Assets::deletemodal_bsml, parent, this);
    }

    void DeleteLevelButton::Dispose() {
        _levelSelect->LevelWasSelected -= {&DeleteLevelButton::LevelWasSelected, this};
    }

    void DeleteLevelButton::Tick() {
        using namespace std::chrono_literals;
        if (_songDeleteFuture.valid() && _songDeleteFuture.wait_for(0ns) == std::future_status::ready) {
            _songDeleteFuture = std::future<void>();
            _runtimeSongLoader->RefreshSongs();
        }
    }

    void DeleteLevelButton::UpdateButtonState() {
        _deleteButtonRoot->SetActive(_lastSelectedCustomLevel != nullptr);
    }

    void DeleteLevelButton::AttemptDeleteCurrentlySelectedLevel() {
        // can't delete a level that isn't custom
        if (!_lastSelectedCustomLevel) return;
        // we're still waiting for a delete to finish up
        if (_songDeleteFuture.valid()) return;
        _levelText->text = _lastSelectedCustomLevel->songName;
        _deleteModal->Show();
    }

    void DeleteLevelButton::CommitDelete() {
        _songDeleteFuture = _runtimeSongLoader->DeleteSong(_lastSelectedCustomLevel);
        _deleteModal->Hide();
        _progressBar->ShowMessage("Deleting level!", 5);
    }

    void DeleteLevelButton::LevelWasSelected(LevelSelect::LevelWasSelectedEventArgs const& eventArgs) {
        if (eventArgs.isCustom) {
            _lastSelectedCustomLevel = eventArgs.customBeatmapLevel;
        } else {
            _lastSelectedCustomLevel = nullptr;
        }
        UpdateButtonState();
    }
}
