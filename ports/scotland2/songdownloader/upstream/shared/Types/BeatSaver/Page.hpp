#pragma once
#include "../TypeMacros.hpp"
#include "Beatmap.hpp"
#include "User.hpp"

#include <vector>

namespace BeatSaver {
    DECLARE_JSON_STRUCT(Page) {
        ERROR_CHECK
        // All values are optional, as it is possible it will return no beatmap and just a user and also possible to just return a beatmap and no user as well as having both
        // Returns beatmap results for searchQuery term, can be empty
        GETTER_VALUE(std::vector<BeatSaver::Beatmap>, Docs, "docs");
        // Returns UserDetail for a Username with the searchQuery term, or std::nullopt if none is found.
        GETTER_CLASS_OPTIONAL(BeatSaver::UserDetail, User, "user");
    };
}
