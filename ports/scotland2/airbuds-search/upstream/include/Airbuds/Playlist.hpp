#pragma once

#include <string>

namespace airbuds {

struct Playlist {
    std::string id;
    std::string name;
    std::string tracksUrl;
    std::string imageUrl;
    size_t totalItemCount;
};

std::string to_string(const Playlist& playlist);

}// namespace airbuds
