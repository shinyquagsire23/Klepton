#pragma once

#include "custom-types/shared/macros.hpp"
#include "../CustomJSONData.hpp"
#include "CustomBeatmapLevel.hpp"

#include "GlobalNamespace/EnvironmentInfoSO.hpp"
#include "GlobalNamespace/ColorScheme.hpp"
#include "GlobalNamespace/BeatmapLevelColorSchemeSaveData.hpp"
#include "GlobalNamespace/SpriteAsyncLoader.hpp"
#include "GlobalNamespace/BeatmapCharacteristicCollection.hpp"
#include "GlobalNamespace/AdditionalContentModel.hpp"
#include "GlobalNamespace/EnvironmentsListModel.hpp"
#include "GlobalNamespace/ColorScheme.hpp"
#include "GlobalNamespace/FileSystemPreviewMediaData.hpp"
#include "GlobalNamespace/FileSystemBeatmapLevelData.hpp"
#include "GlobalNamespace/FileDifficultyBeatmap.hpp"
#include "GlobalNamespace/AudioClipAsyncLoader.hpp"
#include "BeatmapLevelSaveDataVersion4/BeatmapLevelSaveData.hpp"
#include "System/ValueTuple_2.hpp"
#include "System/Collections/Generic/Dictionary_2.hpp"
#include <filesystem>

