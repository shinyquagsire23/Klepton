// libopenxr_loader.so.1, for a Linux guest: Klepton's OpenXR runtime
// (runtime/xr/kl_openxr.c), handed to the guest's ld.so as a host library.
//
// The guest that wants it is not the game. Alyx speaks OpenVR; what speaks
// OpenXR is the translation layer loaded in SteamVR's place (xrizer), and on
// Linux that finds a runtime by opening the Khronos loader. Here the loader's
// name leads straight to the runtime, the same replacement kl_openxr.c already
// makes on Android and for the same reason: the real loader's whole job is to
// find a system service that is not here.
#include <stdlib.h>
#include "kl_lx.h"
#include "../xr/kl_openxr.h"

static size_t build(kl_lx_hostsym **out) {
    size_t n = 0, cap = 0;
    while (kl_openxr_name((int)cap)) cap++;
    kl_lx_hostsym *s = calloc(cap ? cap : 1, sizeof *s);
    for (size_t i = 0; i < cap; i++) {
        const char *name = kl_openxr_name((int)i);
        void *fn = kl_openxr_lookup(name);
        if (fn) s[n++] = (kl_lx_hostsym){ name, fn };
    }
    *out = s;
    return n;
}

void kl_lx_openxr_init(void) {
    kl_lx_hostlib_register("libopenxr_loader.so.1", build);
    kl_lx_hostlib_register("libopenxr_loader.so", build);
}
