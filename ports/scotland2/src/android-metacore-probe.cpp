#if !defined(SC2_DIAGNOSTICS) || SC2_DIAGNOSTICS
// Host-only entry to the original installer, without creating Unity objects.
// The normal device build continues to use MetaCore's original late_load.
#include "hooks.hpp"
extern "C" __attribute__((visibility("default")))
int sc2_test_metacore_install_hooks() {
    Hooks::Install();
    return 0;
}

#endif
