#pragma once

#include "../macros.hpp"
#include "./UserDiffStats.hpp"

SERDE_STRUCT(BeatSaver::Models, UserStats,
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, TotalUpvotes, "totalUpvotes");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, TotalDownvotes, "totalDownvotes");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, TotalMaps, "totalMaps");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, RankedMaps, "rankedMaps");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(float, AvgBpm, "avgBpm");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(float, AvgScore, "avgScore");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(float, AvgDuration, "avgDuration");
    BEATSAVER_PLUSPLUS_GETTER_FIELD_OPTIONAL(std::string, FirstUpload, "firstUpload");
    BEATSAVER_PLUSPLUS_GETTER_FIELD_OPTIONAL(std::string, LastUpload, "lastUpload");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(UserDiffStats, DiffStats, "diffStats");
)
