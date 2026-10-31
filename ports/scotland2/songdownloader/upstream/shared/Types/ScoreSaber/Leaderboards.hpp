#pragma once
#include "../TypeMacros.hpp"
#include "Leaderboard.hpp"
#include "Metadata.hpp"

namespace ScoreSaber {
    DECLARE_JSON_STRUCT(Leaderboards) {
        ERROR_CHECK
        GETTER_VALUE(std::vector<ScoreSaber::Leaderboard>, Leaderboards, "leaderboards");
        GETTER_VALUE_OPTIONAL(Metadata_t, Metadata, "metadata");
    };
}
