#include "hooking.hpp"
#include "config.hpp"
#include "logging.hpp"

#include "GlobalNamespace/StandardLevelScenesTransitionSetupDataSO.hpp"
#include "GlobalNamespace/MultiplayerLevelScenesTransitionSetupDataSO.hpp"
#include "GlobalNamespace/EnvironmentInfoSO.hpp"
#include "GlobalNamespace/EnvironmentsListModel.hpp"
#include "GlobalNamespace/ColorSchemeSO.hpp"
#include "GlobalNamespace/ColorScheme.hpp"
#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"
#include "GlobalNamespace/RecordingToolManager.hpp"
#include "UnityEngine/Color.hpp"
#include "System/Nullable_1.hpp"
#include "System/Collections/Generic/IReadOnlyList_1.hpp"

#include "SongLoader/CustomBeatmapLevel.hpp"
#include "CustomJSONData.hpp"

/// @brief method to merge the given custom colors into a newly created color scheme, or nothing if all custom color overrides are disabled
GlobalNamespace::ColorScheme* ApplyOverrideColors(GlobalNamespace::ColorScheme* baseColorScheme, SongCore::CustomJSONData::CustomSaveDataInfo::BasicCustomDifficultyBeatmapDetails::CustomColors const& customColors);

// TODO: extract similiar code into seperate method from both these hooks
MAKE_AUTO_HOOK_MATCH(
    StandardLevelScenesTransitionSetupDataSO_Init_1,
    static_cast<
        void
        (GlobalNamespace::StandardLevelScenesTransitionSetupDataSO::*)(
            ::StringW, 
            ::GlobalNamespace::IBeatmapLevelData*,
            ByRef<::GlobalNamespace::BeatmapKey>,
            ::GlobalNamespace::BeatmapLevel*,
            ::GlobalNamespace::OverrideEnvironmentSettings*,
            ::GlobalNamespace::ColorScheme*,
            bool,
            ::GlobalNamespace::ColorScheme*,
            ::GlobalNamespace::GameplayModifiers*,
            ::GlobalNamespace::PlayerSpecificSettings*,
            ::GlobalNamespace::PracticeSettings*,
            ::GlobalNamespace::EnvironmentsListModel*,
            ::GlobalNamespace::AudioClipAsyncLoader*,
            ::GlobalNamespace::SettingsManager*,
            ::GlobalNamespace::BeatmapDataLoader*,
            ::GlobalNamespace::BeatmapLevelsEntitlementModel*,
            ::StringW,
            bool,
            bool,
            ::System::Nullable_1<::GlobalNamespace::RecordingToolManager::SetupData>
    )>(&GlobalNamespace::StandardLevelScenesTransitionSetupDataSO::Init),
    void,
    GlobalNamespace::StandardLevelScenesTransitionSetupDataSO* self,
    ::StringW gameMode, 
    ::GlobalNamespace::IBeatmapLevelData* beatmapLevelData, 
    ByRef<::GlobalNamespace::BeatmapKey> beatmapKey,
    ::GlobalNamespace::BeatmapLevel* beatmapLevel,
    ::GlobalNamespace::OverrideEnvironmentSettings* overrideEnvironmentSettings,
    ::GlobalNamespace::ColorScheme* overrideColorScheme,
    bool playerOverrideLightshowColors,
    ::GlobalNamespace::ColorScheme* beatmapOverrideColorScheme,
    ::GlobalNamespace::GameplayModifiers* gameplayModifiers,
    ::GlobalNamespace::PlayerSpecificSettings* playerSpecificSettings,
    ::GlobalNamespace::PracticeSettings* practiceSettings,
    ::GlobalNamespace::EnvironmentsListModel* environmentsListModel,
    ::GlobalNamespace::AudioClipAsyncLoader* audioClipAsyncLoader,
    ::GlobalNamespace::SettingsManager* settingsManager,
    ::GlobalNamespace::BeatmapDataLoader* beatmapDataLoader,
    ::GlobalNamespace::BeatmapLevelsEntitlementModel* beatmapLevelsEntitlementModel,
    ::StringW backButtonText,
    bool useTestNoteCutSoundEffects,
    bool startPaused,
    ::System::Nullable_1<::GlobalNamespace::RecordingToolManager::SetupData> recordingToolData
) {
    auto customLevel = il2cpp_utils::try_cast<SongCore::SongLoader::CustomBeatmapLevel>(beatmapLevel).value_or(nullptr);
    if (!customLevel) {
        return StandardLevelScenesTransitionSetupDataSO_Init_1(
            self,
            gameMode, 
            beatmapLevelData, 
            beatmapKey,
            beatmapLevel,
            overrideEnvironmentSettings,
            overrideColorScheme,
            playerOverrideLightshowColors,
            beatmapOverrideColorScheme,
            gameplayModifiers,
            playerSpecificSettings,
            practiceSettings,
            environmentsListModel,
            audioClipAsyncLoader,
            settingsManager,
            beatmapDataLoader,
            beatmapLevelsEntitlementModel,
            backButtonText,
            useTestNoteCutSoundEffects,
            startPaused,
            recordingToolData
        );
    }

    auto characteristic = beatmapKey->beatmapCharacteristic;
    auto diff = beatmapKey->difficulty;
    bool containsRotation = characteristic->containsRotationEvents;

    auto customSaveDataInfoOpt = customLevel->CustomSaveDataInfo;
    if (customSaveDataInfoOpt) {
        auto& customSaveDataInfo = customSaveDataInfoOpt->get();
        auto diffDetailsOpt = customSaveDataInfo.TryGetCharacteristicAndDifficulty(characteristic->serializedName, diff);
        if (diffDetailsOpt) {
            auto& diffDetails = diffDetailsOpt->get();
            // map requests rotation events to be enabled or not, so we do that here
            if (diffDetails.environmentType.has_value()) {
                auto& envType = diffDetails.environmentType.value();
                if (envType == "allDirections") {
                    characteristic->_containsRotationEvents = true;
                } else if (envType == "default"){
                    characteristic->_containsRotationEvents = false;
                }
            }
        }
    }

    StandardLevelScenesTransitionSetupDataSO_Init_1(
        self,
        gameMode, 
        beatmapLevelData, 
        beatmapKey,
        beatmapLevel,
        overrideEnvironmentSettings,
        overrideColorScheme,
        playerOverrideLightshowColors,
        beatmapOverrideColorScheme,
        gameplayModifiers,
        playerSpecificSettings,
        practiceSettings,
        environmentsListModel,
        audioClipAsyncLoader,
        settingsManager,
        beatmapDataLoader,
        beatmapLevelsEntitlementModel,
        backButtonText,
        useTestNoteCutSoundEffects,
        startPaused,
        recordingToolData
    );

    characteristic->_containsRotationEvents = containsRotation;
}

