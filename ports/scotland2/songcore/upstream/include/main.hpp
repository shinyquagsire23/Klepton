#pragma once

// Include the modloader header, which allows us to tell the modloader which mod this is, and the version etc.
#include "scotland2/shared/modloader.h"

// beatsaber-hook is a modding framework that lets us call functions and fetch field values from in the game
// It also allows creating objects, configuration, and importantly, hooking methods to modify their values
#include "beatsaber-hook/shared/utils/logging.hpp"
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-functions.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"

// Define these functions here so that we can easily read configuration and log information from other files
Configuration& getConfig();
