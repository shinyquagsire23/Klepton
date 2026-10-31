#include "internals.hpp"

#include "GlobalNamespace/AudioTimeSyncController.hpp"
#include "GlobalNamespace/BeatmapCallbacksUpdater.hpp"
#include "GlobalNamespace/BeatmapDataSortedListForTypeAndIds_1.hpp"
#include "GlobalNamespace/GameplayCoreInstaller.hpp"
#include "GlobalNamespace/GameplayCoreSceneSetupData.hpp"
#include "GlobalNamespace/GameplayModifierParamsSO.hpp"
#include "GlobalNamespace/GameplayModifiersModelSO.hpp"
#include "GlobalNamespace/IGameEnergyCounter.hpp"
#include "GlobalNamespace/ISortedList_1.hpp"
#include "GlobalNamespace/PlayerAllOverallStatsData.hpp"
#include "GlobalNamespace/PlayerData.hpp"
#include "GlobalNamespace/PlayerDataModel.hpp"
#include "GlobalNamespace/PlayerLevelStatsData.hpp"
#include "GlobalNamespace/Saber.hpp"
#include "GlobalNamespace/ScoreModel.hpp"
#include "System/Collections/Generic/Dictionary_2.hpp"
#include "System/Collections/Generic/LinkedList_1.hpp"
#include "System/Collections/IEnumerator.hpp"
#include "UnityEngine/AudioClip.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Time.hpp"
#include "UnityEngine/Transform.hpp"
#include "delegates.hpp"
#include "events.hpp"
#include "main.hpp"
#include "stats.hpp"
#include "types.hpp"
#include "unity.hpp"

using namespace GlobalNamespace;
using namespace MetaCore;
using namespace UnityEngine;

static std::string lastBeatmap;
static float timeSinceSlowUpdate;

static std::pair<int, int> GetNoteCount(BeatmapCallbacksUpdater* updater, bool left) {
    using LinkedList = System::Collections::Generic::LinkedList_1<NoteData*>;

    if (!updater)
        return {0, 0};

    int noteCount = 0;
    int totalCount = 0;

    auto bcc = updater->_beatmapCallbacksController;
    auto songTime = bcc->_startFilterTime;

    auto data = il2cpp_utils::try_cast<BeatmapData>(bcc->_beatmapData).value_or(nullptr);
    if (!data) {
        logger.warn("IReadonlyBeatmapData was {} not BeatmapData", il2cpp_functions::class_get_name(((Il2CppObject*) bcc->_beatmapData)->klass));
        return {0, 0};
    }

    auto noteDataItemsList = (LinkedList*) data->_beatmapDataItemsPerTypeAndId->GetList(csTypeOf(NoteData*), 0)->items;
    auto enumerator = noteDataItemsList->GetEnumerator();
    while (enumerator.MoveNext()) {
        auto noteData = (NoteData*) enumerator.Current;
        if (Stats::ShouldCountNote(noteData) && (noteData->colorType == ColorType::ColorA) == left) {
            totalCount++;
            if (noteData->time >= songTime)
                noteCount++;
        }
    }
    return {noteCount, totalCount};
}

static int GetMaxScore(BeatmapCallbacksUpdater* updater) {
    if (!updater)
        return 0;
    return ScoreModel::ComputeMaxMultipliedScoreForBeatmap(updater->_beatmapCallbacksController->_beatmapData);
}

static float GetSongLength(ScoreController* controller) {
    if (!controller)
        return 0;
    return controller->_audioTimeSyncController->_initData->audioClip->length;
}

static float GetSongSpeed(ScoreController* controller) {
    if (!controller)
        return 1;
    return controller->_audioTimeSyncController->_initData->timeScale;
}

static int GetFailCount(PlayerDataModel* data) {
    if (!data)
        return 0;
    return data->playerData->playerAllOverallStatsData->get_allOverallStatsData()->failedLevelsCount;
}

static float GetPositiveMods(ScoreController* controller) {
    if (!controller || !controller->_gameplayModifierParams)
        return 0;
    float ret = 0;
    auto mods = ListW<GameplayModifierParamsSO*>(controller->_gameplayModifierParams);
    for (auto& mod : mods) {
        float mult = mod->multiplier;
        if (mult > 0)
            ret += mult;
    }
    return ret;
}

