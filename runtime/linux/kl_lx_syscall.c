// The Linux syscall table, as far as the guest has needed it so far.
//
// Numbers are the arm64 (asm-generic) ones. Every answer is a value or a negated
// LINUX errno — Darwin's errno numbering diverges above 34, and glibc compares
// against its own. A number nobody serves yet is reported once, by number, and
// answered ENOSYS: glibc treats that as "kernel too old" for most of the newer
// calls and falls back, and for the rest the report is the work list.
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include "kl_lx.h"
#include "../klepton.h"
#include "../kl_env.h"

// Told first when the guest process ends, for a host that is more than a
// launcher (the visionOS app writes its report and status before the process
// goes away).
static void (*g_exit_hook)(int status);
void kl_lx_set_exit_hook(void (*fn)(int status)) { g_exit_hook = fn; }

_Static_assert(offsetof(kl_lx_frame, nzcv) == 248 && offsetof(kl_lx_frame, q) == 256 &&
               offsetof(kl_lx_frame, guest_x16) == 768,
               "kl_lx_frame no longer matches kl_lx_svc.S");

#define LX_ENOSYS 38

static int64_t lx_err(void) { return -(int64_t)kl_errno_to_linux(errno); }
static int64_t lx_ret(int64_t r) { return r < 0 ? lx_err() : r; }

// Kernel-shaped calls the Android personality already serves (kl_libc_slink.c):
// they take Linux's flag words and struct layouts, and answer -1 with errno.
int klb_epoll_create1(int flags);
int klb_epoll_ctl(int epfd, int op, int fd, void *ev);
int klb_epoll_wait(int epfd, void *events, int maxevents, int timeout_ms);
int klb_ppoll(struct pollfd *fds, unsigned long nfds, const struct timespec *ts, const void *sigmask);
int klb_pipe2(int fds[2], int flags);

static int trace_on(void) {
    static int cached = -1;
    if (cached < 0) cached = kl_env_on("KL_LX_TRACE", 0);
    return cached;
}

