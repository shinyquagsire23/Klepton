#pragma once

#include <string>
#include <string_view>

#include "beatsaber-hook/shared/config/config-utils.hpp"

namespace AirbudsSearch {

// Stores the ID and version of our mod, and is sent to
// the modloader upon startup
static modloader::ModInfo modInfo{MOD_ID, VERSION, 0};

// Define these functions here so that we can easily read configuration and
// log information from other files
Configuration& getConfig();

std::filesystem::path getDataDirectory();

bool hasAirbudsRefreshToken();
std::string getAirbudsRefreshToken();
void setAirbudsRefreshToken(std::string_view token);
void clearAirbudsRefreshToken();

}