static float GetNegativeMods(ScoreController* controller) {
    if (!controller || !controller->_gameplayModifierParams)
        return 0;
    float ret = 0;
    auto mods = ListW<GameplayModifierParamsSO*>(controller->_gameplayModifierParams);
    for (auto& mod : mods) {
        if (mod == controller->_gameplayModifiersModel->_noFailOn0Energy.ptr()) {
            Internals::noFail = true;
            continue;
        }
        float mult = mod->multiplier;
        if (mult < 0)
            ret += mult;
    }
    return ret;
}

static int GetHighScore(PlayerDataModel* data, GameplayCoreSceneSetupData* setupData) {
    if (!data || !setupData)
        return -1;
    auto beatmap = setupData->beatmapKey;
    if (!data->playerData->levelsStatsData->ContainsKey(beatmap))
        return -1;
    return data->playerData->levelsStatsData->get_Item(beatmap)->highScore;
}

static float GetHealth(ScoreController* controller) {
    if (!controller)
        return 1;
    return controller->_gameEnergyCounter->energy;
}

static GameplayCoreSceneSetupData* FindSceneSetupData() {
    auto gameplayCoreInstallers = Resources::FindObjectsOfTypeAll<GameplayCoreInstaller*>();
    for (auto& installer : gameplayCoreInstallers) {
        if (installer->isActiveAndEnabled && installer->_sceneSetupData != nullptr)
            return installer->_sceneSetupData;
    }
    if (auto installer = gameplayCoreInstallers->FirstOrDefault())
        return installer->_sceneSetupData;
    return nullptr;
}

int Internals::leftScore;
int Internals::rightScore;
int Internals::leftMaxScore;
int Internals::rightMaxScore;
int Internals::songMaxScore;
int Internals::leftCombo;
int Internals::rightCombo;
int Internals::combo;
int Internals::highestLeftCombo;
int Internals::highestRightCombo;
int Internals::highestCombo;
int Internals::multiplier;
int Internals::multiplierProgress;
float Internals::health;
float Internals::songTime;
float Internals::songLength;
float Internals::songSpeed;
int Internals::notesLeftCut;
int Internals::notesRightCut;
int Internals::notesLeftBadCut;
int Internals::notesRightBadCut;
int Internals::notesLeftMissed;
int Internals::notesRightMissed;
int Internals::bombsLeftHit;
int Internals::bombsRightHit;
int Internals::wallsHit;
int Internals::uncountedNotesLeftCut;
int Internals::uncountedNotesRightCut;
int Internals::remainingNotesLeft;
int Internals::remainingNotesRight;
int Internals::songNotesLeft;
int Internals::songNotesRight;
int Internals::leftPreSwing;
int Internals::rightPreSwing;
int Internals::leftPostSwing;
int Internals::rightPostSwing;
int Internals::leftAccuracy;
int Internals::rightAccuracy;
float Internals::leftTimeDependence;
float Internals::rightTimeDependence;
std::vector<float> Internals::leftSpeeds;
std::vector<float> Internals::rightSpeeds;
std::vector<float> Internals::leftAngles;
std::vector<float> Internals::rightAngles;
bool Internals::noFail;
float Internals::positiveMods;
float Internals::negativeMods;
int Internals::personalBest;
int Internals::fails;
int Internals::restarts;
int Internals::leftMissedMaxScore;
int Internals::rightMissedMaxScore;
int Internals::leftMissedFixedScore;
int Internals::rightMissedFixedScore;
Quaternion Internals::prevRotLeft;
Quaternion Internals::prevRotRight;

GameplayModifiers* Internals::modifiers;
ColorScheme* Internals::colors;
BeatmapLevel* Internals::beatmapLevel;
BeatmapKey Internals::beatmapKey;
BeatmapData* Internals::beatmapData;
EnvironmentInfoSO* Internals::environment;
AudioTimeSyncController* Internals::audioTimeSyncController;
BeatmapCallbacksController* Internals::beatmapCallbacksController;
BeatmapObjectManager* Internals::beatmapObjectManager;
ComboController* Internals::comboController;
GameEnergyCounter* Internals::gameEnergyCounter;
ScoreController* Internals::scoreController;
SaberManager* Internals::saberManager;
Camera* Internals::mainCamera;

bool Internals::stateValid = false;
bool Internals::referencesValid = false;
bool Internals::mapWasQuit = false;
bool Internals::mapWasRestarted = false;

