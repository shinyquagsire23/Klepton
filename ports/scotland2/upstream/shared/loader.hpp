#pragma once
#include "_config.h"

#include <array>
#include <deque>
#include <filesystem>
#include <new>
#include <optional>
#include <span>
#include <stack>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "constexpr-map.hpp"
#include "loader.hpp"
#include "modloader.h"

// Make fmt an optional dependency, we will just improve debugging facilities if it is defined
#if __has_include(<fmt/core.h>) && !defined(MODLOADER_NO_FMT)
#include <fmt/core.h>
#define MODLOADER_USE_FMT
#endif

namespace modloader {

struct Dependency;
struct SharedObject;

enum struct LoadPhase {
  None,
  Libs,
  EarlyMods,
  Mods,
  Shim,
};

#ifdef MODLOADER_USE_FMT
inline auto format_as(LoadPhase p) {
  return fmt::underlying(p);
}
#endif

using namespace std::literals::string_view_literals;
constexpr static ConstexprMap loadPhaseMap(std::array<std::pair<LoadPhase, std::string_view>, 4>{
    { { LoadPhase::Libs, "libs"sv },
      { LoadPhase::EarlyMods, "early_mods"sv },
      { LoadPhase::Mods, "mods"sv },
      { LoadPhase::Shim, "shims"sv } } });

using MissingDependency = SharedObject;
using DependencyResult = std::variant<MissingDependency, Dependency>;

struct ModInfo;

struct FailedMod;
struct LoadedMod;
struct ModData;

using LoadResult = std::variant<std::monostate, FailedMod, LoadedMod>;
using ModResult = std::variant<std::monostate, FailedMod, ModData>;

using SetupFunc = void (*)(CModInfo* modInfo) noexcept;
using LoadFunc = void (*)() noexcept;
using LateLoadFunc = void (*)() noexcept;
using UnloadFunc = void (*)() noexcept;

struct SharedObject {
  std::filesystem::path path;
  explicit SharedObject(std::filesystem::path path) : path(std::move(path)) {}
  // Copies are explicit ONLY. Should move otherwise.
  explicit SharedObject(SharedObject const&) = default;
  SharedObject& operator=(SharedObject const&) = delete;
  SharedObject(SharedObject&&) noexcept = default;
  SharedObject& operator=(SharedObject&&) noexcept = default;

  /// @brief Returns a collection of dependency results for all of the dependencies of a SharedObject, as read from
  /// the ELF. Requires that the SharedObject is readable.
  /// @param dependencyDir The top level directory to load from
  /// @param phase The phase to start the reverse search from
  /// @param loadedDependencies The dependecies that we have loaded already. The string_views must remain in lifetime
  /// for the duration of this call.
  /// @return The collection of dependency results that were attempted to be resolved
  [[nodiscard]] std::vector<DependencyResult> getToLoad(
      std::filesystem::path const& dependencyDir, LoadPhase phase,
      std::unordered_map<std::string_view, std::vector<DependencyResult>>& loadedDependencies) const;

  [[nodiscard]] std::vector<DependencyResult> getToLoad(std::filesystem::path const& dependencyDir,
                                                        LoadPhase phase) const;
};

struct Dependency {
  SharedObject object;
  std::vector<DependencyResult> dependencies;

  Dependency(Dependency&&) noexcept = default;
  Dependency& operator=(Dependency&&) noexcept = default;
  // Copies are explicit ONLY. Should move otherwise.
  explicit Dependency(Dependency const&) = default;
  Dependency& operator=(Dependency const&) = delete;

  Dependency(SharedObject object, std::vector<DependencyResult> dependencies)
      : object(std::move(object)), dependencies(std::move(dependencies)) {}
  ~Dependency() = default;
};

[[nodiscard]] inline std::vector<DependencyResult> SharedObject::getToLoad(std::filesystem::path const& dependencyDir,
                                                                           LoadPhase phase) const {
  std::unordered_map<std::string_view, std::vector<DependencyResult>> loadedDependencies{};
  return getToLoad(dependencyDir, phase, loadedDependencies);
}

enum struct MatchType {
  kStrict,
  kIdOnly,
  kIdVersion,
  kIdVersionLong,
  kUnknown,
};
inline MatchType from_c_match_type(CMatchType type) {
  switch (type) {
    case MatchType_Strict:
      return MatchType::kStrict;
    case MatchType_IdOnly:
      return MatchType::kIdOnly;
    case MatchType_IdVersion:
      return MatchType::kIdVersion;
    case MatchType_IdVersionLong:
      return MatchType::kIdVersionLong;
    default:
      return MatchType::kUnknown;
  }
}
struct ModInfo {
  std::string id{};       // nullable
  std::string version{};  // nullable
  size_t versionLong{};