MAKE_AUTO_HOOK_MATCH(
    StandardLevelScenesTransitionSetupDataSO_Init_2,
    static_cast<
        void
        (GlobalNamespace::StandardLevelScenesTransitionSetupDataSO::*)(
            ::StringW,
            ByRef<::GlobalNamespace::BeatmapKey>,
            ::GlobalNamespace::BeatmapLevel*,
            ::GlobalNamespace::OverrideEnvironmentSettings*,
            ::GlobalNamespace::ColorScheme*,
            bool playerOverrideLightshowColors,
            ::GlobalNamespace::ColorScheme*,
            ::GlobalNamespace::GameplayModifiers*,
            ::GlobalNamespace::PlayerSpecificSettings*,
            ::GlobalNamespace::PracticeSettings*,
            ::GlobalNamespace::EnvironmentsListModel*,
            ::GlobalNamespace::AudioClipAsyncLoader*,
            ::GlobalNamespace::BeatmapDataLoader*,
            ::GlobalNamespace::SettingsManager*,
            ::StringW,
            ::GlobalNamespace::BeatmapLevelsModel*,
            ::GlobalNamespace::BeatmapLevelsEntitlementModel*,
            bool,
            bool,
            ::System::Nullable_1<::GlobalNamespace::RecordingToolManager::SetupData>
    )>(&GlobalNamespace::StandardLevelScenesTransitionSetupDataSO::Init),
    void,
    GlobalNamespace::StandardLevelScenesTransitionSetupDataSO* self,
    ::StringW gameMode, 
    ByRef<::GlobalNamespace::BeatmapKey> beatmapKey, 
    ::GlobalNamespace::BeatmapLevel* beatmapLevel,
    ::GlobalNamespace::OverrideEnvironmentSettings* overrideEnvironmentSettings, 
    ::GlobalNamespace::ColorScheme* overrideColorScheme,
    bool playerOverrideLightshowColors,
    ::GlobalNamespace::ColorScheme* beatmapOverrideColorScheme, 
    ::GlobalNamespace::GameplayModifiers* gameplayModifiers,
    ::GlobalNamespace::PlayerSpecificSettings* playerSpecificSettings, 
    ::GlobalNamespace::PracticeSettings* practiceSettings,
    ::GlobalNamespace::EnvironmentsListModel* environmentsListModel, 
    ::GlobalNamespace::AudioClipAsyncLoader* audioClipAsyncLoader,
    ::GlobalNamespace::BeatmapDataLoader* beatmapDataLoader, 
    ::GlobalNamespace::SettingsManager* settingsManager,
    ::StringW backButtonText,
    ::GlobalNamespace::BeatmapLevelsModel* beatmapLevelsModel, 
    ::GlobalNamespace::BeatmapLevelsEntitlementModel* beatmapLevelsEntitlementModel,
    bool useTestNoteCutSoundEffects, 
    bool startPaused,
    ::System::Nullable_1<::GlobalNamespace::RecordingToolManager::SetupData> recordingToolData
) {
    auto customLevel = il2cpp_utils::try_cast<SongCore::SongLoader::CustomBeatmapLevel>(beatmapLevel).value_or(nullptr);
    if (!customLevel) {
        return StandardLevelScenesTransitionSetupDataSO_Init_2(
            self,
            gameMode,
            beatmapKey,
            beatmapLevel,
            overrideEnvironmentSettings,
            overrideColorScheme,
            playerOverrideLightshowColors,
            beatmapOverrideColorScheme,
            gameplayModifiers,
            playerSpecificSettings,
            practiceSettings,
            environmentsListModel,
            audioClipAsyncLoader,
            beatmapDataLoader,
            settingsManager,
            backButtonText,
            beatmapLevelsModel,
            beatmapLevelsEntitlementModel,
            useTestNoteCutSoundEffects,
            startPaused,
            recordingToolData
        );
    }

    auto characteristic = beatmapKey->beatmapCharacteristic;
    auto diff = beatmapKey->difficulty;
    bool containsRotation = characteristic->containsRotationEvents;

    auto customSaveDataInfoOpt = customLevel->CustomSaveDataInfo;
    if (customSaveDataInfoOpt) {
        auto& customSaveDataInfo = customSaveDataInfoOpt->get();
        auto diffDetailsOpt = customSaveDataInfo.TryGetCharacteristicAndDifficulty(characteristic->serializedName, diff);
        if (diffDetailsOpt) {
            auto& diffDetails = diffDetailsOpt->get();
            // map requests rotation events to be enabled or not, so we do that here
            if (diffDetails.environmentType.has_value()) {
                auto& envType = diffDetails.environmentType.value();
                if (envType == "allDirections") {
                    characteristic->_containsRotationEvents = true;
                } else if (envType == "default"){
                    characteristic->_containsRotationEvents = false;
                }
            }
        }
    }

    StandardLevelScenesTransitionSetupDataSO_Init_2(
        self,
        gameMode,
        beatmapKey,
        beatmapLevel,
        overrideEnvironmentSettings,
        overrideColorScheme,
        playerOverrideLightshowColors,
        beatmapOverrideColorScheme,
        gameplayModifiers,
        playerSpecificSettings,
        practiceSettings,
        environmentsListModel,
        audioClipAsyncLoader,
        beatmapDataLoader,
        settingsManager,
        backButtonText,
        beatmapLevelsModel,
        beatmapLevelsEntitlementModel,
        useTestNoteCutSoundEffects,
        startPaused,
        recordingToolData
    );

    characteristic->_containsRotationEvents = containsRotation;
}

