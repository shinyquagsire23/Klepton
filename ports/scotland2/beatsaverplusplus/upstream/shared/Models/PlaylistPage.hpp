#pragma once

#include "../macros.hpp"
#include "./BeatmapWithOrder.hpp"
#include "./Playlist.hpp"

SERDE_STRUCT(BeatSaver::Models, PlaylistPage,
    BEATSAVER_PLUSPLUS_GETTER_FIELD(std::vector<BeatmapWithOrder>, Maps, "maps");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(struct Playlist, Playlist, "playlist");
);
