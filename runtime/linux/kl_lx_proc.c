// /proc and /sys, as far as a guest has read them.
//
// Nothing here is a real file. A read of one is answered by writing what Linux
// would have said into an unlinked temporary file and handing over its
// descriptor, so everything downstream — read, lseek, fstat, mmap, close — is
// the ordinary path. Only what a guest has actually asked for is here; an
// unknown path is ENOENT, and under KL_LX_TRACE it is named.
//
// "/proc/self", "/proc/thread-self" and "/proc/<our pid>" are one directory.
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "kl_lx.h"
#include "../klepton.h"

static int    g_argc;
static char **g_argv;
static uint64_t g_stack_lo, g_stack_hi;

void kl_lx_proc_init(int argc, char **argv, uint64_t stack_lo, uint64_t stack_hi) {
    g_argc = argc;
    g_argv = calloc((size_t)argc + 1, sizeof *g_argv);
    for (int i = 0; i < argc; i++) g_argv[i] = strdup(argv[i]);
    g_stack_lo = stack_lo;
    g_stack_hi = stack_hi;
}

// The part after "/proc/self/", or NULL if `path` is not in that directory.
static const char *self_entry(const char *path) {
    static const char *const pre[] = { "/proc/self/", "/proc/thread-self/" };
    for (int i = 0; i < 2; i++)
        if (!strncmp(path, pre[i], strlen(pre[i]))) return path + strlen(pre[i]);
    char mine[64];
    int n = snprintf(mine, sizeof mine, "/proc/%d/", getpid());
    return strncmp(path, mine, (size_t)n) ? NULL : path + n;
}

typedef struct { char *p; size_t n, cap; } buf;
static void put(buf *b, const char *fmt, ...) {
    va_list a;
    for (;;) {
        va_start(a, fmt);
        int w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, a);
        va_end(a);
        if ((size_t)w < b->cap - b->n) { b->n += (size_t)w; return; }
        b->cap = (b->cap + (size_t)w) * 2 + 256;
        b->p = realloc(b->p, b->cap);
    }
}

// One line per mapping the guest could mean something by: every image's text
// and data under the name the guest loaded it by, and the initial thread's
// stack — glibc's pthread_getattr_np reads this file to learn where that is.
static void gen_maps(buf *b) {
    uint64_t lo, data, hi;
    const char *name;
    for (int i = 0; kl_lx_image_info(i, &lo, &data, &hi, &name); i++) {
        put(b, "%08llx-%08llx r-xp 00000000 fe:00 %d %s\n", (unsigned long long)lo,
            (unsigned long long)data, 1000 + i, name);
        if (hi > data)
            put(b, "%08llx-%08llx rw-p %08llx fe:00 %d %s\n", (unsigned long long)data,
                (unsigned long long)hi, (unsigned long long)(data - lo), 1000 + i, name);
    }
    put(b, "%08llx-%08llx rw-p 00000000 00:00 0 [stack]\n",
        (unsigned long long)g_stack_lo, (unsigned long long)g_stack_hi);
}

static void gen_cpuinfo(buf *b) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    for (long i = 0; i < n; i++)
        put(b, "processor\t: %ld\nBogoMIPS\t: 48.00\n"
               "Features\t: fp asimd aes pmull sha1 sha2 crc32 atomics\n"
               "CPU implementer\t: 0x61\nCPU architecture: 8\nCPU variant\t: 0x1\n"
               "CPU part\t: 0x023\nCPU revision\t: 0\n\n", i);
}

