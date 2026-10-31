#include "main.hpp"
#ifdef SC2_AOT_HOOKS
#include "safe-save.hpp"
#endif

#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"
#include "GlobalNamespace/ColorSchemesSettings.hpp"
#include "GlobalNamespace/EnvironmentInfoSO.hpp"
#include "GlobalNamespace/EnvironmentType.hpp"
#include "GlobalNamespace/EnvironmentsListModel.hpp"
#include "GlobalNamespace/GameplayModifiers.hpp"
#include "GlobalNamespace/MultiplayerModeSettings.hpp"
#include "GlobalNamespace/OverrideEnvironmentSettings.hpp"
#include "GlobalNamespace/PlayerAgreements.hpp"
#include "GlobalNamespace/PlayerAllOverallStatsData.hpp"
#include "GlobalNamespace/PlayerData.hpp"
#include "GlobalNamespace/PlayerDataFileModel.hpp"
#include "GlobalNamespace/PlayerLevelStatsData.hpp"
#include "GlobalNamespace/PlayerMissionStatsData.hpp"
#include "GlobalNamespace/PlayerSaveData.hpp"
#include "GlobalNamespace/PlayerSpecificSettings.hpp"
#include "GlobalNamespace/PracticeSettings.hpp"
#include "System/Collections/Generic/List_1.hpp"
#include "System/IO/File.hpp"
#include "config.hpp"
#include "hooks.hpp"
#include "scotland2/shared/modloader.h"

using namespace GlobalNamespace;
using namespace System::IO;
using namespace System::Collections::Generic;

static bool lightsSet = false;
static std::string filesPath;
static std::string localFilesPath;

static constexpr auto copyopt = std::filesystem::copy_options::overwrite_existing;

static inline std::string GetBackupPath() {
    return getConfig().backupPath.GetValue();
}

static inline std::string GetLocalsBackupPath() {
    return std::filesystem::path(getConfig().backupPath.GetValue()) / LOCAL_FILES_DIR;
}

static void HandleSave(std::filesystem::path fullPath, std::string checkPath, std::string backupPath) {
    if (fullPath.parent_path() == checkPath && !std::filesystem::is_directory(fullPath) && !std::regex_search(fullPath.string(), BLACKLIST)) {
        logger.info("Copying for backup: {} -> {}", fullPath.string(), backupPath);
#ifdef SC2_AOT_HOOKS
        if (!sc2_save::atomic_copy(fullPath, backupPath / fullPath.filename()))
            logger.error("Backup failed; previous backup retained: {}", fullPath.string());
#else
        std::filesystem::copy(fullPath, backupPath / fullPath.filename(), copyopt);
#endif
    }
}

static void HandleSave(std::string filePath) {
#ifdef SC2_AOT_HOOKS
    try {
#endif
    auto fullPath = std::filesystem::canonical(filePath);
    logger.info("File saved, path: {} ({})", fullPath.string(), filePath);

    HandleSave(fullPath, filesPath, GetBackupPath());
    HandleSave(fullPath, localFilesPath, GetLocalsBackupPath());
#ifdef SC2_AOT_HOOKS
    } catch (const std::exception& error) { logger.error("Backup failed: {}",error.what()); }
#endif
}

MAKE_AUTO_HOOK_MATCH(File_WriteAllText, &File::WriteAllText, void, StringW path, StringW contents) {
#ifdef SC2_AOT_HOOKS
    std::lock_guard<std::recursive_mutex> lock(sc2_save::mutex);
#endif
    File_WriteAllText(path, contents);
    HandleSave(path);
}

MAKE_AUTO_HOOK_MATCH(
    File_Replace,
    &File::Replace,
    void,
    StringW sourceFileName,
    StringW destinationFileName,
    StringW destinationBackupFileName,
    bool ignoreMetadataErrors
) {
#ifdef SC2_AOT_HOOKS
    std::lock_guard<std::recursive_mutex> lock(sc2_save::mutex);
#endif
    File_Replace(sourceFileName, destinationFileName, destinationBackupFileName, ignoreMetadataErrors);
    HandleSave(destinationFileName);
}

