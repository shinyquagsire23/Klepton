// The Linux process runtime: what a glibc guest stands on.
//
// An Android guest is a set of libraries we load and call into, with a libc of
// ours underneath. A Linux guest is the opposite shape — a PROGRAM, with its own
// dynamic loader and its own glibc, that expects a kernel. So nothing above the
// syscall boundary is replaced here: the guest's real ld.so relocates the guest's
// real libc, and this directory answers `svc #0` (veneered by klepton-ld under
// --abi linux, see kl_x18.h) the way Linux would.
#ifndef KL_LX_H
#define KL_LX_H
#include <stddef.h>
#include <stdint.h>

// The guest's registers at an `svc`, as kl_lx_svc.S saves them. Linux preserves
// everything but x0 across a syscall, flags and vector registers included, and
// compiled code relies on that, so everything is here and everything is put
// back. x[16] and x[30] are the veneer's (it called us through x16); the
// guest's own values are the pair the veneer pushed, directly above the frame.
typedef struct {
    uint64_t    x[31];
    uint64_t    nzcv;
    __uint128_t q[32];
    uint64_t    guest_x16, guest_x30;
} kl_lx_frame;

// What tsd[KLX_SVC_SLOT] holds on every guest thread.
void kl_lx_svc_entry(void);

// The syscall table. Returns what the guest's x0 becomes: a value, or a negated
// LINUX errno.
int64_t kl_lx_syscall(kl_lx_frame *f);

// Never returns: sets sp and branches, with every other register zeroed the way
// the kernel hands a new process to its entry point.
_Noreturn void kl_lx_enter(uint64_t entry, uint64_t sp);

// Run a translated program, named by its GUEST path; its translation and its
// interpreter's are found through kl_lx_add_dylib_dir(). Returns only on
// failure — the guest leaves through exit_group.
int kl_lx_exec(const char *prog, int argc, char **argv, char **envp);
// Called with the status just before the process ends on the guest's exit_group.
void kl_lx_set_exit_hook(void (*fn)(int status));

// ---- the file namespace (kl_lx_vfs.c) ----
// Mount a host directory at a guest prefix ("/usr", "/game"); longest wins.
void kl_lx_mount(const char *guest_prefix, const char *host_dir);
// ...or mount it read-only: opens for writing and anything that would create,
// remove or rename there answer EROFS.
void kl_lx_mount_ro(const char *guest_prefix, const char *host_dir);
int  kl_lx_host_ro(const char *host_path);
// Where translations live; searched in order by kl_lx_find_dylib().
void kl_lx_add_dylib_dir(const char *dir);
// `fd` is the ELF itself, open: a translation is found by name AND content
// (kl_lx_name.h).
int  kl_lx_find_dylib(const char *elf_name, int fd, char *out, size_t cap);
// Resolve a guest path chroot-style to a host path. 0, or -1 with errno set.
// `guest_out` (PATH_MAX, may be NULL) receives the normalised guest path.
int  kl_lx_resolve(const char *path, int follow_last, char *host, size_t cap, char *guest_out);
int  kl_lx_chdir(const char *path);
const char *kl_lx_cwd(void);
void        kl_lx_set_exe(const char *guest_path);
const char *kl_lx_exe(void);

// What is remembered about an open descriptor.
typedef struct {
    int      elf;           // 1: an ELF we hold a translation of; -1: an ELF we do not;
                            // 2: a host library — data only, mapped like any file
    uint64_t phoff;
    uint16_t phnum;
    char     guest[1024];
    char     dylib[1024];
    void    *dir;           // a DIR* over a dup of this fd, once getdents64 has asked
    struct kl_lx_eventfd *efd;   // set if this is an eventfd (kl_lx_event.c)
} kl_lx_fdinfo;
kl_lx_fdinfo *kl_lx_fd(int fd);
int  kl_lx_open(const char *path, int lx_flags, int mode);   // fd, or -1 with errno
void kl_lx_close(int fd);
void kl_lx_elf_view(int fd, uint8_t *buf, size_t n, uint64_t off);
int64_t kl_lx_getdents64(int fd, void *buf, size_t cap);     // bytes, or -1 with errno
struct stat;
void kl_lx_stat_out(const struct stat *s, void *out);

