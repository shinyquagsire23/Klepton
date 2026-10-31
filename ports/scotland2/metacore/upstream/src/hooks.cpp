#include "hooks.hpp"

#include "GlobalNamespace/AnnotatedBeatmapLevelCollectionsViewController.hpp"
#include "GlobalNamespace/AudioTimeSyncController.hpp"
#include "GlobalNamespace/BeatmapObjectExecutionRatingsRecorder.hpp"
#include "GlobalNamespace/BeatmapObjectManager.hpp"
#include "GlobalNamespace/CutScoreBuffer.hpp"
#include "GlobalNamespace/FadeInOutController.hpp"
#include "GlobalNamespace/GameEnergyCounter.hpp"
#include "GlobalNamespace/GameScenesManager.hpp"
#include "GlobalNamespace/LevelCompletionResults.hpp"
#include "GlobalNamespace/LevelCompletionResultsHelper.hpp"
#include "GlobalNamespace/MenuTransitionsHelper.hpp"
#include "GlobalNamespace/MissionCompletionResults.hpp"
#include "GlobalNamespace/MissionDataSO.hpp"
#include "GlobalNamespace/MissionLevelDetailViewController.hpp"
#include "GlobalNamespace/MissionNode.hpp"
#include "GlobalNamespace/MultiplayerLocalActivePlayerInGameMenuController.hpp"
#include "GlobalNamespace/NoteController.hpp"
#include "GlobalNamespace/NoteCutInfo.hpp"
#include "GlobalNamespace/OVRInput.hpp"
#include "GlobalNamespace/OculusAdvancedHapticFeedbackPlayer.hpp"
#include "GlobalNamespace/OculusVRHelper.hpp"
#include "GlobalNamespace/PartyFreePlayFlowCoordinator.hpp"
#include "GlobalNamespace/PauseMenuManager.hpp"
#include "GlobalNamespace/ScoreController.hpp"
#include "GlobalNamespace/ScoreModel.hpp"
#include "GlobalNamespace/ScoreMultiplierCounter.hpp"
#include "GlobalNamespace/ScoringElement.hpp"
#include "GlobalNamespace/StandardLevelDetailView.hpp"
#include "GlobalNamespace/UIKeyboardManager.hpp"
#include "HMUI/IconSegmentedControl.hpp"
#include "HMUI/IconSegmentedControlCell.hpp"
#include "HMUI/InputFieldView.hpp"
#include "System/Action.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Time.hpp"
#include "UnityEngine/WaitForSeconds.hpp"
#include "VRUIControls/VRGraphicRaycaster.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "custom-types/shared/coroutine.hpp"
#include "events.hpp"
#include "game.hpp"
#include "input.hpp"
#include "internals.hpp"
#include "main.hpp"
#include "songs.hpp"
#include "stats.hpp"
#include "types.hpp"
#include "unity.hpp"

using namespace GlobalNamespace;
using namespace MetaCore;

static bool inGameplayScene = false;

static std::set<int> pressedButtons;

static bool IsGameplayScene(UnityW<ScenesTransitionSetupDataSO> scene) {
    return scene && scene.try_cast<LevelScenesTransitionSetupDataSO>();
}

static void CheckInitialize(UnityW<ScenesTransitionSetupDataSO> scene) {
    if (!IsGameplayScene(scene))
        return;
    logger.debug("gameplay scene start");
    Internals::Initialize();
    Events::Broadcast(Events::GameplaySceneStarted);
    inGameplayScene = true;
}

static void CheckEarlyFinish(LevelCompletionResults::LevelEndAction action) {
    if (!inGameplayScene)
        return;
    Internals::Finish(action == LevelCompletionResults::LevelEndAction::Quit, action == LevelCompletionResults::LevelEndAction::Restart);
    if (Internals::mapWasRestarted)
        Events::Broadcast(Events::MapRestarted);
    else
        Events::Broadcast(Events::MapEnded);
}

static void CheckSceneFinish() {
    if (!inGameplayScene)
        return;
    logger.debug("gameplay scene finish");
    Events::Broadcast(Events::GameplaySceneEnded);
    inGameplayScene = false;
}

