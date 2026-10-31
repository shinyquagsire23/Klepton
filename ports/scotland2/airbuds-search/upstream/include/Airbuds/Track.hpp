#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "Artist.hpp"
#include "Album.hpp"

namespace airbuds {

struct Track {
    std::string id;
    std::string name;
    std::vector<Artist> artists;
    Album album;

    auto operator<=>(const Track&) const = default;
};

struct PlaylistTrack : public Track {
    std::string dateAdded;

    std::chrono::milliseconds dateAdded_;
};

std::string to_string(const Track& track);

}// namespace airbuds
