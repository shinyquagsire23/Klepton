#pragma once
#include "../TypeMacros.hpp"

namespace BeatSaver {
    DECLARE_JSON_STRUCT(Stats) {
        ERROR_CHECK
        GETTER_VALUE_DEFAULT(int, Plays, -1, "plays");
        GETTER_VALUE_DEFAULT(int, Downloads, -1, "downloads");
        GETTER_VALUE(int, UpVotes, "upvotes");
        GETTER_VALUE(int, DownVotes, "downvotes");
        GETTER_VALUE(float, Score, "score");
    };
}
