#include "Util/Debug.hpp"

#include <string>

#include "DataHolder.hpp"
#include "logging.hpp"
#include "Util/SongUtil.hpp"

std::string BetterSongSearch::Util::SortToString(int sortMode) {
    switch (sortMode) {
        case 0:
            return "Newest";
        case 1:
            return "Oldest";
        case 2:
            return "Latest_Ranked";
        case 3:
            return "Most_Stars";
        case 4:
            return "Least_Stars";
        case 5:
            return "Best_rated";
        case 6:
            return "Worst_rated";
        default:
            return "Unknown";
    }
}

std::string BetterSongSearch::Util::RequirementTypeToString(int requirementType) {
    switch (requirementType) {
        case 0:
            return "Any";
        case 1:
            return "NoodleExtensions";
        case 2:
            return "MappingExtensions";
        case 3:
            return "Chroma";
        case 4:
            return "Cinema";
        case 5:
            return "None";
        default:
            return "Unknown";
    }
}

std::string BetterSongSearch::Util::CharFilterTypeToString(int charFilterType) {
    switch (charFilterType) {
        case 0:
            return "All";
        case 1:
            return "Custom";
        case 2:
            return "Standard";
        case 3:
            return "OneSaber";
        case 4:
            return "NoArrows";
        case 5:
            return "NinetyDegrees";
        case 6:
            return "ThreeSixtyDegrees";
        case 7:
            return "LightShow";
        case 8:
            return "Lawless";
        default:
            return "Unknown";
    }
}

std::string BetterSongSearch::Util::DifficultyFilterTypeToString(int diffFilterType) {
    switch (diffFilterType) {
        case 0:
            return "All";
        case 1:
            return "Easy";
        case 2:
            return "Normal";
        case 3:
            return "Hard";
        case 4:
            return "Expert";
        case 5:
            return "ExpertPlus";
        default:
            return "Unknown";
    }
}

std::string BetterSongSearch::Util::RankedFilterTypeToString(int rankedFilterType) {
    switch (rankedFilterType) {
        case 0:
            return "ShowAll";
        case 1:
            return "ScoreSaberRanked";
        case 2:
            return "BeatLeaderRanked";
        case 3:
            return "ScoreSaberQualified";
        case 4:
            return "BeatLeaderQualified";
        default:
            return "Unknown";
    }
}

std::string BetterSongSearch::Util::LocalScoreFilterTypeToString(int localScoreFilterType) {
    switch (localScoreFilterType) {
        case 0:
            return "All";
        case 1:
            return "HidePassed";
        case 2:
            return "OnlyPassed";
        default:
            return "Unknown";
    }
}

std::string BetterSongSearch::Util::DownloadFilterTypeToString(int downloadFilterType) {
    switch (downloadFilterType) {
        case 0:
            return "All";
        case 1:
            return "HidePassed";
        case 2:
            return "OnlyPassed";
        default:
            return "Unknown";
    }
}

// Logs song info for checking sort and options
void BetterSongSearch::Util::LogSongInfo(SongDetailsCache::Song const* song) {
    // DEBUG info
    auto struct1DiffVec = song->rankedChangeUnix;
    DEBUG(
        "Name: {}, bpm {}, upvotes: {}, downvotes: {}, diffCount: {}, rankedStatus: {}, rankedStates: {} ",
        (std::string) song->songName(),
        song->bpm,
        song->upvotes,
        song->downvotes,
        song->diffCount,
        song->rankedStatus,
        song->rankedStates
    );
    DEBUG("Ranked time: {}", struct1DiffVec);
    for (SongDetailsCache::SongDifficulty const& diff : *song) {
        DEBUG(
            "Diff: {}, Ranked: {}, starsSS: {}, starsBL: {}, notes: {}, characteristic: {}, njs: {}, mods: {} ",
            diff.difficulty,
            diff.ranked(),
            diff.starsSS,
            diff.starsBL,
            diff.notes,
            diff.characteristic,
            diff.njs,
            diff.mods
        );
    }
    DEBUG("Sort score: {}", sortFunctionMap.at(dataHolder.currentSort)(song));
}

// Prints to the provided buffer a nice number of bytes (KB, MB, GB, etc)
std::string BetterSongSearch::Util::pretty_bytes(int64_t bytes) {
    std::string prefix = "";

    if (bytes < 0) {
        prefix = "-";
    }
    char const* suffixes[7];
    suffixes[0] = "B";
    suffixes[1] = "KB";
    suffixes[2] = "MB";
    suffixes[3] = "GB";
    suffixes[4] = "TB";
    suffixes[5] = "PB";
    suffixes[6] = "EB";
    uint s = 0;  // which suffix to use
    double count = abs(bytes);
    while (count >= 1024 && s < 7) {
        s++;
        count /= 1024;
    }
    if (count - floor(count) == 0.0) {
        return fmt::format("{}{} {}", prefix, (int) count, suffixes[s]);
    } else {
        return fmt::format("{}{:.2f} {}", prefix, count, suffixes[s]);
    }
}
