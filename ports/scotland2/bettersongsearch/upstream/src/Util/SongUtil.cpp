#include "Util/SongUtil.hpp"

#include "bsml/shared/BSML-Lite/Creation/Image.hpp"
#include "DataHolder.hpp"
#include "logging.hpp"
#include "songcore/shared/SongLoader/CustomBeatmapLevel.hpp"
#include "Util/TextUtil.hpp"

using namespace SongDetailsCache;

namespace BetterSongSearch::Util {
    UnityW<UnityEngine::Sprite> getLocalCoverSync(SongCore::SongLoader::CustomBeatmapLevel* level) {
        if (level == nullptr) {
            return nullptr;
        }

        std::string imageFileName = "";
        {
            auto saveDataV2 = level->get_standardLevelInfoSaveDataV2();
            if (saveDataV2.has_value()) {
                auto name = saveDataV2.value()->get_coverImageFilename();
                imageFileName = static_cast<std::string>(name);
            }
        }
        {
            if (imageFileName.empty()) {
                auto saveData = level->get_beatmapLevelSaveDataV4();
                if (saveData.has_value()) {
                    auto name = saveData.value()->__cordl_internal_get_coverImageFilename();
                    imageFileName = static_cast<std::string>(name);
                }
            }
        }
        if (imageFileName.empty()) {
            DEBUG("No cover image found in level data");
            return nullptr;
        }

        StringW path = System::IO::Path::Combine(level->get_customLevelPath(), imageFileName);

        if (!System::IO::File::Exists(path)) {
            DEBUG("File does not exist");
            return nullptr;
        }

        auto sprite = BSML::Lite::FileToSprite((std::string) path);
        return sprite;
    }

    UnityW<UnityEngine::Sprite> getLocalCoverSync(StringW songHash) {
        auto beatmap = SongCore::API::Loading::GetLevelByHash(std::string(songHash));

        if (beatmap) {
            return getLocalCoverSync(beatmap);
        } else {
            return nullptr;
        }
    }

    // Gets preferred leaderboard for a song difficulty
    SongDetailsCache::RankedStates GetTargetedRankLeaderboardService(SongDetailsCache::SongDifficulty const* diff) {
        auto& rStates = diff->song().rankedStates;

        auto& filterOptions = dataHolder.filterOptionsCache;

        // If song is scoresaber ranked
        if (hasFlags(rStates, RankedStates::ScoresaberRanked) &&
            // And Not Filtering by BeatLeader ranked
            static_cast<FilterTypes::RankedFilter>(dataHolder.filterOptionsCache.rankedType) != FilterTypes::RankedFilter::BeatLeaderRanked &&
            (
                // Beatleader is not preferred leaderboard
                dataHolder.preferredLeaderboard != FilterTypes::PreferredLeaderBoard::BeatLeader ||
                // Song has no BeatLeader rank
                !hasFlags(rStates, RankedStates::BeatleaderRanked) ||
                // Filtering by SS ranked
                static_cast<FilterTypes::RankedFilter>(dataHolder.filterOptionsCache.rankedType) == FilterTypes::RankedFilter::ScoreSaberRanked
            )) {
            return SongDetailsCache::RankedStates::ScoresaberRanked;
        }

        // If song has beatleader rank then return beatleader
        if (hasFlags(rStates, SongDetailsCache::RankedStates::BeatleaderRanked)) {
            return SongDetailsCache::RankedStates::BeatleaderRanked;
        }

        return SongDetailsCache::RankedStates::Unranked;
    }

    float getStars(SongDetailsCache::SongDifficulty const* diff, SongDetailsCache::RankedStates state) {
        if (state == SongDetailsCache::RankedStates::ScoresaberRanked && diff->starsSS > 0) {
            return diff->starsSS;
        }
        if (state == SongDetailsCache::RankedStates::BeatleaderRanked && diff->starsBL > 0) {
            return diff->starsBL;
        }
        return 0;
    }

    float getStars(SongDetailsCache::SongDifficulty const* diff) {
        return getStars(diff, GetTargetedRankLeaderboardService(diff));
    }

