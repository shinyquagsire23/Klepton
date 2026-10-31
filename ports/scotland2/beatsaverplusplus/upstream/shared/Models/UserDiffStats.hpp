#pragma once

#include "../macros.hpp"

SERDE_STRUCT(BeatSaver::Models, UserDiffStats,
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, Total, "total");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, Easy, "easy");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, Normal, "normal");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, Hard, "hard");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, Expert, "expert");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, ExpertPlus, "expertPlus");
)
