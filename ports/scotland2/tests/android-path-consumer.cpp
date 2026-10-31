// A separate Android module consumes the actual C++ API across a DSO boundary.
#include "modloader.h"
#include <cstring>

extern "C" __attribute__((visibility("default"))) int sc2_test_android_paths() {
  using namespace modloader;
  struct Entry {
    std::filesystem::path const& (*cpp)() noexcept;
    const char *(*c)();
  };
  const Entry entries[] = {
    {get_modloader_path,modloader_get_path},
    {get_modloader_source_path,modloader_get_source_path},
    {get_modloader_root_load_path,modloader_get_root_load_path},
    {get_files_dir,modloader_get_files_dir},
    {get_external_dir,modloader_get_external_dir},
    {get_libil2cpp_path,modloader_get_libil2cpp_path},
  };
  for (auto entry : entries) {
    const auto& value = entry.cpp();
    if (&value != &entry.cpp() || value.native() != entry.c()) return 1;
    auto copy = value;
    copy /= "a-long-enough-component-to-force-heap-allocation";
    if (copy.filename() != "a-long-enough-component-to-force-heap-allocation") return 2;
    if (copy.parent_path() != value || value.native() != entry.c()) return 3;
  }
  const auto& id = get_application_id();
  if (&id != &get_application_id() || id != modloader_get_application_id()) return 4;
  auto copied = id;
  copied += "-modified-by-another-Android-DSO";
  if (id != modloader_get_application_id() || copied == id) return 5;
  return 0;
}
