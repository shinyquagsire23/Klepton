#pragma once

#include "../macros.hpp"

SERDE_STRUCT(BeatSaver::Models, PlaylistStats,
    BEATSAVER_PLUSPLUS_GETTER_FIELD(float, AverageScore, "avgScore");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, DownVotes, "downVotes");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int64_t, MapperCount, "mapperCount");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(double, MaxNps, "maxNpns");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(double, MaxNpsTwoDP, "maxNpnsTwoDP");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(double, MinNps, "minNpns");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(double, MinNpsTwoDP, "minNpnsTwoDP");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(float, ScoreOneDP, "scoreOneDP");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, TotalDuration, "totalDuration");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, TotalMaps, "totalMaps");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, UpVotes, "upVotes");

)
