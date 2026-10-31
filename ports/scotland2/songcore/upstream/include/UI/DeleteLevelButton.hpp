#pragma once

#include "custom-types/shared/macros.hpp"
#include "GlobalNamespace/StandardLevelDetailViewController.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "Zenject/IInitializable.hpp"
#include "Zenject/ITickable.hpp"
#include "System/IDisposable.hpp"
#include "TMPro/TextMeshProUGUI.hpp"


#include "UI/ProgressBar.hpp"
#include "SongLoader/RuntimeSongLoader.hpp"
#include "SongLoader/CustomBeatmapLevel.hpp"
#include "LevelSelect.hpp"
#include "IconCache.hpp"
#include "ProgressBar.hpp"
#include "bsml/shared/BSML/Components/ModalView.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(SongCore::UI, DeleteLevelButton, System::Object, Zenject::IInitializable*, Zenject::ITickable*, System::IDisposable*) {
        DECLARE_CTOR(ctor, SongLoader::RuntimeSongLoader* runtimeSongLoader, GlobalNamespace::StandardLevelDetailViewController* standardLevelDetailViewController, LevelSelect* levelSelect, IconCache* iconCache, ProgressBar* progressBar);
        DECLARE_INSTANCE_FIELD_PRIVATE(SongLoader::RuntimeSongLoader*, _runtimeSongLoader);
        DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::StandardLevelDetailViewController*, _levelDetailViewController);
        DECLARE_INSTANCE_FIELD_PRIVATE(LevelSelect*, _levelSelect);
        DECLARE_INSTANCE_FIELD_PRIVATE(IconCache*, _iconCache);
        DECLARE_INSTANCE_FIELD_PRIVATE(ProgressBar*, _progressBar);

        DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::GameObject*, _deleteButtonRoot);
        DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::UI::Button*, _deleteButton);
        DECLARE_INSTANCE_FIELD_PRIVATE(BSML::ModalView*, _deleteModal);
        DECLARE_INSTANCE_FIELD_PRIVATE(TMPro::TextMeshProUGUI*, _levelText);

        DECLARE_INSTANCE_FIELD_PRIVATE(SongLoader::CustomBeatmapLevel*, _lastSelectedCustomLevel);

        DECLARE_OVERRIDE_METHOD_MATCH(void, Initialize, &Zenject::IInitializable::Initialize);
        DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &System::IDisposable::Dispose);
        DECLARE_OVERRIDE_METHOD_MATCH(void, Tick, &Zenject::ITickable::Tick);
        DECLARE_INSTANCE_METHOD(void, AttemptDeleteCurrentlySelectedLevel);
        DECLARE_INSTANCE_METHOD(void, CommitDelete);
    private:
        std::future<void> _songDeleteFuture;
        void UpdateButtonState();

        void LevelWasSelected(LevelSelect::LevelWasSelectedEventArgs const& eventArgs);
};