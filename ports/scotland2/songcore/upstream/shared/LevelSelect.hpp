#pragma once

#include "custom-types/shared/macros.hpp"
#include "CustomJSONData.hpp"
#include "SongCore.hpp"

#include "Zenject/IInitializable.hpp"
#include "System/IDisposable.hpp"
#include "GlobalNamespace/StandardLevelDetailViewController.hpp"


DECLARE_CLASS_CODEGEN_INTERFACES(SongCore, LevelSelect, System::Object, Zenject::IInitializable*, System::IDisposable*) {
        DECLARE_CTOR(ctor, GlobalNamespace::StandardLevelDetailViewController* levelDetailViewController);

        DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::StandardLevelDetailViewController*, _levelDetailViewController);

        DECLARE_OVERRIDE_METHOD_MATCH(void, Initialize, &Zenject::IInitializable::Initialize);
        DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &System::IDisposable::Dispose);

        using ChangeDifficultyBeatmapAction = System::Action_1<UnityW<GlobalNamespace::StandardLevelDetailViewController>>;
        using ChangeContentAction = System::Action_2<UnityW<GlobalNamespace::StandardLevelDetailViewController>, GlobalNamespace::StandardLevelDetailViewController::ContentType>;
        DECLARE_INSTANCE_FIELD_PRIVATE(ChangeDifficultyBeatmapAction*, _changeDifficultyBeatmapAction);
        DECLARE_INSTANCE_FIELD_PRIVATE(ChangeContentAction*, _changeContentAction);

    public:
        using LevelWasSelectedEventArgs = SongCore::API::LevelSelect::LevelWasSelectedEventArgs;

        UnorderedEventCallback<LevelWasSelectedEventArgs const&> LevelWasSelected;

        GlobalNamespace::BeatmapLevelPack* GetSelectedLevelPack();
        __declspec(property(get=GetSelectedLevelPack)) GlobalNamespace::BeatmapLevelPack* SelectedLevelPack;

        GlobalNamespace::BeatmapLevel* GetSelectedBeatmapLevel();
        __declspec(property(get=GetSelectedBeatmapLevel)) GlobalNamespace::BeatmapLevel* SelectedBeatmapLevel;

        GlobalNamespace::BeatmapKey GetSelectedBeatmapKey();
        __declspec(property(get=GetSelectedBeatmapKey)) GlobalNamespace::BeatmapKey SelectedBeatmapKey;

        GlobalNamespace::BeatmapCharacteristicSO* GetSelectedCharacteristic();
        __declspec(property(get=GetSelectedCharacteristic)) GlobalNamespace::BeatmapCharacteristicSO* SelectedCharacteristic;

        GlobalNamespace::BeatmapDifficulty GetSelectedDifficulty();
        __declspec(property(get=GetSelectedDifficulty)) GlobalNamespace::BeatmapDifficulty SelectedDifficulty;

    private:
        void LevelDetailContentChanged(GlobalNamespace::StandardLevelDetailViewController* levelDetailViewController, GlobalNamespace::StandardLevelDetailViewController::ContentType contentType);
        void BeatmapLevelSelected(GlobalNamespace::StandardLevelDetailViewController* levelDetailViewController);

        void HandleCustomLevelWasSelected(LevelWasSelectedEventArgs& eventArgs);

        void StartLevelWasSelectedInvoke();

        /// @brief invoker method for the events
        void InvokeLevelWasSelected(LevelWasSelectedEventArgs const& eventArgs);
};