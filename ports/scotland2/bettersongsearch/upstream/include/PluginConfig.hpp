#pragma once

#include <string>
#include <unordered_map>

#include "config-utils/shared/config-utils.hpp"
#include "song-details/shared/Data/MapCharacteristic.hpp"
#include "song-details/shared/Data/MapDifficulty.hpp"
#include "song-details/shared/Data/RankedStates.hpp"
#include "song-details/shared/Data/SongDifficulty.hpp"

static inline float const SONG_LENGTH_FILTER_MAX = 15.0f;
static inline float const STAR_FILTER_MAX = 18.0f;
static inline float const NJS_FILTER_MAX = 25.0f;
static inline float const NPS_FILTER_MAX = 12.0f;
static inline int64_t const BEATSAVER_EPOCH = 1525136400;
static inline std::chrono::system_clock::time_point const BEATSAVER_EPOCH_TIME_POINT{std::chrono::seconds(BEATSAVER_EPOCH)};

static inline std::vector<std::string> const MAP_STYLES_OPTIONS = {"Any", "accuracy", "balanced", "challenge", "dance", "fitness", "speed", "tech"};

namespace FilterTypes {
    enum class DownloadFilter { All, OnlyDownloaded, HideDownloaded };
    enum class LocalScoreFilter { All, HidePassed, OnlyPassed };
    enum class RankedFilter { ShowAll, ScoreSaberRanked, BeatLeaderRanked, ScoreSaberQualified, BeatLeaderQualified };
    enum class DifficultyFilter { All, Easy, Normal, Hard, Expert, ExpertPlus };
    enum class CharFilter {
        All,
        Custom,
        Standard,
        OneSaber,
        NoArrows,
        NinetyDegrees,
        ThreeSixtyDegrees,
        LightShow,
        Lawless,
    };

    enum class Requirement { Any, NoodleExtensions, MappingExtensions, Chroma, Cinema, None };

    enum class SortMode { Newest, Oldest, Latest_Ranked, Most_Stars, Least_Stars, Best_rated, Worst_rated };

    enum class PreferredLeaderBoard { ScoreSaber = 0, BeatLeader = 1 };

}  // namespace FilterTypes

// Map for characteristics
static std::unordered_map<FilterTypes::CharFilter, SongDetailsCache::MapCharacteristic> const CHARACTERISTIC_MAP = {
    {FilterTypes::CharFilter::Custom, SongDetailsCache::MapCharacteristic::Custom},
    {FilterTypes::CharFilter::Standard, SongDetailsCache::MapCharacteristic::Standard},
    {FilterTypes::CharFilter::OneSaber, SongDetailsCache::MapCharacteristic::OneSaber},
    {FilterTypes::CharFilter::NoArrows, SongDetailsCache::MapCharacteristic::NoArrows},
    {FilterTypes::CharFilter::NinetyDegrees, SongDetailsCache::MapCharacteristic::NinetyDegree},
    {FilterTypes::CharFilter::ThreeSixtyDegrees, SongDetailsCache::MapCharacteristic::ThreeSixtyDegree},
    {FilterTypes::CharFilter::LightShow, SongDetailsCache::MapCharacteristic::LightShow},
    {FilterTypes::CharFilter::Lawless, SongDetailsCache::MapCharacteristic::Lawless}
};

// Map for difficulties
static std::unordered_map<FilterTypes::DifficultyFilter, SongDetailsCache::MapDifficulty> const DIFFICULTY_MAP = {
    {FilterTypes::DifficultyFilter::Easy, SongDetailsCache::MapDifficulty::Easy},
    {FilterTypes::DifficultyFilter::Normal, SongDetailsCache::MapDifficulty::Normal},
    {FilterTypes::DifficultyFilter::Hard, SongDetailsCache::MapDifficulty::Hard},
    {FilterTypes::DifficultyFilter::Expert, SongDetailsCache::MapDifficulty::Expert},
    {FilterTypes::DifficultyFilter::ExpertPlus, SongDetailsCache::MapDifficulty::ExpertPlus}
};

