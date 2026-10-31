#include "loader.hpp"
#include "constexpr-map.hpp"
#include "elf-utils.hpp"
#include "internal-loader.hpp"
#include "log.h"
#include "modloader.h"

#include <dlfcn.h>
#ifndef SL2_KLEPTON
#include <elf.h>
#endif
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef SL2_KLEPTON
#include "apple-platform.hpp"
#define open64 open
#define fstat64 fstat
#define stat64 stat
#define dlopen sc2_apple::open
#define dlsym sc2_apple::symbol
#define dlerror sc2_apple::error
#define dlclose sc2_apple::close
#endif

namespace modloader {

using namespace elf_utils;
/// @brief Find and create a SharedObject representing the resolved dependency, if it can be found.
/// @param dependencyDir The top level directory
/// @param phase The load phase of the dependency to start the search from
/// @param name The dependency's name to open
/// @return The returned pair on success, nullopt otherwise
std::optional<std::pair<SharedObject, LoadPhase>> findSharedObject(std::filesystem::path const& dependencyDir,
                                                                   LoadPhase phase, std::filesystem::path const& name) {
  // Search in reverse load order, starting at phase
  std::error_code error_code;
  for (auto const& it : loadPhaseMap.arr) {
    if (static_cast<int>(it.first) > static_cast<int>(phase) && it.first != LoadPhase::Shim) {
      continue;
    }
    auto path_to_check = dependencyDir / it.second / name;
    LOG_DEBUG("Searching for dependency: {} at: {}", name.c_str(), path_to_check.c_str());
    if (std::filesystem::exists(path_to_check, error_code)) {
      // Dependency exists at this phase.
      // TODO: This should actually check to ensure that this file is actually readable, not just exists
      return { std::make_pair(SharedObject(path_to_check), it.first) };
    }
    if (error_code) {
      LOG_ERROR("Failed to check for existence of: {}: {}", path_to_check.c_str(), error_code.message().c_str());
      return std::nullopt;
    }
    // Otherwise, this filename doesn't exist under this phase. Try the next one.
  }
  // If we get to a point where we tried all of our explicit dependencies, return a None phase and try to let the
  // linker determine it when opening it.
  return { { SharedObject(name), LoadPhase::None } };
}

std::vector<DependencyResult> SharedObject::getToLoad(
    std::filesystem::path const& dependencyDir, LoadPhase phase,
    std::unordered_map<std::string_view, std::vector<DependencyResult>>& loadedDependencies) const {
  auto depIt = loadedDependencies.find(path.c_str());
  LOG_DEBUG("Getting dependencies for: {} under root: {} for phase: {}", path.c_str(), dependencyDir.c_str(), phase);

  if (depIt != loadedDependencies.end()) {
    LOG_DEBUG("Hit in dependencies cache, have {} loaded dependencies", depIt->second.size());
    return depIt->second;
  }

#ifdef SL2_KLEPTON
  auto& dependencies = loadedDependencies.emplace(path.c_str(), std::vector<DependencyResult>{}).first->second;
  for (auto const& name : sc2_apple::dependencies(path)) {
    auto found = findSharedObject(dependencyDir, phase, name);
    if (!found) continue;
    auto [object, object_phase] = std::move(*found);
    if (object_phase == LoadPhase::None) {
      dependencies.emplace_back(std::in_place_type_t<MissingDependency>{}, std::move(object));
    } else {
      auto children = object.getToLoad(dependencyDir, object_phase, loadedDependencies);
      dependencies.emplace_back(std::in_place_type_t<Dependency>{}, std::move(object), std::move(children));
    }
  }
  return dependencies;
#else
  // TODO: RAII-ify this
  int fd = open64(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    LOG_ERROR("Failed to open dependency: {}: {}", path.c_str(), std::strerror(errno));
    return {};
  }

  struct stat64 st {};
  if (fstat64(fd, &st) != 0) {
    LOG_ERROR("Failed to stat dependency: {}: {}", path.c_str(), std::strerror(errno));
    return {};
  }
  size_t size = st.st_size;

  LOG_DEBUG("mmapping with size: {} on fd: {}", size, fd);
  // TODO: RAII-ify this
  void* mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);

