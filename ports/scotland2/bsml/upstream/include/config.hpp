#pragma once

#include <vector>
#include <string>

struct config_t {
    std::vector<std::string> hiddenTabs = {};
};

extern config_t config;

void SaveConfig();
bool LoadConfig();