// Map for ranked states
static std::unordered_map<FilterTypes::RankedFilter, SongDetailsCache::RankedStates> const RANK_MAP = {
    {FilterTypes::RankedFilter::ScoreSaberRanked, SongDetailsCache::RankedStates::ScoresaberRanked},
    {FilterTypes::RankedFilter::BeatLeaderRanked, SongDetailsCache::RankedStates::BeatleaderRanked},
    {FilterTypes::RankedFilter::ScoreSaberQualified, SongDetailsCache::RankedStates::ScoresaberQualified},
    {FilterTypes::RankedFilter::BeatLeaderQualified, SongDetailsCache::RankedStates::BeatleaderQualified}
};

// Map for preferred leaderboard
static std::unordered_map<std::string, FilterTypes::PreferredLeaderBoard> const LEADERBOARD_MAP = {
    {"Scoresaber", FilterTypes::PreferredLeaderBoard::ScoreSaber}, {"Beatleader", FilterTypes::PreferredLeaderBoard::BeatLeader}
};

DECLARE_CONFIG(PluginConfig) {
    CONFIG_VALUE(ReturnToBSS, bool, "Return to BSS from Solo", true);
    CONFIG_VALUE(LoadSongPreviews, bool, "Load song previews", true);
    CONFIG_VALUE(SmallerFontSize, bool, "Smaller font size", true);
    CONFIG_VALUE(SortMode, int, "Sort Mode", 0);
    CONFIG_VALUE(DownloadType, int, "Download Type", 0);
    CONFIG_VALUE(LocalScoreType, int, "Local Score Type", 0);
    CONFIG_VALUE(MinLength, float, "Minimum Song Length", 0);
    CONFIG_VALUE(MaxLength, float, "Maximum Song Length", 900);
    CONFIG_VALUE(MinNJS, float, "Minimum Note Jump Speed", 0);
    CONFIG_VALUE(MaxNJS, float, "Maximum Note Jump Speed", NJS_FILTER_MAX);
    CONFIG_VALUE(MinNPS, float, "Minimum Notes Per Second", 0);
    CONFIG_VALUE(MaxNPS, float, "Maximum Note Per Second", NPS_FILTER_MAX);
    CONFIG_VALUE(RankedType, int, "Ranked Type", 0);
    CONFIG_VALUE(MinStars, float, "Minimum Ranked Stars", 0);
    CONFIG_VALUE(MaxStars, float, "Maximum Ranked Stars", STAR_FILTER_MAX);
    CONFIG_VALUE(MinUploadDateInMonths, int, "Minimum Upload Date In Months", 0);  // TEMPORARY UNTIL I FIX CONVERTING UNIX -> MONTHS SINCE BEAT SAVER
    CONFIG_VALUE(MinUploadDate, int, "Minimum Upload Date", BEATSAVER_EPOCH);
    CONFIG_VALUE(MinRating, float, "Minimum Rating", 0);
    CONFIG_VALUE(MinVotes, int, "Minimum Votes", 0);
    CONFIG_VALUE(DifficultyType, int, "Difficulty Type", 0);
    CONFIG_VALUE(CharacteristicType, int, "Characteristic Type", 0);
    CONFIG_VALUE(Uploaders, std::string, "Uploaders filter", "");
    CONFIG_VALUE(RequirementType, int, "Requirement Type", 0);
    CONFIG_VALUE(PreferredLeaderboard, std::string, "Preferred Leaderboard", "Scoresaber");
    CONFIG_VALUE(OnlyVerifiedMappers, bool, "Only Verified Mappers", false);
    CONFIG_VALUE(OnlyCuratedMaps, bool, "Only Curated Maps", false);
    CONFIG_VALUE(OnlyV3Maps, bool, "Only V3 Maps", false);
    CONFIG_VALUE(MapStyleString, std::string, "Map Style", "All");
    CONFIG_VALUE(MapGenreString, std::string, "Map Genres", "");
    CONFIG_VALUE(MapGenreExcludeString, std::string, "Map Genre Exclude", "");
};
