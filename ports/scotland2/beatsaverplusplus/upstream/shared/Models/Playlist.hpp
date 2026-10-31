#pragma once

#include "../macros.hpp"
#include "./UserDetail.hpp"
#include "./PlaylistStats.hpp"

// TODO: check if there are optional fields
SERDE_STRUCT(BeatSaver::Models, Playlist,
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, CreatedAt, "createdAt"); // datetime
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, CuratedAt, "curatedAt"); // datetime
    BEATSAVER_PLUSPLUS_GETTER_FIELD(UserDetail, Curator, "curator");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, DeletedAt, "deletedAt"); // datetime
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, Description, "description");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, DownloadURL, "downloadURL");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, Name, "name");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(UserDetail, Owner, "owner");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(int, PlaylistID, "playlistId");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, PlaylistImage, "playlistImage"); // base64 image
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, PlaylistImage512, "playlistImage512"); // base512 image
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, SongsChangedAt, "songsChangedAt"); // datetime
    BEATSAVER_PLUSPLUS_GETTER_FIELD(PlaylistStats, Stats, "stats");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, Type, "type"); // string enum, Private, Public, System, Search
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::string, UpdatedAt, "updatedAt"); // datetime
);
