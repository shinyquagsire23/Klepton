#pragma once

#include <string>

#include "song-details/shared/Data/MapCharacteristic.hpp"
#include "song-details/shared/Data/MapDifficulty.hpp"
#include "song-details/shared/Data/Song.hpp"

namespace BetterSongSearch::Util {
    std::string SortToString(int sortMode);

    std::string RequirementTypeToString(int requirementType);

    std::string CharFilterTypeToString(int charFilterType);

    std::string DifficultyFilterTypeToString(int diffFilterType);

    std::string RankedFilterTypeToString(int rankedFilterType);
    std::string LocalScoreFilterTypeToString(int localScoreFilterType);

    std::string DownloadFilterTypeToString(int downloadFilterType);

    // Logs song info for checking sort and options
    void LogSongInfo(SongDetailsCache::Song const* song);

    // Prints to the provided buffer a nice number of bytes (KB, MB, GB, etc)
    std::string pretty_bytes(int64_t bytes);
}  // namespace BetterSongSearch::Util