// update score and max score
MAKE_AUTO_HOOK_MATCH(
    ScoreController_DespawnScoringElement, &ScoreController::DespawnScoringElement, void, ScoreController* self, ScoringElement* scoringElement
) {
    ScoreController_DespawnScoringElement(self, scoringElement);

    int cutScore = scoringElement->cutScore * scoringElement->multiplier;
    int maxCutScore = scoringElement->maxPossibleCutScore * scoringElement->maxMultiplier;

    bool badCut = scoringElement->multiplierEventType == ScoreMultiplierCounter::MultiplierEventType::Negative &&
                  scoringElement->wouldBeCorrectCutBestPossibleMultiplierEventType == ScoreMultiplierCounter::MultiplierEventType::Positive &&
                  cutScore == 0 && maxCutScore > 0;

    // NoteScoreDefinition fixedCutScore, for now only this case
    bool isGoodScoreFixed = scoringElement->noteData->gameplayType == NoteData::GameplayType::BurstSliderElement;

    if (scoringElement->noteData->colorType == ColorType::ColorA) {
        Internals::leftScore += cutScore;
        Internals::leftMaxScore += maxCutScore;
        if (badCut) {
            if (isGoodScoreFixed)
                Internals::leftMissedFixedScore += maxCutScore;
            else
                Internals::leftMissedMaxScore += maxCutScore;
        } else
            Internals::leftMissedFixedScore += (scoringElement->cutScore * scoringElement->maxMultiplier) - cutScore;
    } else {
        Internals::rightScore += cutScore;
        Internals::rightMaxScore += maxCutScore;
        if (badCut) {
            if (isGoodScoreFixed)
                Internals::rightMissedFixedScore += maxCutScore;
            else
                Internals::rightMissedMaxScore += maxCutScore;
        } else
            Internals::rightMissedFixedScore += (scoringElement->cutScore * scoringElement->maxMultiplier) - cutScore;
    }
    Events::Broadcast(Events::ScoreChanged);
}

// update combo and good/bad cuts
MAKE_AUTO_HOOK_MATCH(
    BeatmapObjectManager_HandleNoteControllerNoteWasCut,
    &BeatmapObjectManager::HandleNoteControllerNoteWasCut,
    void,
    BeatmapObjectManager* self,
    NoteController* noteController,
    ByRef<NoteCutInfo> info
) {
    BeatmapObjectManager_HandleNoteControllerNoteWasCut(self, noteController, info);

    bool left = info->saberType == SaberType::SaberA;
    bool bomb = noteController->noteData->gameplayType == NoteData::GameplayType::Bomb;
    if (!bomb && Stats::IsFakeNote(noteController->noteData))
        return;

    if (!bomb && Stats::ShouldCountNote(noteController->noteData)) {
        if (left)
            Internals::remainingNotesLeft--;
        else
            Internals::remainingNotesRight--;
    }

    if (info->allIsOK) {
        if (++Internals::combo > Internals::highestCombo)
            Internals::highestCombo = Internals::combo;
        if (left) {
            if (++Internals::leftCombo > Internals::highestLeftCombo)
                Internals::highestLeftCombo = Internals::leftCombo;
        } else {
            if (++Internals::rightCombo > Internals::highestRightCombo)
                Internals::highestRightCombo = Internals::rightCombo;
        }
    } else {
        Internals::combo = 0;
        if (left) {
            if (bomb)
                Internals::bombsLeftHit++;
            else
                Internals::notesLeftBadCut++;
            Internals::leftCombo = 0;
        } else {
            if (bomb)
                Internals::bombsRightHit++;
            else
                Internals::notesRightBadCut++;
            Internals::rightCombo = 0;
        }
        if (bomb)
            Events::Broadcast(Events::BombCut);
        else
            Events::Broadcast(Events::NoteCut);
    }
    Events::Broadcast(Events::ComboChanged);
}