MAKE_AUTO_HOOK_MATCH(
    PlayerData_ctor,
    &PlayerData::_ctor,
    void,
    PlayerData* self,
    StringW playerId,
    StringW playerName,
    bool shouldShowTutorialPrompt,
    bool shouldShow360Warning,
    bool agreedToEula,
    bool didSelectLanguage,
    bool agreedToMultiplayerDisclaimer,
    int didSelectRegionVersion,
    StringW selectedAvatarSystemTypeId,
    PlayerAgreements* playerAgreements,
    BeatmapDifficulty lastSelectedBeatmapDifficulty,
    BeatmapCharacteristicSO* lastSelectedBeatmapCharacteristic,
    GameplayModifiers* gameplayModifiers,
    PlayerSpecificSettings* playerSpecificSettings,
    PracticeSettings* practiceSettings,
    PlayerAllOverallStatsData* playerAllOverallStatsData,
    List_1<PlayerLevelStatsData*>* levelsStatsData,
    List_1<PlayerMissionStatsData*>* missionsStatsData,
    List_1<StringW>* showedMissionHelpIds,
    List_1<StringW>* guestPlayerNames,
    ColorSchemesSettings* colorSchemesSettings,
    OverrideEnvironmentSettings* overrideEnvironmentSettings,
    List_1<StringW>* favoritesLevelIds,
    MultiplayerModeSettings* multiplayerModeSettings,
    int currentDlcPromoDisplayCount,
    StringW currentDlcPromoId,
    UserAgeCategory userAgeCategory,
    PlayerSensitivityFlag desiredSensitivityFlag
) {
    if (!lightsSet) {
        playerSpecificSettings->_environmentEffectsFilterDefaultPreset = EnvironmentEffectsFilterPreset::AllEffects;
        playerSpecificSettings->_environmentEffectsFilterExpertPlusPreset = EnvironmentEffectsFilterPreset::AllEffects;
        lightsSet = true;
    }

    PlayerData_ctor(
        self,
        playerId,
        playerName,
        shouldShowTutorialPrompt,
        shouldShow360Warning,
        agreedToEula,
        didSelectLanguage,
        agreedToMultiplayerDisclaimer,
        didSelectRegionVersion,
        selectedAvatarSystemTypeId,
        playerAgreements,
        lastSelectedBeatmapDifficulty,
        lastSelectedBeatmapCharacteristic,
        gameplayModifiers,
        playerSpecificSettings,
        practiceSettings,
        playerAllOverallStatsData,
        levelsStatsData,
        missionsStatsData,
        showedMissionHelpIds,
        guestPlayerNames,
        colorSchemesSettings,
        overrideEnvironmentSettings,
        favoritesLevelIds,
        multiplayerModeSettings,
        currentDlcPromoDisplayCount,
        currentDlcPromoId,
        userAgeCategory,
        desiredSensitivityFlag
    );
}

MAKE_AUTO_HOOK_MATCH(
    PlayerDataFileModel_LoadFromCurrentVersion,
    &PlayerDataFileModel::LoadFromCurrentVersion,
    PlayerData*,
    PlayerDataFileModel* self,
    PlayerSaveData* playerSaveData
) {
    auto loadedData = PlayerDataFileModel_LoadFromCurrentVersion(self, playerSaveData);
    if (!loadedData)
        return loadedData;

    auto player = playerSaveData->localPlayers->get_Item(0);

    // fix override environment settings
    bool override = player->overrideEnvironmentSettings->overrideEnvironments;
    auto override360Env = self->GetEnvironmentInfoBySerializedName(player->overrideEnvironmentSettings->override360EnvironmentName);
    auto overrideNormalEnv = self->GetEnvironmentInfoBySerializedName(player->overrideEnvironmentSettings->overrideNormalEnvironmentName);

    if (!override360Env || override360Env->_environmentType != EnvironmentType::Circle)
        override360Env = self->_environmentsListModel->GetFirstEnvironmentInfoWithType(EnvironmentType::Circle);
    if (!overrideNormalEnv || overrideNormalEnv->_environmentType != EnvironmentType::Normal)
        overrideNormalEnv = self->_environmentsListModel->GetFirstEnvironmentInfoWithType(EnvironmentType::Normal);

    loadedData->overrideEnvironmentSettings->overrideEnvironments = override;
    loadedData->overrideEnvironmentSettings->SetEnvironmentInfoForType(EnvironmentType::Circle, override360Env);
    loadedData->overrideEnvironmentSettings->SetEnvironmentInfoForType(EnvironmentType::Normal, overrideNormalEnv);

    return loadedData;
}

static void CopyFolder(std::string backupFolder, std::string destFolder) {
    if (std::filesystem::exists(backupFolder)) {
        for (auto const& file : std::filesystem::directory_iterator(backupFolder)) {
            if (!std::filesystem::is_directory(file)) {
                logger.info("Loading backup: {}", file.path().string());
#ifdef SC2_AOT_HOOKS
                auto dest=std::filesystem::path(destFolder)/file.path().filename();
                if (sc2_save::should_restore(file.path(),dest) && !sc2_save::atomic_copy(file.path(),dest))
                    logger.error("Restore failed; destination retained: {}",dest.string());
#else
                std::filesystem::copy(file, destFolder / file.path().filename(), copyopt);
#endif
            }
        }
    } else
        std::filesystem::create_directories(backupFolder);
}

PLAYERDATAKEEPER_EXPORT_FUNC void setup(CModInfo* info) {
    *info = modInfo.to_c();
    Paper::Logger::RegisterFileContextId(MOD_ID);
    getConfig().Init(modInfo);

    filesPath = std::filesystem::canonical(modloader::get_external_dir());
    localFilesPath = std::filesystem::path(filesPath).parent_path() / LOCAL_FILES_DIR;
    std::filesystem::create_directories(filesPath);
    std::filesystem::create_directories(localFilesPath);

#ifdef SC2_AOT_HOOKS
    try {
#endif
    CopyFolder(GetBackupPath(), filesPath);
    CopyFolder(GetLocalsBackupPath(), localFilesPath);
#ifdef SC2_AOT_HOOKS
    } catch (const std::exception& error) { logger.error("Restore failed: {}",error.what()); }
#endif

#ifdef SC2_AOT_HOOKS
    // Installing backup support must preserve the player's current effects settings.
    // The old one-time upstream lighting reset is unnecessary for this host port.
    lightsSet = true;
#else
    lightsSet = getConfig().lightsSet.GetValue();
#endif
    if (!lightsSet)
        getConfig().lightsSet.SetValue(true);

    logger.info("Completed setup!");
}

PLAYERDATAKEEPER_EXPORT_FUNC void load() {
    Hooks::Install();
}
