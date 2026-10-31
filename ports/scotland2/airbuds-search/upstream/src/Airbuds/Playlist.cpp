#include "Airbuds/Playlist.hpp"

namespace airbuds {

std::string airbuds::to_string(const Playlist& playlist) {
    std::string result = "{ id: \"" + playlist.id + "\", name: \"" + playlist.name + "\" }";
    return result;
}

}// namespace airbuds
