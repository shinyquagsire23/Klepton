#pragma once

#include <string>

namespace airbuds {

struct Artist {
    std::string id;
    std::string name;

    auto operator<=>(const Artist&) const = default;
};

}// namespace airbuds
