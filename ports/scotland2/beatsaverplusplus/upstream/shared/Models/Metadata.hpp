#pragma once

#include "../macros.hpp"

SERDE_STRUCT(BeatSaver::Models, Metadata,
    BEATSAVER_PLUSPLUS_GETTER_FIELD(float, BPM, "bpm");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, Duration, "duration");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, SongName, "songName");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, SongSubName, "songSubName");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, SongAuthorName, "songAuthorName");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, LevelAuthorName, "levelAuthorName");
);
