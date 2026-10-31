#pragma once

#include "CustomJSONData.hpp"

namespace SongCore::Utils {
    std::optional<std::string> GetCustomLevelHash(std::filesystem::path const& levelPath, SongCore::CustomJSONData::CustomLevelInfoSaveDataV2* saveData);
    std::optional<std::string> GetCustomLevelHash(std::filesystem::path const& levelPath, SongCore::CustomJSONData::CustomBeatmapLevelSaveDataV4* saveData);
    std::optional<int> GetDirectoryHash(std::filesystem::path const& directoryPath);
}
