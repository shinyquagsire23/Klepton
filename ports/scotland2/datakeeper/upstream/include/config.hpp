#pragma once

#include "config-utils/shared/config-utils.hpp"
#include "main.hpp"

DECLARE_CONFIG(Config) {
    CONFIG_VALUE(backupPath, std::string, "backupPath", getDataDir(modInfo), "Where to backup the data.");
    CONFIG_VALUE(lightsSet, bool, "lightsSet", false, "Whether the lights have been set.");
};
