#include "SongLoader/LevelLoader.hpp"
#include "BeatmapLevelSaveDataVersion4/AudioSaveData.hpp"
#include "BeatmapLevelSaveDataVersion4/BeatmapLevelSaveData.hpp"
#include "CustomJSONData.hpp"
#include "GlobalNamespace/ColorScheme.hpp"
#include "SongLoader/RuntimeSongLoader.hpp"
#include "UnityEngine/Color.hpp"
#include "Utils/WavRiff.hpp"
#include "logging.hpp"
#include "Utils/Hashing.hpp"
#include "Utils/File.hpp"
#include "Utils/OggVorbis.hpp"
#include "Utils/WavRiff.hpp"
#include "Utils/Cache.hpp"

#include "bsml/shared/Helpers/utilities.hpp"
#include "GlobalNamespace/BeatmapDifficultySerializedMethods.hpp"
#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"
#include "GlobalNamespace/BeatmapLevelColorSchemeSaveData.hpp"
#include "GlobalNamespace/PlayerSaveData.hpp"
#include "GlobalNamespace/EnvironmentName.hpp"
#include "GlobalNamespace/BeatmapBasicData.hpp"
#include "GlobalNamespace/BpmTimeProcessor.hpp"
#include "UnityEngine/JsonUtility.hpp"
#include "BeatmapSaveDataVersion3/BeatmapSaveData.hpp"
#include "BeatmapSaveDataVersion3/BeatmapSaveDataItem.hpp"
#include "BeatmapSaveDataVersion4/BeatmapSaveData.hpp"
#include "BeatmapSaveDataVersion4/LightshowSaveData.hpp"
#include "BeatmapSaveDataVersion4/BeatIndex.hpp"
#include "BeatmapSaveDataVersion4/BeatmapBeatIndex.hpp"
#include "BeatmapLevelSaveDataVersion4/AudioSaveData.hpp"
#include "Newtonsoft/Json/JsonConvert.hpp"
#include "utf8.h"
#include <cmath>
#include <exception>
#include <filesystem>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <limits>

DEFINE_TYPE(SongCore::SongLoader, LevelLoader);

namespace SongCore::SongLoader {
    void LevelLoader::ctor(GlobalNamespace::SpriteAsyncLoader* spriteAsyncLoader, GlobalNamespace::BeatmapCharacteristicCollection* beatmapCharacteristicCollection, GlobalNamespace::IAdditionalContentModel* additionalContentModel, GlobalNamespace::EnvironmentsListModel* environmentsListModel) {
        INVOKE_CTOR();
        _spriteAsyncLoader = spriteAsyncLoader;
        _beatmapCharacteristicCollection = beatmapCharacteristicCollection;
        _additionalContentModel = il2cpp_utils::try_cast<GlobalNamespace::AdditionalContentModel>(additionalContentModel).value_or(nullptr);
        _environmentsListModel = environmentsListModel;
        _clipLoader = GlobalNamespace::AudioClipAsyncLoader::CreateDefault();
    }

    SongCore::CustomJSONData::CustomLevelInfoSaveDataV2* LevelLoader::GetStandardSaveData(std::filesystem::path const& path) {
        return GetSaveDataFromV3(path);
    }

    SongCore::CustomJSONData::CustomLevelInfoSaveDataV2* LevelLoader::GetSaveDataFromV3(std::filesystem::path const& path) {
        if (path.empty()) {
            ERROR("Provided path was empty!");
            return nullptr;
        }

        auto infoPath = path / "info.dat";
        if (!std::filesystem::exists(infoPath)) {
            infoPath = path / "Info.dat";
            if (!std::filesystem::exists(infoPath)) {
                ERROR("no info.dat found for song @ '{}', returning null!", path.string());
                return nullptr;
            }
        }

        try {
            auto text = Utils::ReadText(infoPath);
            auto standardSaveData = LoadCustomSaveData(GlobalNamespace::StandardLevelInfoSaveData::DeserializeFromJSONString(text), text);

            if (!standardSaveData) {
                ERROR("Cannot load file from path: {}!", path.string());
                return nullptr;
            }

            auto opt = il2cpp_utils::try_cast<SongCore::CustomJSONData::CustomLevelInfoSaveDataV2>(standardSaveData);
            if (!opt.has_value()) {
                ERROR("Cannot load file {} as CustomLevelInfoSaveDataV2!", path.string());
                return nullptr;
            }

            return opt.value();
        } catch(std::runtime_error& e) {
            ERROR("GetSaveDataFromV3 can't Load File {}: {}!", path.string(), e.what());
        }

        // we errored out while doing things. return null
        return nullptr;
    }

    SongCore::CustomJSONData::CustomBeatmapLevelSaveDataV4* LevelLoader::GetSaveDataFromV4(std::filesystem::path const& path) {
        if (path.empty()) {
            ERROR("Provided path was empty!");
            return nullptr;
        }

        auto infoPath = path / "info.dat";
        if (!std::filesystem::exists(infoPath)) {
            infoPath = path / "Info.dat";
            if (!std::filesystem::exists(infoPath)) {
                ERROR("no info.dat found for song @ '{}', returning null!", path.string());
                return nullptr;
            }
        }

        try {
            auto infoText = Utils::ReadText(infoPath);
            auto beatmapLevelSaveData = LoadCustomSaveData(Newtonsoft::Json::JsonConvert::DeserializeObject<BeatmapLevelSaveDataVersion4::BeatmapLevelSaveData*>(infoText), infoText);

            if (!beatmapLevelSaveData) {
                ERROR("Cannot load file from path: {}!", path.string());
                return nullptr;
            }

            auto opt = il2cpp_utils::try_cast<SongCore::CustomJSONData::CustomBeatmapLevelSaveDataV4>(beatmapLevelSaveData);
            if (!opt.has_value()) {
                ERROR("Cannot load file {} as CustomBeatmapLevelSaveDataV4!", path.string());
                return nullptr;
            }

            return opt.value();
        } catch(std::runtime_error& e) {
            ERROR("GetSaveDataFromV4 can't Load File {}: {}!", path.string(), e.what());
        }

        // we errored out while doing things. return null
        return nullptr;
    }

