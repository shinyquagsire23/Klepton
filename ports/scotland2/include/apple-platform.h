#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// A native Apple loader core calls into Klepton through this C boundary.
// This does not expose Darwin C++ objects to Android std::__ndk1 consumers.
typedef struct {
  void *(*open)(const char *guest_path, int flags);
  void *(*symbol)(void *handle, const char *name);
  const char *(*error)(void);
  int (*close)(void *handle);
  int (*owns)(void *handle, const void *symbol, const char *guest_path);
} sc2_platform_api;

// Once per process. Metadata roots contain the selected signed composition.
int sc2_apple_configure(const sc2_platform_api *api, const char *root,
                        const char *application_id, void *java_vm);
// Explicit lifecycle entry points. No runtime code patching or automatic hooks.
int sc2_apple_open_libraries(void);
int sc2_apple_open_early(void);
int sc2_apple_set_game_handles(void *il2cpp_handle, void *unity_handle);
int sc2_apple_set_runtime_paths(const char *files, const char *external,
                                 const char *loader, const char *il2cpp);
int sc2_apple_after_il2cpp_init(void);
int sc2_apple_before_first_scene(void);
const char *sc2_apple_error(void);

#ifdef __cplusplus
}
#endif
