// libvulkan.so.1, for a Linux guest: Klepton's synthetic loader over MoltenVK
// (runtime/gfx/kl_vulkan.c), handed to the guest's ld.so as a host library.
//
// The symbol table is every Vulkan command kl_vulkan_sym() can answer for. That
// is asked lazily, when the guest first opens the library, because the answer
// involves loading MoltenVK and a guest that never draws should not pay for it.
#include <stdlib.h>
#include "kl_lx.h"
#include "../gfx/kl_vulkan.h"

static const char *const k_names[] = {
#include "kl_lx_vk_names.h"
};

static size_t build(kl_lx_hostsym **out) {
    size_t max = sizeof k_names / sizeof *k_names, n = 0;
    kl_lx_hostsym *s = calloc(max, sizeof *s);
    for (size_t i = 0; i < max; i++) {
        void *fn = kl_vulkan_sym(k_names[i]);
        if (fn) s[n++] = (kl_lx_hostsym){ k_names[i], fn };
    }
    *out = s;
    return n;
}

void kl_lx_vulkan_init(void) {
    kl_lx_hostlib_register("libvulkan.so.1", build);
    kl_lx_hostlib_register("libvulkan.so", build);
}