// update combo and misses
MAKE_AUTO_HOOK_MATCH(
    BeatmapObjectManager_HandleNoteControllerNoteWasMissed,
    &BeatmapObjectManager::HandleNoteControllerNoteWasMissed,
    void,
    BeatmapObjectManager* self,
    NoteController* noteController
) {
    BeatmapObjectManager_HandleNoteControllerNoteWasMissed(self, noteController);

    if (noteController->noteData->gameplayType == NoteData::GameplayType::Bomb || Stats::IsFakeNote(noteController->noteData))
        return;

    Internals::combo = 0;
    if (noteController->noteData->colorType == ColorType::ColorA) {
        Internals::leftCombo = 0;
        Internals::notesLeftMissed++;
        if (Stats::ShouldCountNote(noteController->noteData))
            Internals::remainingNotesLeft--;
    } else {
        Internals::rightCombo = 0;
        Internals::notesRightMissed++;
        if (Stats::ShouldCountNote(noteController->noteData))
            Internals::remainingNotesRight--;
    }
    Events::Broadcast(Events::NoteMissed);
    Events::Broadcast(Events::ComboChanged);
}

// update swing statistics
static void HandleCutFinish(CutScoreBuffer* buffer) {
    if (!buffer->noteCutInfo.allIsOK)
        return;
    if (Stats::ShouldCountNote(buffer->noteCutInfo.noteData)) {
        int after = buffer->afterCutScore;
        if (buffer->noteScoreDefinition->maxAfterCutScore == 0)  // TODO: selectively exclude from averages?
            after = 30;
        if (buffer->noteCutInfo.saberType == SaberType::SaberA) {
            Internals::notesLeftCut++;
            Internals::leftPreSwing += buffer->beforeCutScore;
            Internals::leftPostSwing += after;
            Internals::leftAccuracy += buffer->centerDistanceCutScore;
            Internals::leftTimeDependence += std::abs(buffer->noteCutInfo.cutNormal.z);
        } else {
            Internals::notesRightCut++;
            Internals::rightPreSwing += buffer->beforeCutScore;
            Internals::rightPostSwing += after;
            Internals::rightAccuracy += buffer->centerDistanceCutScore;
            Internals::rightTimeDependence += std::abs(buffer->noteCutInfo.cutNormal.z);
        }
        Events::Broadcast(Events::NoteCut);
    } else if (!Stats::IsFakeNote(buffer->noteCutInfo.noteData)) {
        if (buffer->noteCutInfo.saberType == SaberType::SaberA)
            Internals::uncountedNotesLeftCut++;
        else
            Internals::uncountedNotesRightCut++;
        Events::Broadcast(Events::NoteCut);
    }
}

MAKE_AUTO_HOOK_MATCH(
    CutScoreBuffer_HandleSaberSwingRatingCounterDidFinish,
    &CutScoreBuffer::HandleSaberSwingRatingCounterDidFinish,
    void,
    CutScoreBuffer* self,
    ISaberSwingRatingCounter* swingRatingCounter
) {
    CutScoreBuffer_HandleSaberSwingRatingCounterDidFinish(self, swingRatingCounter);
    HandleCutFinish(self);
}

MAKE_AUTO_HOOK_MATCH(CutScoreBuffer_Init, &CutScoreBuffer::Init, bool, CutScoreBuffer* self, ByRef<NoteCutInfo> noteCutInfo) {
    bool notYetFinished = CutScoreBuffer_Init(self, noteCutInfo);
    if (!notYetFinished)
        HandleCutFinish(self);
    return notYetFinished;
}

// update combo and walls hit
MAKE_AUTO_HOOK_MATCH(
    BeatmapObjectExecutionRatingsRecorder_HandlePlayerHeadDidEnterObstacle,
    &BeatmapObjectExecutionRatingsRecorder::HandlePlayerHeadDidEnterObstacle,
    void,
    BeatmapObjectExecutionRatingsRecorder* self,
    ObstacleController* obstacleController
) {
    BeatmapObjectExecutionRatingsRecorder_HandlePlayerHeadDidEnterObstacle(self, obstacleController);

    Internals::wallsHit++;
    Internals::combo = 0;
    Events::Broadcast(Events::WallHit);
    Events::Broadcast(Events::ComboChanged);
}

