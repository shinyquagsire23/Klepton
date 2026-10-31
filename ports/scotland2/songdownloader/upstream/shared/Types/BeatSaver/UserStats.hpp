#pragma once
#include "../TypeMacros.hpp"
#include "UserDiffStats.hpp"

namespace BeatSaver {
    DECLARE_JSON_STRUCT(UserStats) {
        ERROR_CHECK
        GETTER_VALUE_DEFAULT(int, TotalUpvotes, 0, "totalUpvotes");
        GETTER_VALUE_DEFAULT(int, TotalDownvotes, 0, "totalDownvotes");
        GETTER_VALUE_DEFAULT(int, TotalMaps, 0, "totalMaps");
        GETTER_VALUE_DEFAULT(int, RankedMaps, 0, "rankedMaps");
        GETTER_VALUE_DEFAULT(float, AvgBpm, 0.0f, "avgBpm");
        GETTER_VALUE_DEFAULT(float, AvgScore, 0.0f, "avgScore");
        GETTER_VALUE_DEFAULT(float, AvgDuration, 0.0f, "avgDuration");
        GETTER_VALUE_OPTIONAL(std::string, FirstUpload, "firstUpload");
        GETTER_VALUE_OPTIONAL(std::string, LastUpload, "lastUpload");
        GETTER_VALUE(BeatSaver::UserDiffStats, DiffStats, "diffStats");
    };
}