DECLARE_CLASS_CODEGEN(SongCore::SongLoader, LevelLoader, System::Object) {
    DECLARE_CTOR(ctor, GlobalNamespace::SpriteAsyncLoader* spriteAsyncLoader, GlobalNamespace::BeatmapCharacteristicCollection* beatmapCharacteristicCollection, GlobalNamespace::IAdditionalContentModel* additionalContentModel, GlobalNamespace::EnvironmentsListModel* environmentsListModel);
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::SpriteAsyncLoader*, _spriteAsyncLoader);
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::BeatmapCharacteristicCollection*, _beatmapCharacteristicCollection);
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::AdditionalContentModel*, _additionalContentModel);
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::EnvironmentsListModel*, _environmentsListModel);
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::AudioClipAsyncLoader*, _clipLoader);

    public:
        /// @brief gets the v3 savedata from the path
        [[deprecated("Use GetSaveDataFromV3 instead, this just redirects to that")]]
        SongCore::CustomJSONData::CustomLevelInfoSaveDataV2* GetStandardSaveData(std::filesystem::path const& path);

        /// @brief gets the v3 savedata from the path
        SongCore::CustomJSONData::CustomLevelInfoSaveDataV2* GetSaveDataFromV3(std::filesystem::path const& path);

        /// @brief gets the v4 savedata from the path
        SongCore::CustomJSONData::CustomBeatmapLevelSaveDataV4* GetSaveDataFromV4(std::filesystem::path const& path);

        /// @brief Loads song at given path
        /// @param path the path to the song
        /// @param isWip is this a wip song
        /// @param saveData the level save data, for custom levels this is always a custom level info savedata
        /// @param outHash output for the hash of this level, might be unneeded though
        /// @return loaded beatmap level, or nullptr if failed
        CustomBeatmapLevel* LoadCustomBeatmapLevel(std::filesystem::path const& levelPath, bool wip, SongCore::CustomJSONData::CustomLevelInfoSaveDataV2* saveData, std::string& hashOut);

        /// @brief Loads song at given path
        /// @param path the path to the song
        /// @param isWip is this a wip song
        /// @param saveData the level save data, for v4 levels this is a CustomBeatmapLevelSaveDataV4
        /// @param outHash output for the hash of this level, might be unneeded though
        /// @return loaded beatmap level, or nullptr if failed
        CustomBeatmapLevel* LoadCustomBeatmapLevel(std::filesystem::path const& levelPath, bool wip, SongCore::CustomJSONData::CustomBeatmapLevelSaveDataV4* saveData, std::string& hashOut);

    private:
        /// @brief does basic verification on a map to catch any problems before they actually occur
        bool BasicVerifyMap(std::filesystem::path const& levelPath, SongCore::CustomJSONData::CustomLevelInfoSaveDataV2* saveData);
        /// @brief does basic verification on a map to catch any problems before they actually occur
        bool BasicVerifyMap(std::filesystem::path const& levelPath, SongCore::CustomJSONData::CustomBeatmapLevelSaveDataV4* saveData);

        using CharacteristicDifficultyPair = System::ValueTuple_2<UnityW<GlobalNamespace::BeatmapCharacteristicSO>, GlobalNamespace::BeatmapDifficulty>;
        using BeatmapBasicDataDict = System::Collections::Generic::Dictionary_2<CharacteristicDifficultyPair, GlobalNamespace::BeatmapBasicData*>;
        using BeatmapLevelDataDict = System::Collections::Generic::Dictionary_2<CharacteristicDifficultyPair, GlobalNamespace::FileDifficultyBeatmap*>;

        /// @brief preview media data from filesystem
        GlobalNamespace::FileSystemPreviewMediaData* GetPreviewMediaData(std::filesystem::path const& levelPath, StringW coverImageFilename, StringW songFilename);

        /// @brief beatmap level data from filesystem & basic beatmap data from savedata
        std::pair<GlobalNamespace::FileSystemBeatmapLevelData*, BeatmapBasicDataDict*> GetBeatmapLevelAndBasicData(std::filesystem::path const& levelPath, std::string_view levelID, std::span<GlobalNamespace::EnvironmentName const> environmentNames, std::span<GlobalNamespace::ColorScheme* const> colorSchemes, CustomJSONData::CustomLevelInfoSaveDataV2* saveData);

        /// @brief beatmap level data from filesystem & basic beatmap data from savedata
        std::pair<GlobalNamespace::FileSystemBeatmapLevelData*, BeatmapBasicDataDict*> GetBeatmapLevelAndBasicData(std::filesystem::path const& levelPath, std::string_view levelID, CustomJSONData::CustomBeatmapLevelSaveDataV4* saveData);

        /// @brief gets the environment info for the environmentName and whether it's all directions or not
        GlobalNamespace::EnvironmentInfoSO* GetEnvironmentInfo(StringW environmentName, bool allDirections);

        /// @brief gets the environmentinfos for the environmentNames
        ArrayW<GlobalNamespace::EnvironmentInfoSO*> GetEnvironmentInfos(std::span<StringW const> environmentsNames);

        /// @brief creates the color schemes for the savedata
        ArrayW<GlobalNamespace::ColorScheme*> GetColorSchemes(std::span<GlobalNamespace::BeatmapLevelColorSchemeSaveData* const> colorSchemeDatas);

        /// @brief gets the length for a level
        static float GetLengthForLevel(std::filesystem::path const& levelPath, CustomJSONData::CustomLevelInfoSaveDataV2* saveData);

        /// @brief gets the length for a level
        static float GetLengthForLevel(std::filesystem::path const& levelPath, CustomJSONData::CustomBeatmapLevelSaveDataV4* saveData);

        /// @brief calculates the song duration by parsing the first characteristic, first difficulty for the last note and seeing the time on it
        static float GetLengthFromMap(std::filesystem::path const& levelPath, CustomJSONData::CustomLevelInfoSaveDataV2* saveData);

        /// @brief calculates the song duration by parsing the first characteristic, first difficulty for the last note and seeing the time on it
        static float GetLengthFromMap(std::filesystem::path const& levelPath, CustomJSONData::CustomBeatmapLevelSaveDataV4* saveData);

        /// @brief gets the v3 savedata with custom data from the base game save data
        SongCore::CustomJSONData::CustomLevelInfoSaveDataV2* LoadCustomSaveData(GlobalNamespace::StandardLevelInfoSaveData* saveData, std::u16string_view stringData);

        /// @brief gets the v4 savedata with custom data from the base game save data
        SongCore::CustomJSONData::CustomBeatmapLevelSaveDataV4* LoadCustomSaveData(BeatmapLevelSaveDataVersion4::BeatmapLevelSaveData* saveData, std::u16string_view stringData);
};