// update health and no fail
MAKE_AUTO_HOOK_MATCH(
    GameEnergyCounter_ProcessEnergyChange, &GameEnergyCounter::ProcessEnergyChange, void, GameEnergyCounter* self, float energyChange
) {
    bool wasAbove0 = !self->_didReach0Energy;

    GameEnergyCounter_ProcessEnergyChange(self, energyChange);

    if (Internals::noFail && wasAbove0 && self->_didReach0Energy) {
        Internals::negativeMods -= 0.5;
        Events::Broadcast(Events::ScoreChanged);
    }
    Internals::health = self->energy;
    Events::Broadcast(Events::HealthChanged);
}

// initialize as soon as the scene is loaded
MAKE_AUTO_HOOK_MATCH(
    GameScenesManager_PushScenes_Delegate,
    &GameScenesManager::__c__DisplayClass40_0::_PushScenes_b__1,
    void,
    GameScenesManager::__c__DisplayClass40_0* self,
    Zenject::DiContainer* container
) {
    CheckInitialize(self->scenesTransitionSetupData);

    GameScenesManager_PushScenes_Delegate(self, container);
}

// initialize on level restarts as well
MAKE_AUTO_HOOK_MATCH(
    GameScenesManager_ReplaceScenes_Delegate_AfterLoad,
    &GameScenesManager::__c__DisplayClass42_0::_ReplaceScenes_b__2,
    void,
    GameScenesManager::__c__DisplayClass42_0* self,
    Zenject::DiContainer* container
) {
    CheckInitialize(self->scenesTransitionSetupData);

    GameScenesManager_ReplaceScenes_Delegate_AfterLoad(self, container);
}

// broadcast level start
MAKE_AUTO_HOOK_MATCH(
    AudioTimeSyncController_StartSong, &AudioTimeSyncController::StartSong, void, AudioTimeSyncController* self, float startTimeOffset
) {
    logger.info("level start");
    Events::Broadcast(Events::MapStarted);

    AudioTimeSyncController_StartSong(self, startTimeOffset);
}

// update song time and run update events
MAKE_AUTO_HOOK_MATCH(AudioTimeSyncController_Update, &AudioTimeSyncController::Update, void, AudioTimeSyncController* self) {

    AudioTimeSyncController_Update(self);

    if (!Internals::stateValid)
        return;

    Internals::DoSlowUpdate();
    Internals::songTime = self->songTime;
    Events::Broadcast(Events::Update);
}

// run pause event
MAKE_AUTO_HOOK_MATCH(PauseMenuManager_ShowMenu, &PauseMenuManager::ShowMenu, void, PauseMenuManager* self) {

    PauseMenuManager_ShowMenu(self);
    Events::Broadcast(Events::MapPaused);
}

// run unpause event
MAKE_AUTO_HOOK_MATCH(
    PauseMenuManager_HandleResumeFromPauseAnimationDidFinish, &PauseMenuManager::HandleResumeFromPauseAnimationDidFinish, void, PauseMenuManager* self
) {
    PauseMenuManager_HandleResumeFromPauseAnimationDidFinish(self);
    Events::Broadcast(Events::MapUnpaused);
}

// run pause event in multiplayer
MAKE_AUTO_HOOK_MATCH(
    MultiplayerLocalActivePlayerInGameMenuController_ShowInGameMenu,
    &MultiplayerLocalActivePlayerInGameMenuController::ShowInGameMenu,
    void,
    MultiplayerLocalActivePlayerInGameMenuController* self
) {
    MultiplayerLocalActivePlayerInGameMenuController_ShowInGameMenu(self);
    Events::Broadcast(Events::MapPaused);
}

// run unpause event in multiplayer
MAKE_AUTO_HOOK_MATCH(
    MultiplayerLocalActivePlayerInGameMenuController_HideInGameMenu,
    &MultiplayerLocalActivePlayerInGameMenuController::HideInGameMenu,
    void,
    MultiplayerLocalActivePlayerInGameMenuController* self
) {
    MultiplayerLocalActivePlayerInGameMenuController_HideInGameMenu(self);
    Events::Broadcast(Events::MapUnpaused);
}

