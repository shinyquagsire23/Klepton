#pragma once
#include "../TypeMacros.hpp"
#include <vector>

namespace BeatSaver {
    DECLARE_JSON_STRUCT(Metadata) {
        ERROR_CHECK
        GETTER_VALUE(float, BPM, "bpm");
        GETTER_VALUE(int, Duration, "duration");
        GETTER_VALUE(std::string, SongName, "songName");
        GETTER_VALUE(std::string, SongSubName, "songSubName");
        GETTER_VALUE(std::string, SongAuthorName, "songAuthorName");
        GETTER_VALUE(std::string, LevelAuthorName, "levelAuthorName");
    };
}
