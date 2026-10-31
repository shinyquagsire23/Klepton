#ifdef LINUX_TEST

#include "internal-loader.hpp"
#include "tests.hpp"

int main() {
  fprintf(stderr, "PLEASE\n");
  auto dependencyPath = std::filesystem::current_path() / "test";
  auto modPath = dependencyPath / "mods" / "libchroma.so";
  // Load

  auto deps = tests::getDependencyTreeTest(dependencyPath, modPath);
  tests::sortDependencyTreeTest(deps);

  // tests::loadModsTest(dependencyPath);
}

#endif