static int generate(const char *path, buf *b) {
    const char *e = self_entry(path);
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (e && !strcmp(e, "cmdline")) {
        for (int i = 0; i < g_argc; i++) {
            put(b, "%s", g_argv[i]);
            b->p[b->n++] = 0;                       // put() always leaves room for one more
        }
    } else if (e && !strcmp(e, "maps")) {
        gen_maps(b);
    } else if (e && !strcmp(e, "status")) {
        put(b, "Name:\t%s\nState:\tR (running)\nTgid:\t%d\nPid:\t%d\nPPid:\t1\nTracerPid:\t0\n"
               "Uid:\t1000\t1000\t1000\t1000\nGid:\t1000\t1000\t1000\t1000\n"
               "VmSize:\t%llu kB\nVmRSS:\t%llu kB\nThreads:\t1\n",
            strrchr(kl_lx_exe(), '/') ? strrchr(kl_lx_exe(), '/') + 1 : kl_lx_exe(),
            getpid(), getpid(), (unsigned long long)(kl_mem_budget_bytes() >> 10),
            (unsigned long long)((kl_mem_budget_bytes() - kl_mem_available_bytes()) >> 10));
    } else if (!strcmp(path, "/proc/cpuinfo")) {
        gen_cpuinfo(b);
    } else if (!strcmp(path, "/proc/meminfo")) {
        unsigned long long tot = kl_mem_budget_bytes() >> 10, av = kl_mem_available_bytes() >> 10;
        put(b, "MemTotal:       %llu kB\nMemFree:        %llu kB\nMemAvailable:   %llu kB\n"
               "Buffers:               0 kB\nCached:                0 kB\nSwapTotal:             0 kB\n"
               "SwapFree:              0 kB\n", tot, av, av);
    } else if (!strcmp(path, "/proc/version")) {
        put(b, "Linux version 6.12.0 (klepton) #1 SMP PREEMPT\n");
    } else if (!strcmp(path, "/proc/sys/vm/overcommit_memory")) {
        put(b, "0\n");
    } else if (!strcmp(path, "/proc/sys/vm/max_map_count")) {
        put(b, "1048576\n");
    } else if (!strcmp(path, "/proc/sys/kernel/osrelease")) {
        put(b, "6.12.0\n");
    } else if (!strcmp(path, "/sys/devices/system/cpu/online") ||
               !strcmp(path, "/sys/devices/system/cpu/possible") ||
               !strcmp(path, "/sys/devices/system/cpu/present")) {
        put(b, ncpu > 1 ? "0-%ld\n" : "0\n", ncpu - 1);
    } else {
        return -1;
    }
    return 0;
}

int kl_lx_proc_is(const char *path) {
    return !strncmp(path, "/proc/", 6) || !strncmp(path, "/sys/", 5) || !strcmp(path, "/proc") ||
           !strcmp(path, "/sys");
}

int kl_lx_proc_open(const char *path) {
    buf b = { malloc(512), 0, 512 };
    if (generate(path, &b) != 0) { free(b.p); errno = ENOENT; return -1; }
    char tmpl[PATH_MAX];
    const char *tmp = getenv("TMPDIR");
    snprintf(tmpl, sizeof tmpl, "%s/kl-proc-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    int fd = mkstemp(tmpl);
    if (fd >= 0) {
        unlink(tmpl);
        if (write(fd, b.p, b.n) != (ssize_t)b.n) { close(fd); fd = -1; }
        else lseek(fd, 0, SEEK_SET);
    }
    free(b.p);
    return fd;
}

// stat and lstat. procfs reports every file as empty, and its magic links as
// links of length zero; code that sizes a buffer from st_size finds that out on
// Linux too.
int kl_lx_proc_stat(const char *path, int follow, struct stat *st) {
    const char *e = self_entry(path);
    memset(st, 0, sizeof *st);
    st->st_nlink = 1;
    st->st_ino = 4026531840u;
    if (e && !strcmp(e, "exe")) {
        if (follow) {
            char host[PATH_MAX];
            if (kl_lx_resolve(kl_lx_exe(), 1, host, sizeof host, NULL) != 0) return -1;
            return stat(host, st);
        }
        st->st_mode = S_IFLNK | 0777;
        return 0;
    }
    if (!strcmp(path, "/proc") || !strcmp(path, "/sys") || !strcmp(path, "/proc/self") ||
        (e && !*e)) {
        st->st_mode = S_IFDIR | 0555;
        return 0;
    }
    buf b = { malloc(512), 0, 512 };
    int ok = generate(path, &b);
    free(b.p);
    if (ok != 0) { errno = ENOENT; return -1; }
    st->st_mode = S_IFREG | 0444;
    return 0;
}

// readlink. Returns the length, or -1 with errno.
ssize_t kl_lx_proc_readlink(const char *path, char *out, size_t cap) {
    const char *e = self_entry(path), *target = NULL;
    char tmp[32];
    if (e && !strcmp(e, "exe")) target = kl_lx_exe();
    else if (e && !strcmp(e, "cwd")) target = kl_lx_cwd();
    else if (e && !strncmp(e, "fd/", 3)) {
        kl_lx_fdinfo *fi = kl_lx_fd(atoi(e + 3));
        if (fi && fi->guest[0]) target = fi->guest;
    } else if (!strcmp(path, "/proc/self")) {
        snprintf(tmp, sizeof tmp, "%d", getpid());
        target = tmp;
    }
    if (!target) { errno = e ? ENOENT : EINVAL; return -1; }
    size_t n = strlen(target);
    if (n > cap) n = cap;
    memcpy(out, target, n);
    return (ssize_t)n;
}