    bool MeetsFilter(SongDetailsCache::Song const* song) {
        auto& filterOptions = dataHolder.filterOptionsCache;
        std::string songHash = song->hash();

        if (filterOptions.onlyCuratedMaps) {
            if (!hasFlags(song->uploadFlags, SongDetailsCache::UploadFlags::Curated)) {
                return false;
            }
        }

        if (filterOptions.onlyVerifiedMappers) {
            if (!hasFlags(song->uploadFlags, SongDetailsCache::UploadFlags::VerifiedUploader)) {
                return false;
            }
        }

        if (filterOptions.onlyV3Maps) {
            if (!hasFlags(song->uploadFlags, SongDetailsCache::UploadFlags::HasV3Environment)) {
                return false;
            }
        }

        if (filterOptions._mapStyleBitfield != 0 && (song->tags & filterOptions._mapStyleBitfield) == 0) {
            return false;
        }

        if (filterOptions._mapGenreBitfield != 0 && (song->tags & filterOptions._mapGenreBitfield) == 0) {
            return false;
        }

        if ((song->tags & filterOptions._mapGenreExcludeBitfield) != 0) {
            return false;
        }

        if (!filterOptions.uploaders.empty()) {
            if (std::find(filterOptions.uploaders.begin(), filterOptions.uploaders.end(), removeSpecialCharacter(toLower(song->uploaderName()))) !=
                filterOptions.uploaders.end()) {
                if (filterOptions.uploadersBlackList) {
                    return false;
                }
            } else if (!filterOptions.uploadersBlackList) {
                return false;
            }
        }

        if (song->uploadTimeUnix < filterOptions.minUploadDate) {
            return false;
        }

        float songRating = song->rating();
        if (songRating < filterOptions.minRating) {
            return false;
        }

        if (((int) song->upvotes + (int) song->downvotes) < filterOptions.minVotes) {
            return false;
        }

        // Skip if not needed
        auto localScoreType = static_cast<FilterTypes::LocalScoreFilter>(filterOptions.localScoreType);
        if (localScoreType != FilterTypes::LocalScoreFilter::All) {
            bool hasLocalScore = false;
            if (dataHolder.SongHasScore(songHash)) {
                hasLocalScore = true;
            }
            if (hasLocalScore) {
                if (localScoreType == FilterTypes::LocalScoreFilter::HidePassed) {
                    return false;
                }
            } else {
                if (localScoreType == FilterTypes::LocalScoreFilter::OnlyPassed) {
                    return false;
                }
            }
        }

        if (static_cast<FilterTypes::RankedFilter>(filterOptions.rankedType) != FilterTypes::RankedFilter::ShowAll) {
            // if not the ranked that we want, skip
            if (!hasFlags(song->rankedStates, RANK_MAP.at(static_cast<FilterTypes::RankedFilter>(filterOptions.rankedType)))) {
                return false;
            }
        }

        bool passesDiffFilter = true;

        for (auto const& diff : *song) {
            if (DifficultyCheck(&diff, song)) {
                passesDiffFilter = true;
                break;
            } else {
                passesDiffFilter = false;
            }
        }

        if (!passesDiffFilter) {
            return false;
        }

        if (song->songDurationSeconds < filterOptions.minLength) {
            return false;
        }
        if (song->songDurationSeconds > filterOptions.maxLength) {
            return false;
        }

        // This is the most heavy filter, check it last
        if (static_cast<FilterTypes::DownloadFilter>(filterOptions.downloadType) != FilterTypes::DownloadFilter::All) {
            bool downloaded = SongCore::API::Loading::GetLevelByHash(songHash) != nullptr;
            if (downloaded) {
                if (static_cast<FilterTypes::DownloadFilter>(filterOptions.downloadType) == FilterTypes::DownloadFilter::HideDownloaded) {
                    return false;
                }
            } else {
                if (static_cast<FilterTypes::DownloadFilter>(filterOptions.downloadType) == FilterTypes::DownloadFilter::OnlyDownloaded) {
                    return false;
                }
            }
        }

        return true;
    }

