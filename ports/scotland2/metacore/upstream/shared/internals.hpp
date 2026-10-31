#pragma once

#include "GlobalNamespace/BeatmapCallbacksController.hpp"
#include "GlobalNamespace/BeatmapData.hpp"
#include "GlobalNamespace/BeatmapKey.hpp"
#include "GlobalNamespace/BeatmapLevel.hpp"
#include "GlobalNamespace/BeatmapLevelPack.hpp"
#include "GlobalNamespace/BeatmapObjectManager.hpp"
#include "GlobalNamespace/ColorScheme.hpp"
#include "GlobalNamespace/ComboController.hpp"
#include "GlobalNamespace/EnvironmentInfoSO.hpp"
#include "GlobalNamespace/GameEnergyCounter.hpp"
#include "GlobalNamespace/GameplayModifiers.hpp"
#include "GlobalNamespace/SaberManager.hpp"
#include "GlobalNamespace/ScoreController.hpp"
#include "UnityEngine/Camera.hpp"
#include "UnityEngine/Quaternion.hpp"
#include "export.h"

// no per-variable documentation here, sorry

namespace MetaCore::Internals {
    METACORE_EXPORT extern int leftScore;
    METACORE_EXPORT extern int rightScore;
    METACORE_EXPORT extern int leftMaxScore;
    METACORE_EXPORT extern int rightMaxScore;
    METACORE_EXPORT extern int songMaxScore;
    METACORE_EXPORT extern int leftCombo;
    METACORE_EXPORT extern int rightCombo;
    METACORE_EXPORT extern int combo;
    METACORE_EXPORT extern int highestLeftCombo;
    METACORE_EXPORT extern int highestRightCombo;
    METACORE_EXPORT extern int highestCombo;
    METACORE_EXPORT extern int multiplier;
    METACORE_EXPORT extern int multiplierProgress;
    METACORE_EXPORT extern float health;
    METACORE_EXPORT extern float songTime;
    METACORE_EXPORT extern float songLength;
    METACORE_EXPORT extern float songSpeed;
    METACORE_EXPORT extern int notesLeftCut;
    METACORE_EXPORT extern int notesRightCut;
    METACORE_EXPORT extern int notesLeftBadCut;
    METACORE_EXPORT extern int notesRightBadCut;
    METACORE_EXPORT extern int notesLeftMissed;
    METACORE_EXPORT extern int notesRightMissed;
    METACORE_EXPORT extern int bombsLeftHit;
    METACORE_EXPORT extern int bombsRightHit;
    METACORE_EXPORT extern int wallsHit;
    METACORE_EXPORT extern int uncountedNotesLeftCut;
    METACORE_EXPORT extern int uncountedNotesRightCut;
    METACORE_EXPORT extern int remainingNotesLeft;
    METACORE_EXPORT extern int remainingNotesRight;
    METACORE_EXPORT extern int songNotesLeft;
    METACORE_EXPORT extern int songNotesRight;
    METACORE_EXPORT extern int leftPreSwing;
    METACORE_EXPORT extern int rightPreSwing;
    METACORE_EXPORT extern int leftPostSwing;
    METACORE_EXPORT extern int rightPostSwing;
    METACORE_EXPORT extern int leftAccuracy;
    METACORE_EXPORT extern int rightAccuracy;
    METACORE_EXPORT extern float leftTimeDependence;
    METACORE_EXPORT extern float rightTimeDependence;
    METACORE_EXPORT extern std::vector<float> leftSpeeds;
    METACORE_EXPORT extern std::vector<float> rightSpeeds;
    METACORE_EXPORT extern std::vector<float> leftAngles;
    METACORE_EXPORT extern std::vector<float> rightAngles;
    METACORE_EXPORT extern bool noFail;
    METACORE_EXPORT extern float positiveMods;
    METACORE_EXPORT extern float negativeMods;
    METACORE_EXPORT extern int personalBest;
    METACORE_EXPORT extern int fails;
    METACORE_EXPORT extern int restarts;
    METACORE_EXPORT extern int leftMissedMaxScore;
    METACORE_EXPORT extern int rightMissedMaxScore;
    METACORE_EXPORT extern int leftMissedFixedScore;
    METACORE_EXPORT extern int rightMissedFixedScore;
    METACORE_EXPORT extern UnityEngine::Quaternion prevRotLeft;
    METACORE_EXPORT extern UnityEngine::Quaternion prevRotRight;

    // references
    METACORE_EXPORT extern GlobalNamespace::GameplayModifiers* modifiers;
    METACORE_EXPORT extern GlobalNamespace::ColorScheme* colors;
    METACORE_EXPORT extern GlobalNamespace::BeatmapLevel* beatmapLevel;
    METACORE_EXPORT extern GlobalNamespace::BeatmapKey beatmapKey;
    METACORE_EXPORT extern GlobalNamespace::BeatmapData* beatmapData;
    METACORE_EXPORT extern GlobalNamespace::EnvironmentInfoSO* environment;
    METACORE_EXPORT extern GlobalNamespace::AudioTimeSyncController* audioTimeSyncController;
    METACORE_EXPORT extern GlobalNamespace::BeatmapCallbacksController* beatmapCallbacksController;
    METACORE_EXPORT extern GlobalNamespace::BeatmapObjectManager* beatmapObjectManager;
    METACORE_EXPORT extern GlobalNamespace::ComboController* comboController;
    METACORE_EXPORT extern GlobalNamespace::GameEnergyCounter* gameEnergyCounter;
    METACORE_EXPORT extern GlobalNamespace::ScoreController* scoreController;
    METACORE_EXPORT extern GlobalNamespace::SaberManager* saberManager;
    METACORE_EXPORT extern UnityEngine::Camera* mainCamera;

    METACORE_EXPORT extern bool stateValid;
    METACORE_EXPORT extern bool referencesValid;
    METACORE_EXPORT extern bool mapWasQuit;
    METACORE_EXPORT extern bool mapWasRestarted;

    METACORE_EXPORT void Initialize();
    METACORE_EXPORT void DoSlowUpdate();
    METACORE_EXPORT void Finish(bool quit, bool restart);

    METACORE_EXPORT extern GlobalNamespace::BeatmapKey selectedKey;
    METACORE_EXPORT extern GlobalNamespace::BeatmapLevel* selectedLevel;
    METACORE_EXPORT extern GlobalNamespace::BeatmapLevelPack* selectedPlaylist;
    METACORE_EXPORT extern bool isLevelSelected;
    METACORE_EXPORT extern bool isPlaylistSelected;

    METACORE_EXPORT void SetLevel(GlobalNamespace::BeatmapKey key, GlobalNamespace::BeatmapLevel* level);
    METACORE_EXPORT void ClearLevel();
    METACORE_EXPORT void SetPlaylist(GlobalNamespace::BeatmapLevelPack* playlist);
    METACORE_EXPORT void ClearPlaylist();

    METACORE_EXPORT void SetEndDragUI(UnityEngine::Component* component, std::function<void()> callback);
    METACORE_EXPORT std::function<void()>
    SetKeyboardCloseUI(UnityEngine::Component* component, std::function<void()> onClosed, std::function<void()> onOk);

    METACORE_EXPORT bool IsAprilFirst();
}