  if (mapped == MAP_FAILED) {
    LOG_ERROR("Failed to mmap dependency: {}: {}", path.c_str(), std::strerror(errno));
  }

  std::span<uint8_t> f(static_cast<uint8_t*>(mapped), static_cast<uint8_t*>(mapped) + size);

  auto elf = readAtOffset<Elf64_Ehdr>(f, 0);
  LOG_DEBUG("Header read: ehsize: {}, type: {}, version: {}, shentsize: {}", elf.e_ehsize, elf.e_type, elf.e_version,
            elf.e_shentsize);

  // Using the c_str here is OK because the lifetime of the path is tied to this instance
  std::vector<DependencyResult>& dependencies =
      loadedDependencies.emplace(path.c_str(), std::vector<DependencyResult>{}).first->second;
  // Micro-optimization: Small dependency sets will not alloc, larger ones will alloc less frequently
  constexpr static auto kGuessDependencyCount = 16;
  dependencies.reserve(kGuessDependencyCount);

  auto sections = readManyAtOffset<Elf64_Shdr>(f, elf.e_shoff, elf.e_shnum, elf.e_shentsize);

  for (auto const& sectionHeader : sections) {
    if (sectionHeader.sh_type != SHT_DYNAMIC) {
      continue;
    }
    // sectionHeader is the dynamic symbols table
    // We must walk until we see a NULL symbol
    // We want to grab the STRTAB entry from here and use it
    // The number of entries we have seen, mostly for debugging
    int dynamic_count = 0;
    // This vector holds the actual string table offsets for the DT_NEEDED entries
    std::vector<uint64_t> needed_offsets{};
    // The strtab offset to use for name resolution
    uint64_t strtab_offset = 0;
    needed_offsets.reserve(kGuessDependencyCount);
    while (true) {
      // Get the dynamic symbol for this entry
      auto const& dyn = readAtOffset<Elf64_Dyn>(f, sectionHeader.sh_offset + sizeof(Elf64_Dyn) * dynamic_count++);
      if (dyn.d_tag == DT_NULL) {
        LOG_DEBUG(
            "End of dynamic section. Counted a total of: {} dynamic entries, of which {} were needed dependencies",
            dynamic_count, needed_offsets.size());
        break;
      }
      if (dyn.d_tag == DT_NEEDED) {
        LOG_DEBUG("Found DT_NEEDED entry: {} with string table offset: {}", dynamic_count - 1, dyn.d_un.d_val);
        needed_offsets.push_back(dyn.d_un.d_val);
      } else if (dyn.d_tag == DT_STRTAB) {
        strtab_offset = dyn.d_un.d_ptr;
      }
    }
    if (strtab_offset == 0) {
      // We failed to read a DT_STRTAB entry from the dynamic section!
      LOG_DEBUG("Failed to find pointer to strtab! No DT_STRTAB section in: {}", path.c_str());
      if (munmap(mapped, size) != 0) {
        LOG_ERROR("Failed to munmap {}: {}", path.c_str(), std::strerror(errno));
      }
      if (close(fd) != 0) {
        LOG_ERROR("Failed to close fd for: {}: {}", path.c_str(), std::strerror(errno));
      }
      return dependencies;
    }
    for (auto needed_offset : needed_offsets) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
      std::string_view name = &readAtOffset<char const>(f, strtab_offset + needed_offset);

      if (name.empty()) {
        LOG_WARN("DT_NEEDED str is null! Bad ELF: {}, but continuing anyways...", path.c_str());
        continue;
      }
      LOG_DEBUG("DT_NEEDED name: {}", name);

      auto optObj = findSharedObject(dependencyDir, phase, name);

      if (optObj) {
        auto [obj, openedPhase] = std::move(*optObj);
        if (openedPhase == LoadPhase::None) {
          LOG_DEBUG("Unresolved dependency for: {}", obj.path.c_str());
          // Unresolved dependency
          dependencies.emplace_back(std::in_place_type_t<MissingDependency>{}, std::move(obj));
        } else {
          LOG_DEBUG("Resolved dependency for: {}", obj.path.c_str());
          // Resolved dependency
          // TODO: Make this avoid potentially stack overflowing on extremely nested dependency trees
          auto loadList = obj.getToLoad(dependencyDir, openedPhase, loadedDependencies);
          dependencies.emplace_back(std::in_place_type_t<Dependency>{}, std::move(obj), loadList);
        }
      } else {
        // Failed dependency (failed to check if it exists?)
        LOG_WARN("Skipping FAILED dependency: {}", name.data());
      }
    }
    // Because there is only one dynamic table, once we see it, we can just exit right away.
    break;
  }

  if (munmap(mapped, size) != 0) {
    LOG_ERROR("Failed to munmap {}: {}", path.c_str(), std::strerror(errno));
  }
  if (close(fd) != 0) {
    LOG_ERROR("Failed to close fd for: {}: {}", path.c_str(), std::strerror(errno));
  }
  LOG_DEBUG("Found a total of: {} dependencies successfully for: {}", dependencies.size(), path.c_str());

  return dependencies;
