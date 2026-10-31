#pragma once

#include "System/IO/Path.hpp"
#include "System/IO/File.hpp"
#include "System/String.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/Sprite.hpp"
#include "songcore/shared/SongCore.hpp"
#include "GlobalNamespace/BeatmapLevel.hpp"
#include "song-details/shared/Data/RankedStates.hpp"
#include "song-details/shared/Data/SongDifficulty.hpp"
#include "FilterOptions.hpp"
#include "UI/ViewControllers/SongList.hpp"


namespace BetterSongSearch::Util {
    UnityW<UnityEngine::Sprite> getLocalCoverSync(SongCore::SongLoader::CustomBeatmapLevel* level);


    UnityW<UnityEngine::Sprite> getLocalCoverSync(StringW songHash);

    SongDetailsCache::RankedStates GetTargetedRankLeaderboardService(const SongDetailsCache::SongDifficulty* diff);

    float getStars(const SongDetailsCache::SongDifficulty* diff, SongDetailsCache::RankedStates state);

    float getStars(const SongDetailsCache::SongDifficulty* diff);


    #define PROP_GET(jsonName, varName)                                \
    static auto jsonNameHash_##varName = stringViewHash(jsonName); \
    if (nameHash == (jsonNameHash_##varName))                      \
        return &varName;


    constexpr std::string_view ShortMapDiffNames(std::string_view input) {
        // order variables from most likely to least likely to at least improve branch checks
        // optimization switch idea stolen from Red's SDC wrapper. Good job red 👏
        switch (input.front()) {
            case 'E':
                // ExpertPlus
                if (input.back() == 's') {
                    return "Ex+";
                }
                // Easy
                if (input.size() == 4) {
                    return "E";
                }
                // Expert
                return "Ex";
            case 'N':
                return "N";
            case 'H':
                return "H";
            default:
                return "UNKNOWN";
        }
    }

        #undef PROP_GET

    bool MeetsFilter(const SongDetailsCache::Song* song);
    bool DifficultyCheck(const SongDetailsCache::SongDifficulty* diff, const SongDetailsCache::Song* song);

    using SortFunction = std::function< float (SongDetailsCache::Song const*)>;
    extern std::unordered_map<FilterTypes::SortMode, SortFunction> sortFunctionMap;
}
