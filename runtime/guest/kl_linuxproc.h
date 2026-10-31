// A whole Linux process as the guest — kl_linuxproc's door.
//
// Every other door here enters an Android library through the entry point its
// Java would have called. This one has no library and no Java: the guest is a
// glibc program, started the way a kernel starts one (runtime/linux/kl_lx_exec.c)
// with the Steam Runtime's own ld.so doing the loading. What the door owns is
// where that process's world is on THIS machine — the mounts, the environment
// and the command line — and the thread it lives on.
#ifndef KL_LINUXPROC_H
#define KL_LINUXPROC_H
#include <stdio.h>

// `root` holds the staged tree (visionos/stage_sync.py writes it):
//
//   <root>/sysroot   the Steam Linux Runtime's files/            -> /usr /lib /bin /etc
//   <root>/game      the game's install directory               -> /game
//   <root>/xrizer    OpenVR-over-OpenXR, bin/linuxarm64/vrclient.so -> /opt/xrizer
//   <root>/home      the guest's HOME, writable                  -> /home/user
//   <root>/tmp                                                   -> /tmp
//   <root>/links.tsv every symbolic link of the trees above, because the copy
//                    onto a device does not carry them; recreated at configure.
//
// `dylibs` is where the translations are: a directory of <name>.dylib on the
// host, the bundle's Frameworks/ in the app. May be called with several
// directories separated by ':'.
//
// The program and its arguments come from the environment, so a launch can
// change them without a rebuild:
//
//   KL_LX_PROG   guest path of the program   (default /game/bin/linuxsteamrtarm64/hlvr)
//   KL_LX_ARGS   its arguments, blank-separated
//                (default "-vr -vulkan +r_vulkan_sw_cmd_lists_suspend_resume 0
//                 +vr_symmetric_fov 0 +map startup")
//   KL_LX_ARGS_EXTRA  more arguments, placed before those (default none)
//   KL_LX_CWD    its working directory       (default /game)
//   KL_LX_ENV    extra K=V pairs, blank-separated, replacing the defaults by key
int kl_linuxproc_configure(const char *root, const char *dylibs, FILE *out);

// Start the process on a thread of its own. Returns once it is running; the
// process ending is reported by kl_linuxproc_exited().
int kl_linuxproc_begin(FILE *out);
int kl_linuxproc_exited(void);

double      kl_linuxproc_pump(double seconds, const volatile int *quit);
void        kl_linuxproc_report(FILE *out);
const char *kl_linuxproc_error(void);

#endif