#endif
}

std::optional<std::string> LoadedMod::close() const noexcept {
  if (unloadFn) {
    (*unloadFn)();
  }
  if (dlclose(handle) != 0) {
    return std::string(dlerror());
  }
  return {};
}

void sortDependencies(std::span<Dependency> deps) {
  std::stable_sort(deps.begin(), deps.end(),
                   [](Dependency const& a, Dependency const& b) { return a.object.path > b.object.path; });
}

void sortDependencies(std::span<DependencyResult> deps) {
  std::stable_sort(deps.begin(), deps.end(), [](DependencyResult const& a, DependencyResult const& b) {
    std::string_view aPath;
    std::string_view bPath;
    if (holds_alternative<Dependency>(a)) {
      aPath = get<Dependency>(a).object.path.c_str();
    } else {
      aPath = get<MissingDependency>(a).path.c_str();
    }

    if (holds_alternative<Dependency>(b)) {
      bPath = get<Dependency>(b).object.path.c_str();
    } else {
      bPath = get<MissingDependency>(b).path.c_str();
    }

    return aPath > bPath;
  });
}

// https://www.geeksforgeeks.org/cpp-program-for-topological-sorting/
// Use mutable ref to avoid making a new vector that is sorted

/// @brief Performs a recursive topological sort for the given dependency, stack, and visited collection.
/// @param main The dependency to sort through. Must outlive the visited set
/// @param stack The stack to track the sorted dependencies
/// @param visited The set of all visited paths
void topologicalSortRecurse(Dependency& main, std::deque<Dependency>& stack,
                            std::unordered_set<std::string_view>& visited) {
  if (visited.contains(main.object.path.c_str())) {
    return;
  }

  visited.emplace(main.object.path.c_str());
  sortDependencies(main.dependencies);

  for (auto& depResult : main.dependencies) {
    auto* dep = get_if<Dependency>(&depResult);
    if (dep == nullptr) {
      continue;
    }
    topologicalSortRecurse(*dep, stack, visited);
  }

  stack.emplace_back(main);
}

// Copies FROM list into the dependencies
std::deque<Dependency> topologicalSort(std::span<DependencyResult const> list) {
  std::vector<Dependency> deps;
  deps.reserve(list.size());

  for (auto const& result : list) {
    auto const* dep = get_if<Dependency>(&result);
    if (dep == nullptr) {
      continue;
    }

    deps.push_back(*dep);
  }

  return topologicalSort(std::move(deps));
}

// Moves FROM list
std::deque<Dependency> topologicalSort(std::vector<Dependency>&& list) {
  std::deque<Dependency> dependencies{};
  std::unordered_set<std::string_view> visited{};

  sortDependencies(list);

  for (Dependency& dep : list) {
    topologicalSortRecurse(dep, dependencies, visited);
  }

  return dependencies;
}

