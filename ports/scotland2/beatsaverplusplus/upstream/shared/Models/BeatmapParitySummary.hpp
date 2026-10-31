#pragma once

#include "../macros.hpp"

SERDE_STRUCT(BeatSaver::Models, BeatmapParitySummary,
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, Errors, "errors");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, Warns, "warns");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, Resets, "resets");
);
