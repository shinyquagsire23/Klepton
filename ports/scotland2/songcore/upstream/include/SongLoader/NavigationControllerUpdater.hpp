#pragma once

#include "SongLoader/CustomBeatmapLevel.hpp"
#include "SongLoader/CustomBeatmapLevelsRepository.hpp"
#include "custom-types/shared/macros.hpp"
#include "System/Object.hpp"
#include "System/IDisposable.hpp"
#include "Zenject/IInitializable.hpp"
#include "GlobalNamespace/LevelFilteringNavigationController.hpp"
#include "GlobalNamespace/LevelCollectionViewController.hpp"
#include "GlobalNamespace/LevelCollectionNavigationController.hpp"
#include "GlobalNamespace/SelectLevelCategoryViewController.hpp"
#include "GlobalNamespace/BeatmapLevelsModel.hpp"
#include "SongLoader/RuntimeSongLoader.hpp"
#include <vector>

DECLARE_CLASS_CODEGEN_INTERFACES(SongCore::SongLoader, NavigationControllerUpdater, System::Object, System::IDisposable*, Zenject::IInitializable*) {
    DECLARE_OVERRIDE_METHOD_MATCH(void, Initialize, &Zenject::IInitializable::Initialize);
    DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &System::IDisposable::Dispose);

    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::BeatmapLevelsModel*, _beatmapLevelsModel);
    DECLARE_INSTANCE_FIELD_PRIVATE(RuntimeSongLoader*, _runtimeSongLoader);
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::LevelFilteringNavigationController*, _levelFilteringNavigationController);
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::LevelCollectionNavigationController*, _levelCollectionNavigationController);
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::LevelCollectionViewController*, _levelCollectionViewController);

    DECLARE_CTOR(ctor, GlobalNamespace::BeatmapLevelsModel* beatmapLevelsModel, RuntimeSongLoader* runtimeSongLoader, GlobalNamespace::LevelFilteringNavigationController* levelFilteringNavigationController, GlobalNamespace::LevelCollectionNavigationController* levelCollectionNavigationViewController, GlobalNamespace::LevelCollectionViewController* levelCollectionViewController);

    private:
        void SongsWillRefresh();
        void SongsLoaded(std::span<SongLoader::CustomBeatmapLevel* const> levels);
};