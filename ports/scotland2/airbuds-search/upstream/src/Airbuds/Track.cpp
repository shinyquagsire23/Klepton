#include "Airbuds/Track.hpp"

namespace airbuds {

std::string airbuds::to_string(const Track& track) {
    // Example: simple JSON-like string (or use RapidJSON serialization here)
    std::string result = "{ id: \"" + track.id + "\", name: \"" + track.name + "\", artists: [";
    for (size_t i = 0; i < track.artists.size(); ++i) {
        result += "\"" + track.artists[i].name + "\"";
        if (i + 1 != track.artists.size()) result += ", ";
    }
    result += "] }";
    return result;
}

}// namespace airbuds