// Hooks and methods to fix override color scheme stuff

void FixupAndApplyColorScheme(GlobalNamespace::StandardLevelScenesTransitionSetupDataSO* self);
void FixupAndApplyColorScheme(GlobalNamespace::MultiplayerLevelScenesTransitionSetupDataSO* self);
GlobalNamespace::ColorScheme* GetOverrideColorScheme(GlobalNamespace::ColorScheme* baseColorScheme, SongCore::SongLoader::CustomBeatmapLevel* level, GlobalNamespace::BeatmapKey& beatmapKey);

MAKE_AUTO_HOOK_MATCH(
    StandardLevelScenesTransitionSetupDataSO_InitColorInfo,
    &GlobalNamespace::StandardLevelScenesTransitionSetupDataSO::InitColorInfo,
    void,
    GlobalNamespace::StandardLevelScenesTransitionSetupDataSO* self,
    ::GlobalNamespace::ColorScheme* overrideColorScheme,
    bool playerOverrideLightshowColors,
    ::GlobalNamespace::ColorScheme* beatmapOverrideColorScheme
) {
    StandardLevelScenesTransitionSetupDataSO_InitColorInfo(self, overrideColorScheme, playerOverrideLightshowColors, beatmapOverrideColorScheme);
    FixupAndApplyColorScheme(self);
}