  explicit ModInfo(CModInfo const& modInfo)
      : id(modInfo.id != nullptr ? modInfo.id : ""),
        version(modInfo.version != nullptr ? modInfo.version : ""),
        versionLong(modInfo.version_long) {}
  ModInfo(std::string_view id, std::string_view version, size_t versionLong)
      : id(id), version(version), versionLong(versionLong) {}

  [[nodiscard]] bool equals(ModInfo const& other, MatchType type) const {
    switch (type) {
      case MatchType::kIdOnly:
        return std::string(id) == other.id;
      case MatchType::kIdVersion:
        return std::string(id) == other.id && std::string(version) == other.version;
      case MatchType::kIdVersionLong:
        return std::string(id) == other.id && versionLong == other.versionLong;
      case MatchType::kStrict:
      // Unknown case behaves as strict
      case MatchType::kUnknown:
        return std::string(id) == other.id && std::string(version) == other.version && versionLong == other.versionLong;
    }
  }
  void assign(CModInfo const& other) {
    id = other.id;
    version = other.version;
    versionLong = other.version_long;
  }
  [[nodiscard]] CModInfo to_c() const {
    // TODO: Error handle here
    auto* copy_id = new (std::nothrow) char[id.size() + 1];
    id.copy(copy_id, id.size());
    copy_id[id.size()] = '\0';
    auto* copy_version = new (std::nothrow) char[version.size() + 1];
    version.copy(copy_version, version.size());
    copy_version[version.size()] = '\0';
    return CModInfo{
      .id = copy_id,
      .version = copy_version,
      .version_long = versionLong,
    };
  }
};

struct FailedMod {
  SharedObject object;
  std::string failure;
  std::vector<DependencyResult> dependencies;

  FailedMod(FailedMod&&) noexcept = default;
  FailedMod& operator=(FailedMod&&) noexcept = default;
  FailedMod(FailedMod const&) = delete;
  FailedMod& operator=(FailedMod const&) = delete;

  FailedMod(SharedObject object, std::string failure, std::vector<DependencyResult> dependencies)
      : object(std::move(object)), failure(std::move(failure)), dependencies(std::move(dependencies)) {}
};

struct LoadedMod {
  ModInfo modInfo;
  SharedObject object;
  LoadPhase phase;

  std::optional<SetupFunc> setupFn;
  std::optional<LoadFunc> loadFn;
  std::optional<LateLoadFunc> late_loadFn;
  std::optional<UnloadFunc> unloadFn;

  void* handle;

  bool inited = false;
  bool load_called = false;
  bool late_load_called = false;
  bool unloaded = false;

  LoadedMod(LoadedMod&&) noexcept = default;
  LoadedMod& operator=(LoadedMod&&) noexcept = default;
  LoadedMod(LoadedMod const&) = delete;
  LoadedMod& operator=(LoadedMod const&) = delete;

  LoadedMod(ModInfo modInfo, SharedObject object, LoadPhase phase, std::optional<SetupFunc> setupFn,
            std::optional<LoadFunc> loadFn, std::optional<LateLoadFunc> late_loadFn, std::optional<UnloadFunc> unloadFn,
            void* handle)
      : modInfo(std::move(modInfo)),
        object(std::move(object)),
        phase(phase),
        setupFn(setupFn),
        loadFn(loadFn),
        late_loadFn(late_loadFn),
        unloadFn(unloadFn),
        handle(handle) {}