std::vector<SharedObject> listAllObjectsInPhase(std::filesystem::path const& dependencyDir, LoadPhase phase) {
  std::error_code error_code;
  // Note: We see through this iteration at compile time
  for (auto const& [ph, path] : loadPhaseMap.arr) {
    if (ph != phase) {
      continue;
    }

    std::vector<SharedObject> objects{};

    auto dir = dependencyDir / path;
    std::filesystem::directory_iterator dir_iter(dir, error_code);
    if (error_code) {
      LOG_ERROR("Failed to find objects in phase: {} from: {}: {}", phase, dependencyDir.c_str(),
                error_code.message().c_str());
      return {};
    }
    for (auto const& file : dir_iter) {
      if (error_code) {
        LOG_WARN("Failed to open file while iterating: {}", dir.c_str());
        continue;
      }
      // TODO: Add statcheck here
      LOG_DEBUG("Walking over file: {}", file.path().c_str());
      // All SharedObjects must be valid lib*.so files
      if (file.is_directory()) {
        continue;
      }
      if (file.path().extension() != ".so") {
        continue;
      }
      if (!file.path().filename().string().starts_with("lib")) {
        continue;
      }

      LOG_DEBUG("Adding to attempt load: {}", file.path().c_str());
      objects.emplace_back(file.path());
    }

    std::sort(objects.begin(), objects.end(),
              [](SharedObject const& a, SharedObject const& b) constexpr { return a.path < b.path; });

    return objects;
  }
  return {};
}

// handle or failure message
using OpenLibraryResult = std::variant<void*, std::string>;

OpenLibraryResult openLibrary(std::filesystem::path const& path) {
  LOG_DEBUG("Attempting to dlopen: {}", path.c_str());
  dlerror();  // consume possible previous error
  // TODO: Figure out why symbols are leaking!
  auto* handle = dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW);
  auto* error = dlerror();
  if (handle == nullptr || error != nullptr) {
    // Error logging (for if symbols cannot be resolved)
    return std::string(error != nullptr ? error : "null handle no error");
  }

  return { handle };
}

template <typename T>
std::optional<T> getFunction(void* handle, std::string_view name, std::filesystem::path const& path) {
  dlerror();  // consume possible previous error
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto ptr = reinterpret_cast<T>(dlsym(handle, name.data()));
  auto* error = dlerror();
  // consume and print error
  if (!ptr || error != nullptr) {
    LOG_WARN("Could not find function with name {}: {}", name.data(),
             error != nullptr ? error : "null pointer no error");
    return std::nullopt;
  }

  LOG_DEBUG("Got function from handle {} with name: {}, addr: {}", handle, name.data(), fmt::ptr(ptr));

#ifdef SL2_KLEPTON
  // Klepton's handle is an image-registry handle, and the symbol may live in
  // an embedded framework. Darwin dladdr cannot validate its guest ELF path.
  if (!sc2_apple::owns(handle, reinterpret_cast<void*>(ptr), path.c_str())) {
    LOG_WARN("Symbol {} did not belong to requested object {}", name, path.c_str());
    return std::nullopt;
  }
#else
  Dl_info info;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (dladdr(reinterpret_cast<void* const>(ptr), &info)) {
    auto expectedObjName = path.filename();
    auto expectedObjParent = path.parent_path().filename();
    auto addrObjPath = std::filesystem::path(info.dli_fname);
    auto addrObjName = addrObjPath.filename();
    auto addrObjParent = addrObjPath.parent_path().filename();
    if (addrObjParent != expectedObjParent || addrObjName != expectedObjName) {
      LOG_WARN("The function {} {} is from {} but should be from {}!", name.data(), fmt::ptr(ptr), addrObjName.c_str(),
               expectedObjName.c_str());
      return std::nullopt;
    }
  } else {
    LOG_WARN("Could not find shared library for function {} {}", name.data(), fmt::ptr(ptr));
    return std::nullopt;
  }

#endif
  return ptr;
}

