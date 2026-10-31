#pragma once

#include "custom-types/shared/macros.hpp"
#include "GlobalNamespace/BeatmapLevel.hpp"
#include "GlobalNamespace/IBeatmapLevelData.hpp"
#include "../CustomJSONData.hpp"

// type which is basically a beatmaplevel but one made by songcore, helps with identification
DECLARE_CLASS_CODEGEN(SongCore::SongLoader, CustomBeatmapLevel, GlobalNamespace::BeatmapLevel) {
    DECLARE_CTOR(ctor,
        bool hasPrecalculatedData,
        ::StringW levelID,
        ::StringW songName,
        ::StringW songSubName,
        ::StringW songAuthorName,
        ::ArrayW<::StringW> allMappers,
        ::ArrayW<::StringW> allLighters,
        float_t beatsPerMinute,
        float_t integratedLufs,
        float_t songTimeOffset,
        float_t previewStartTime,
        float_t previewDuration,
        float_t songDuration,
        ::GlobalNamespace::PlayerSensitivityFlag contentRating,
        ::GlobalNamespace::IPreviewMediaData* previewMediaData,
        ::System::Collections::Generic::Dictionary_2<::System::ValueTuple_2<::UnityW<::GlobalNamespace::BeatmapCharacteristicSO>, ::GlobalNamespace::BeatmapDifficulty>, ::GlobalNamespace::BeatmapBasicData*>* beatmapBasicData
    );

    public:
        /// @brief path to the custom level
        std::string_view get_customLevelPath() const { return _customLevelPath; }
        __declspec(property(get=get_customLevelPath)) std::string_view customLevelPath;

        /// @brief gets the CustomSaveDataInfo from either the v2/3 or v4 info savedata
        std::optional<std::reference_wrapper<CustomJSONData::CustomSaveDataInfo>> get_CustomSaveDataInfo() const {
            if (_customLevelSaveDataV2) return _customLevelSaveDataV2->CustomSaveDataInfo;
            if (_customBeatmapLevelSaveDataV4) return _customBeatmapLevelSaveDataV4->CustomSaveDataInfo;
            return std::nullopt;
        }
        __declspec(property(get=get_CustomSaveDataInfo)) std::optional<std::reference_wrapper<CustomJSONData::CustomSaveDataInfo>> CustomSaveDataInfo;

        /// @brief level info.dat save data. Set for V2-V3 levels.
        std::optional<CustomJSONData::CustomLevelInfoSaveDataV2*> get_standardLevelInfoSaveDataV2() { return _customLevelSaveDataV2 ? std::optional(_customLevelSaveDataV2) : std::nullopt; }
        __declspec(property(get=get_standardLevelInfoSaveDataV2)) std::optional<CustomJSONData::CustomLevelInfoSaveDataV2*> standardLevelInfoSaveDataV2;

        /// @brief level info.dat save data. Set for V4 levels.
        std::optional<CustomJSONData::CustomBeatmapLevelSaveDataV4*> get_beatmapLevelSaveDataV4() { return _customBeatmapLevelSaveDataV4 ? std::optional(_customBeatmapLevelSaveDataV4) : std::nullopt; }
        __declspec(property(get=get_beatmapLevelSaveDataV4)) std::optional<CustomJSONData::CustomBeatmapLevelSaveDataV4*> beatmapLevelSaveDataV4;

        /// @brief level beatmapleveldata
        GlobalNamespace::IBeatmapLevelData* get_beatmapLevelData() const { return _beatmapLevelData; }
        __declspec(property(get=get_beatmapLevelData)) GlobalNamespace::IBeatmapLevelData* beatmapLevelData;

        static CustomBeatmapLevel* New(
            std::string_view customLevelPath,
            CustomJSONData::CustomLevelInfoSaveDataV2* saveDataV2,
            CustomJSONData::CustomBeatmapLevelSaveDataV4* saveDataV4,
            GlobalNamespace::IBeatmapLevelData* beatmapLevelData,
            // BeatmapLevelData args
            bool hasPrecalculatedData,
            ::StringW levelID,
            ::StringW songName,
            ::StringW songSubName,
            ::StringW songAuthorName,
            ::ArrayW<::StringW> allMappers,
            ::ArrayW<::StringW> allLighters,
            float_t beatsPerMinute,
            float_t integratedLufs,
            float_t songTimeOffset,
            float_t previewStartTime,
            float_t previewDuration,
            float_t songDuration,
            ::GlobalNamespace::PlayerSensitivityFlag contentRating,
            ::GlobalNamespace::IPreviewMediaData* previewMediaData,
            ::System::Collections::Generic::Dictionary_2<::System::ValueTuple_2<::UnityW<::GlobalNamespace::BeatmapCharacteristicSO>, ::GlobalNamespace::BeatmapDifficulty>, ::GlobalNamespace::BeatmapBasicData*>* beatmapBasicData
        );
    private:
        CustomJSONData::CustomLevelInfoSaveDataV2* _customLevelSaveDataV2;
        CustomJSONData::CustomBeatmapLevelSaveDataV4* _customBeatmapLevelSaveDataV4;
        GlobalNamespace::IBeatmapLevelData* _beatmapLevelData;
        std::string _customLevelPath;
};