void Internals::Initialize() {
    auto beatmapCallbacksUpdater = Object::FindObjectOfType<BeatmapCallbacksUpdater*>(true);
    auto playerDataModel = Object::FindObjectOfType<PlayerDataModel*>(true);

    auto setupData = FindSceneSetupData();

    // out of order since we use it
    scoreController = Object::FindObjectOfType<ScoreController*>(true);

    modifiers = scoreController ? scoreController->_gameplayModifiers : nullptr;

    colors = setupData ? setupData->colorScheme : nullptr;
    beatmapLevel = setupData ? setupData->beatmapLevel : nullptr;
    beatmapKey = setupData ? setupData->beatmapKey : BeatmapKey();
    beatmapData = beatmapCallbacksUpdater ? (BeatmapData*) beatmapCallbacksUpdater->_beatmapCallbacksController->_beatmapData : nullptr;
    environment = setupData ? setupData->targetEnvironmentInfo : nullptr;

    audioTimeSyncController = scoreController ? scoreController->_audioTimeSyncController : nullptr;
    beatmapCallbacksController = beatmapCallbacksUpdater ? beatmapCallbacksUpdater->_beatmapCallbacksController : nullptr;
    beatmapObjectManager = scoreController ? scoreController->_beatmapObjectManager : nullptr;
    comboController = Object::FindObjectOfType<ComboController*>(true);
    gameEnergyCounter = scoreController ? il2cpp_utils::try_cast<GameEnergyCounter>(scoreController->_gameEnergyCounter).value_or(nullptr) : nullptr;
    saberManager = Object::FindObjectOfType<SaberManager*>(true);
    mainCamera = Camera::get_main();

    referencesValid = setupData && scoreController && beatmapCallbacksUpdater && comboController && gameEnergyCounter && saberManager && mainCamera;

    if (!referencesValid)
        logger.critical(
            "Not all expected references were found! Got: {} {} {} {} {} {} {}",
            (bool) setupData,
            (bool) scoreController,
            (bool) beatmapCallbacksUpdater,
            (bool) comboController,
            (bool) gameEnergyCounter,
            (bool) saberManager,
            (bool) mainCamera
        );

    if (scoreController)
        scoreController->add_multiplierDidChangeEvent(Delegates::MakeSystemAction([](int mult, float normalizedProgress) {
            if (!stateValid)
                return;
            multiplier = mult;
            multiplierProgress = normalizedProgress * mult * 2;
        }));

    leftScore = 0;
    rightScore = 0;
    leftMaxScore = 0;
    rightMaxScore = 0;
    songMaxScore = GetMaxScore(beatmapCallbacksUpdater);
    leftCombo = 0;
    rightCombo = 0;
    combo = 0;
    highestLeftCombo = 0;
    highestRightCombo = 0;
    highestCombo = 0;
    multiplier = 1;
    multiplierProgress = 0;
    health = GetHealth(scoreController);
    songTime = 0;
    songLength = GetSongLength(scoreController);
    songSpeed = GetSongSpeed(scoreController);
    notesLeftCut = 0;
    notesRightCut = 0;
    notesLeftBadCut = 0;
    notesRightBadCut = 0;
    notesLeftMissed = 0;
    notesRightMissed = 0;
    bombsLeftHit = 0;
    bombsRightHit = 0;
    wallsHit = 0;
    uncountedNotesLeftCut = 0;
    uncountedNotesRightCut = 0;
    auto pair = GetNoteCount(beatmapCallbacksUpdater, true);
    remainingNotesLeft = pair.first;
    songNotesLeft = pair.second;
    pair = GetNoteCount(beatmapCallbacksUpdater, false);
    remainingNotesRight = pair.first;
    songNotesRight = pair.second;
    leftPreSwing = 0;
    rightPreSwing = 0;
    leftPostSwing = 0;
    rightPostSwing = 0;
    leftAccuracy = 0;
    rightAccuracy = 0;
    leftTimeDependence = 0;
    rightTimeDependence = 0;
    leftSpeeds = {};
    rightSpeeds = {};
    leftAngles = {};
    rightAngles = {};
    noFail = false;
    // GetNegativeMods sets noFail
    positiveMods = GetPositiveMods(scoreController);
    negativeMods = GetNegativeMods(scoreController);
    personalBest = GetHighScore(playerDataModel, setupData);
    fails = GetFailCount(playerDataModel);

    logger.debug("modifiers {} -{}", positiveMods, negativeMods);

    std::string beatmap = "Unknown";
    if (setupData)
        beatmap = (std::string) setupData->beatmapKey.SerializedName();
    if (beatmap != lastBeatmap)
        restarts = 0;
    else
        restarts++;
    lastBeatmap = beatmap;
    timeSinceSlowUpdate = 0;

    leftMissedMaxScore = 0;
    rightMissedMaxScore = 0;
    leftMissedFixedScore = 0;
    rightMissedFixedScore = 0;

    prevRotLeft = Quaternion::get_identity();
    prevRotRight = Quaternion::get_identity();

    stateValid = true;
}

