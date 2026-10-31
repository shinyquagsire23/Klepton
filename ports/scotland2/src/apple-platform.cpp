#include "apple-platform.hpp"
#include "internal-loader.hpp"
#include "modloader.h"
#include <filesystem>
#include <string>
#include <stdexcept>

namespace {
sc2_platform_api platform{};
std::filesystem::path root_path, files_path, external_path, loader_path, il2cpp_path;
std::string app_id, failure;
std::string metadata_failure, pending_error;
enum class State { empty, configured, libraries, early_opened, il2cpp_ready, scene_ready, failed };
State state = State::empty;
int reject(const char *message) { failure = message; return -1; }
int check_phase(State next) {
  for (auto const& result : modloader::get_all()) {
    if (auto failed = std::get_if<modloader::FailedMod>(&result)) {
      state = State::failed;
      failure = failed->object.path.string() + ": " + failed->failure;
      return -1;
    }
  }
  state = next;
  failure.clear();
  return 0;
}
}

namespace sc2_apple {
void *open(const char *path, int flags) {
  if (!metadata_failure.empty()) { pending_error = metadata_failure; return nullptr; }
  // Android RTLD_NOW is 2, Darwin RTLD_NOW is 2; RTLD_LOCAL differs (0 / 4).
  // This backend always requests guest RTLD_NOW | RTLD_LOCAL, independent of host constants.
  (void)flags;
  return platform.open(path, 2);
}
void *symbol(void *handle, const char *name) { return platform.symbol(handle, name); }
const char *error() {
  if (!pending_error.empty()) {
    static std::string message;
    message = std::move(pending_error);
    pending_error.clear();
    return message.c_str();
  }
  return platform.error();
}
int close(void *handle) { return platform.close(handle); }
bool owns(void *handle, const void *symbol, const char *path) {
  return platform.owns(handle, symbol, path) != 0;
}
std::vector<std::string> dependencies(std::filesystem::path const& path) {
  try { return read_dependencies(path); }
  catch (std::exception const& error) {
    metadata_failure = path.string() + ": " + error.what();
    return {};
  }
}
}

namespace modloader {
std::filesystem::path const& get_modloader_path() noexcept { return loader_path; }
std::filesystem::path const& get_modloader_source_path() noexcept { return loader_path; }
std::filesystem::path const& get_modloader_root_load_path() noexcept { return root_path; }
std::filesystem::path const& get_files_dir() noexcept { return files_path; }
std::filesystem::path const& get_external_dir() noexcept { return external_path; }
std::filesystem::path const& get_libil2cpp_path() noexcept { return il2cpp_path; }
std::string const& get_application_id() noexcept { return app_id; }
}

extern "C" {
bool modloader_get_failed() { return !failure.empty(); }
const char *modloader_get_path() { return loader_path.c_str(); }
const char *modloader_get_source_path() { return loader_path.c_str(); }
const char *modloader_get_root_load_path() { return root_path.c_str(); }
const char *modloader_get_files_dir() { return files_path.c_str(); }
const char *modloader_get_external_dir() { return external_path.c_str(); }
const char *modloader_get_libil2cpp_path() { return il2cpp_path.c_str(); }
const char *modloader_get_application_id() { return app_id.c_str(); }
// Klepton owns library lookup; Android linker namespaces do not exist here.
bool modloader_add_ld_library_path(const char *) { return false; }

int sc2_apple_configure(const sc2_platform_api *api, const char *root,
                        const char *application_id, void *java_vm) {
  if (state != State::empty) return reject("already configured in this process");
  if (!api || !api->open || !api->symbol || !api->error || !api->close || !api->owns ||
      !root || !*root || !application_id || !*application_id)
    return reject("missing platform callback or application path");
  std::error_code ec;
  auto candidate = std::filesystem::canonical(root, ec);
  if (ec) return reject("metadata root does not exist");
  for (auto phase : {"libs", "early_mods", "mods", "shims"}) {
    if (!std::filesystem::is_directory(candidate / phase, ec) || ec)
      return reject("metadata root lacks a phase directory");
  }
  platform = *api;
  root_path = candidate;
  files_path = root_path;
  external_path = root_path / "external";
  loader_path = root_path / "libsl2.so";
  il2cpp_path = "libil2cpp.so";
  app_id = application_id;
  modloader_jvm = static_cast<JavaVM*>(java_vm);
  state = State::configured;
  failure.clear();
  return 0;
}
int sc2_apple_set_runtime_paths(const char *files, const char *external,
                                 const char *loader, const char *il2cpp) {
  if (state != State::configured) return reject("runtime paths must be set before libraries open");
  if (!files || !external || !loader || !il2cpp || !*loader || !*il2cpp)
    return reject("missing runtime path");
  std::error_code ec;
  auto candidate_files = std::filesystem::canonical(files, ec);
  if (ec || !std::filesystem::is_directory(candidate_files, ec) || ec) return reject("invalid writable files directory");
  auto candidate_external = std::filesystem::canonical(external, ec);
  if (ec || !std::filesystem::is_directory(candidate_external, ec) || ec) return reject("invalid external directory");
  files_path = candidate_files;
  external_path = candidate_external;
  loader_path = loader;
  il2cpp_path = il2cpp;
  return 0;
}
int sc2_apple_open_libraries() {
  if (state != State::configured) return reject("libraries phase out of order");
  modloader::open_libs(root_path);
  return check_phase(State::libraries);
}
int sc2_apple_open_early() {
  if (state != State::libraries) return reject("early-open phase out of order");
  modloader::open_early_mods(root_path);
  return check_phase(State::early_opened);
}
int sc2_apple_after_il2cpp_init() {
  if (state != State::early_opened) return reject("IL2CPP phase out of order");
  modloader::load_early_mods();
  return check_phase(State::il2cpp_ready);
}
int sc2_apple_set_game_handles(void *il2cpp_handle, void *unity_handle) {
  if (state != State::early_opened || !il2cpp_handle) return reject("game handles must be set before IL2CPP init");
  modloader_libil2cpp_handle = il2cpp_handle;
  modloader_unity_handle = unity_handle;
  return 0;
}
int sc2_apple_before_first_scene() {
  if (state != State::il2cpp_ready) return reject("scene phase out of order");
  modloader::open_mods(root_path);
  if (check_phase(State::il2cpp_ready) != 0) return -1;
  modloader::load_mods();
  return check_phase(State::scene_ready);
}
const char *sc2_apple_error() { return failure.c_str(); }
}
