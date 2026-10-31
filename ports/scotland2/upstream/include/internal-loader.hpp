#pragma once

#include <deque>
#include <filesystem>
#include <span>
#include <stack>
#include <type_traits>
#include <unordered_set>
#include <variant>
#include <vector>

#include "loader.hpp"

namespace modloader {


static_assert(std::is_move_assignable_v<LoadResult> && std::is_move_constructible_v<LoadResult>, "");

std::vector<SharedObject> listAllObjectsInPhase(std::filesystem::path const& dependencyDir, LoadPhase phase);

// Moves FROM mods
[[nodiscard]] std::vector<LoadResult> loadMods(std::span<SharedObject> mods, std::filesystem::path const& dependencyDir,
                                               std::unordered_set<std::string>& skipLoad, LoadPhase phase);
[[nodiscard]] std::vector<LoadResult> loadMod(SharedObject&& mod, std::filesystem::path const& dependencyDir,
                                              std::unordered_set<std::string>& skipLoad, LoadPhase phase);

/// @brief Copies all of the files to be loaded by the modloader to a location that it can mark as executable.
/// Does NOT use symlinks to avoid tainting permissions.
/// @param filesDir The destination folder to copy to
/// @return true on success, false otherwise
[[nodiscard]] bool copy_all(std::filesystem::path const& filesDir) noexcept;

/// @brief Opens the libraries from the @ref loadPhaseMap and places them in the filesDir provided.
/// @param filesDir The destination to copy the libraries to in order to ensure correct permissions.
void open_libs(std::filesystem::path const& filesDir) noexcept;

/// @brief Opens the early mods from the @ref loadPhaseMap and places them in the filesDir provided.
/// @param filesDir The destination to copy the early mods to in order to ensure correct permissions.
void open_early_mods(std::filesystem::path const& filesDir) noexcept;

/// @brief Opens the mods from the @ref loadPhaseMap and places them in the filesDir provided.
/// @param filesDir The destination to copy the mods to in order to ensure correct permissions.
void open_mods(std::filesystem::path const& filesDir) noexcept;

/// @brief Calls load on early mods
void load_early_mods() noexcept;

/// @brief Calls late_load on mods and early mods
void load_mods() noexcept;

void close_all() noexcept;

}  // namespace modloader