// KL_LX_MMTRACE=<file>: every call that changes the address space, one line
// each — who asked, for what, and what they were told. The questions this
// answers ("was that memory ever handed out before?") cannot be asked of the
// full trace, which is a hundred times the volume.
static void mm_trace(const char *what, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                     int64_t r) {
    static int fd = -2;
    if (fd == -2) {
        const char *p = getenv("KL_LX_MMTRACE");
        fd = p && *p ? open(p, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644) : -1;
    }
    if (fd < 0) return;
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    dprintf(fd, "%llu %s %#llx %#llx %#llx %#llx -> %#llx\n", (unsigned long long)tid, what,
            (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2,
            (unsigned long long)a3, (unsigned long long)r);
}

// ---- files ----
#define LX_AT_FDCWD            (-100)
#define LX_AT_SYMLINK_NOFOLLOW 0x100
#define LX_AT_EMPTY_PATH       0x1000

// A path argument, as the *at calls take it. Absolute and cwd-relative paths go
// through the guest namespace; one relative to a real directory fd is the
// host's to resolve, since that fd already names a host directory.
static int at_path(int dirfd, const char *path, int follow, char *host, size_t cap) {
    if (!path) { errno = EFAULT; return -1; }
    if (path[0] != '/' && dirfd != LX_AT_FDCWD) { errno = ENOSYS; return -1; }
    return kl_lx_resolve(path, follow, host, cap, NULL);
}

static int64_t lx_fstatat(int dirfd, const char *path, void *out, int flags) {
    struct stat st;
    char host[PATH_MAX];
    if (path && !path[0] && (flags & LX_AT_EMPTY_PATH)) {
        if (fstat(dirfd, &st) != 0) return lx_err();
    } else if (path && kl_lx_hostlib_is(path)) {
        memset(&st, 0, sizeof st);
        st.st_mode = S_IFREG | 0555; st.st_nlink = 1; st.st_size = 0x4000; st.st_ino = 0x6b6c;
    } else if (path && kl_lx_proc_is(path)) {
        if (kl_lx_proc_stat(path, !(flags & LX_AT_SYMLINK_NOFOLLOW), &st) != 0) return lx_err();
    } else {
        int follow = !(flags & LX_AT_SYMLINK_NOFOLLOW);
        if (at_path(dirfd, path, follow, host, sizeof host) != 0) return lx_err();
        if ((follow ? stat : lstat)(host, &st) != 0) return lx_err();
    }
    kl_lx_stat_out(&st, out);
    return 0;
}

static int64_t lx_readlinkat(int dirfd, const char *path, char *buf, size_t cap) {
    // /proc/self/exe above all: the loader derives $ORIGIN from it.
    if (path && kl_lx_proc_is(path)) return lx_ret(kl_lx_proc_readlink(path, buf, cap));
    char host[PATH_MAX];
    if (at_path(dirfd, path, 0, host, sizeof host) != 0) return lx_err();
    return lx_ret(readlink(host, buf, cap));
}

static int64_t lx_read(int fd, void *buf, size_t n, int64_t off) {
    ssize_t r;
    uint64_t at;
    kl_lx_fdinfo *fi = kl_lx_fd(fd);
    if (fi && fi->efd) return lx_ret(kl_lx_eventfd_read(fd, fi->efd, buf, n));
    if (off < 0) {
        at = (uint64_t)lseek(fd, 0, SEEK_CUR);
        r = read(fd, buf, n);
    } else {
        at = (uint64_t)off;
        r = pread(fd, buf, n, off);
    }
    if (r > 0) kl_lx_elf_view(fd, buf, (size_t)r, at);
    return lx_ret(r);
}

// fcntl: the commands a runtime actually issues. The status flags are Linux's.
static int64_t lx_fcntl(int fd, int cmd, uint64_t arg) {
    switch (cmd) {
    case 0:    return lx_ret(fcntl(fd, F_DUPFD, (int)arg));
    case 1030: return lx_ret(fcntl(fd, F_DUPFD_CLOEXEC, (int)arg));
    case 1:    return lx_ret(fcntl(fd, F_GETFD));
    case 2:    return lx_ret(fcntl(fd, F_SETFD, (int)arg & 1));
    case 3: {
        int f = fcntl(fd, F_GETFL);
        if (f < 0) return lx_err();
        return (f & 3) | ((f & O_NONBLOCK) ? 04000 : 0) | ((f & O_APPEND) ? 02000 : 0);
    }
    case 4: {
        int f = fcntl(fd, F_GETFL);
        if (f < 0) return lx_err();
        f = (f & ~(O_NONBLOCK | O_APPEND)) | ((arg & 04000) ? O_NONBLOCK : 0) | ((arg & 02000) ? O_APPEND : 0);
        return lx_ret(fcntl(fd, F_SETFL, f));
    }
    case 5: case 6: case 7: case 36: case 37: case 38:
        return 0;                                   // advisory locks: one process, granted
    default:
        return -(int64_t)kl_errno_to_linux(EINVAL);
    }
}

static int64_t lx_sleep(int clock, int abstime, const struct timespec *req, struct timespec *rem) {
    struct timespec d = *req;
    if (abstime) {
        struct timespec now;
        clock_gettime(clock == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
        d.tv_sec -= now.tv_sec; d.tv_nsec -= now.tv_nsec;
        if (d.tv_nsec < 0) { d.tv_nsec += 1000000000; d.tv_sec--; }
        if (d.tv_sec < 0) return 0;
    }
    return lx_ret(nanosleep(&d, abstime ? NULL : rem));
}

static int64_t lx_ioctl(int fd, uint64_t req, void *arg) {
    if (req == 0x5401) {                            // TCGETS: stdio asking "is this a terminal"
        if (!isatty(fd)) return -(int64_t)kl_errno_to_linux(ENOTTY);
        memset(arg, 0, 36);
        return 0;
    }
    if (req == 0x5413) {                            // TIOCGWINSZ: same layout both sides
        return lx_ret(ioctl(fd, TIOCGWINSZ, arg));
    }
    return -(int64_t)kl_errno_to_linux(ENOTTY);
}

// ---- identity and time ----
static int64_t lx_uname(char *buf) {
    static const char *const f[6] = { "Linux", "klepton", "6.12.0", "#1 SMP", "aarch64", "(none)" };
    memset(buf, 0, 6 * 65);
    for (int i = 0; i < 6; i++) strlcpy(buf + i * 65, f[i], 65);
    return 0;
}

static int64_t lx_clock_gettime(int id, struct timespec *ts) {
    static const clockid_t map[] = {
        CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID,
        CLOCK_MONOTONIC_RAW, CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_MONOTONIC };
    if (id < 0 || id >= (int)(sizeof map / sizeof *map)) return -(int64_t)kl_errno_to_linux(EINVAL);
    return lx_ret(clock_gettime(map[id], ts));
}

_Thread_local kl_lx_frame *kl_lx_cur;

int64_t kl_lx_syscall(kl_lx_frame *f) {
    kl_lx_cur = f;
    uint64_t nr = f->x[8];
    uint64_t a0 = f->x[0], a1 = f->x[1], a2 = f->x[2], a3 = f->x[3], a4 = f->x[4], a5 = f->x[5];
    int64_t r;

    // A vfork child shares everything with its parent here, where a real one
    // would have copies (kl_lx_clone). What it does to descriptors and signal
    // state on its way to execve must therefore not happen.
    if (kl_lx_in_vfork()) switch (nr) {
    case 23: case 24: case 25: case 57: case 134: case 135: case 56: case 49:
        r = nr == 24 ? (int64_t)a1 : nr == 56 ? 3 : 0;
        goto done;
    case 94: kl_lx_thread_exit((int)a0);
    }

    switch (nr) {
    case 56: {                                      // openat
        char host[PATH_MAX];
        const char *path = (const char *)a1;
        if (path && path[0] != '/' && (int)a0 != LX_AT_FDCWD) { r = -(int64_t)LX_ENOSYS; break; }
        (void)host;
        r = lx_ret(kl_lx_open(path, (int)a2, (int)a3));
        break;
    }
    case 57:  kl_lx_close((int)a0); r = lx_ret(close((int)a0)); break;
    case 63:  r = lx_read((int)a0, (void *)a1, (size_t)a2, -1); break;
    case 67:  r = lx_read((int)a0, (void *)a1, (size_t)a2, (int64_t)a3); break;         // pread64
    case 64: {
        kl_lx_fdinfo *fi = kl_lx_fd((int)a0);
        r = fi && fi->efd ? lx_ret(kl_lx_eventfd_write(fi->efd, (const void *)a1, (size_t)a2))
                          : lx_ret(write((int)a0, (const void *)a1, (size_t)a2));
        break;
    }
    case 66:  r = lx_ret(writev((int)a0, (const struct iovec *)a1, (int)a2)); break;
    case 62:  r = lx_ret(lseek((int)a0, (off_t)a1, (int)a2)); break;
    case 79:  r = lx_fstatat((int)a0, (const char *)a1, (void *)a2, (int)a3); break;    // newfstatat
    case 80:  r = lx_fstatat((int)a0, "", (void *)a1, LX_AT_EMPTY_PATH); break;         // fstat
    case 78:  r = lx_readlinkat((int)a0, (const char *)a1, (char *)a2, (size_t)a3); break;
    case 48: case 439: {                            // faccessat, faccessat2
        char host[PATH_MAX];
        struct stat st;
        if (a1 && kl_lx_hostlib_is((const char *)a1))
            r = 0;
        else if (a1 && kl_lx_proc_is((const char *)a1))
            r = kl_lx_proc_stat((const char *)a1, 1, &st) ? lx_err() : 0;
        else
            r = at_path((int)a0, (const char *)a1, 1, host, sizeof host) ? lx_err()
                                                                         : lx_ret(access(host, (int)a2));
        break;
    }
    case 43: case 44: {                             // statfs, fstatfs: a plain, roomy filesystem
        struct { int64_t type, bsize; uint64_t blocks, bfree, bavail, files, ffree; int32_t fsid[2];
                 int64_t namelen, frsize, flags, spare[4]; } *sf = (void *)a1;
        memset(sf, 0, sizeof *sf);
        sf->type = 0xEF53; sf->bsize = sf->frsize = 4096; sf->namelen = 255;
        sf->blocks = 1ull << 28; sf->bfree = sf->bavail = 1ull << 27;
        sf->files = 1 << 24; sf->ffree = 1 << 23;
        r = 0;
        break;
    }
    case 260: r = kl_lx_wait4((int)a0, (int *)a1); break;
    case 221: r = kl_lx_execve((const char *)a0, (char *const *)a1); break;
    case 17: {                                      // getcwd: the length, NUL included
        size_t n = strlen(kl_lx_cwd()) + 1;
        if (n > a1) { r = -(int64_t)kl_errno_to_linux(ERANGE); break; }
        memcpy((void *)a0, kl_lx_cwd(), n);
        r = (int64_t)n;
        break;
    }
    case 49:  r = lx_ret(kl_lx_chdir((const char *)a0)); break;
    case 29:  r = lx_ioctl((int)a0, a1, (void *)a2); break;
    case 25:  r = lx_fcntl((int)a0, (int)a1, a2); break;
    case 23:  r = lx_ret(dup((int)a0)); break;
    case 24:  r = lx_ret(dup2((int)a0, (int)a1)); break;                                 // dup3
    case 59:  r = lx_ret(klb_pipe2((int *)a0, (int)a1)); break;
    case 61:  r = kl_lx_getdents64((int)a0, (void *)a1, (size_t)a2); if (r < 0) r = lx_err(); break;
    case 46:  r = lx_ret(ftruncate((int)a0, (off_t)a1)); break;
    case 82: case 83: r = lx_ret(fsync((int)a0)); break;
    case 34: case 35: case 38: case 276: {          // mkdirat, unlinkat, renameat, renameat2
        char h1[PATH_MAX], h2[PATH_MAX];
        const char *p2 = (const char *)(nr == 38 || nr == 276 ? a3 : a1);
        if (at_path((int)a0, (const char *)a1, 0, h1, sizeof h1) != 0 ||
            ((nr == 38 || nr == 276) && at_path((int)a2, p2, 0, h2, sizeof h2) != 0)) { r = lx_err(); break; }
        if (kl_lx_host_ro(h1) || ((nr == 38 || nr == 276) && kl_lx_host_ro(h2))) {
            r = -(int64_t)kl_errno_to_linux(EROFS);
            break;
        }
        r = lx_ret(nr == 34 ? mkdir(h1, (mode_t)a2)
                 : nr == 35 ? ((a2 & 0x200) ? rmdir(h1) : unlink(h1))
                 : rename(h1, h2));
        break;
    }
    case 19:  r = lx_ret(kl_lx_eventfd((unsigned)a0, (int)a1)); break;                   // eventfd2
    case 52:  r = lx_ret(fchmod((int)a0, (mode_t)a1)); break;
    // flock: LOCK_SH/EX/NB/UN are 1/2/4/8 on both systems. Not optional — Source
    // 2 opens its settings files for writing (truncating them), takes this lock,
    // and writes only if it got it: answered "no such call", every .vcfg the
    // game saved was left at zero bytes and no setting survived its menu.
    case 32:  r = lx_ret(flock((int)a0, (int)a1)); break;
    case 20:  r = lx_ret(klb_epoll_create1((int)a0)); break;
    case 21:  r = lx_ret(klb_epoll_ctl((int)a0, (int)a1, (int)a2, (void *)a3)); break;
    case 22:  r = lx_ret(klb_epoll_wait((int)a0, (void *)a1, (int)a2, (int)a3)); break;  // epoll_pwait
    case 73:  r = lx_ret(klb_ppoll((struct pollfd *)a0, a1, (const struct timespec *)a2, NULL)); break;

    case 93:  kl_lx_thread_exit((int)a0);           // exit: this thread only
    case 94:                                        // exit_group
        fflush(NULL);
        if ((int)a0) {                              // a failure exit says where it came from
            fprintf(stderr, "  [klepton] linux: the guest exited with status %d\n", (int)a0);
            kl_lx_backtrace(f);
            kl_lx_stack_strings((uint64_t)(uintptr_t)f);
        }
        if (g_exit_hook) g_exit_hook((int)a0);
        _exit((int)a0);

    case 220: r = kl_lx_clone(f); break;
    case 435: r = -(int64_t)LX_ENOSYS; break;       // clone3: glibc falls back to clone
    case 96:  r = kl_lx_set_tid_address((uint32_t *)a0); break;
    case 99:  r = 0; break;                         // set_robust_list
    case 98:  r = kl_lx_futex((uint32_t *)a0, (int)a1, (uint32_t)a2, (const struct timespec *)a3,
                              (uint32_t *)a4, (uint32_t)a5); break;
    case 134: r = kl_lx_rt_sigaction((int)a0, (const void *)a1, (void *)a2); break;
    case 135: r = kl_lx_rt_sigprocmask((int)a0, (const uint64_t *)a1, (uint64_t *)a2); break;
    case 131:                                       // tgkill(tgid, tid, sig)
        r = (int)a0 == getpid() ? kl_lx_tkill((int)a1, (int)a2) : -(int64_t)kl_errno_to_linux(ESRCH);
        break;
    case 124: sched_yield(); r = 0; break;
    case 129:                                       // kill(pid, sig): this process or nobody
        r = (int)a0 != getpid() && (int)a0 != 0 ? -(int64_t)kl_errno_to_linux(ESRCH)
          : (int)a1 == 0 ? 0 : kl_lx_raise((int)a1);
        break;
    case 130: r = kl_lx_tkill((int)a0, (int)a1); break;   // tkill(tid, sig)
    case 132: r = 0; break;                         // sigaltstack: handlers run where they are raised
    case 101: r = lx_sleep(1, 0, (const struct timespec *)a0, (struct timespec *)a1); break;
    case 115: r = lx_sleep((int)a0, (int)a1 & 1, (const struct timespec *)a2, (struct timespec *)a3); break;
    case 169: {                                     // gettimeofday: Linux's tv_usec is 64 bits wide
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (a0) { ((int64_t *)a0)[0] = tv.tv_sec; ((int64_t *)a0)[1] = tv.tv_usec; }
        r = 0;
        break;
    }
    case 122: r = 0; break;                         // sched_setaffinity: the host schedules
    case 118: case 119: case 120: r = 0; break;     // sched_setparam/setscheduler/getscheduler: SCHED_OTHER
    case 121: *(int *)a1 = 0; r = 0; break;         // sched_getparam
    case 140: r = 0; break;                         // setpriority
    case 141: r = 20; break;                        // getpriority: nice 0, as the kernel encodes it
    case 114: if (a1) *(struct timespec *)a1 = (struct timespec){ 0, 1 }; r = 0; break;  // clock_getres
    case 148: case 150:                             // getresuid, getresgid
        *(uint32_t *)a0 = *(uint32_t *)a1 = *(uint32_t *)a2 = 1000; r = 0; break;
    case 95:  r = -(int64_t)kl_errno_to_linux(ECHILD); break;    // waitid
    case 123: {                                     // sched_getaffinity: every core, as one word
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (a1 < 8) { r = -(int64_t)kl_errno_to_linux(EINVAL); break; }
        memset((void *)a2, 0, (size_t)a1);
        *(uint64_t *)a2 = n >= 64 ? ~0ull : (1ull << n) - 1;
        r = 8;
        break;
    }
    case 168: if (a0) *(unsigned *)a0 = 0; if (a1) *(unsigned *)a1 = 0; r = 0; break;       // getcpu
    case 216: r = -(int64_t)kl_errno_to_linux(ENOMEM); break;   // mremap: glibc copes with "no"
    case 293: r = -(int64_t)LX_ENOSYS; break;       // rseq: glibc carries on without it

    case 172: r = getpid(); break;
    case 173: r = 1; break;                         // getppid
    case 178: r = kl_lx_gettid(); break;
    case 174: case 175: case 176: case 177: r = 1000; break;      // uid, euid, gid, egid

    case 160: r = lx_uname((char *)a0); break;
    case 179: {                                     // sysinfo
        struct { int64_t uptime; uint64_t loads[3], totalram, freeram, sharedram, bufferram,
                 totalswap, freeswap; uint16_t procs, pad; uint64_t totalhigh, freehigh;
                 uint32_t mem_unit; } *si = (void *)a0;
        memset(si, 0, sizeof *si);
        si->totalram = kl_mem_budget_bytes();
        si->freeram  = kl_mem_available_bytes();
        si->procs = 1; si->mem_unit = 1;
        r = 0;
        break;
    }
    // prctl: thread names and VMA labels are cosmetic; nothing else is offered.
    case 167: r = (a0 == 15 || a0 == 0x53564d41) ? 0 : -(int64_t)kl_errno_to_linux(EINVAL); break;
    case 113: r = lx_clock_gettime((int)a0, (struct timespec *)a1); break;
    case 278: arc4random_buf((void *)a0, (size_t)a1); r = (int64_t)a1; break;         // getrandom

    // No program break. glibc's malloc asks once, is told it cannot grow, and
    // serves every arena from mmap instead.
    case 214: r = 0; break;
    case 222: r = kl_lx_mmap(a0, a1, (int)a2, (int)a3, (int)a4, (int64_t)a5);
              mm_trace("mmap", a0, a1, a2, a3, r); break;
    case 215: r = kl_lx_munmap(a0, a1); mm_trace("munmap", a0, a1, 0, 0, r); break;
    case 226: r = kl_lx_mprotect(a0, a1, (int)a2); mm_trace("mprotect", a0, a1, a2, 0, r); break;
    case 233:                                       // madvise(addr, len, advice)
        // MADV_DONTNEED (4) promises zeroed pages and callers rely on it; the
        // rest really is advice.
        r = (int)a2 == 4 ? kl_lx_madvise_dontneed(a0, a1) : 0;
        mm_trace("madvise", a0, a1, a2, 0, r);
        break;

    case 261: {                                     // prlimit64(pid, resource, new, old)
        struct { uint64_t cur, max; } *old = (void *)a3;
        if (old) { old->cur = a1 == 3 ? 8ull << 20 : ~0ull; old->max = ~0ull; }   // RLIMIT_STACK
        r = 0;
        break;
    }

    default: {
        static uint64_t seen[8];
        if (nr < 512 && !(seen[nr / 64] & (1ull << (nr % 64)))) {
            seen[nr / 64] |= 1ull << (nr % 64);
            fprintf(stderr, "  [klepton] linux: syscall %llu is not served yet "
                            "(args %#llx %#llx %#llx) — answering ENOSYS\n",
                    (unsigned long long)nr, (unsigned long long)a0,
                    (unsigned long long)a1, (unsigned long long)a2);
        }
        r = -(int64_t)LX_ENOSYS;
    }
    }

done:
    if (trace_on()) {
        // The path, for the calls that take one in their second argument — a
        // failed open is only a work item once it says of what.
        const char *path = (nr == 56 || nr == 78 || nr == 79 || nr == 48 || nr == 439 ||
                            nr == 34 || nr == 35) ? (const char *)a1
                         : nr == 49 ? (const char *)a0 : NULL;
        fprintf(stderr, "  [lx] %3llu(%#llx, %#llx, %#llx, %#llx) = %lld%s%s\n",
                (unsigned long long)nr, (unsigned long long)a0, (unsigned long long)a1,
                (unsigned long long)a2, (unsigned long long)a3, (long long)r,
                path ? "  " : "", path ? path : "");
    }
    return r;
}