    StringW EmptyString() {
        static ConstString empty("");
        return empty;
    }

    #define FixEmptyString(name) \
    if (!name) { \
        name = EmptyString(); \
    }

    CustomBeatmapLevel* LevelLoader::LoadCustomBeatmapLevel(std::filesystem::path const& levelPath, bool wip, SongCore::CustomJSONData::CustomLevelInfoSaveDataV2* saveData, std::string& hashOut) {
        if (!saveData) {
            #ifdef THROW_ON_MISSING_DATA
            throw std::runtime_error(fmt::format("saveData was null for level @ {}", levelPath.string()));
            #else
            WARNING("saveData was null for level @ {}", levelPath.string());
            return nullptr;
            #endif
        }

        if (!BasicVerifyMap(levelPath, saveData)) {
            #ifdef THROW_ON_MISSING_DATA
            throw std::runtime_error(fmt::format("Map {} was missing files!", levelPath.string()));
            #else
            WARNING("Map {} was missing files!", levelPath.string());
            return nullptr;
            #endif
        }

        auto hashOpt = Utils::GetCustomLevelHash(levelPath, saveData);
        hashOut = *hashOpt;

        std::string levelId = fmt::format("{}{}{}", RuntimeSongLoader::CUSTOM_LEVEL_PREFIX_ID, hashOut, wip ? " WIP" : "");

        auto songName = saveData->songName;
        FixEmptyString(songName)
        auto songSubName = saveData->songSubName;
        FixEmptyString(songSubName);
        auto songAuthorName = saveData->songAuthorName;
        FixEmptyString(songAuthorName);
        auto levelAuthorName = saveData->levelAuthorName;
        FixEmptyString(levelAuthorName);

        auto bpm = saveData->beatsPerMinute;
        auto songTimeOffset = saveData->songTimeOffset;
        auto previewStartTime = saveData->previewStartTime;
        auto previewDuration = saveData->previewDuration;

        if (!saveData->environmentName) saveData->_environmentName = EmptyString();
        auto environmentInfo = GetEnvironmentInfo(saveData->environmentName, false);
        if (!saveData->allDirectionsEnvironmentName) saveData->_allDirectionsEnvironmentName = EmptyString();
        auto allDirectionsEnvironmentInfo = GetEnvironmentInfo(saveData->allDirectionsEnvironmentName, true);
        if (!saveData->environmentNames) saveData->_environmentNames = ArrayW<StringW>::Empty();
        auto environmentInfos = GetEnvironmentInfos(saveData->environmentNames);
        if (!saveData->colorSchemes) saveData->_colorSchemes = ArrayW<GlobalNamespace::BeatmapLevelColorSchemeSaveData*>::Empty();
        auto colorSchemes = GetColorSchemes(saveData->colorSchemes);
        if (!saveData->difficultyBeatmapSets) saveData->_difficultyBeatmapSets = ArrayW<GlobalNamespace::StandardLevelInfoSaveData::DifficultyBeatmapSet*>::Empty();

        float songDuration = GetLengthForLevel(levelPath, saveData);

        std::vector<GlobalNamespace::EnvironmentName> environmentNameList;
        if (environmentInfos.size() == 0) {
            environmentNameList.emplace_back(
                environmentInfo->serializedName
            );
            environmentNameList.emplace_back(
                allDirectionsEnvironmentInfo->serializedName
            );
        } else {
            for (auto info : environmentInfos) {
                environmentNameList.emplace_back(
                    info->serializedName
                );
            }
        }

        auto allMappers = ArrayW<StringW>({ levelAuthorName });
        auto allLighters = ArrayW<StringW>::Empty();

        auto previewMediaData = GetPreviewMediaData(levelPath, saveData->coverImageFilename, saveData->songFilename);
        auto [beatmapLevelData, beatmapBasicData] = GetBeatmapLevelAndBasicData(levelPath, levelId, environmentNameList, colorSchemes, saveData);
        
        if(beatmapBasicData->Count == 0) {
            return nullptr;
        }

        auto result = CustomBeatmapLevel::New(
            levelPath.string(),
            saveData,
            nullptr,
            beatmapLevelData->i___GlobalNamespace__IBeatmapLevelData(),
            false,
            levelId,
            songName,
            songSubName,
            songAuthorName,
            allMappers,
            allLighters,
            bpm,
            -6.0f,
            songTimeOffset,
            previewStartTime,
            previewDuration,
            songDuration,
            GlobalNamespace::PlayerSensitivityFlag::Safe,
            previewMediaData->i___GlobalNamespace__IPreviewMediaData(),
            beatmapBasicData
        );

        return result;
    }

