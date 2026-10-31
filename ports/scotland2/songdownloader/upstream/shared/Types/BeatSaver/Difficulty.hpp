#pragma once
#include "../TypeMacros.hpp"
#include "ParitySummary.hpp"

namespace BeatSaver {
    DECLARE_JSON_STRUCT(BeatmapDifficulty) {
        ERROR_CHECK
        GETTER_VALUE(float, NJS, "njs");
        GETTER_VALUE(float, Offset, "offset");
        GETTER_VALUE(int, Notes, "notes");
        GETTER_VALUE(int, Bombs, "bombs");
        GETTER_VALUE(int, Obstacles, "obstacles");
        GETTER_VALUE(double, NPS, "nps");
        GETTER_VALUE(double, Length, "length");
        GETTER_VALUE(std::string, Characteristic, "characteristic"); // Enum string [ Standard, OneSaber, NoArrows, _90Degree, _360Degree, Lightshow, Lawless ] for characteristic
        GETTER_VALUE(std::string, Difficulty, "difficulty"); // Enum string [ Easy, Normal, Hard, Expert, ExpertPlus ] for diffculty
        GETTER_VALUE(int, Events, "events");
        GETTER_VALUE(bool, Chroma, "chroma");
        GETTER_VALUE(bool, ME, "me");
        GETTER_VALUE(bool, NE, "ne");
        GETTER_VALUE(bool, Cinema, "cinema");
        GETTER_VALUE(double, Seconds, "seconds");
        GETTER_VALUE(BeatSaver::BeatmapParitySummary, ParitySummary, "paritySummary");
        GETTER_VALUE_OPTIONAL(float, Stars, "stars"); // No idea what that value really is for, but it sure exists on some maps
    };
}