MAKE_AUTO_HOOK_MATCH(
    MultiplayerLevelScenesTransitionSetupDataSO_InitColorInfo,
    &GlobalNamespace::MultiplayerLevelScenesTransitionSetupDataSO::InitColorInfo,
    void,
    GlobalNamespace::MultiplayerLevelScenesTransitionSetupDataSO* self,
    ::GlobalNamespace::ColorScheme* overrideColorScheme
) {
    MultiplayerLevelScenesTransitionSetupDataSO_InitColorInfo(self, overrideColorScheme);
    FixupAndApplyColorScheme(self);
}

static bool operator==(UnityEngine::Color lhs, UnityEngine::Color rhs) {
    return !(
        lhs.r != rhs.r &&
        lhs.g != rhs.g &&
        lhs.b != rhs.b &&
        lhs.a != rhs.a
    );
}

#define FIX_BOOST(colortype) if (colorScheme->_##colortype##Boost == DefaultColor) colorScheme->_##colortype##Boost = colorScheme->colortype

void Fixup(GlobalNamespace::ColorScheme* colorScheme) {
    static auto DefaultColor = ::UnityEngine::Color();
    FIX_BOOST(environmentColor0);
    FIX_BOOST(environmentColor1);
    FIX_BOOST(environmentColorW);
}

void FixupAndApplyColorScheme(GlobalNamespace::StandardLevelScenesTransitionSetupDataSO* self) {
    auto level = self->beatmapLevel;
    auto beatmapKey = self->beatmapKey;

    auto colorScheme = self->colorScheme;
    Fixup(colorScheme);

    auto customLevel = il2cpp_utils::try_cast<SongCore::SongLoader::CustomBeatmapLevel>(level).value_or(nullptr);
    if (!customLevel) return;

    auto overrideColorScheme = GetOverrideColorScheme(colorScheme, customLevel, beatmapKey);
    if (overrideColorScheme) {
        self->colorScheme = overrideColorScheme;
        self->usingOverrideColorScheme = true;
    }
}

