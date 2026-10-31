#pragma once

#include "config-utils/shared/config-utils.hpp"

DECLARE_CONFIG(Config) {
    CONFIG_VALUE(ScrollSpeed, float, "Playlist Scroll Speed", 2, "The speed at which to scroll through the playlists with the joystick.");
    CONFIG_VALUE(Order, std::vector<std::string>, "order", {});
};