    // LevelLoader.CreateBeatmapLevelFromV4
    CustomBeatmapLevel* LevelLoader::LoadCustomBeatmapLevel(std::filesystem::path const& levelPath, bool wip, SongCore::CustomJSONData::CustomBeatmapLevelSaveDataV4* saveData, std::string& hashOut) {
        if (!saveData) {
            WARNING("saveData was null for level @ {}", levelPath.string());
            #ifdef THROW_ON_MISSING_DATA
            throw std::runtime_error(fmt::format("saveData was null for level @ {}", levelPath.string()));
            #else
            return nullptr;
            #endif
        }

        if (!BasicVerifyMap(levelPath, saveData)) {
            WARNING("Map {} was missing files!", levelPath.string());
            #ifdef THROW_ON_MISSING_DATA
            throw std::runtime_error(fmt::format("Map {} was missing files!", levelPath.string()));
            #else
            return nullptr;
            #endif
        }

        auto hashOpt = Utils::GetCustomLevelHash(levelPath, saveData);
        hashOut = *hashOpt;

        std::string levelId = fmt::format("{}{}{}", RuntimeSongLoader::CUSTOM_LEVEL_PREFIX_ID, hashOut, wip ? " WIP" : "");

        auto [songName, songSubName, songAuthorName] = saveData->song;
        FixEmptyString(songName)
        FixEmptyString(songSubName);
        FixEmptyString(songAuthorName);

        auto bpm = saveData->audio.bpm;
        auto lufs = (saveData->audio.lufs != 0.0f) ? saveData->audio.lufs : (-6.0f);
        auto previewStartTime = saveData->audio.previewStartTime;
        auto previewDuration = saveData->audio.previewDuration;

        float songDuration = GetLengthForLevel(levelPath, saveData);

        auto previewMediaData = GetPreviewMediaData(levelPath, saveData->coverImageFilename, saveData->audio.songFilename);
        auto [beatmapLevelData, beatmapBasicData] = GetBeatmapLevelAndBasicData(levelPath, levelId, saveData);

        if(beatmapBasicData->Count == 0) {
            return nullptr;
        }

        auto allMappers = ListW<StringW>::New();
        auto allLighters = ListW<StringW>::New();

        for (auto diffBeatmap : saveData->difficultyBeatmaps) {
            for (auto author : diffBeatmap->beatmapAuthors.mappers) {
                allMappers->Add(author);
            }
            for (auto author : diffBeatmap->beatmapAuthors.lighters) {
                allLighters->Add(author);
            }
        }

        auto result = CustomBeatmapLevel::New(
            levelPath.string(),
            nullptr,
            saveData,
            beatmapLevelData->i___GlobalNamespace__IBeatmapLevelData(),
            false,
            levelId,
            songName,
            songSubName,
            songAuthorName,
            allMappers.to_array(),
            allLighters.to_array(),
            bpm,
            lufs,
            0.0f,
            previewStartTime,
            previewDuration,
            songDuration,
            GlobalNamespace::PlayerSensitivityFlag::Safe,
            previewMediaData->i___GlobalNamespace__IPreviewMediaData(),
            beatmapBasicData
        );

        return result;
    }


    // V2 | V3
    std::pair<GlobalNamespace::FileSystemBeatmapLevelData*, LevelLoader::BeatmapBasicDataDict*> LevelLoader::GetBeatmapLevelAndBasicData(std::filesystem::path const& levelPath, std::string_view levelID, std::span<GlobalNamespace::EnvironmentName const> environmentNames, std::span<GlobalNamespace::ColorScheme* const> colorSchemes, CustomJSONData::CustomLevelInfoSaveDataV2* saveData) {
        auto fileDifficultyBeatmapsDict = System::Collections::Generic::Dictionary_2<CharacteristicDifficultyPair, GlobalNamespace::FileDifficultyBeatmap*>::New_ctor();
        auto basicDataDict = LevelLoader::BeatmapBasicDataDict::New_ctor();
        bool saveDataHadEnvNames = saveData->environmentNames.size() > 0;

        for (auto beatmapSet : saveData->difficultyBeatmapSets) {
            auto characteristic = _beatmapCharacteristicCollection->GetBeatmapCharacteristicBySerializedName(beatmapSet->beatmapCharacteristicName);
            if (!characteristic) {
                #ifdef THROW_ON_MISSING_DATA
                    throw std::runtime_error(fmt::format("Got null characteristic for characteristic name {}", beatmapSet->beatmapCharacteristicName));
                #else
                    WARNING("Got null characteristic for characteristic name {}, skipping...", beatmapSet->beatmapCharacteristicName);
                    continue;
                #endif
            }

            for (auto difficultyBeatmap : beatmapSet->difficultyBeatmaps) {
                GlobalNamespace::BeatmapDifficulty difficulty;
                auto parseSuccess = GlobalNamespace::BeatmapDifficultySerializedMethods::BeatmapDifficultyFromSerializedName(
                    difficultyBeatmap->difficulty,
                    byref(difficulty)
                );

                if (!parseSuccess) {
                    #ifdef THROW_ON_MISSING_DATA
                        throw std::runtime_error(fmt::format("Failed to parse a diff string: {}", difficultyBeatmap->difficulty));
                    #else
                        WARNING("Failed to parse a diff string: {}, skipping...", difficultyBeatmap->difficulty);
                        continue;
                    #endif
                }

                auto beatmapPath = levelPath / std::string(difficultyBeatmap->beatmapFilename);
                if (!std::filesystem::exists(beatmapPath)) {
                    #ifdef THROW_ON_MISSING_DATA
                        throw std::runtime_error(fmt::format("Diff file '{}' does not exist", beatmapPath.string()));
                    #else
                        WARNING("Diff file '{}' does not exist, skipping...", beatmapPath.string());
                        continue;
                    #endif
                }

                auto const dictKey = CharacteristicDifficultyPair(
                    characteristic,
                    difficulty
                );
                if (fileDifficultyBeatmapsDict->ContainsKey(dictKey)) {
                    #ifdef THROW_ON_MISSING_DATA
                        throw std::runtime_error(fmt::format("Duplicate characteristic/difficulty: {}/{}", characteristic->_serializedName, (int) difficulty));
                    #else
                        WARNING("Duplicate characteristic/difficulty: {}/{}", characteristic->_serializedName, (int) difficulty);
                        continue;
                    #endif
                }

                // This is v3 apparently so no need for a lightshow
                fileDifficultyBeatmapsDict->Add(
                    dictKey,
                    GlobalNamespace::FileDifficultyBeatmap::New_ctor(
                        beatmapPath.string(),
                        ""
                    )
                );

                // if we have env names, use the idx, otherwise use whether the char had rotation (no rot means use default env, otherwise use rotation env)
                int envNameIndex = saveDataHadEnvNames ? difficultyBeatmap->environmentNameIdx : characteristic->containsRotationEvents ? 1 : 0;
                envNameIndex = std::clamp<int>(envNameIndex, 0, environmentNames.size());
                int colorSchemeIndex = difficultyBeatmap->beatmapColorSchemeIdx;
                auto colorScheme = (colorSchemeIndex >= 0 && colorSchemeIndex < colorSchemes.size()) ? colorSchemes[colorSchemeIndex] : nullptr;

                basicDataDict->Add(
                    dictKey,
                    GlobalNamespace::BeatmapBasicData::New_ctor(
                        difficultyBeatmap->noteJumpMovementSpeed,
                        difficultyBeatmap->noteJumpStartBeatOffset,
                        environmentNames[envNameIndex],
                        colorScheme,
                        0,
                        0,
                        0,
                        0,
                        ArrayW<StringW>::Empty(),
                        ArrayW<StringW>::Empty()
                    )
                );
            }
        }

        return {
            GlobalNamespace::FileSystemBeatmapLevelData::New_ctor(
                levelID,
                (levelPath / std::string(saveData->songFilename)).string(),
                "",
                fileDifficultyBeatmapsDict
            ),
            basicDataDict
        };
    }

