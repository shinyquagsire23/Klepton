#pragma once

#include "../macros.hpp"
#include "./Beatmap.hpp"

SERDE_STRUCT(BeatSaver::Models, BeatmapWithOrder,
    BEATSAVER_PLUSPLUS_GETTER_FIELD(Beatmap, Map, "map");
    BEATSAVER_PLUSPLUS_GETTER_FIELD(float, Order, "order");
);
