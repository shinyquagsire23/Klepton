#pragma once

#include <string>

namespace airbuds {

struct Album {
    std::string url;

    auto operator<=>(const Album&) const = default;
};

}// namespace airbuds