// handle level end and restart
MAKE_AUTO_HOOK_MATCH(
    MenuTransitionsHelper_HandleMainGameSceneDidFinish,
    &MenuTransitionsHelper::HandleMainGameSceneDidFinish,
    void,
    MenuTransitionsHelper* self,
    StandardLevelScenesTransitionSetupDataSO* standardLevelScenesTransitionSetupData,
    LevelCompletionResults* levelCompletionResults
) {
    logger.info("standard level end {}", (int) levelCompletionResults->levelEndAction);
    CheckEarlyFinish(levelCompletionResults->levelEndAction);

    MenuTransitionsHelper_HandleMainGameSceneDidFinish(self, standardLevelScenesTransitionSetupData, levelCompletionResults);
}

// handle campaign level end and restart
MAKE_AUTO_HOOK_MATCH(
    MenuTransitionsHelper_HandleMissionLevelSceneDidFinish,
    &MenuTransitionsHelper::HandleMissionLevelSceneDidFinish,
    void,
    MenuTransitionsHelper* self,
    MissionLevelScenesTransitionSetupDataSO* missionLevelScenesTransitionSetupData,
    MissionCompletionResults* missionCompletionResults
) {
    logger.info("campaign level end {}", (int) missionCompletionResults->levelCompletionResults->levelEndAction);
    CheckEarlyFinish(missionCompletionResults->levelCompletionResults->levelEndAction);

    MenuTransitionsHelper_HandleMissionLevelSceneDidFinish(self, missionLevelScenesTransitionSetupData, missionCompletionResults);
}

// handle multiplayer level end
MAKE_AUTO_HOOK_MATCH(
    MenuTransitionsHelper_HandleMultiplayerLevelDidFinish,
    &MenuTransitionsHelper::HandleMultiplayerLevelDidFinish,
    void,
    MenuTransitionsHelper* self,
    MultiplayerLevelScenesTransitionSetupDataSO* multiplayerLevelScenesTransitionSetupData,
    MultiplayerResultsData* multiplayerResultsData
) {
    logger.info("multiplayer level end");
    CheckEarlyFinish(LevelCompletionResults::LevelEndAction::None);

    MenuTransitionsHelper_HandleMultiplayerLevelDidFinish(self, multiplayerLevelScenesTransitionSetupData, multiplayerResultsData);
}

// handle multiplayer level end by disconnect
MAKE_AUTO_HOOK_MATCH(
    MenuTransitionsHelper_HandleMultiplayerLevelDidDisconnect,
    &MenuTransitionsHelper::HandleMultiplayerLevelDidDisconnect,
    void,
    MenuTransitionsHelper* self,
    MultiplayerLevelScenesTransitionSetupDataSO* multiplayerLevelScenesTransitionSetupData,
    DisconnectedReason disconnectedReason
) {
    logger.info("multiplayer level disconnect");
    CheckEarlyFinish(LevelCompletionResults::LevelEndAction::Quit);

    MenuTransitionsHelper_HandleMultiplayerLevelDidDisconnect(self, multiplayerLevelScenesTransitionSetupData, disconnectedReason);
}

// track when gameplay scenes are removed
MAKE_AUTO_HOOK_MATCH(
    GameScenesManager_PopScenes_Delegate,
    &GameScenesManager::__c__DisplayClass41_0::_PopScenes_b__0,
    void,
    GameScenesManager::__c__DisplayClass41_0* self,
    Zenject::DiContainer* container
) {
    CheckSceneFinish();

    GameScenesManager_PopScenes_Delegate(self, container);
}

