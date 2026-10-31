#pragma once

#include "../macros.hpp"
#include "UserStats.hpp"

SERDE_STRUCT(BeatSaver::Models, UserDetail,
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, Id, "id");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, Username, "name");
    BEATSAVER_PLUSPLUS_GETTER_FIELD_OPTIONAL(std::string, Hash, "hash");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, AvatarURL, "avatar");
    BEATSAVER_PLUSPLUS_GETTER_FIELD_OPTIONAL(bool, Testplay, "testplay");
    BEATSAVER_PLUSPLUS_GETTER_FIELD_OPTIONAL(UserStats, Stats, "stats");

    public:
        void GetAvatarImageAsync(std::function<void(std::optional<std::vector<uint8_t>>)>) const;
        std::optional<std::vector<uint8_t>> GetAvatarImage() const;
)
