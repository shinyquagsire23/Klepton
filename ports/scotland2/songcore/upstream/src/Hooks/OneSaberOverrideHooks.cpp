#include "hooking.hpp"
#include "logging.hpp"
#include "config.hpp"

#include "GlobalNamespace/GameplayCoreInstaller.hpp"
#include "GlobalNamespace/GameplayCoreSceneSetupData.hpp"
#include "GlobalNamespace/BeatmapCharacteristicCollection.hpp"
#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"
#include "SongLoader/CustomBeatmapLevel.hpp"
#include "CustomJSONData.hpp"

MAKE_AUTO_HOOK_MATCH(GameplayCoreInstaller_InstallBindings, &GlobalNamespace::GameplayCoreInstaller::InstallBindings, void, GlobalNamespace::GameplayCoreInstaller* self) {
    // if override is disabledl, just run orig and nothing else
    if (config.disableOneSaberOverride) return GameplayCoreInstaller_InstallBindings(self);

    auto sceneSetupData = self->_sceneSetupData;
    // get custom preview level
    auto level = il2cpp_utils::try_cast<SongCore::SongLoader::CustomBeatmapLevel>(sceneSetupData->beatmapLevel).value_or(nullptr);
    if (!level) return GameplayCoreInstaller_InstallBindings(self);

    // get custom level save data if possible
    auto customSaveDataInfoOpt = level->CustomSaveDataInfo;
    if (!customSaveDataInfoOpt) return GameplayCoreInstaller_InstallBindings(self);
    auto& customSaveDataInfo = customSaveDataInfoOpt->get();

    // get difficulty data
    auto beatmapKey = sceneSetupData->beatmapKey;
    auto characteristic = beatmapKey.beatmapCharacteristic;
    auto difficulty = beatmapKey.difficulty;
    auto difficultyDataOpt = customSaveDataInfo.TryGetCharacteristicAndDifficulty(characteristic->serializedName, difficulty);
    if (!difficultyDataOpt.has_value()) return GameplayCoreInstaller_InstallBindings(self);

    // get if one saber set
    auto& difficultyData = difficultyDataOpt->get();
    if (!difficultyData.oneSaber.has_value()) return GameplayCoreInstaller_InstallBindings(self);

    int saberCount = difficultyData.oneSaber.value() ? 1 : 2;
    int origCount = characteristic->_numberOfColors;

    // override the color number during the invoke of orig, restore afterwards
    characteristic->_numberOfColors = saberCount;
    GameplayCoreInstaller_InstallBindings(self);
    characteristic->_numberOfColors = origCount;
}
