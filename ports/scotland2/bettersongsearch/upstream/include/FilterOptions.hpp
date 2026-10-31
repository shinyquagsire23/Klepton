#pragma once

#include <tuple>
#include <vector>

#include "PluginConfig.hpp"
#include "rapidjson-macros/shared/macros.hpp"

namespace BetterSongSearch {
    DECLARE_JSON_STRUCT(FilterProfile) {
        VALUE_DEFAULT(int, downloadType, (int) FilterTypes::DownloadFilter::All);
        VALUE_DEFAULT(int, localScoreType, (int) FilterTypes::LocalScoreFilter::All);
        VALUE_DEFAULT(int, rankedType, (int) FilterTypes::RankedFilter::ShowAll);
        VALUE_DEFAULT(int, difficultyFilter, (int) FilterTypes::DifficultyFilter::All);
        VALUE_DEFAULT(int, charFilter, (int) FilterTypes::CharFilter::All);
        VALUE_DEFAULT(int, modRequirement, (int) FilterTypes::Requirement::Any);

        VALUE_DEFAULT(float, minLength, 0);
        VALUE_DEFAULT(float, maxLength, 900);
        VALUE_DEFAULT(float, minNJS, 0);
        VALUE_DEFAULT(float, maxNJS, NJS_FILTER_MAX);
        VALUE_DEFAULT(float, minNPS, 0);
        VALUE_DEFAULT(float, maxNPS, NPS_FILTER_MAX);

        VALUE_DEFAULT(float, minStars, 0);
        VALUE_DEFAULT(float, maxStars, STAR_FILTER_MAX);
        VALUE_DEFAULT(int, minUploadDate, BEATSAVER_EPOCH);
        VALUE_DEFAULT(int, minUploadDateInMonths, 0);
        VALUE_DEFAULT(float, minRating, 0);
        VALUE_DEFAULT(int, minVotes, 0);

        VECTOR_DEFAULT(std::string, uploaders, {});
        VALUE_DEFAULT(bool, uploadersBlackList, false);

        VALUE_DEFAULT(bool, onlyCuratedMaps, false);
        VALUE_DEFAULT(bool, onlyVerifiedMappers, false);
        VALUE_DEFAULT(bool, onlyV3Maps, false);

        VALUE_DEFAULT(std::string, mapStyleString, "All");
        VALUE_DEFAULT(std::string, mapGenreString, "");
        VALUE_DEFAULT(std::string, mapGenreExcludeString, "");

       public:
        bool IsEqual(FilterProfile const& other) const;

        // Because RapidJSON does not support enums... we have to make methods to convert them to/from int
        FilterTypes::DownloadFilter getDownloadType() {
            return (FilterTypes::DownloadFilter) downloadType;
        };
        FilterTypes::LocalScoreFilter getLocalScoreType() {
            return (FilterTypes::LocalScoreFilter) localScoreType;
        };
        FilterTypes::RankedFilter getRankedType() {
            return (FilterTypes::RankedFilter) rankedType;
        };
        FilterTypes::DifficultyFilter getDifficultyFilter() {
            return (FilterTypes::DifficultyFilter) difficultyFilter;
        };
        FilterTypes::CharFilter getCharFilter() {
            return (FilterTypes::CharFilter) charFilter;
        };
        FilterTypes::Requirement getModRequirement() {
            return (FilterTypes::Requirement) modRequirement;
        };
        void setDownloadType(FilterTypes::DownloadFilter value) {
            downloadType = (int) value;
        };
        void setLocalScoreType(FilterTypes::LocalScoreFilter value) {
            localScoreType = (int) value;
        };
        void setRankedType(FilterTypes::RankedFilter value) {
            rankedType = (int) value;
        };
        void setDifficultyFilter(FilterTypes::DifficultyFilter value) {
            difficultyFilter = (int) value;
        };
        void setCharFilter(FilterTypes::CharFilter value) {
            charFilter = (int) value;
        };
        void setModRequirement(FilterTypes::Requirement value) {
            modRequirement = (int) value;
        };

        // This value is valid only if the related filter is not set to "All" and recalculated
        SongDetailsCache::MapCharacteristic charFilterPreprocessed = SongDetailsCache::MapCharacteristic::Custom;
        // This value is valid only if the related filter is not set to "All" and recalculated
        SongDetailsCache::MapDifficulty difficultyFilterPreprocessed = SongDetailsCache::MapDifficulty::Easy;

        uint64_t _mapStyleBitfield = 0;
        uint64_t _mapGenreBitfield = 0;
        uint64_t _mapGenreExcludeBitfield = 0;

        bool isDefaultPreprocessed = true;

        // @brief Checks if the profile is the default profile (no filters)
        bool IsDefault();

        // @brief Recalculates preprocessed values (variables that are used in search)
        void RecalculatePreprocessedValues();

        // @brief Loads the profile from the mod config
        void LoadFromConfig();

        // @brief Saves the profile to the mod config
        void SaveToConfig();

        void PrintToDebug();

        // @brief Counts the included and excluded tags in the profile
        // @return A tuple with the count of included and excluded tags
        std::tuple<int, int> CountTags();

        // @brief Saves the profile to a preset
        // @param presetName The name of the preset to save
        // @return True if the preset was saved successfully
        bool SaveToPreset(std::string presetName) const;

        // @brief Deletes a preset
        // @param presetName The name of the preset to delete
        // @return True if the preset was deleted successfully
        static bool DeletePreset(std::string presetName);

        // @brief Gets a list of all available presets
        // @return A list of all available presets
        static std::vector<std::string> GetPresetList();

        // @brief Loads the profile from a preset
        // @param presetName The name of the preset to load
        // @return The loaded profile
        static std::optional<FilterProfile> LoadFromPreset(std::string presetName);

        void CopyFrom(FilterProfile const& other);
    };
}  // namespace BetterSongSearch