std::vector<LoadResult> loadMod(SharedObject&& mod, std::filesystem::path const& dependencyDir,
                                std::unordered_set<std::string>& skipLoad, LoadPhase phase) {
  if (skipLoad.contains(mod.path)) {
    LOG_WARN("Already loaded object at path: {}", mod.path.c_str());
    return {};
  }

  auto handleResult = [phase](OpenLibraryResult&& result, SharedObject&& obj,
                              std::vector<DependencyResult>&& dependencies) -> LoadResult {
    if (auto const* error = get_if<std::string>(&result)) {
      return FailedMod(std::move(obj), *error, std::move(dependencies));
    }

    auto* handle = get<void*>(result);

    LOG_INFO("Using handle {} for {}", handle, obj.path.c_str());

    // Default modinfo is full path and v0.0.0, 0
    // The lifetime of the fullpath's c_str() is longer than this ModInfo, since this SharedObject will live forever
    ModInfo modInfo(obj.path.c_str(), "0.0.0", 0);

    auto setupFn = getFunction<SetupFunc>(handle, "setup", obj.path);
    auto loadFn = getFunction<LoadFunc>(handle, "load", obj.path);
    auto late_loadFn = getFunction<LateLoadFunc>(handle, "late_load", obj.path);
    auto unloadFn = getFunction<UnloadFunc>(handle, "unload", obj.path);

    return LoadedMod(modInfo, std::move(obj), phase, setupFn, loadFn, late_loadFn, unloadFn, handle);
  };

  auto deps = mod.getToLoad(dependencyDir, phase);
  LOG_DEBUG("Fetched dependencies");
  // Sorted is a COPY of deps
  auto sorted = topologicalSort(static_cast<std::span<DependencyResult const>>(deps));
  LOG_DEBUG("Sorted dependencies");

  std::vector<LoadResult> results{};
  results.reserve(sorted.size());

  for (auto&& dep : sorted) {
    if (skipLoad.contains(dep.object.path)) {
      continue;
    }

    auto result = openLibrary(dep.object.path);
    skipLoad.emplace(dep.object.path);
    // The moves here are correct, because we are moving from the copied sorted collection
    auto const& handled =
        results.emplace_back(handleResult(std::move(result), std::move(dep.object), std::move(dep.dependencies)));

    if (auto const* failed = get_if<FailedMod>(&handled)) {
      // If we fail to open a dependency of the mod we are trying to open, we continue anyways, hoping that we will be
      // able to open later
      LOG_INFO("Failed to open dependency: {} for object: {}, {}! Trying anyways...", failed->object.path.c_str(),
               mod.path.c_str(), failed->failure.c_str());
    }
  }

  auto result = openLibrary(mod.path);
  skipLoad.emplace(mod.path);

  LOG_DEBUG("Loaded mod from path: {} with: {} (1 indicates failure that will be logged later)", mod.path.c_str(),
            result.index());
  results.emplace_back(handleResult(std::move(result), std::move(mod), std::move(deps)));
  return results;
}

// mods is an OWNING span of SharedObjects! They will be moved FROM mods into results
std::vector<LoadResult> loadMods(std::span<SharedObject> mods, std::filesystem::path const& dependencyDir,
                                 std::unordered_set<std::string>& skipLoad, LoadPhase phase) {
  std::vector<LoadResult> results;
  results.reserve(mods.size());

  for (auto&& mod : mods) {
    if (skipLoad.contains(mod.path)) {
      continue;
    }

    LOG_DEBUG("Attempting to dlopen and setup (moved) mod: {}", mod.path.c_str());
    auto otherResults = loadMod(std::move(mod), dependencyDir, skipLoad, phase);
    LOG_DEBUG("After opening mod, now have: {} opened libraries", otherResults.size());
    std::move(otherResults.begin(), otherResults.end(), std::back_inserter(results));
  }

  return results;
}
}  // namespace modloader
