// Build with the Android NDK, never Darwin libc++. Only C strings cross into
// the native loader; callers receive genuine Android C++ objects.
#include "modloader.h"

namespace modloader {
#define PATH_GETTER(cpp, c) \
  std::filesystem::path const& cpp() noexcept { \
    static const std::filesystem::path value{::c()}; \
    return value; \
  }
PATH_GETTER(get_modloader_path, modloader_get_path)
PATH_GETTER(get_modloader_source_path, modloader_get_source_path)
PATH_GETTER(get_modloader_root_load_path, modloader_get_root_load_path)
PATH_GETTER(get_files_dir, modloader_get_files_dir)
PATH_GETTER(get_external_dir, modloader_get_external_dir)
PATH_GETTER(get_libil2cpp_path, modloader_get_libil2cpp_path)
#undef PATH_GETTER

std::string const& get_application_id() noexcept {
  static const std::string value{::modloader_get_application_id()};
  return value;
}
}
