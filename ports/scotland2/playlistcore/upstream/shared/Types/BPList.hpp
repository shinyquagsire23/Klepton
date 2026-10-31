#pragma once

#include "rapidjson-macros/shared/macros.hpp"

namespace PlaylistCore {
    DECLARE_JSON_STRUCT(Difficulty) {
        NAMED_VALUE(std::string, Characteristic, "characteristic");
        NAMED_VALUE(std::string, Name, "name");
        KEEP_EXTRA_FIELDS;
    };

    DECLARE_JSON_STRUCT(BPSong) {
        NAMED_VALUE_OPTIONAL(std::string, Hash, "hash");
        NAMED_VALUE_DEFAULT(std::string, LevelID, self->TryMakeLevelID(), "levelid");
        NAMED_VALUE_OPTIONAL(std::string, SongName, "songName");
        NAMED_VALUE_OPTIONAL(std::string, Key, "key");
        NAMED_VECTOR_OPTIONAL(PlaylistCore::Difficulty, Difficulties, "difficulties");
        KEEP_EXTRA_FIELDS;

       private:
        std::string TryMakeLevelID() {
            if (!Hash)
                throw JSONException("Song had no ID and no hash");
            return "custom_level_" + *Hash;
        }
    };

    DECLARE_JSON_STRUCT(CustomData) {
        NAMED_VALUE_OPTIONAL(std::string, SyncURL, "syncURL");
        KEEP_EXTRA_FIELDS;
    };

    DECLARE_JSON_STRUCT(BPList) {
        NAMED_VALUE(std::string, PlaylistTitle, "playlistTitle");
        NAMED_VALUE_OPTIONAL(std::string, PlaylistAuthor, "playlistAuthor");
        NAMED_VALUE_OPTIONAL(std::string, PlaylistDescription, "playlistDescription");
        NAMED_VECTOR(PlaylistCore::BPSong, Songs, "songs");
        NAMED_VALUE_OPTIONAL(std::string, ImageString, NAME_OPTS("imageString", "image"));
        NAMED_VALUE_OPTIONAL(PlaylistCore::CustomData, CustomData, "customData");
        KEEP_EXTRA_FIELDS;

        DESERIALIZE_FUNCTION(SetSyncURL) {
            if (jsonValue.HasMember("downloadURL") && jsonValue["downloadURL"].IsString()) {
                if (!CustomData.has_value())
                    CustomData.emplace();
                CustomData->SyncURL = jsonValue["downloadURL"].GetString();
                jsonValue.RemoveMember("downloadURL");
            }
        };
    };
}
