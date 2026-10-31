#pragma once
#include "../TypeMacros.hpp"

namespace ScoreSaber {
    DECLARE_JSON_STRUCT(Difficulty_t) {
        ERROR_CHECK
        GETTER_VALUE(int, LeaderboardId, "leaderboardId");
        GETTER_VALUE(int, Difficulty, "difficulty");
        GETTER_VALUE(std::string, GameMode, "gameMode");
        GETTER_VALUE(std::string, DifficultyRaw, "difficultyRaw");
    };
}
