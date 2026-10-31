#include <algorithm>
#include <functional>
#include <new>
#include <optional>
#include <variant>
#include <vector>
#ifndef LINUX_TEST
#ifndef SL2_KLEPTON
#include <jni.h>
#endif
#include <sys/stat.h>
#include <filesystem>
#include <system_error>
#include "_config.h"
#include "internal-loader.hpp"
#include "loader.hpp"
#include "log.h"
#include "modloader.h"

MODLOADER_EXPORT JavaVM* modloader_jvm;
MODLOADER_EXPORT void* modloader_libil2cpp_handle;
MODLOADER_EXPORT void* modloader_unity_handle;

// more useless than a landline
MODLOADER_EXPORT bool libs_opened;
MODLOADER_EXPORT bool early_mods_opened;
MODLOADER_EXPORT bool late_mods_opened;
MODLOADER_EXPORT CLoadPhase current_load_phase = CLoadPhase::LoadPhase_None;

namespace {

// Private set for libs
std::vector<modloader::LoadResult> loaded_libs;
// Private set for early mods
std::vector<modloader::LoadResult> loaded_early_mods;
// Private set for mods
std::vector<modloader::LoadResult> loaded_mods;
// Private set to avoid dlopening redundantly
std::unordered_set<std::string> skip_load{};

// Get status type as string
char const* status_type(std::filesystem::file_type const type) {
  switch (type) {
    case std::filesystem::file_type::none:
      return "none";
    case std::filesystem::file_type::not_found:
      return "not found";
    case std::filesystem::file_type::regular:
      return "regular";
    case std::filesystem::file_type::directory:
      return "directory";
    case std::filesystem::file_type::symlink:
      return "symlink";
    case std::filesystem::file_type::block:
      return "block";
    case std::filesystem::file_type::character:
      return "character";
    case std::filesystem::file_type::fifo:
      return "fifo";
    case std::filesystem::file_type::socket:
      return "socket";
    case std::filesystem::file_type::unknown:
    default:
      return "unknown";
  }
}

#if defined(__aarch64__) && !defined(NO_STAT_DUMPS)
void statdump(std::filesystem::path const& path) {
  struct stat64 buffer {};
  if (stat64(path.c_str(), &buffer) != 0) {
    LOG_DEBUG("stat64 of file: {} failed: {}, {}", path.c_str(), errno, strerror(errno));
  } else {
    LOG_DEBUG(
        "File: {}, dev: {}, ino: {}, mode: {}, nlink: {}, uid: {}, gid: {}, rdid: {}, sz: {}, atime: {}, "
        "mtime: {}, ctime: {}",
        path.c_str(), buffer.st_dev, buffer.st_ino, buffer.st_mode, buffer.st_nlink, buffer.st_uid, buffer.st_gid,
        buffer.st_rdev, buffer.st_size, buffer.st_atime, buffer.st_mtime, buffer.st_ctime);
    std::error_code err{};
    auto status = std::filesystem::status(path, err);
    if (err) {
      LOG_ERROR("Failed to get status of path: {}", path.c_str());
      return;
    }
    LOG_DEBUG("File: {}, type: {}, perms: 0x{:x}", path.c_str(), status_type(status.type()),
              static_cast<uint32_t>(status.permissions()));
  }
}
#else
void statdump([[maybe_unused]] std::filesystem::path const& path) {}
#endif

void ensure_dir_exists(std::filesystem::path const& dir) {
  // First, statdump the dir
  statdump(dir);
  std::error_code err{};
  // Then check whether it exists
  if (std::filesystem::exists(dir, err)) {
    LOG_WARN("Directory {} already existed", dir.c_str());
    return;
  }
  // Next, attempt to create the directory
  if (!std::filesystem::create_directories(dir, err)) {
    LOG_WARN("Failed to make directory: {}, err: {}", dir.c_str(), err.message());
    return;
  }
  LOG_DEBUG("Trying to chmod {}", dir.c_str());
  auto flags = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IWGRP | S_IROTH | S_IXOTH;
  if (chmod(dir.c_str(), flags) < 0) {
    LOG_ERROR("Failed to chmod {} with flags: 0x{:x}! errno: {}, {}", dir.c_str(), flags, errno, std::strerror(errno));
  }
}

bool remove_dir(std::filesystem::path const& dir) {
  // First, statdump
  statdump(dir);
  // Next, try to recursively remove it
  std::error_code err{};
  std::filesystem::remove_all(dir, err);
  if (err) {
    LOG_ERROR("Failed to remove directory: {}", dir.c_str());
    return false;
  }
  return true;
}

}  // namespace

