#pragma once

#include "GlobalNamespace/BeatmapKey.hpp"
#include "GlobalNamespace/BeatmapLevel.hpp"
#include "GlobalNamespace/BeatmapLevelPack.hpp"
#include "GlobalNamespace/IReadonlyBeatmapData.hpp"
#include "UnityEngine/Sprite.hpp"
#include "export.h"

namespace MetaCore::Songs {
    /// @brief Finds the hash in a level id
    /// @param levelId The level id
    /// @return The hash of the level if found, otherwise an empty string
    METACORE_EXPORT std::string GetHash(std::string levelId);
    /// @brief Finds the hash in a beatmap key
    /// @param beatmap The beatmap key
    /// @return The hash of the beatmap key if found, otherwise an empty string
    METACORE_EXPORT std::string GetHash(GlobalNamespace::BeatmapKey beatmap);
    /// @brief Finds the hash in a beatmap level
    /// @param beatmap The beatmap level
    /// @return The hash of the beatmap level if found, otherwise an empty string
    METACORE_EXPORT std::string GetHash(GlobalNamespace::BeatmapLevel* beatmap);

    /// @brief Asynchronously retrieves the BeatmapData of a beatmap, will only run one task per beatmap at a time
    /// @param beatmap The beatmap key
    /// @param callback The callback with the data once it has been retrieved, or nullptr if it fails
    METACORE_EXPORT void GetBeatmapData(GlobalNamespace::BeatmapKey beatmap, std::function<void(GlobalNamespace::IReadonlyBeatmapData*)> callback);

    /// @brief Asynchronously retrieves the cover sprite of a beatmap
    /// @param beatmap The beatmap level
    /// @param callback The callback with the sprite once it has been retrieved, or nullptr if it fails
    METACORE_EXPORT void GetSongCover(GlobalNamespace::BeatmapLevel* beatmap, std::function<void(UnityEngine::Sprite*)> callback);

    /// @brief Finds the BeatmapLevel for a level id
    /// @param levelId The level id
    /// @return The beatmap level, or nullptr if not found
    METACORE_EXPORT GlobalNamespace::BeatmapLevel* FindLevel(std::string levelId);
    /// @brief Finds the BeatmapLevel for a beatmap key
    /// @param beatmap The beatmap key
    /// @return The beatmap level, or nullptr if not found
    METACORE_EXPORT GlobalNamespace::BeatmapLevel* FindLevel(GlobalNamespace::BeatmapKey beatmap);

    /// @brief Gets the currently selected beatmap key
    /// @param last If the last selected beatmap should be returned even if the detail view has been closed
    /// @return The currently selected beatmap key, or default if none
    METACORE_EXPORT GlobalNamespace::BeatmapKey GetSelectedKey(bool last = true);
    /// @brief Gets the currently selected beatmap level
    /// @param last If the last selected beatmap should be returned even if the detail view has been closed
    /// @return The currently selected beatmap level, or nullptr if none
    METACORE_EXPORT GlobalNamespace::BeatmapLevel* GetSelectedLevel(bool last = true);
    /// @brief Gets the currently selected playlist, either OST or custom
    /// @param last If the last selected playlist should be returned even if none is currently selected
    /// @return The currently selected playlist, or nullptr if none
    METACORE_EXPORT GlobalNamespace::BeatmapLevelPack* GetSelectedPlaylist(bool last = true);

    /// @brief Navigates to and selects a given beatmap level in the single player level selection, optionally in a playlist
    /// @param level The level to select, will not select anything if nullptr or not in the playlist
    /// @param playlist The playlist to select, will open the "All Songs" menu if nullptr
    METACORE_EXPORT void SelectLevel(GlobalNamespace::BeatmapLevel* level, GlobalNamespace::BeatmapLevelPack* playlist = nullptr);
    /// @brief Navigates to and selects a given beatmap level and difficulty in the single player level selection, optionally in a playlist
    /// @param key The level, characteristic, and difficulty to select, will not select anything if nullptr or not in the playlist
    /// @param playlist The playlist to select, will open the "All Songs" menu if nullptr
    METACORE_EXPORT void SelectLevel(GlobalNamespace::BeatmapKey key, GlobalNamespace::BeatmapLevelPack* playlist = nullptr);

    /// @brief Plays the preview audio of a level
    /// @param beatmap The level to play the preview of
    METACORE_EXPORT void PlayLevelPreview(GlobalNamespace::BeatmapLevel* beatmap);
}