// track when gameplay scenes are replaced for level restarts
MAKE_AUTO_HOOK_MATCH(
    GameScenesManager_ReplaceScenes_Delegate_AfterUnload,
    &GameScenesManager::__c__DisplayClass42_0::_ReplaceScenes_b__0,
    void,
    GameScenesManager::__c__DisplayClass42_0* self,
    Zenject::DiContainer* container
) {
    CheckSceneFinish();

    GameScenesManager_ReplaceScenes_Delegate_AfterUnload(self, container);
}

// handle soft restart
MAKE_AUTO_HOOK_MATCH(
    MenuTransitionsHelper_RestartGame,
    &MenuTransitionsHelper::RestartGame,
    void,
    MenuTransitionsHelper* self,
    System::Action_1<Zenject::DiContainer*>* finishCallback
) {
    logger.info("soft restart");
    Events::Broadcast(Events::SoftRestart);

    MenuTransitionsHelper_RestartGame(self, finishCallback);
}

// prevent score submission in solo and multiplayer mode
MAKE_AUTO_HOOK_MATCH(
    LevelCompletionResultsHelper_ProcessScore,
    &LevelCompletionResultsHelper::ProcessScore,
    void,
    ByRef<BeatmapKey> beatmapKey,
    PlayerData* playerData,
    PlayerLevelStatsData* playerLevelStats,
    LevelCompletionResults* levelCompletionResults,
    IReadonlyBeatmapData* transformedBeatmapData,
    PlatformLeaderboardsModel* platformLeaderboardsModel
) {
    if (!Game::IsScoreSubmissionDisabled())
        LevelCompletionResultsHelper_ProcessScore(
            beatmapKey, playerData, playerLevelStats, levelCompletionResults, transformedBeatmapData, platformLeaderboardsModel
        );
    else
        logger.info("disabling submission of score");
}

// prevent score submission in party mode
MAKE_AUTO_HOOK_MATCH(
    PartyFreePlayFlowCoordinator_WillScoreGoToLeaderboard,
    &PartyFreePlayFlowCoordinator::WillScoreGoToLeaderboard,
    bool,
    PartyFreePlayFlowCoordinator* self,
    LevelCompletionResults* levelCompletionResults,
    StringW leaderboardId,
    bool practice
) {
    bool ret = PartyFreePlayFlowCoordinator_WillScoreGoToLeaderboard(self, levelCompletionResults, leaderboardId, practice);
    if (Game::IsScoreSubmissionDisabled()) {
        logger.info("disabling submission of score");
        return false;
    }
    return ret;
}

static void AddSignalUpdates(UnityEngine::Component* self, std::function<void()> disable, std::function<void()> enable) {
    auto signal = Engine::GetOrAddComponent<ObjectSignal*>(self);
    signal->onDisable = disable;
    // although having onEnable will make for duplicate updates most of the time,
    // it's needed if a submenu such as replay, playlist manager, etc is opened (for when it goes back to the level selection)
    signal->onEnable = enable;
    signal->onEnable();
}

// track level selection
MAKE_AUTO_HOOK_MATCH(
    StandardLevelDetailView_SetContentForBeatmapData, &StandardLevelDetailView::SetContentForBeatmapData, void, StandardLevelDetailView* self
) {
    AddSignalUpdates(self, Internals::ClearLevel, [self]() { Internals::SetLevel(self->beatmapKey, self->_beatmapLevel); });

    StandardLevelDetailView_SetContentForBeatmapData(self);
}

// track level selection in campaign
MAKE_AUTO_HOOK_MATCH(
    MissionLevelDetailViewController_RefreshContent, &MissionLevelDetailViewController::RefreshContent, void, MissionLevelDetailViewController* self
) {
    AddSignalUpdates(self, Internals::ClearLevel, [key = self->missionNode->missionData->beatmapKey]() {
        Internals::SetLevel(key, Songs::FindLevel(key));
    });

    MissionLevelDetailViewController_RefreshContent(self);
}