    // V4
    // implementation of CustomLevelLoader.CreateBeatmapLevelDataFromV4
    std::pair<GlobalNamespace::FileSystemBeatmapLevelData*, LevelLoader::BeatmapBasicDataDict*> LevelLoader::GetBeatmapLevelAndBasicData(std::filesystem::path const& levelPath, std::string_view levelID, CustomJSONData::CustomBeatmapLevelSaveDataV4* saveData) {
        auto fileDifficultyBeatmapsDict = System::Collections::Generic::Dictionary_2<CharacteristicDifficultyPair, GlobalNamespace::FileDifficultyBeatmap*>::New_ctor();
        auto basicDataDict = LevelLoader::BeatmapBasicDataDict::New_ctor();

        std::vector<GlobalNamespace::EnvironmentName> environmentNames;
        INFO("Environments {}", fmt::join(saveData->environmentNames, ";"));

        for (StringW name : saveData->environmentNames) {
            environmentNames.emplace_back(
                GetEnvironmentInfo(name, false)->serializedName
            );
            INFO("Environment {}",
                 GetEnvironmentInfo(name, false)->serializedName);
        }

        auto colorSchemes = ListW<GlobalNamespace::ColorScheme*>::New();
        static auto ConvertHTMLStringToColor = [](std::string colorHtmlString) {
            // if color does not start with # the library fails to parse the hex color. need to prepend that.
            // this is different from PC where it just assumes a hex string means hex color
            if (!colorHtmlString.starts_with('#')) colorHtmlString = fmt::format("#{}", colorHtmlString);
            auto color = BSML::Utilities::ParseHTMLColorOpt(colorHtmlString);
            if (!color)
                return UnityEngine::Color::get_black();
            return color.value();
        };
        if (saveData->colorSchemes) {
            for (int i = 0; i < saveData->colorSchemes.size(); i++) {
                auto colorScheme = saveData->colorSchemes[i];
                auto name = colorScheme->colorSchemeName;
                bool nonLocalized = true;
                bool editable = false;
                auto saberAColor = ConvertHTMLStringToColor(colorScheme->saberAColor);
                auto saberBColor = ConvertHTMLStringToColor(colorScheme->saberBColor);
                auto obstaclesColor = ConvertHTMLStringToColor(colorScheme->obstaclesColor);
                auto envColor0 = ConvertHTMLStringToColor(colorScheme->environmentColor0);
                auto envColor1 = ConvertHTMLStringToColor(colorScheme->environmentColor1);
                auto envColor0Boost = ConvertHTMLStringToColor(colorScheme->environmentColor0Boost);
                auto envColor1Boost = ConvertHTMLStringToColor(colorScheme->environmentColor1Boost);

                auto scheme = GlobalNamespace::ColorScheme::New_ctor(
                    name,
                    name,
                    nonLocalized,
                    name,
                    editable,
                    true,
                    saberAColor,
                    saberBColor,
                    true,
                    envColor0,
                    envColor1,
                    {1, 1, 1, 1},
                    true,
                    envColor0Boost,
                    envColor1Boost,
                    {1, 1, 1, 1},
                    obstaclesColor
                );

                INFO("ColorScheme ptr: {}", fmt::ptr(scheme));

                colorSchemes->Add(scheme);
            }
        }

        for (auto diffBeatmap : saveData->difficultyBeatmaps) {
            auto characteristic = _beatmapCharacteristicCollection->GetBeatmapCharacteristicBySerializedName(diffBeatmap->characteristic);
            if (!characteristic) {
                WARNING("Got null characteristic for characteristic name {}, skipping...", diffBeatmap->characteristic);
                #ifdef THROW_ON_MISSING_DATA
                    throw std::runtime_error(fmt::format("Got null characteristic for characteristic name {}", diffBeatmap->characteristic));
                #else
                    continue;
                #endif
            }

            GlobalNamespace::BeatmapDifficulty difficulty;
            auto parseSuccess = GlobalNamespace::BeatmapDifficultySerializedMethods::BeatmapDifficultyFromSerializedName(
                diffBeatmap->difficulty,
                byref(difficulty)
            );

            if (!parseSuccess) {
                WARNING("Failed to parse a diff string: {}, skipping...", diffBeatmap->difficulty);
                #ifdef THROW_ON_MISSING_DATA
                    throw std::runtime_error(fmt::format("Failed to parse a diff string: {}", diffBeatmap->difficulty));
                #else
                    continue;
                #endif
            }

            auto beatmapPath = levelPath / std::string(diffBeatmap->beatmapDataFilename);
            if (!std::filesystem::exists(beatmapPath)) {
                WARNING("Diff file '{}' does not exist, skipping...", beatmapPath.string());
                #ifdef THROW_ON_MISSING_DATA
                    throw std::runtime_error(fmt::format("Diff file '{}' does not exist", beatmapPath.string()));
                #else
                    continue;
                #endif
            }

            auto lightingPath = levelPath / std::string(diffBeatmap->lightshowDataFilename);
            if (!std::filesystem::exists(lightingPath)) {
                WARNING("Diff Lighting file '{}' does not exist, skipping...", lightingPath.string());
                #ifdef THROW_ON_MISSING_DATA
                    throw std::runtime_error(fmt::format("Diff Lighting file '{}' does not exist", lightingPath.string()));
                #else
                    continue;
                #endif
            }

            auto const dictKey = CharacteristicDifficultyPair(
                characteristic,
                difficulty
            );
            if (fileDifficultyBeatmapsDict->ContainsKey(dictKey)) {
                #ifdef THROW_ON_MISSING_DATA
                    throw std::runtime_error(fmt::format("Duplicate characteristic/difficulty: {}/{}", characteristic->_serializedName, (int) difficulty));
                #else
                    WARNING("Duplicate characteristic/difficulty: {}/{}", characteristic->_serializedName, (int) difficulty);
                    continue;
                #endif
            }

            fileDifficultyBeatmapsDict->Add(
                dictKey,
                GlobalNamespace::FileDifficultyBeatmap::New_ctor(
                    beatmapPath.string(),
                    lightingPath.string()
                )
            );

            ListW<StringW> mappers = ListW<StringW>::New();
            ListW<StringW> lighters = ListW<StringW>::New();

            for (auto author : diffBeatmap->beatmapAuthors.mappers) {
                mappers->Add(author);
            }
            for (auto author : diffBeatmap->beatmapAuthors.lighters) {
                lighters->Add(author);
            }

            auto environmentName = environmentNames[diffBeatmap->environmentNameIdx];
			auto colorScheme = ((diffBeatmap->beatmapColorSchemeIdx >= 0 && diffBeatmap->beatmapColorSchemeIdx < colorSchemes.size()) ? colorSchemes[diffBeatmap->beatmapColorSchemeIdx] : nullptr);

            INFO("Creating basic data with env name {} and color scheme {}", environmentName._environmentName, colorScheme ? colorScheme->colorSchemeId : "null");

            basicDataDict->Add(
                    dictKey,
                    GlobalNamespace::BeatmapBasicData::New_ctor(
                        diffBeatmap->noteJumpMovementSpeed,
                        diffBeatmap->noteJumpStartBeatOffset,
                        environmentName,
                        colorScheme,
                        0,
                        0,
                        0,
                        0,
                        mappers.to_array(),
                        lighters.to_array()
                    )
                );
        }

        return {
            GlobalNamespace::FileSystemBeatmapLevelData::New_ctor(
                levelID,
                (levelPath / std::string(saveData->audio.songFilename)).string(),
                (levelPath / std::string(saveData->audio.audioDataFilename)).string(),
                fileDifficultyBeatmapsDict
            ),
            basicDataDict
        };
    }