namespace modloader {

bool copy_all(std::filesystem::path const& filesDir) noexcept {
  auto const& base_path = get_modloader_root_load_path();
  std::error_code error_code;
  for (auto const& [phase, path] : loadPhaseMap.arr) {
    auto dst = filesDir / path;
    auto src = base_path / path;
    ensure_dir_exists(src);
    if (!remove_dir(dst)) {
      LOG_ERROR("Failed to remove dst directory, stopping loading process early to avoid grabbing old mods...");
      return false;
    }
    ensure_dir_exists(dst);
    std::filesystem::copy(src, dst,
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                          error_code);
    if (error_code) {
      LOG_ERROR("Failed during phase: {} to copy directory: {} to: {}: {}", phase, src.c_str(), dst.c_str(),
                error_code.message().c_str());
      return false;
    }
    std::filesystem::permissions(dst, std::filesystem::perms::all, error_code);
    if (error_code) {
      LOG_ERROR("Failed during phase: {} to set permissions on copied directory: {}: {}", phase, dst.c_str(),
                error_code.message().c_str());
      return false;
    }
  }
  return true;
}

void open_libs(std::filesystem::path const& filesDir) noexcept {
  current_load_phase = CLoadPhase::LoadPhase_Libs;
  // Not thread safe: mutates skip_load
  LOG_DEBUG("Opening libs using root: {}", filesDir.c_str());
  auto lib_sos = listAllObjectsInPhase(filesDir, LoadPhase::Libs);
  LOG_DEBUG("Found: {} candidates! Attempting to load them...", lib_sos.size());
  // TODO: Libs are stored as LoadedMod which is redundant
  loaded_libs = loadMods(lib_sos, filesDir, skip_load, LoadPhase::Libs);
  // Report errors
  for (auto& l : loaded_libs) {
    if (auto* fail = std::get_if<FailedMod>(&l)) {
      LOG_WARN("Skipping lib load of library: {} because it failed with: {}", fail->object.path.c_str(),
               fail->failure.c_str());
    }
  }
  libs_opened = true;
}

void open_early_mods(std::filesystem::path const& filesDir) noexcept {
  current_load_phase = CLoadPhase::LoadPhase_EarlyMods;
  // Construct early mods
  // Not thread safe: mutates skip_load, initializes in sequential order
  auto early_mod_sos = listAllObjectsInPhase(filesDir, LoadPhase::EarlyMods);
  loaded_early_mods = loadMods(early_mod_sos, filesDir, skip_load, LoadPhase::EarlyMods);
  // Call initialize and report errors
  for (auto& m : loaded_early_mods) {
    if (auto* loaded_mod = std::get_if<LoadedMod>(&m)) {
      // Call init, note that it is a mutable call
      if (!loaded_mod->init()) {
        // Setup call does not exist, but the mod was still loaded
        LOG_INFO("No setup on mod: {}", loaded_mod->object.path.c_str());
      }
    } else if (auto* fail = std::get_if<FailedMod>(&m)) {
      LOG_WARN("Skipping setup call on: {} because it failed: {}", fail->object.path.c_str(), fail->failure.c_str());
    }
  }
  early_mods_opened = true;
}

void open_mods(std::filesystem::path const& filesDir) noexcept {
  current_load_phase = CLoadPhase::LoadPhase_Mods;
  // Construct mods (aka 'late' unity mods), should be happening after unity is inited (first scene loaded)
  auto mod_sos = listAllObjectsInPhase(filesDir, LoadPhase::Mods);
  loaded_mods = loadMods(mod_sos, filesDir, skip_load, LoadPhase::Mods);

  LOG_INFO("Found late mods:");
  for (auto& m : loaded_mods) {
    if (auto* loaded_mod = std::get_if<LoadedMod>(&m)) {
      LOG_INFO("{}", loaded_mod->object.path.c_str());
    }
  }
  
  // Call initialize and report errors
  for (auto& m : loaded_mods) {
    if (auto* loaded_mod = std::get_if<LoadedMod>(&m)) {
      // Call init, note that it is a mutable call
      if (!loaded_mod->init()) {
        // Setup call does not exist, but the mod was still loaded
        LOG_INFO("No setup on mod: {}", loaded_mod->object.path.c_str());
      }
    } else if (auto* fail = std::get_if<FailedMod>(&m)) {
      LOG_WARN("Skipping setup call on: {} because it failed: {}", fail->object.path.c_str(), fail->failure.c_str());
    }
  }
  late_mods_opened = true;
}

void load_early_mods() noexcept {
  // Call load on all early mods
  for (auto& m : loaded_early_mods) {
    if (auto* loaded_mod = std::get_if<LoadedMod>(&m)) {
      LOG_DEBUG("Attempting to call load on early mod: {}", loaded_mod->object.path.c_str());
      if (!loaded_mod->load()) {
        // Load call does not exist, but the mod was still loaded
        LOG_INFO("No load function on mod: {}", loaded_mod->object.path.c_str());
      }
    } else if (auto* fail = std::get_if<FailedMod>(&m)) {
      LOG_WARN("Skipping load call on: {} because it failed to be constructed: {}", fail->object.path.c_str(),
               fail->failure.c_str());
    }
  }
}

// calls late_load on mods and early mods
void load_mods() noexcept {
  LOG_DEBUG("Early mods to late load:");
  for (auto const& m : loaded_early_mods) {
    if (auto* loaded_mod = std::get_if<LoadedMod>(&m)) {
      LOG_DEBUG("{} -> {}", loaded_mod->object.path.c_str(), loaded_mod->modInfo);
    }
  }

  // call late_load on all early mods
  for (auto& m : loaded_early_mods) {
    if (auto* loaded_mod = std::get_if<LoadedMod>(&m)) {
      LOG_DEBUG("Attempting to call late_load on early mod: {}", loaded_mod->object.path.c_str());
      if (!loaded_mod->late_load()) {
        // Late load call does not exist, but the mod was still loaded
        LOG_INFO("No late_load function on early mod: {}", loaded_mod->object.path.c_str());
      }
    } else if (auto* fail = std::get_if<FailedMod>(&m)) {
      LOG_WARN("Skipping load_late call on: {} because it failed to be constructed: {}", fail->object.path.c_str(),
               fail->failure.c_str());
    }
  }

  // call late_load on all mods
  LOG_DEBUG("Late mods to late load:");
  for (auto const& m : loaded_mods) {
    if (auto* loaded_mod = std::get_if<LoadedMod>(&m)) {
      LOG_DEBUG("{} -> {}", loaded_mod->object.path.c_str(), loaded_mod->modInfo);
    }
  }

  for (auto& m : loaded_mods) {
    if (auto* loaded_mod = std::get_if<LoadedMod>(&m)) {
      LOG_DEBUG("Attempting to call late_load on mod: {} {}", loaded_mod->object.path.c_str(), fmt::ptr(loaded_mod->late_loadFn.value_or(nullptr)));
      if (!loaded_mod->late_load()) {
        // Load call does not exist, but the mod was still loaded
        LOG_INFO("No late_load function on mod: {}", loaded_mod->object.path.c_str());
      }
    } else if (auto* fail = std::get_if<FailedMod>(&m)) {
      LOG_WARN("Skipping late_load call on: {} because it failed to be constructed: {}", fail->object.path.c_str(),
               fail->failure.c_str());
    }
  }
}

/// Gets all loaded objects for a particular phase
std::vector<ModData> get_for(LoadPhase phase) noexcept {
  std::vector<ModData> result{};
  auto callback = [&result](auto const& m) {
    if (auto const* mod = std::get_if<LoadedMod>(&m)) {
      result.emplace_back(*mod);
    }
  };
  switch (phase) {
    case LoadPhase::Libs: {
      result.reserve(loaded_libs.size());
      std::for_each(loaded_libs.cbegin(), loaded_libs.cend(), callback);
    } break;
    case LoadPhase::EarlyMods: {
      result.reserve(loaded_early_mods.size());
      std::for_each(loaded_early_mods.cbegin(), loaded_early_mods.cend(), callback);
    } break;
    case LoadPhase::Mods: {
      result.reserve(loaded_mods.size());
      std::for_each(loaded_mods.cbegin(), loaded_mods.cend(), callback);
    } break;
    default:
      break;
  }
  return result;
}
/// Gets all loaded libs, early mods, and mods and returns the ModResult types.
std::vector<ModData> get_loaded() noexcept {
  std::vector<ModData> result{};
  result.reserve(loaded_libs.size() + loaded_early_mods.size() + loaded_mods.size());
  auto callback = [&result](auto const& m) {
    if (auto const* mod = std::get_if<LoadedMod>(&m)) {
      result.emplace_back(*mod);
    }
  };
  std::for_each(loaded_libs.cbegin(), loaded_libs.cend(), callback);
  std::for_each(loaded_early_mods.cbegin(), loaded_early_mods.cend(), callback);
  std::for_each(loaded_mods.cbegin(), loaded_mods.cend(), callback);
  return result;
}

/// Gets all loaded libs, early mods, and mods and returns the ModResult types.
std::vector<ModResult> get_all() noexcept {
  std::vector<ModResult> result{};
  result.reserve(loaded_libs.size() + loaded_early_mods.size() + loaded_mods.size());
  auto callback = [&result](auto const& m) {
    if (auto const* mod = std::get_if<LoadedMod>(&m)) {
      auto modData = static_cast<ModData>(*mod);
      result.emplace_back(std::move(modData));
      return;
    }

    auto const& failedMod = std::get<FailedMod>(m);
    FailedMod copiedFailure{ SharedObject(failedMod.object), failedMod.failure, failedMod.dependencies };
    result.emplace_back(std::move(copiedFailure));
  };
  std::for_each(loaded_libs.cbegin(), loaded_libs.cend(), callback);
  std::for_each(loaded_early_mods.cbegin(), loaded_early_mods.cend(), callback);
  std::for_each(loaded_mods.cbegin(), loaded_mods.cend(), callback);
  return result;
}

void close_all() noexcept {
  constexpr auto try_close = [](LoadResult& r) {
    if (auto* loaded = std::get_if<LoadedMod>(&r)) {
      if (auto err = loaded->close()) {
        LOG_WARN("Failed to close mod: {}: {}", loaded->object.path.c_str(), err->c_str());
      }
    }
  };
  // Moving out of these collections is fine, because this is teardown
  std::for_each(loaded_mods.begin(), loaded_mods.end(), try_close);
  std::for_each(loaded_early_mods.begin(), loaded_early_mods.end(), try_close);
  std::for_each(loaded_libs.begin(), loaded_libs.end(), try_close);
  loaded_libs.clear();
  loaded_early_mods.clear();
  loaded_mods.clear();
}

std::optional<std::reference_wrapper<LoadedMod>> get_mod(ModInfo info, MatchType match_type) noexcept {
  LOG_DEBUG("Attempting to force unload: {}", info);
  // First, see if we have a mod that matches
  auto find_match = [&info, match_type](LoadResult const& r) -> bool {
    if (auto const* loaded = std::get_if<LoadedMod>(&r)) {
      if (loaded->modInfo.equals(info, match_type)) {
        // Found a match!
        LOG_DEBUG("Found matching mod info: {} at: {}", loaded->modInfo, loaded->object.path.c_str());
        return true;
      }
    }
    return false;
  };
  // Then, try to unload it
  // Make a wrapper enum to hold some concepts here
  enum struct UnloadResult {
    kNotFound,
    kFailed,
    kSuccess,
  };
  constexpr static auto try_unload = [](std::vector<LoadResult>& results, auto const& find_match) -> LoadedMod* {
    auto found = std::find_if(results.begin(), results.end(), find_match);
    if (found == results.end()) {
      return nullptr;
    }
    if (auto* loaded = std::get_if<LoadedMod>(&*found)) {
      return loaded;
    }

    // FailedMod
    // Remove match from the collection
    results.erase(found);
    return nullptr;
  };
  // Now search the lists.
  // Start with the "best" matches first, ex: mods, then try early_mods
  // libs will never have ANY info
  auto* result = try_unload(loaded_mods, find_match);
  if (result == nullptr) {
    result = try_unload(loaded_early_mods, find_match);
  }
  if (result == nullptr) {
    return std::nullopt;
  }

  return std::ref(*result);
}

bool force_unload(ModInfo info, MatchType match_type) noexcept {
  // TODO: Use modloader::get_mod instead

  LOG_DEBUG("Attempting to force unload: {}", info);
  // First, see if we have a mod that matches
  auto find_match = [&info, match_type](LoadResult const& r) -> bool {
    if (auto const* loaded = std::get_if<LoadedMod>(&r)) {
      if (loaded->modInfo.equals(info, match_type)) {
        // Found a match!
        LOG_DEBUG("Found matching mod info: {} at: {}", loaded->modInfo, loaded->object.path.c_str());
        return true;
      }
    }
    return false;
  };
  // Then, try to unload it
  // Make a wrapper enum to hold some concepts here
  enum struct UnloadResult {
    kNotFound,
    kFailed,
    kSuccess,
  };
  constexpr static auto try_unload = [](std::vector<LoadResult>& results, auto const& find_match) -> UnloadResult {
    auto found = std::find_if(results.begin(), results.end(), find_match);
    if (found == results.end()) {
      return UnloadResult::kNotFound;
    }
    if (auto* loaded = std::get_if<LoadedMod>(&*found)) {
      if (auto err = loaded->close()) {
        LOG_WARN("Failed to close mod: {}: {}", loaded->object.path.c_str(), err->c_str());
        return UnloadResult::kFailed;
      }
    }
    // Remove match from the collection
    results.erase(found);
    return UnloadResult::kSuccess;
  };
  // Now search the lists.
  // Start with the "best" matches first, ex: mods, then try early_mods
  // libs will never have ANY info
  auto result = try_unload(loaded_mods, find_match);
  if (result == UnloadResult::kNotFound) {
    result = try_unload(loaded_early_mods, find_match);
  }
  return result == UnloadResult::kSuccess || result == UnloadResult::kNotFound;
}

}  // namespace modloader

