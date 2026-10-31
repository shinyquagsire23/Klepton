// Run a translated Linux/glibc program on the host: `make lx-boot`.
//
//   m_linux [--mount[-ro] <guest>=<host>]... [--dylibs <dir>]... [--env K=V]...
//           [--cwd <guest dir>] <guest program> [args...]
//
// The mounts build the guest's root (kl_lx_vfs.c); the dylib directories hold
// `klepton-ld --abi linux` output, ad-hoc signed — the same signed-Mach-O path a
// headset takes, never the mmap one, so that what differs on device is the
// platform and not the loader. The guest gets the environment given here and
// nothing of the host's.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/linux/kl_lx.h"

int main(int argc, char **argv) {
    char *env[64] = { "PATH=/usr/bin:/bin", "HOME=/tmp", "LANG=C", "TERM=xterm" };
    int nenv = 4, i = 1;
    const char *cwd = NULL;
    for (; i + 1 < argc && argv[i][0] == '-'; i += 2) {
        char *eq = strchr(argv[i + 1], '=');
        if (!strcmp(argv[i], "--mount") && eq) { *eq = 0; kl_lx_mount(argv[i + 1], eq + 1); }
        else if (!strcmp(argv[i], "--mount-ro") && eq) { *eq = 0; kl_lx_mount_ro(argv[i + 1], eq + 1); }
        else if (!strcmp(argv[i], "--dylibs")) kl_lx_add_dylib_dir(argv[i + 1]);
        else if (!strcmp(argv[i], "--env") && nenv < 63) {
            // Replacing, not appending: getenv returns the first match, so a
            // second HOME= after the default one would never be seen.
            size_t klen = eq ? (size_t)(eq - argv[i + 1]) + 1 : 0;
            int at = nenv;
            for (int k = 0; k < nenv && klen; k++)
                if (!strncmp(env[k], argv[i + 1], klen)) at = k;
            env[at] = argv[i + 1];
            if (at == nenv) nenv++;
        }
        else if (!strcmp(argv[i], "--cwd")) cwd = argv[i + 1];
        else break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: m_linux [--mount <guest>=<host>]... [--dylibs <dir>]... "
                        "[--env K=V]... [--cwd <dir>] <guest program> [args...]\n");
        return 2;
    }
    if (cwd && kl_lx_chdir(cwd) != 0) { perror(cwd); return 2; }
    kl_lx_vulkan_init();
    kl_lx_steam_init();
    kl_lx_openxr_init();
    kl_lx_diag_init();
    kl_lx_exec(argv[i], argc - i, argv + i, env);
    fprintf(stderr, "m_linux: could not start %s\n", argv[i]);
    return 1;
}
