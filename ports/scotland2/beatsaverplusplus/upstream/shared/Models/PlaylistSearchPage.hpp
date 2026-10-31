#pragma once

#include "../macros.hpp"
#include "./Playlist.hpp"

SERDE_STRUCT(BeatSaver::Models, PlaylistSearchPage,
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::vector<Playlist>, Docs, "docs");
);