    GlobalNamespace::FileSystemPreviewMediaData* LevelLoader::GetPreviewMediaData(std::filesystem::path const& levelPath, StringW coverImageFilename, StringW songFilename) {
        return GlobalNamespace::FileSystemPreviewMediaData::New_ctor(
            _spriteAsyncLoader,
            _clipLoader,
            levelPath.string(),
            coverImageFilename,
            songFilename
        );
    }

    GlobalNamespace::EnvironmentInfoSO* LevelLoader::GetEnvironmentInfo(StringW environmentName, bool allDirections) {
        auto env = _environmentsListModel->GetEnvironmentInfoBySerializedName(environmentName);
        if (!env)
            env = _environmentsListModel->GetFirstEnvironmentInfoWithType(allDirections ? GlobalNamespace::EnvironmentType::Circle : GlobalNamespace::EnvironmentType::Normal);
        return env;
    }

    ArrayW<GlobalNamespace::EnvironmentInfoSO*> LevelLoader::GetEnvironmentInfos(std::span<StringW const> environmentsNames) {
        if (environmentsNames.empty()) return ArrayW<GlobalNamespace::EnvironmentInfoSO*>::Empty();
        auto envs = ListW<GlobalNamespace::EnvironmentInfoSO*>::New();

        for (auto environmentName : environmentsNames) {
            auto env = _environmentsListModel->GetEnvironmentInfoBySerializedName(environmentName);
            if (env) envs->Add(env);
        }

        return envs->ToArray();
    }

    ArrayW<GlobalNamespace::ColorScheme*> LevelLoader::GetColorSchemes(std::span<GlobalNamespace::BeatmapLevelColorSchemeSaveData* const> colorSchemeDatas) {
        if (colorSchemeDatas.empty()) return ArrayW<GlobalNamespace::ColorScheme*>::Empty();

        auto colorSchemes = ListW<GlobalNamespace::ColorScheme*>::New();
        for (auto saveData : colorSchemeDatas) {
            auto colorScheme = saveData->colorScheme;
            if (colorScheme) {
                colorSchemes->Add(
                    GlobalNamespace::ColorScheme::New_ctor(
                        colorScheme->colorSchemeId,
                        "",
                        false,
                        "",
                        false,
                        true,
                        colorScheme->saberAColor,
                        colorScheme->saberBColor,
                        true,
                        colorScheme->environmentColor0,
                        colorScheme->environmentColor1,
                        {1, 1, 1, 1},
                        true,
                        colorScheme->environmentColor0Boost,
                        colorScheme->environmentColor1Boost,
                        {1, 1, 1, 1},
                        colorScheme->obstaclesColor
                    )
                );
            }
        }
        return colorSchemes->ToArray();
    }

    float LevelLoader::GetLengthForLevel(std::filesystem::path const& levelPath, CustomJSONData::CustomLevelInfoSaveDataV2* saveData) {
        // check the cached info
        auto cachedInfoOpt = Utils::GetCachedInfo(levelPath);
        if (cachedInfoOpt.has_value() && cachedInfoOpt->songDuration.has_value()) {
            return cachedInfoOpt->songDuration.value();
        } else {
            // try to get the info from the ogg file
            std::string songFilePath = levelPath / static_cast<std::string>(saveData->songFilename);
            if (std::filesystem::exists(songFilePath)) {
                float songDuration = Utils::GetLengthFromOggVorbis(songFilePath);
                if (songDuration >= 0 && !std::isnan(songDuration)) { // found duration was valid
                    // update cache with new duration
                    auto info = cachedInfoOpt.value_or(Utils::CachedSongData());
                    info.songDuration = songDuration;
                    Utils::SetCachedInfo(levelPath, info);
                    return songDuration;
                }
                songDuration = Utils::GetLengthFromWavRiff(songFilePath);
                if (songDuration >= 0 && !std::isnan(songDuration)) { // found duration was valid
                    // update cache with new duration
                    auto info = cachedInfoOpt.value_or(Utils::CachedSongData());
                    info.songDuration = songDuration;
                    Utils::SetCachedInfo(levelPath, info);
                    return songDuration;
                }
            }

            // if the file didn't exist or we didn't get a valid length from the ogg, we go and get it from the map
            float songDuration = GetLengthFromMap(levelPath, saveData);
            auto info = cachedInfoOpt.value_or(Utils::CachedSongData());
            info.songDuration = songDuration;
            Utils::SetCachedInfo(levelPath, info);
            return songDuration;
        }
    }

