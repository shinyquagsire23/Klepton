#pragma once

#include "Utils.hpp"
#include "Configuration.hpp"

namespace AirbudsSearch {

struct CustomSongFilter {

    CustomSongFilter() : includeDownloadedSongs_(true) {
        const std::string difficulty = getConfig().config["filter"]["difficulty"].GetString();
        if (difficulty != "Any") {
            difficulties_.push_back(Utils::getMapDifficultyFromString(difficulty));
        }
    }

    std::vector<SongDetailsCache::MapDifficulty> difficulties_;
    bool includeDownloadedSongs_ = true;
};

}
