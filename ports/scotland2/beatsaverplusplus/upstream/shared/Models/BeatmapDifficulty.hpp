#pragma once

#include "../macros.hpp"
#include "./BeatmapParitySummary.hpp"

SERDE_STRUCT(BeatSaver::Models, BeatmapDifficulty,
    BEATSAVER_PLUSPLUS_GETTER_FIELD(float, NJS, "njs");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(float, Offset, "offset");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, Notes, "notes");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, Bombs, "bombs");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, Obstacles, "obstacles");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(double, NPS, "nps");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(double, Length, "length");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, Characteristic, "characteristic");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, Difficulty, "difficulty");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, Events, "events");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(bool, Chroma, "chroma");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(bool, ME, "me");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(bool, NE, "ne");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(bool, Cinema, "cinema");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(double, Seconds, "seconds");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(BeatmapParitySummary, ParitySummary, "paritySummary");
    BEATSAVER_PLUSPLUS_GETTER_FIELD_OPTIONAL(float, Stars, "stars");
);