    bool DifficultyCheck(SongDetailsCache::SongDifficulty const* diff, SongDetailsCache::Song const* song) {
        auto const& currentFilter = dataHolder.filterOptionsCache;

        // if all filters are default, skip
        if (currentFilter.isDefaultPreprocessed) {
            return true;
        }

        if (static_cast<FilterTypes::RankedFilter>(currentFilter.rankedType) != FilterTypes::RankedFilter::ShowAll) {
            // if not the ranked that we want, skip
            if (!hasFlags(song->rankedStates, RANK_MAP.at(static_cast<FilterTypes::RankedFilter>(currentFilter.rankedType)))) {
                return false;
            }
        }

        // Min and max stars
        if (currentFilter.maxStars != STAR_FILTER_MAX) {
            if (getStars(diff) > currentFilter.maxStars) {
                return false;
            }
        }
        if (currentFilter.minStars > 0) {
            if (getStars(diff) < currentFilter.minStars) {
                return false;
            }
        }

        if (static_cast<FilterTypes::DifficultyFilter>(currentFilter.difficultyFilter) != FilterTypes::DifficultyFilter::All) {
            if (diff->difficulty != currentFilter.difficultyFilterPreprocessed) {
                return false;
            }
        }

        if (static_cast<FilterTypes::CharFilter>(currentFilter.charFilter) != FilterTypes::CharFilter::All) {
            if (diff->characteristic != currentFilter.charFilterPreprocessed) {
                return false;
            }
        }

        if (diff->njs < currentFilter.minNJS || diff->njs > currentFilter.maxNJS) {
            return false;
        }

        if (static_cast<FilterTypes::Requirement>(currentFilter.modRequirement) != FilterTypes::Requirement::Any) {
            switch (static_cast<FilterTypes::Requirement>(currentFilter.modRequirement)) {
                case FilterTypes::Requirement::Chroma:
                    if (!hasFlags(diff->mods, MapMods::Chroma)) {
                        return false;
                    }
                    break;
                case FilterTypes::Requirement::Cinema:
                    if (!hasFlags(diff->mods, MapMods::Cinema)) {
                        return false;
                    }
                    break;
                case FilterTypes::Requirement::MappingExtensions:
                    if (!hasFlags(diff->mods, MapMods::MappingExtensions)) {
                        return false;
                    }
                    break;
                case FilterTypes::Requirement::NoodleExtensions:
                    if (!hasFlags(diff->mods, MapMods::NoodleExtensions)) {
                        return false;
                    }
                    break;
                case FilterTypes::Requirement::None:
                    if (!((diff->mods & (MapMods::NE | MapMods::ME)) == MapMods::None)) {
                        return false;
                    }
                    break;
                default:
                    break;
            }
        }

        if (song->songDurationSeconds > 0) {
            float nps = (float) diff->notes / (float) song->songDurationSeconds;

            if (nps < currentFilter.minNPS || nps > currentFilter.maxNPS) {
                return false;
            }
        }

        return true;
    }

    using SortFunction = std::function<float(SongDetailsCache::Song const*)>;
    std::unordered_map<FilterTypes::SortMode, SortFunction> sortFunctionMap = {
        {FilterTypes::SortMode::Newest,
         [](SongDetailsCache::Song const* x)  // Newest
         {
             return (x->uploadTimeUnix);
         }},
        {FilterTypes::SortMode::Oldest,
         [](SongDetailsCache::Song const* x)  // Oldest
         {
             return (std::numeric_limits<uint32_t>::max() - x->uploadTimeUnix);
         }},
        {FilterTypes::SortMode::Latest_Ranked,
         [](SongDetailsCache::Song const* x)  // Latest Ranked
         {
             return (hasFlags(x->rankedStates, (SongDetailsCache::RankedStates::BeatleaderRanked | SongDetailsCache::RankedStates::ScoresaberRanked)))
                      ? x->rankedChangeUnix
                      : 0.0f;
         }},
        {FilterTypes::SortMode::Most_Stars,
         [](SongDetailsCache::Song const* x)  // Most Stars
         {
             return x->max([x](auto const& diff) {
                 bool passesFilter = DifficultyCheck(&diff, x);
                 if (passesFilter && (getStars(&diff) > 0)) {
                     return getStars(&diff);
                 } else {
                     return 0.0f;
                 }
             });
         }},
        {FilterTypes::SortMode::Least_Stars,
         [](SongDetailsCache::Song const* x)  // Least Stars
         {
             return 420.0f - x->min([x](auto const& diff) {
                 bool passesFilter = DifficultyCheck(&diff, x);
                 if (passesFilter && (getStars(&diff) > 0)) {
                     return getStars(&diff);
                 } else {
                     return 420.0f;
                 }
             });
         }},
        {FilterTypes::SortMode::Best_rated,
         [](SongDetailsCache::Song const* x)  // Best rated
         {
             return x->rating();
         }},
        {FilterTypes::SortMode::Worst_rated,
         [](SongDetailsCache::Song const* x)  // Worst rated
         {
             return 420.0f - (x->rating() != 0 ? x->rating() : 420.0f);
         }}
    };
}  // namespace BetterSongSearch::Util