void FixupAndApplyColorScheme(GlobalNamespace::MultiplayerLevelScenesTransitionSetupDataSO* self) {
    auto level = self->beatmapLevel;
    auto beatmapKey = self->beatmapKey;

    auto colorScheme = self->colorScheme;
    Fixup(colorScheme);

    auto customLevel = il2cpp_utils::try_cast<SongCore::SongLoader::CustomBeatmapLevel>(level).value_or(nullptr);
    if (!customLevel) return;

    auto overrideColorScheme = GetOverrideColorScheme(colorScheme, customLevel, beatmapKey);
    if (overrideColorScheme) {
        self->colorScheme = overrideColorScheme;
        self->usingOverrideColorScheme = true;
    }
}

GlobalNamespace::ColorScheme* GetOverrideColorScheme(GlobalNamespace::ColorScheme* baseColorScheme, SongCore::SongLoader::CustomBeatmapLevel* level, GlobalNamespace::BeatmapKey& beatmapKey) {
    // if we're not allowed to apply any colors, don't do anything
    if (!config.customSongObstacleColors && !config.customSongEnvironmentColors && !config.customSongNoteColors) return nullptr;

    auto customSaveDataInfoOpt = level->CustomSaveDataInfo;
    if (!customSaveDataInfoOpt) return nullptr;
    auto& customSaveDataInfo = customSaveDataInfoOpt->get();

    auto diffDetailsOpt = customSaveDataInfo.TryGetCharacteristicAndDifficulty(beatmapKey.beatmapCharacteristic->serializedName, beatmapKey.difficulty);
    if (!diffDetailsOpt.has_value()) return nullptr;
    auto& diffDetails = diffDetailsOpt->get();

    if (!diffDetails.customColors.has_value()) return nullptr;
    auto& customColors = diffDetails.customColors.value();

    // we just grab all colors by default
    UnityEngine::Color saberAColor = baseColorScheme->saberAColor;
    UnityEngine::Color saberBColor = baseColorScheme->saberBColor;
    UnityEngine::Color environmentColor0 = baseColorScheme->environmentColor0;
    UnityEngine::Color environmentColor1 = baseColorScheme->environmentColor1;
    UnityEngine::Color environmentColorW = baseColorScheme->environmentColorW;
    UnityEngine::Color environmentColor0Boost = baseColorScheme->environmentColor0Boost;
    UnityEngine::Color environmentColor1Boost = baseColorScheme->environmentColor1Boost;
    UnityEngine::Color environmentColorWBoost = baseColorScheme->environmentColorWBoost;
    UnityEngine::Color obstaclesColor = baseColorScheme->obstaclesColor;

    if (config.customSongObstacleColors) {
        obstaclesColor = customColors.obstacleColor.value_or(obstaclesColor);
    }

    if (config.customSongNoteColors) {
        saberAColor = customColors.colorLeft.value_or(saberAColor);
        saberBColor = customColors.colorRight.value_or(saberBColor);
    }

    if (config.customSongEnvironmentColors) {
        environmentColor0 = customColors.envColorLeft.value_or(environmentColor0);
        environmentColor1 = customColors.envColorRight.value_or(environmentColor1);
        environmentColorW = customColors.envColorWhite.value_or(environmentColorW);
        environmentColor0Boost = customColors.envColorLeftBoost.value_or(environmentColor0Boost);
        environmentColor1Boost = customColors.envColorRightBoost.value_or(environmentColor1Boost);
        environmentColorWBoost = customColors.envColorWhiteBoost.value_or(environmentColorWBoost);
    }

    return GlobalNamespace::ColorScheme::New_ctor(
        "SongCoreOverrideColorScheme",
        "SongCoreOverrideColorScheme",
        false,
        "SongCoreOverrideColorScheme",
        false,
        true,
        saberAColor,
        saberBColor,
        true,
        environmentColor0,
        environmentColor1,
        environmentColorW,
        true,
        environmentColor0Boost,
        environmentColor1Boost,
        environmentColorWBoost,
        obstaclesColor
    );
}
