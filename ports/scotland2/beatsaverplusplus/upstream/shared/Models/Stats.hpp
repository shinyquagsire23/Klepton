#pragma once

#include "../macros.hpp"

SERDE_STRUCT(BeatSaver::Models, Stats,
    BEATSAVER_PLUSPLUS_GETTER_FIELD_DEFAULT(int, Plays, -1, "plays");
    BEATSAVER_PLUSPLUS_GETTER_FIELD_DEFAULT(int, Downloads, -1, "downloads");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, UpVotes, "upvotes");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, DownVotes, "downvotes");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(float, Score, "score");
);