// ---- memory (kl_lx_mm.c) ----
// Map a translation and register its range under the guest path it was loaded
// by. Returns the address its ELF header was mapped at, or NULL.
const void *kl_lx_map_image(const char *dylib_path, const char *guest_path);
// The i-th registered image; 0 when there is no such index.
int kl_lx_image_info(int i, uint64_t *lo, uint64_t *data, uint64_t *hi, const char **guest_path);
int64_t kl_lx_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, int64_t off);
int64_t kl_lx_munmap(uint64_t addr, uint64_t len);
int64_t kl_lx_mprotect(uint64_t addr, uint64_t len, int prot);
int64_t kl_lx_madvise_dontneed(uint64_t addr, uint64_t len);
// Was this address in memory the guest asked to be executable (and was given
// without the bit)? A fault with its pc there is a JIT.
int     kl_lx_wanted_exec(uint64_t addr);

// ---- host libraries (kl_lx_hostlib.c) ----
// A library of host functions, shown to the guest's loader as an ELF of that
// soname. `build` is called once, on first open, and returns a malloc'd array.
typedef struct { const char *name; void *addr; } kl_lx_hostsym;
void kl_lx_hostlib_register(const char *soname, size_t (*build)(kl_lx_hostsym **out));
int  kl_lx_hostlib_is(const char *path);
int  kl_lx_hostlib_open(const char *path);                       // fd, or -1 with errno
// libvulkan.so.1 as one of those, over MoltenVK (kl_lx_vulkan.c).
void kl_lx_vulkan_init(void);
// libsteam_api.so as another: Steam, offline (kl_lx_steam.c).
void kl_lx_steam_init(void);
// libopenxr_loader.so.1: Klepton's OpenXR runtime (kl_lx_openxr.c).
void kl_lx_openxr_init(void);
// Sound: the guest's SDL writes PCM to /dev/klepton-audio (kl_lx_audio.c).
int  kl_lx_audio_is(const char *path);
int  kl_lx_audio_open(void);                                     // fd, or -1 with errno
// The loading screen a SteamVR game describes instead of drawing: its messages
// arrive at /dev/klepton-interstitial (kl_lx_interstitial.c).
int  kl_lx_interstitial_is(const char *path);
int  kl_lx_interstitial_open(void);                              // fd, or -1 with errno
// The dominant hand a SteamVR game sets, from xrizer: one byte "L" or "R" at
// /dev/klepton-dominant-hand (kl_lx_interstitial.c).
int  kl_lx_hand_is(const char *path);
int  kl_lx_hand_open(void);                                      // fd, or -1 with errno
// Opt-in diagnostics that interpose on the guest's libc (kl_lx_diag.c).
void kl_lx_diag_init(void);

// ---- eventfd (kl_lx_event.c) ----
int     kl_lx_eventfd(unsigned initval, int flags);              // fd, or -1 with errno
void    kl_lx_eventfd_close(struct kl_lx_eventfd *e);
int64_t kl_lx_eventfd_write(struct kl_lx_eventfd *e, const void *buf, size_t n);
int64_t kl_lx_eventfd_read(int fd, struct kl_lx_eventfd *e, void *buf, size_t n);

// ---- /proc and /sys (kl_lx_proc.c) ----
void    kl_lx_proc_init(int argc, char **argv, uint64_t stack_lo, uint64_t stack_hi);
int     kl_lx_proc_is(const char *path);
int     kl_lx_proc_open(const char *path);                       // fd, or -1 with errno
int     kl_lx_proc_stat(const char *path, int follow, struct stat *st);
long    kl_lx_proc_readlink(const char *path, char *out, size_t cap);

// Report the guest's fatal signals in the guest's terms (kl_lx_signal.c).
void    kl_lx_signal_init(void);
void    kl_lx_backtrace(const kl_lx_frame *f);
void    kl_lx_stack_strings(uint64_t sp);
// The frame of the syscall this thread is in, for code below kl_lx_syscall.
extern _Thread_local kl_lx_frame *kl_lx_cur;

// ---- threads, futexes, signals (kl_lx_thread.c) ----
struct timespec;
int     kl_lx_gettid(void);
int64_t kl_lx_set_tid_address(uint32_t *p);
int64_t kl_lx_futex(uint32_t *uaddr, int op, uint32_t val, const struct timespec *ts,
                    uint32_t *uaddr2, uint32_t val3);
int64_t kl_lx_clone(kl_lx_frame *f);
_Noreturn void kl_lx_thread_exit(int code);
int     kl_lx_in_vfork(void);
int64_t kl_lx_tkill(int tid, int sig);               // signal 0 = "does that thread exist?"
int64_t kl_lx_execve(const char *path, char *const *argv);
int64_t kl_lx_wait4(int pid, int *status);
int64_t kl_lx_rt_sigaction(int sig, const void *act, void *old);
int64_t kl_lx_rt_sigprocmask(int how, const uint64_t *set, uint64_t *old);
int64_t kl_lx_raise(int sig);

#endif