// track playlist selection
MAKE_AUTO_HOOK_MATCH(
    AnnotatedBeatmapLevelCollectionsViewController_HandleDidSelectAnnotatedBeatmapLevelCollection,
    &AnnotatedBeatmapLevelCollectionsViewController::HandleDidSelectAnnotatedBeatmapLevelCollection,
    void,
    AnnotatedBeatmapLevelCollectionsViewController* self,
    BeatmapLevelPack* pack
) {
    AnnotatedBeatmapLevelCollectionsViewController_HandleDidSelectAnnotatedBeatmapLevelCollection(self, pack);

    AddSignalUpdates(self, Internals::ClearPlaylist, [pack]() { Internals::SetPlaylist(pack); });
}

// track initial playlist selection
MAKE_AUTO_HOOK_MATCH(
    AnnotatedBeatmapLevelCollectionsViewController_SetData,
    &AnnotatedBeatmapLevelCollectionsViewController::SetData,
    void,
    AnnotatedBeatmapLevelCollectionsViewController* self,
    System::Collections::Generic::IReadOnlyList_1<BeatmapLevelPack*>* packs,
    int selectedItemIndex,
    bool hideIfOneOrNoPacks
) {
    AnnotatedBeatmapLevelCollectionsViewController_SetData(self, packs, selectedItemIndex, hideIfOneOrNoPacks);

    AddSignalUpdates(self, Internals::ClearPlaylist, [pack = packs->get_Item(selectedItemIndex)]() { Internals::SetPlaylist(pack); });
}

// run input button events
MAKE_AUTO_HOOK_MATCH(OVRInput_Update, &OVRInput::Update, void) {
    OVRInput_Update();

    for (int i = 0; i <= Input::ButtonsMax; i++) {
        bool pressed = Input::GetPressed(Input::Either, (Input::Buttons) i);
        bool wasPressed = pressedButtons.contains(i);
        if (pressed && !wasPressed) {
            Events::Broadcast(Input::PressEvents, i);
            pressedButtons.emplace(i);
        }
        if (pressed)
            Events::Broadcast(Input::HoldEvents, i);
        if (!pressed && wasPressed) {
            Events::Broadcast(Input::ReleaseEvents, i);
            pressedButtons.erase(i);
        }
    }
}

// run keyboard closed callbacks
MAKE_AUTO_HOOK_MATCH(
    InputFieldView_DeactivateKeyboard, &HMUI::InputFieldView::DeactivateKeyboard, void, HMUI::InputFieldView* self, HMUI::UIKeyboard* keyboard
) {
    InputFieldView_DeactivateKeyboard(self, keyboard);

    auto handler = self->GetComponent<KeyboardCloseHandler*>();
    if (handler && handler->closeCallback)
        handler->closeCallback();
}

// run keyboard ok button callbacks
MAKE_AUTO_HOOK_MATCH(UIKeyboardManager_HandleKeyboardOkButton, &UIKeyboardManager::HandleKeyboardOkButton, void, UIKeyboardManager* self) {

    auto handler = self->_selectedInput->GetComponent<KeyboardCloseHandler*>();
    if (handler && handler->okCallback)
        handler->okCallback();

    UIKeyboardManager_HandleKeyboardOkButton(self);
}

// fix non interactable IconSegmentedControl cells
MAKE_AUTO_HOOK_MATCH(
    IconSegmentedControl_CellForCellNumber,
    &HMUI::IconSegmentedControl::CellForCellNumber,
    UnityW<HMUI::SegmentedControlCell>,
    HMUI::IconSegmentedControl* self,
    int cellNumber
) {
    auto cell = IconSegmentedControl_CellForCellNumber(self, cellNumber);

    if (!cell->interactable) {
        cell->enabled = false;
        if (auto cast = cell.try_cast<HMUI::IconSegmentedControlCell>().value_or(nullptr))
            cast->hideBackgroundImage = true;
    } else
        cell->enabled = true;

    return cell;
}

static custom_types::Helpers::Coroutine DelayCallback(System::Action* callback, float duration) {
    co_yield (System::Collections::IEnumerator*) UnityEngine::WaitForSeconds::New_ctor(duration);
    callback->Invoke();
    co_return;
}

