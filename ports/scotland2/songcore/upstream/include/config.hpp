#pragma once

#include <filesystem>
#include <vector>

struct Config {
    /// @brief whether to apply note colors from diffs
    bool customSongNoteColors = true;
    /// @brief whether to apply obstacle colors from diffs
    bool customSongObstacleColors = true;
    /// @brief whether to apply environment colors from diffs
    bool customSongEnvironmentColors = true;

    /// @brief whether the saber count override should be honored
    bool disableOneSaberOverride = false;

    /// @brief whether to not show the songloader warning again
    bool dontShowSongloaderWarningAgain = false;

    /// @brief multiple paths to folders to load songs from, in case user has multiple folders. Not exposed
    std::vector<std::filesystem::path> RootCustomLevelPaths {
        "/sdcard/ModData/com.beatgames.beatsaber/Mods/SongCore/CustomLevels",
        "/sdcard/ModData/com.beatgames.beatsaber/Mods/SongLoader/CustomLevels"
    };

    /// @brief multiple paths to folders to load WIP songs from, in case user has multiple folders. Not exposed
    std::vector<std::filesystem::path> RootCustomWIPLevelPaths {
        "/sdcard/ModData/com.beatgames.beatsaber/Mods/SongCore/CustomWIPLevels",
        "/sdcard/ModData/com.beatgames.beatsaber/Mods/SongLoader/CustomWIPLevels"
    };
};

extern Config config;

void SaveConfig();
bool LoadConfig();