    float LevelLoader::GetLengthForLevel(std::filesystem::path const& levelPath, CustomJSONData::CustomBeatmapLevelSaveDataV4* saveData) {
        // check the cached info
        auto cachedInfoOpt = Utils::GetCachedInfo(levelPath);
        if (cachedInfoOpt.has_value() && cachedInfoOpt->songDuration.has_value()) {
            return cachedInfoOpt->songDuration.value();
        } else {
            // try to get the info from the ogg file
            std::string songFilePath = levelPath / static_cast<std::string>(saveData->audio.songFilename);
            if (std::filesystem::exists(songFilePath)) {
                float songDuration = Utils::GetLengthFromOggVorbis(songFilePath);
                if (songDuration >= 0 && !std::isnan(songDuration)) { // found duration was valid
                    // update cache with new duration
                    auto info = cachedInfoOpt.value_or(Utils::CachedSongData());
                    info.songDuration = songDuration;
                    Utils::SetCachedInfo(levelPath, info);
                    return songDuration;
                }
                songDuration = Utils::GetLengthFromWavRiff(songFilePath);
                if (songDuration >= 0 && !std::isnan(songDuration)) { // found duration was valid
                    // update cache with new duration
                    auto info = cachedInfoOpt.value_or(Utils::CachedSongData());
                    info.songDuration = songDuration;
                    Utils::SetCachedInfo(levelPath, info);
                    return songDuration;
                }
            }

            // if the file didn't exist or we didn't get a valid length from the ogg, we go and get it from the map
            float songDuration = GetLengthFromMap(levelPath, saveData);
            auto info = cachedInfoOpt.value_or(Utils::CachedSongData());
            info.songDuration = songDuration;
            Utils::SetCachedInfo(levelPath, info);
            return songDuration;
        }
    }

    float LevelLoader::GetLengthFromMap(std::filesystem::path const& levelPath, CustomJSONData::CustomLevelInfoSaveDataV2* saveData) {
        try {
            static auto GetFirstAvailableDiffFile = [](std::filesystem::path const& levelPath, CustomJSONData::CustomLevelInfoSaveDataV2* saveData) -> GlobalNamespace::StandardLevelInfoSaveData::DifficultyBeatmap* {
                for (auto set : saveData->difficultyBeatmapSets) {
                    auto beatmaps = set->difficultyBeatmaps;
                    for (auto itr = beatmaps.rbegin(); itr != beatmaps.rend(); itr ++) {
                        std::string fileName((*itr)->beatmapFilename);
                        if (!fileName.empty() && std::filesystem::exists(levelPath / fileName)) {
                            return *itr;
                        }
                    }
                }

                return nullptr;
            };

            auto diff = GetFirstAvailableDiffFile(levelPath, saveData);
            if (!diff) {
                WARNING("No diff files were found for level {} to get beatmap length", levelPath.string());
                return 0;
            }

            auto beatmapFilePath = levelPath / std::string(diff->beatmapFilename);
            auto saveDataString = Utils::ReadText(beatmapFilePath);
            auto beatmapSaveData = UnityEngine::JsonUtility::FromJson<BeatmapSaveDataVersion3::BeatmapSaveData*>(saveDataString);

            if (!beatmapSaveData) {
                WARNING("Could not parse beatmap savedata for time from savedata string {}", utf8::utf16to8(saveDataString));
                return 0;
            }

            float highestBeat = 0.0f;
            if (beatmapSaveData->colorNotes && beatmapSaveData->colorNotes->Count > 0) {
                for (auto note : ListW<::BeatmapSaveDataVersion3::ColorNoteData*>(beatmapSaveData->colorNotes)) {
                    auto beat = note->beat;
                    if (beat > highestBeat) highestBeat = beat;
                }
            }

            if (beatmapSaveData->basicBeatmapEvents && beatmapSaveData->basicBeatmapEvents->Count > 0) {
                for (auto event : ListW<::BeatmapSaveDataVersion3::BasicEventData*>(beatmapSaveData->basicBeatmapEvents)) {
                    auto beat = event->beat;
                    if (beat > highestBeat) highestBeat = beat;
                }
            }

            auto bmpInTimeProcessor = GlobalNamespace::BpmTimeProcessor::New_ctor(saveData->beatsPerMinute, beatmapSaveData->bpmEvents->i___System__Collections__Generic__IReadOnlyList_1_T_());
            return bmpInTimeProcessor->ConvertBeatToTime(highestBeat);
        } catch (std::exception const& e) {
            ERROR("While determining length from map, caught exception {}: {}", typeid(e).name(), e.what());
        } catch (...) {
            ERROR("While determining length from map, caught error {} with no known what()!", typeid(std::current_exception()).name());
        }

        WARNING("Getting length from map failed, so duration will be 0");
        // we somehow failed, so we return 0
        return 0;
    }

