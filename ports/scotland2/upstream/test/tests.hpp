#include <filesystem>
#include <span>

#include "internal-loader.hpp"

namespace tests {
void loadModsTest(const std::filesystem::path& path);

std::vector<modloader::DependencyResult> getDependencyTreeTest(const std::filesystem::path& dependencyPath,
                                                               std::filesystem::path modPath);
void sortDependencyTreeTest(std::span<modloader::DependencyResult const> dependencies);
}  // namespace tests