// redirect fade in through our system
MAKE_AUTO_ORIG_HOOK_MATCH(
    FadeInOutController_FadeIn,
    static_cast<void (FadeInOutController::*)(float, System::Action*)>(&FadeInOutController::FadeIn),
    void,
    FadeInOutController* self,
    float duration,
    System::Action* finishedCallback
) {
    Game::SetCameraFadeOut(BASE_GAME_ID, false, duration);
    if (!finishedCallback)
        return;
    if (duration == 0)
        finishedCallback->Invoke();
    else
        self->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(DelayCallback(finishedCallback, duration + self->_fadeInStartDelay)));
}

// redirect fade out through our system
MAKE_AUTO_ORIG_HOOK_MATCH(
    FadeInOutController_FadeOut,
    static_cast<void (FadeInOutController::*)(float, System::Action*)>(&FadeInOutController::FadeOut),
    void,
    FadeInOutController* self,
    float duration,
    System::Action* finishedCallback
) {
    Game::SetCameraFadeOut(BASE_GAME_ID, true, duration);
    if (!finishedCallback)
        return;
    if (duration == 0)
        finishedCallback->Invoke();
    else
        self->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(DelayCallback(finishedCallback, duration)));
}

// disable haptics if requested
MAKE_AUTO_HOOK_MATCH(
    OculusVRHelper_TriggerHapticPulse,
    &OculusVRHelper::TriggerHapticPulse,
    void,
    OculusVRHelper* self,
    UnityEngine::XR::XRNode node,
    float duration,
    float strength,
    float frequency
) {
    if (!Input::IsHapticsDisabled())
        OculusVRHelper_TriggerHapticPulse(self, node, duration, strength, frequency);
}

MAKE_AUTO_HOOK_MATCH(
    OculusAdvancedHapticFeedbackPlayer_PlayHapticFeedback,
    &OculusAdvancedHapticFeedbackPlayer::PlayHapticFeedback,
    void,
    OculusAdvancedHapticFeedbackPlayer* self,
    UnityEngine::XR::XRNode node,
    Libraries::HM::HMLib::VR::HapticPresetSO* hapticPreset
) {
    if (!Input::IsHapticsDisabled())
        OculusAdvancedHapticFeedbackPlayer_PlayHapticFeedback(self, node, hapticPreset);
}

// hook abort and provide backtraces
MAKE_HOOK(Abort, nullptr, void) {
    auto logger = Paper::ConstLoggerContext("abort_hook");
    logger.info("abort called");
    logger.Backtrace(40);

    Abort();
}

AUTO_INSTALL_FUNCTION(Abort) {
    auto libc = dlopen("libc.so", RTLD_NOW);
    auto abort_address = dlsym(libc, "abort");
    INSTALL_HOOK_DIRECT(logger, Abort, abort_address);
}

// hook abort and provide backtraces
MAKE_HOOK(delete_object_internal_step1, nullptr, void, char* object) {
#ifdef SC2_METACORE_DELETE_OFFSET
    // The measured entry in this Unity build accepts null before its ID read.
    if (!object) { delete_object_internal_step1(object); return; }
#endif
    int instanceId = *(int*) (object + 8);
    auto destroy = ObjectSignal::onDestroys.find(instanceId);
    if (destroy != ObjectSignal::onDestroys.end() && destroy->second) {
#if defined(SC2_METACORE_DELETE_OFFSET) && SC2_DIAGNOSTICS
        logger.info("Native object destruction callback instanceId={}", instanceId);
#endif
        destroy->second();
        ObjectSignal::onDestroys.erase(destroy);
    }
    delete_object_internal_step1(object);
}

AUTO_INSTALL_FUNCTION(delete_object_internal_step1) {
#ifdef SC2_METACORE_DELETE_OFFSET
    // Supplied Unity image has a different native layout; the build validates
    // this measured deletion entry and its callsites before preparing gates.
    uintptr_t addr = baseAddr("libunity.so") + SC2_METACORE_DELETE_OFFSET;
#else
    uintptr_t addr = baseAddr("libunity.so") + 0x8d2898;
#endif
    INSTALL_HOOK_DIRECT(logger, delete_object_internal_step1, (void*) addr);
}