  /// @brief Calls the setup function on the mod
  /// @return true if the call exists and was called, false otherwise
  inline bool init() noexcept {
    if (inited) {
      return true;
    }

    if (setupFn) {
      // Need to make a CModInfo here to ensure ABI correctness
      CModInfo info{
        .id = modInfo.id.c_str(),
        .version = modInfo.version.c_str(),
        .version_long = modInfo.versionLong,
      };
      (*setupFn)(&info);
      // After the call, take the info and write it back
      modInfo.assign(info);
      inited = true;
      return true;
    }
    return false;
  }
  /// @brief Calls the load function on the mod
  /// @return true if the call exists and was called, false otherwise
  inline bool load() noexcept {
    if (load_called) {
      return true;
    }

    if (loadFn) {
      (*loadFn)();
      load_called = true;
      return true;
    }
    return false;
  }

  /// @brief Calls the late_load function on the mod
  /// @return true if the call exists and was called, false otherwise
  inline bool late_load() noexcept {
    if (late_load_called) {
      return true;
    }

    if (late_loadFn) {
      (*late_loadFn)();
      late_load_called = true;
      return true;
    }
    return false;
  }

  /// @brief Calls the unload function on the mod
  /// @return true if the call exists and was called, false otherwise
  inline bool unload() noexcept {
    if (unloaded) {
      return unloaded;
    }
    if (unloadFn) {
      (*unloadFn)();
      unloaded = true;
      return true;
    }
    return false;
  }
  /// @brief Closes the loaded mod by dlclosing it
  /// @return An optional holding the error message, or nullopt on success
  [[nodiscard]] std::optional<std::string> close() const noexcept;
};

/// @brief Represents the type exposed via API calls
struct MODLOADER_EXPORT ModData {
  ModInfo info;
  std::filesystem::path path;
  LoadPhase phase;

  // TODO: Consider hiding these optional fields entirely (no user should be able to call them, generally)
  std::optional<SetupFunc> setupFn;
  std::optional<LoadFunc> loadFn;
  std::optional<LateLoadFunc> late_loadFn;
  std::optional<UnloadFunc> unloadFn;

  void* handle;

  explicit ModData(LoadedMod const& mod)
      : info(mod.modInfo),
        path(mod.object.path),
        phase(mod.phase),
        setupFn(mod.setupFn),
        loadFn(mod.loadFn),
        late_loadFn(mod.late_loadFn),
        unloadFn(mod.unloadFn),
        handle(mod.handle) {}
  ModData(ModData const&) = default;
  ModData(ModData&&) = default;
  ModData& operator=(ModData const&) = default;
  ModData& operator=(ModData&&) = default;
  ~ModData() = default;

  CModResult to_c() {
    auto path_str = this->path.string();
    auto* copy_path = new (std::nothrow) char[path_str.size() + 1];
    path_str.copy(copy_path, path_str.size());
    copy_path[path_str.size()] = '\0';

    return CModResult{
      .info = info.to_c(),
      .path = copy_path,
      .handle = handle,
    };
  }
};

std::deque<Dependency> MODLOADER_EXPORT topologicalSort(std::span<DependencyResult const> list);
std::deque<Dependency> MODLOADER_EXPORT topologicalSort(std::vector<Dependency>&& list);

/// @brief Triggers an unload of the specified mod, which will in turn call the unload() method of it.
/// It will also be removed from any collections. It is UB if the mod to be unloaded is the currently executing mod.
/// @return False if the mod failed to be unloaded in any way, true if it either did not exist or was successfully
/// unloaded.
MODLOADER_EXPORT bool force_unload(ModInfo info, MatchType type) noexcept;

/// Gets all loaded objects for a particular phase
MODLOADER_EXPORT std::vector<ModData> get_for(LoadPhase phase) noexcept;
/// Gets all loaded libs, early mods, and mods and returns the ModResult types.
MODLOADER_EXPORT std::vector<ModData> get_loaded() noexcept;
/// Gets all loaded libs, early mods, and mods and returns the ModResult types.
MODLOADER_EXPORT std::vector<ModResult> get_all() noexcept;

}  // namespace modloader

#ifdef MODLOADER_USE_FMT
// Format specializations
template <>
struct fmt::formatter<modloader::ModInfo> {
  template <typename ParseContext>
  constexpr auto parse(ParseContext& ctx) {
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(modloader::ModInfo const& info, FormatContext& ctx) const {
    return fmt::format_to(ctx.out(), "id: {} version: {} version tag: {}", info.id, info.version, info.versionLong);
  }
};
#endif
