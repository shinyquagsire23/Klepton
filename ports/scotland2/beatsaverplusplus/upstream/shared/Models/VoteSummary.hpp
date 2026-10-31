#pragma once

#include "../macros.hpp"

SERDE_STRUCT(BeatSaver::Models, VoteSummary,
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, DownVotes, "downVotes");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, UpVotes, "upVotes");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, Hash, "hash");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, Key, "key64");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, MapId, "mapId");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(double, Score, "score");
)
