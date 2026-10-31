#pragma once
#include "../TypeMacros.hpp"

namespace BeatSaver {
    DECLARE_JSON_STRUCT(BeatmapParitySummary) {
        ERROR_CHECK
        GETTER_VALUE(int, Errors, "errors");
        GETTER_VALUE(int, Warns, "warns");
        GETTER_VALUE(int, Resets, "resets");
        
    };
}
