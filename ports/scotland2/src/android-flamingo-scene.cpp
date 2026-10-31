// Original Scotland2 first-DestroyObjectHighLevel lifecycle, via its real
// Flamingo installer and a C boundary to the native loader core.
#include "installer.hpp"
#include <atomic>
#include <cstdlib>
namespace {
flamingo::HookHandle scene_handle;
void (*scene_original)(void *,bool);
int (*scene_callback)();
std::atomic<bool> armed{false},fired{false};
void scene_hook(void *object,bool flag) noexcept {
    scene_original(object,flag);
    if (armed.load(std::memory_order_acquire) && !fired.exchange(true,std::memory_order_acq_rel)) {
        auto result=flamingo::Uninstall(scene_handle);
        if (!result.has_value() || scene_callback()) std::abort();
    }
}
}
extern "C" __attribute__((visibility("default")))
int sc2_install_original_scene_hook(void *target,int (*callback)()) {
    if (!target || !callback || armed.load()) return -1;
    scene_callback=callback;
    auto result=flamingo::Install(flamingo::HookInfo(scene_hook,target,&scene_original));
    if (!result.has_value()) return -1;
    scene_handle=result.value().returned_handle;
    armed.store(true,std::memory_order_release);
    return 0;
}