    float LevelLoader::GetLengthFromMap(std::filesystem::path const& levelPath, CustomJSONData::CustomBeatmapLevelSaveDataV4* saveData) {
        try {
            static auto GetFirstAvailableDiffFile = [](std::filesystem::path const& levelPath, CustomJSONData::CustomBeatmapLevelSaveDataV4* saveData) -> BeatmapLevelSaveDataVersion4::BeatmapLevelSaveData::DifficultyBeatmap* {
                for (auto beatmap : saveData->difficultyBeatmaps) {
                    std::string fileName(beatmap->beatmapDataFilename);
                    if (!fileName.empty() && std::filesystem::exists(levelPath / fileName)) {
                        return beatmap;
                    }
                }

                return nullptr;
            };

            auto diff = GetFirstAvailableDiffFile(levelPath, saveData);
            if (!diff) {
                WARNING("No diff files were found for level {} to get beatmap length", levelPath.string());
                return 0;
            }

            auto beatmapFilePath = levelPath / std::string(diff->beatmapDataFilename);
            auto saveDataString = Utils::ReadText(beatmapFilePath);
            auto beatmapSaveData = UnityEngine::JsonUtility::FromJson<BeatmapSaveDataVersion4::BeatmapSaveData*>(saveDataString);

            auto lightshowFilePath = levelPath / std::string(diff->lightshowDataFilename);
            auto lightshowSaveDataString = Utils::ReadText(lightshowFilePath);
            auto lightshowSaveData = UnityEngine::JsonUtility::FromJson<BeatmapSaveDataVersion4::LightshowSaveData*>(lightshowSaveDataString);

            if (!beatmapSaveData) {
                WARNING("Could not parse beatmap savedata for time from savedata string {}", utf8::utf16to8(saveDataString));
                return 0;
            }

            float highestBeat = 0.0f;
            if (beatmapSaveData->colorNotes && beatmapSaveData->colorNotes->get_Length() > 0) {
                for (auto note : ListW<::BeatmapSaveDataVersion4::BeatmapBeatIndex*>(beatmapSaveData->colorNotes)) {
                  auto beat = note->beat;
                  highestBeat = std::max(beat, highestBeat);
                  
                }
            }
            if (lightshowSaveData->basicEvents && lightshowSaveData->basicEvents->get_Length() > 0) {
                for (auto event : ListW<::BeatmapSaveDataVersion4::BeatIndex*>(lightshowSaveData->basicEvents)) {
                    auto beat = event->beat;
                    highestBeat = std::max(beat, highestBeat);
                }
            }

            auto audioFilePath = levelPath / std::string(saveData->audio.audioDataFilename);
            auto audioSaveDataString = Utils::ReadText(audioFilePath);
            auto audioSaveData = UnityEngine::JsonUtility::FromJson<BeatmapLevelSaveDataVersion4::AudioSaveData*>(audioSaveDataString);

            auto bmpInTimeProcessor = GlobalNamespace::BpmTimeProcessor::New_ctor(audioSaveData);
            return bmpInTimeProcessor->ConvertBeatToTime(highestBeat);
        } catch (std::exception const& e) {
            ERROR("While determining length from map, caught exception {}: {}", typeid(e).name(), e.what());
        } catch (...) {
            ERROR("While determining length from map, caught error {} with no known what()!", typeid(std::current_exception()).name());
        }

        WARNING("Getting length from map failed, so duration will be 0");
        // we somehow failed, so we return 0
        return 0;
    }

    bool LevelLoader::BasicVerifyMap(std::filesystem::path const& levelPath, CustomJSONData::CustomLevelInfoSaveDataV2* saveData) {
        std::string songFile(saveData->songFilename);
        std::string coverFile(saveData->coverImageFilename);

        if (!std::filesystem::exists(levelPath / songFile)) return false;
        if (!std::filesystem::exists(levelPath / coverFile)) return false;

        for (auto set : saveData->difficultyBeatmapSets) {
            for (auto diff : set->difficultyBeatmaps) {
                std::string diffFile(diff->beatmapFilename);
                if (!std::filesystem::exists(levelPath / diffFile)) return false;
            }
        }

        // no files were found to be missing, return success
        return true;
    }

    bool LevelLoader::BasicVerifyMap(std::filesystem::path const& levelPath, CustomJSONData::CustomBeatmapLevelSaveDataV4* saveData) {
        std::string songFile(saveData->audio.songFilename);
        std::string coverFile(saveData->coverImageFilename);
        std::string audioFile(saveData->audio.audioDataFilename);

        if (!std::filesystem::exists(levelPath / songFile)) return false;
        if (!std::filesystem::exists(levelPath / coverFile)) return false;

        if (!std::filesystem::exists(levelPath / audioFile)) return false;

        for (auto diff : saveData->difficultyBeatmaps) {
            std::string diffFile(diff->beatmapDataFilename);
            std::string lightFile(diff->lightshowDataFilename);
            if (!std::filesystem::exists(levelPath / diffFile)) return false;
            if (!std::filesystem::exists(levelPath / lightFile)) return false;
        }

        // no files were found to be missing, return success
        return true;
    }