void Internals::DoSlowUpdate() {
    if (!referencesValid)
        return;

    timeSinceSlowUpdate += Time::get_deltaTime();
    if (timeSinceSlowUpdate > 1 / (float) SLOW_UPDATES_PER_SEC) {
        if (saberManager && saberManager->leftSaber && saberManager->rightSaber) {
            leftSpeeds.emplace_back(saberManager->leftSaber->bladeSpeed);
            rightSpeeds.emplace_back(saberManager->rightSaber->bladeSpeed);

            auto rotLeft = saberManager->leftSaber->transform->rotation;
            auto rotRight = saberManager->rightSaber->transform->rotation;
            // use speeds array as tracker for if prevRots have accurate values
            if (leftSpeeds.size() > 1) {
                leftAngles.emplace_back(Quaternion::Angle(rotLeft, prevRotLeft));
                rightAngles.emplace_back(Quaternion::Angle(rotRight, prevRotRight));
            }
            prevRotLeft = rotLeft;
            prevRotRight = rotRight;
        }
        timeSinceSlowUpdate -= 1 / (float) SLOW_UPDATES_PER_SEC;
        Events::Broadcast(Events::SlowUpdate);
    }
}

void Internals::Finish(bool quit, bool restart) {
    stateValid = false;
    referencesValid = false;
    mapWasQuit = quit;
    mapWasRestarted = restart;
}

BeatmapKey Internals::selectedKey = {};
BeatmapLevel* Internals::selectedLevel = nullptr;
BeatmapLevelPack* Internals::selectedPlaylist = nullptr;
bool Internals::isLevelSelected = false;
bool Internals::isPlaylistSelected = false;

void Internals::SetLevel(BeatmapKey key, BeatmapLevel* level) {
    logger.debug("set level {}", level ? level->songName : "none");
    if (!level || !key.IsValid()) {
        ClearLevel();
        return;
    }
    if (isLevelSelected && level == selectedLevel && key.Equals(selectedKey))
        return;
    selectedKey = key;
    selectedLevel = level;
    isLevelSelected = true;
    Events::Broadcast(Events::MapSelected);
}

void Internals::ClearLevel() {
    logger.debug("clear level");
    isLevelSelected = false;
    Events::Broadcast(Events::MapDeselected);
}

void Internals::SetPlaylist(BeatmapLevelPack* playlist) {
    logger.debug("set playlist {}", playlist ? playlist->packName : "none");
    if (!playlist) {
        ClearLevel();
        return;
    }
    if (isPlaylistSelected && playlist == selectedPlaylist)
        return;
    selectedPlaylist = playlist;
    isPlaylistSelected = true;
    Events::Broadcast(Events::PlaylistSelected);
}

void Internals::ClearPlaylist() {
    logger.debug("clear playlist");
    isPlaylistSelected = false;
    Events::Broadcast(Events::PlaylistDeselected);
}

void Internals::SetEndDragUI(Component* component, std::function<void()> callback) {
    Engine::GetOrAddComponent<EndDragHandler*>(component)->callback = std::move(callback);
}

std::function<void()> Internals::SetKeyboardCloseUI(Component* component, std::function<void()> onClosed, std::function<void()> onOk) {
    auto handler = Engine::GetOrAddComponent<KeyboardCloseHandler*>(component);
    handler->closeCallback = std::move(onClosed);
    handler->okCallback = std::move(onOk);
    return [handler]() {
        if (handler->okCallback)
            handler->okCallback();
        else if (handler->closeCallback)
            handler->closeCallback();
    };
}

bool Internals::IsAprilFirst() {
    auto time = std::time(nullptr);
    auto info = std::localtime(&time);
    if (!info)
        return false;
    return info->tm_mon == 3 && info->tm_mday == 1;
}