MODLOADER_FUNC CModResult modloader_get_mod(CModInfo* info, CMatchType match_type) {
  auto modResult = modloader::get_mod(modloader::ModInfo(*info), modloader::from_c_match_type(match_type));

  if (!modResult.has_value()) {
    return {};
  }

  return modloader::ModData(modResult.value()).to_c();
}

// C API loader related interop
MODLOADER_FUNC bool modloader_force_unload(CModInfo info, CMatchType match_type) {
  return modloader::force_unload(modloader::ModInfo(info), modloader::from_c_match_type(match_type));
}

MODLOADER_FUNC CModResults modloader_get_loaded() {
  auto results = modloader::get_loaded();
  CModResults output{
    .array = new (std::nothrow) CModResult[results.size()],
    .size = results.size(),
  };
  for (size_t i = 0; i < results.size(); i++) {
    auto const path = results[i].path.string();
    auto* copy_path = new (std::nothrow) char[path.size() + 1];
    path.copy(copy_path, path.size());
    copy_path[path.size()] = '\0';
    output.array[i] = CModResult{
      .info = results[i].info.to_c(),
      .path = copy_path,
      .handle = results[i].handle,
    };
  }
  return output;
}
MODLOADER_FUNC CLoadResults modloader_get_all() {
  auto results = modloader::get_all();
  CLoadResults output{
    .array = new (std::nothrow) CLoadResult[results.size()],
    .size = results.size(),
  };
  auto copy_path_to_cstr = [](std::filesystem::path const& path) {
    auto path_string = path.string();
    auto* copy_path = new (std::nothrow) char[path_string.size() + 1];
    path_string.copy(copy_path, path_string.size());
    copy_path[path_string.size()] = '\0';

    return copy_path;
  };

  for (size_t i = 0; i < results.size(); i++) {
    auto result = std::move(results[i]);
    if (auto const* modData = std::get_if<modloader::ModData>(&result)) {
      output.array[i] = CLoadResult{
        .result = CLoadResultEnum::MatchType_Loaded,
        .loaded =
            CModResult{
                .info = modData->info.to_c(),
                .path = copy_path_to_cstr(modData->path),
                .handle = modData->handle,
            },
      };
      continue;
    }

    auto failedMod = std::get<modloader::FailedMod>(std::move(result));

    output.array[i] = CLoadResult{
      .result = CLoadResultEnum::LoadResult_Failed,
      .failed =
          CLoadFailed{
              .failure = copy_path_to_cstr(failedMod.failure),
              .path = copy_path_to_cstr(failedMod.object.path),
          },
    };
  }
  return output;
}
/// @brief Frees a CModResults object
MODLOADER_FUNC void modloader_free_results(CModResults* results) {
  for (size_t i = 0; i < results->size; i++) {
    delete[] results->array[i].info.id;
    delete[] results->array[i].info.version;
    delete[] results->array[i].path;
  }
  delete[] results->array;
}

