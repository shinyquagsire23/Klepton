#include "apple-platform.h"
#include "modloader.h"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
struct Image { std::string name; } support{"libzsupport.so"}, consumer{"libaconsumer.so"}, early{"libearly.so"}, late{"liblate.so"};
std::vector<std::string> events;
std::string scenario, last_error;
const char *error() {
  static std::string result;
  if (last_error.empty()) return nullptr;
  result = std::move(last_error); last_error.clear(); return result.c_str();
}
void setup_early(CModInfo *info) noexcept {
  events.emplace_back("early.setup"); info->id="early"; info->version="1.0.0"; info->version_long=1;
}
void setup_late(CModInfo *info) noexcept {
  events.emplace_back("late.setup"); info->id="late"; info->version="1.0.0"; info->version_long=1;
}
void load_early() noexcept { events.emplace_back("early.load"); }
void late_early() noexcept { events.emplace_back("early.late_load"); }
void late_late() noexcept { events.emplace_back("late.late_load"); }
void *open(const char *path, int flags) {
  assert(flags == 2); // guest RTLD_NOW|RTLD_LOCAL, not Darwin's RTLD_LOCAL=4
  std::string name = std::filesystem::path(path).filename();
  if (scenario == "open-failure") { last_error="fixture dlopen failure"; return nullptr; }
  events.emplace_back("open:" + name);
  for (auto image : {&support, &consumer, &early, &late}) if (image->name == name) return image;
  assert(false); return nullptr;
}
void *symbol(void *handle, const char *name) {
  std::string query(name);
  if (handle == &early) {
    if (query == "setup") return reinterpret_cast<void*>(setup_early);
    if (query == "load") return reinterpret_cast<void*>(load_early);
    if (query == "late_load") return reinterpret_cast<void*>(late_early);
  }
  if (handle == &late) {
    if (query == "setup") return reinterpret_cast<void*>(setup_late);
    if (query == "late_load") return reinterpret_cast<void*>(late_late);
  }
  last_error="optional export absent"; return nullptr;
}
int close(void *) { return 0; }
int owns(void *handle, const void *, const char *path) {
  return scenario != "foreign-export" && static_cast<Image*>(handle)->name == std::filesystem::path(path).filename();
}
}
int main(int argc, char **argv) {
  assert(argc==3); scenario=argv[1];
  sc2_platform_api api{open, symbol, error, close, owns};
  assert(sc2_apple_before_first_scene() == -1);
  assert(sc2_apple_configure(&api, argv[2], "com.beatgames.beatsaber", nullptr) == 0);
  assert(std::string(modloader_get_application_id()) == "com.beatgames.beatsaber");
  auto writable = std::filesystem::path(argv[2]) / "userdata";
  std::filesystem::create_directory(writable);
  assert(sc2_apple_set_runtime_paths("/nonexistent/scotland2", writable.c_str(), "loader", "il2cpp") == -1);
  assert(sc2_apple_set_runtime_paths(writable.c_str(), writable.c_str(), "loader", "il2cpp") == 0);
  assert(std::filesystem::equivalent(modloader_get_files_dir(), writable));
  assert(std::filesystem::equivalent(modloader_get_external_dir(), writable));
  assert(sc2_apple_after_il2cpp_init() == -1);
  if (scenario == "malformed" || scenario == "open-failure") {
    assert(sc2_apple_open_libraries() == -1);
    assert(std::string(sc2_apple_error()).find(scenario == "malformed" ? "ELF" : "fixture dlopen failure") != std::string::npos);
    assert(modloader_get_failed());
    assert(sc2_apple_open_early() == -1);
    if (scenario == "malformed") assert(events.empty());
    std::cout << "PASS " << scenario << '\n'; return 0;
  }
  assert(sc2_apple_open_libraries() == 0);
  assert(sc2_apple_set_runtime_paths(argv[2], argv[2], "loader", "il2cpp") == -1);
  assert((events == std::vector<std::string>{"open:libzsupport.so", "open:libaconsumer.so"}));
  assert(sc2_apple_open_libraries() == -1);
  assert(sc2_apple_open_early() == 0);
  assert(early_mods_opened && !late_mods_opened);
  assert(sc2_apple_after_il2cpp_init() == 0);
  assert(sc2_apple_before_first_scene() == 0);
  assert(late_mods_opened && current_load_phase == LoadPhase_Mods);
  auto loaded = modloader_get_loaded();
  assert(loaded.size == 4);
  modloader_free_results(&loaded);
  if (scenario == "foreign-export") {
    assert((events == std::vector<std::string>{"open:libzsupport.so", "open:libaconsumer.so", "open:libearly.so", "open:liblate.so"}));
  } else {
    assert((events == std::vector<std::string>{"open:libzsupport.so", "open:libaconsumer.so", "open:libearly.so",
      "early.setup", "early.load", "open:liblate.so", "late.setup", "early.late_load", "late.late_load"}));
    CModInfo info{"early", "1.0.0", 1};
    auto found = modloader_get_mod(&info, MatchType_Strict);
    assert(found.handle == &early);
    // C API query makes copies; the aggregate free API handles this ownership.
    CModResults one{&found, 1};
    delete[] one.array[0].info.id; delete[] one.array[0].info.version; delete[] one.array[0].path;
  }
  assert(!modloader_get_failed());
  assert(sc2_apple_before_first_scene() == -1);
  assert(sc2_apple_configure(&api, argv[2], "other", nullptr) == -1);
  std::cout << "PASS " << scenario << '\n';
}