    SongCore::CustomJSONData::CustomLevelInfoSaveDataV2* LevelLoader::LoadCustomSaveData(GlobalNamespace::StandardLevelInfoSaveData* saveData, std::u16string_view stringData) {
        if (!saveData) {
            WARNING("Save Data is not valid!");
            return nullptr;
        }
        if (!saveData->difficultyBeatmapSets) {
            WARNING("Save Data has no difficulty beatmaps!");
            return nullptr;
        }
        
        auto customBeatmapSets = ArrayW<GlobalNamespace::StandardLevelInfoSaveData::DifficultyBeatmapSet*>(il2cpp_array_size_t(saveData->difficultyBeatmapSets.size()));

        SongCore::CustomJSONData::CustomLevelInfoSaveDataV2 *customSaveData =
                SongCore::CustomJSONData::CustomLevelInfoSaveDataV2::New_ctor(
                    saveData->songName,
                    saveData->songSubName,
                    saveData->songAuthorName,
                    saveData->levelAuthorName,
                    saveData->beatsPerMinute,
                    saveData->songTimeOffset,
                    saveData->shuffle,
                    saveData->shufflePeriod,
                    saveData->previewStartTime,
                    saveData->previewDuration,
                    saveData->songFilename,
                    saveData->coverImageFilename,
                    saveData->environmentName,
                    saveData->allDirectionsEnvironmentName,
                    saveData->environmentNames,
                    saveData->colorSchemes,
                    customBeatmapSets
                );


        auto sharedDoc = std::make_shared<SongCore::CustomJSONData::DocumentUTF16>();
        customSaveData->_customSaveDataInfo = SongCore::CustomJSONData::CustomSaveDataInfo();
        customSaveData->_customSaveDataInfo->saveDataVersion = SongCore::CustomJSONData::CustomSaveDataInfo::SaveDataVersion::V3;
        customSaveData->_customSaveDataInfo->doc = sharedDoc;

        rapidjson::GenericDocument<rapidjson::UTF16<char16_t>> &doc = *sharedDoc;
        doc.Parse(stringData.data());

        auto dataItr = doc.FindMember(u"_customData");
        if (dataItr != doc.MemberEnd()) {
            customSaveData->_customSaveDataInfo->customData = dataItr->value;
        }

        SongCore::CustomJSONData::ValueUTF16 const& beatmapSetsArr = doc.FindMember(u"_difficultyBeatmapSets")->value;

        for (rapidjson::SizeType i = 0; i < beatmapSetsArr.Size(); i++) {
            SongCore::CustomJSONData::ValueUTF16 const& beatmapSetJson = beatmapSetsArr[i];

            auto originalBeatmapSet = saveData->difficultyBeatmapSets[i];
            auto customBeatmaps = ArrayW<GlobalNamespace::StandardLevelInfoSaveData::DifficultyBeatmap *>(originalBeatmapSet->difficultyBeatmaps.size());

            auto const& difficultyBeatmaps = beatmapSetJson.FindMember(u"_difficultyBeatmaps")->value;

            for (rapidjson::SizeType j = 0; j < originalBeatmapSet->difficultyBeatmaps.size(); j++) {
                SongCore::CustomJSONData::ValueUTF16 const& difficultyBeatmapJson = difficultyBeatmaps[j];
                auto originalBeatmap = originalBeatmapSet->difficultyBeatmaps[j];

                auto customBeatmap =
                    SongCore::CustomJSONData::CustomDifficultyBeatmap::New_ctor(
                        originalBeatmap->difficulty,
                        originalBeatmap->difficultyRank,
                        originalBeatmap->beatmapFilename,
                        originalBeatmap->noteJumpMovementSpeed,
                        originalBeatmap->noteJumpStartBeatOffset,
                        originalBeatmap->beatmapColorSchemeIdx,
                        originalBeatmap->environmentNameIdx
                    );

                auto customDataItr = difficultyBeatmapJson.FindMember(u"_customData");
                if (customDataItr != difficultyBeatmapJson.MemberEnd()) {
                    customBeatmap->customData = customDataItr->value;
                }

                customBeatmaps[j] = customBeatmap;
            }

            auto customBeatmapSet = SongCore::CustomJSONData::CustomDifficultyBeatmapSet::New_ctor(
                originalBeatmapSet->beatmapCharacteristicName,
                customBeatmaps
            );

            auto customDataItr = beatmapSetJson.FindMember(u"_customData");
            if (customDataItr != beatmapSetJson.MemberEnd()) {
                customBeatmapSet->customData = customDataItr->value;
            }

            customBeatmapSets[i] = customBeatmapSet;
        }
        return customSaveData;
    }

    SongCore::CustomJSONData::CustomBeatmapLevelSaveDataV4* LevelLoader::LoadCustomSaveData(BeatmapLevelSaveDataVersion4::BeatmapLevelSaveData* saveData, std::u16string_view stringData) {
        if (!saveData) {
            WARNING("Save Data is not valid!");
            return nullptr;
        }
        if (!saveData->difficultyBeatmaps) {
            WARNING("Save Data has no difficulty beatmaps!");
            return nullptr;
        }

        auto customDiffBeatmaps = ArrayW<BeatmapLevelSaveDataVersion4::BeatmapLevelSaveData::DifficultyBeatmap*>(il2cpp_array_size_t(saveData->difficultyBeatmaps.size()));

        SongCore::CustomJSONData::CustomBeatmapLevelSaveDataV4 *customSaveData =
                SongCore::CustomJSONData::CustomBeatmapLevelSaveDataV4::New_ctor();
        customSaveData->song = saveData->song;
        customSaveData->audio = saveData->audio;
        customSaveData->colorSchemes = saveData->colorSchemes;
        customSaveData->difficultyBeatmaps = customDiffBeatmaps;
        customSaveData->coverImageFilename = saveData->coverImageFilename;
        customSaveData->songPreviewFilename = saveData->songPreviewFilename;
        customSaveData->environmentNames = saveData->environmentNames;
        customSaveData->version = saveData->version;

        std::u16string_view str(stringData);

        auto sharedDoc = std::make_shared<SongCore::CustomJSONData::DocumentUTF16>();
        customSaveData->_customSaveDataInfo = SongCore::CustomJSONData::CustomSaveDataInfo();
        customSaveData->_customSaveDataInfo->saveDataVersion = SongCore::CustomJSONData::CustomSaveDataInfo::SaveDataVersion::V4;
        customSaveData->_customSaveDataInfo->doc = sharedDoc;

        rapidjson::GenericDocument<rapidjson::UTF16<char16_t>> &doc = *sharedDoc;
        doc.Parse(str.data());

        auto dataItr = doc.FindMember(u"customData");
        if (dataItr != doc.MemberEnd()) {
            customSaveData->_customSaveDataInfo->customData = dataItr->value;
        }

        SongCore::CustomJSONData::ValueUTF16 const& beatmapsArr = doc.FindMember(u"difficultyBeatmaps")->value;

        for (rapidjson::SizeType i = 0; i < beatmapsArr.Size(); i++) {
            SongCore::CustomJSONData::ValueUTF16 const& diffBeatmapJson = beatmapsArr[i];

            auto originalDiffBeatmap = saveData->difficultyBeatmaps[i];
            auto customDiffBeatmap = SongCore::CustomJSONData::CustomDifficultyBeatmapV4::New_ctor(
                originalDiffBeatmap->beatmapAuthors,
                originalDiffBeatmap->beatmapColorSchemeIdx,
                originalDiffBeatmap->beatmapDataFilename,
                originalDiffBeatmap->characteristic,
                originalDiffBeatmap->difficulty,
                originalDiffBeatmap->environmentNameIdx,
                originalDiffBeatmap->lightshowDataFilename,
                originalDiffBeatmap->noteJumpMovementSpeed,
                originalDiffBeatmap->noteJumpStartBeatOffset
            );

            auto customDataItr = diffBeatmapJson.FindMember(u"customData");
            if (customDataItr != diffBeatmapJson.MemberEnd()) {
                customDiffBeatmap->customData = customDataItr->value;
            }

            customDiffBeatmaps[i] = customDiffBeatmap;
        }
        return customSaveData;
    }
}