MODLOADER_FUNC CLoadResultEnum modloader_require_mod(CModInfo* info, CMatchType match_type) {
  LOG_VERBOSE("Mod {} is being attempted to load!", info->id);

  auto result = modloader::get_mod(modloader::ModInfo(*info), modloader::from_c_match_type(match_type));

  if (!result) {
    LOG_ERROR("Unable to find {}", info->id);
    return CLoadResultEnum::LoadResult_NotFound;
  }

  auto& loadedMod = result.value().get();
  LOG_VERBOSE("Found mod {}, loading for phase {}", loadedMod.modInfo.id, fmt::underlying(loadedMod.phase));

  switch (loadedMod.phase) {
    case modloader::LoadPhase::None:
    case modloader::LoadPhase::Libs:
    case modloader::LoadPhase::Shim:
      break;
    case modloader::LoadPhase::EarlyMods:
      if (!loadedMod.init()) {
        LOG_ERROR("Unable to init {}", loadedMod.modInfo.id);
        return CLoadResultEnum::LoadResult_Failed;
      }

      // if early load
      if (current_load_phase == CLoadPhase::LoadPhase_EarlyMods) {
        LOG_VERBOSE("Attempting to load early mod {}!", info->id);
        if (!loadedMod.load()) {
          LOG_ERROR("Unable to early load {}", loadedMod.modInfo.id);
          return CLoadResultEnum::LoadResult_Failed;
        }
      }
      // if late load
      if (current_load_phase == CLoadPhase::LoadPhase_Mods) {
        LOG_VERBOSE("Attempting to late load early mod {}!", info->id);
        if (!loadedMod.late_load()) {
          LOG_ERROR("Unable to late load {}", loadedMod.modInfo.id);
          return CLoadResultEnum::LoadResult_Failed;
        }
      }

      break;
    case modloader::LoadPhase::Mods:
      if (!loadedMod.init()) {
        LOG_ERROR("Unable to init {}", loadedMod.modInfo.id);
        return CLoadResultEnum::LoadResult_Failed;
      }

      // if late load
      if (current_load_phase == CLoadPhase::LoadPhase_Mods) {
        LOG_VERBOSE("Attempting to late load late mod {}!", info->id);
        if (!loadedMod.late_load()) {
          LOG_ERROR("Unable to late load {}", loadedMod.modInfo.id);
          return CLoadResultEnum::LoadResult_Failed;
        }
      }
  }

  return CLoadResultEnum::MatchType_Loaded;
}

#endif
