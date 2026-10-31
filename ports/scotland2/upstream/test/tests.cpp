#include "tests.hpp"

#include "internal-loader.hpp"

#ifdef LINUX_TEST
#include <iostream>

template <typename T>
void writeSingle(T&& arg) {
  std::cout << std::forward<T>(arg);
}
template <>
void writeSingle(wchar_t const*& arg) {
  std::wcout << arg;
}

template <typename... TArgs>
void write(TArgs&&... args) {
  (writeSingle(args), ...);
  std::cout << std::endl;
}
#else
template <typename... TArgs>
void write(TArgs&&...) {}
#endif

// recursion
void logDependencies(std::span<modloader::DependencyResult const> dependencies, size_t indent = 1) {
  for (auto const& result : dependencies) {
    std::string depName;

    auto const* dep = get_if<modloader::Dependency>(&result);
    if (dep != nullptr) {
      depName = dep->object.path.filename().string();
    } else {
      depName = "missing dependency " + get<modloader::MissingDependency>(result).path.string();
    }

    write(std::string(indent * 3, ' '), '-', depName);
    if (dep != nullptr) {
      logDependencies(dep->dependencies, indent + 1);
    }
  }
}

void tests::loadModsTest(std::filesystem::path const& path) {
  auto earlyMods = modloader::listAllObjectsInPhase(path, modloader::LoadPhase::EarlyMods);
  auto mods = modloader::listAllObjectsInPhase(path, modloader::LoadPhase::Mods);

  std::unordered_set<std::string> loadedPaths = {};

  write("Loading early mods");
  auto results = modloader::loadMods(earlyMods, path, loadedPaths, modloader::LoadPhase::EarlyMods);

  write("Test double load");
  static_cast<void>(modloader::loadMods(earlyMods, path, loadedPaths, modloader::LoadPhase::EarlyMods));

  write("Loading late mods");
  auto late_results = modloader::loadMods(mods, path, loadedPaths, modloader::LoadPhase::Mods);
}
// NOLINTNEXTLINE
std::vector<modloader::DependencyResult> tests::getDependencyTreeTest(std::filesystem::path const& dependencyPath,
                                                                      std::filesystem::path modPath) {
  auto mod = modloader::SharedObject(std::move(modPath));

  write("Loading ", mod.path.c_str());

  auto dependencies = mod.getToLoad(dependencyPath, modloader::LoadPhase::Mods);

  logDependencies(dependencies);

  write(std::string(3, '\n'), "Sorted:");

  return dependencies;
}

void tests::sortDependencyTreeTest(std::span<modloader::DependencyResult const> dependencies) {
  auto sorted = modloader::topologicalSort(dependencies);
  while (!sorted.empty()) {
    auto const& dep = sorted.front();
    write("-", dep.object.path.filename());
    sorted.pop_front();
  }
}