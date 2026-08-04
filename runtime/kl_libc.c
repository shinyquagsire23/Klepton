// libc entry points that cannot be forwarded: divergent struct layouts, Android-only
// APIs, and functions whose ABI differs between Linux/aarch64 and Darwin/arm64.
// Everything whose signature *and* layout match is in kl_libc_table.h instead.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <setjmp.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <wchar.h>
#include <ctype.h>
#include "klepton.h"
#include "kl_va.h"

static void warn_once(const char *what) {
    static const char *seen[64]; static int n = 0;
    for (int i = 0; i < n; i++) if (seen[i] == what) return;
    if (n < 64) seen[n++] = what;
    fprintf(stderr, "  [klepton] TODO: %s called but not fully implemented\n", what);
}

// ---------- errno / environ ----------
int  *klb_errno(void) { return __error(); }          // bionic spells it __errno()
extern char **environ;
char ***klb_environ_ptr(void) { return &environ; }   // registered as data below

// ---------- Android-only ----------
int  klb_gettid(void) { uint64_t t = 0; pthread_threadid_np(NULL, &t); return (int)t; }
const void *klb_sysprop_find(const char *n)  { (void)n; return NULL; }
int  klb_sysprop_get(const char *n, char *v) { (void)n; if (v) v[0] = 0; return 0; }
void klb_sysprop_read(const void *p, char *n, char *v) { (void)p; if (n) n[0]=0; if (v) v[0]=0; }
int  klb_prctl(int op, ...)                  { (void)op; return 0; }
int  klb_sched_getaffinity(int p, size_t sz, void *m) {
    (void)p; if (m && sz) memset(m, 0xFF, sz);       // pretend every CPU is available
    return 0;
}
int  klb_sched_setaffinity(int p, size_t sz, const void *m) { (void)p;(void)sz;(void)m; return 0; }

// ---------- struct stat ----------
// bionic/aarch64 layout (128 bytes) is unrelated to Darwin's; translate field by field.
typedef struct {
    uint64_t st_dev, st_ino;
    uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev, __pad1;
    int64_t  st_size;
    int32_t  st_blksize, __pad2;
    int64_t  st_blocks;
    struct { int64_t tv_sec; int64_t tv_nsec; } st_atim, st_mtim, st_ctim;
    uint32_t __unused4, __unused5;
} bionic_stat;

static void stat_to_bionic(const struct stat *d, bionic_stat *b) {
    memset(b, 0, sizeof *b);
    b->st_dev = (uint64_t)d->st_dev;   b->st_ino  = d->st_ino;
    b->st_mode = d->st_mode;           b->st_nlink = d->st_nlink;
    b->st_uid = d->st_uid;             b->st_gid  = d->st_gid;
    b->st_rdev = (uint64_t)d->st_rdev; b->st_size = d->st_size;
    b->st_blksize = d->st_blksize;     b->st_blocks = d->st_blocks;
    b->st_atim.tv_sec = d->st_atimespec.tv_sec; b->st_atim.tv_nsec = d->st_atimespec.tv_nsec;
    b->st_mtim.tv_sec = d->st_mtimespec.tv_sec; b->st_mtim.tv_nsec = d->st_mtimespec.tv_nsec;
    b->st_ctim.tv_sec = d->st_ctimespec.tv_sec; b->st_ctim.tv_nsec = d->st_ctimespec.tv_nsec;
}
int klb_stat(const char *p, bionic_stat *b)  { struct stat s; int r = stat(p, &s);
                                               if (!r) stat_to_bionic(&s, b); return r; }
int klb_lstat(const char *p, bionic_stat *b) { struct stat s; int r = lstat(p, &s);
                                               if (!r) stat_to_bionic(&s, b); return r; }
int klb_fstat(int fd, bionic_stat *b)        { struct stat s; int r = fstat(fd, &s);
                                               if (!r) stat_to_bionic(&s, b); return r; }
int klb_statfs(const char *p, void *out) { (void)p; warn_once("statfs");
                                           if (out) memset(out, 0, 120); return 0; }

// ---------- struct dirent ----------
typedef struct { uint64_t d_ino; int64_t d_off; uint16_t d_reclen;
                 uint8_t d_type; char d_name[256]; } bionic_dirent;

void *klb_opendir(const char *p) { return opendir(p); }
int   klb_closedir(void *d)      { return closedir((DIR *)d); }
void *klb_readdir(void *d) {
    static _Thread_local bionic_dirent out;    // matches readdir's per-stream lifetime
    struct dirent *e = readdir((DIR *)d);
    if (!e) return NULL;
    memset(&out, 0, sizeof out);
    out.d_ino = e->d_ino;
    out.d_off = 0;
    out.d_reclen = sizeof out;
    out.d_type = e->d_type;
    snprintf(out.d_name, sizeof out.d_name, "%s", e->d_name);
    return &out;
}

// ---------- sigaction ----------
// bionic/LP64: { handler; unsigned long mask; int flags; void (*restorer)(void); } = 32B
// Darwin     : { handler; uint32 mask; int flags; }                                = 16B
typedef struct { void *handler; uint64_t mask; int32_t flags; int32_t _pad;
                 void *restorer; } bionic_sigaction;

int klb_sigaction(int sig, const bionic_sigaction *in, bionic_sigaction *old) {
    struct sigaction d, o;
    if (in) {
        memset(&d, 0, sizeof d);
        d.sa_handler = (void (*)(int))in->handler;
        d.sa_flags   = in->flags;
        d.sa_mask    = (sigset_t)(in->mask & 0xFFFFFFFFu);
    }
    int r = sigaction(sig, in ? &d : NULL, old ? &o : NULL);
    if (!r && old) {
        memset(old, 0, sizeof *old);
        old->handler = (void *)o.sa_handler;
        old->flags   = o.sa_flags;
        old->mask    = o.sa_mask;
    }
    return r;
}

// ---------- uname ----------
// bionic fields are 65 bytes each; Darwin's are 256. Never forward the struct.
typedef struct { char sysname[65], nodename[65], release[65],
                      version[65], machine[65], domainname[65]; } bionic_utsname;
int klb_uname(bionic_utsname *u) {
    memset(u, 0, sizeof *u);
    snprintf(u->sysname,  65, "Linux");
    snprintf(u->nodename, 65, "localhost");
    snprintf(u->release,  65, "4.14.0-klepton");
    snprintf(u->version,  65, "#1 SMP klepton");
    snprintf(u->machine,  65, "aarch64");
    return 0;
}

// ---------- misc ----------
int    klb_FD_ISSET_chk(int fd, void *set) { return FD_ISSET(fd, (fd_set *)set); }
void   klb_FD_SET_chk(int fd, void *set)   { FD_SET(fd, (fd_set *)set); }
size_t klb_ctype_mb_cur_max(void)          { return MB_CUR_MAX; }
off_t  klb_lseek64(int fd, off_t o, int w) { return lseek(fd, o, w); }
void  *klb_getpwuid(unsigned uid)          { (void)uid; warn_once("getpwuid"); return NULL; }
int    klb_getpwuid_r(unsigned uid, void *pw, char *buf, size_t n, void **res) {
    (void)uid; (void)pw; (void)buf; (void)n; if (res) *res = NULL;
    warn_once("getpwuid_r"); return 0;
}
int    klb_execl(const char *p, const char *a, ...) { (void)p; (void)a;
                                                      warn_once("execl"); errno = ENOSYS; return -1; }
long   klb_syscall(long n, ...) { fprintf(stderr, "  [klepton] guest raw syscall(%ld) — refusing\n", n);
                                  errno = ENOSYS; return -1; }
int    klb_swprintf(wchar_t *s, size_t n, const wchar_t *f, ...) {
    (void)f; warn_once("swprintf (wide format strings unsupported)");
    if (s && n) s[0] = 0; return 0;
}

// ---------- guest va_list consumers ----------
int klb_vprintf(const char *fmt, void *gva) {
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, (kl_va *)gva, m, sizeof m, KL_VA_PRINTF) == (size_t)-1) return -1;
    return vprintf(fmt, (va_list)m);
}
int klb_vsscanf(const char *s, const char *fmt, void *gva) {
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, (kl_va *)gva, m, sizeof m, KL_VA_SCANF) == (size_t)-1) return -1;
    return vsscanf(s, fmt, (va_list)m);
}

// ---------- long double ----------
// Linux/aarch64 long double is IEEE binary128 returned in q0; Darwin's is a plain
// 64-bit double. klv_strtold (kl_va_thunks.S) calls strtod then this widener.
__uint128_t kl_f64_to_f128(double d) {
    uint64_t b; memcpy(&b, &d, 8);
    uint64_t sign = b >> 63;
    int64_t  exp  = (int64_t)((b >> 52) & 0x7FF);
    uint64_t man  = b & 0xFFFFFFFFFFFFFull;
    uint64_t hi, lo;
    if (exp == 0 && man == 0) { hi = sign << 63; lo = 0; }            // +-0
    else if (exp == 0x7FF)    { hi = (sign << 63) | 0x7FFFull << 48 | (man >> 4); lo = man << 60; }
    else {
        int64_t e128 = exp - 1023 + 16383;
        hi = (sign << 63) | ((uint64_t)e128 << 48) | (man >> 4);
        lo = man << 60;
    }
    return ((__uint128_t)hi << 64) | lo;
}

// ---------- not present on Darwin ----------
void *klb_memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) if (p[n] == (unsigned char)c) return (void *)(p + n);
    return NULL;
}
void *klb_memalign(size_t align, size_t size) {
    void *p = NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    if (align & (align - 1)) return NULL;             // must be a power of two
    return posix_memalign(&p, align, size) == 0 ? p : NULL;
}

// ---------- flag-value translation ----------
// Same names, different numbers. MAP_ANONYMOUS is 0x20 on Linux and 0x1000 on
// Darwin, so an untranslated guest anonymous mmap becomes a file mapping of fd -1
// and fails -- which is exactly how IL2CPP's Boehm GC died at startup.
#define LX_MAP_SHARED 0x01
#define LX_MAP_PRIVATE 0x02
#define LX_MAP_FIXED 0x10
#define LX_MAP_ANONYMOUS 0x20
#define LX_MAP_NORESERVE 0x4000

int kl_mmap_flags(int lx) {
    int d = 0;
    if (lx & LX_MAP_SHARED)     d |= MAP_SHARED;
    if (lx & LX_MAP_PRIVATE)    d |= MAP_PRIVATE;
    if (lx & LX_MAP_FIXED)      d |= MAP_FIXED;
    if (lx & LX_MAP_ANONYMOUS)  d |= MAP_ANON;
    if (lx & LX_MAP_NORESERVE)  d |= MAP_NORESERVE;
    return d;
}
void *klb_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    int df = kl_mmap_flags(flags);
    if (df & MAP_ANON) fd = -1;                  // Darwin insists on -1 for anonymous
    return mmap(addr, len, prot, df, fd, off);   // PROT_* values match on both sides
}

#define LX_O_CREAT 0x40
#define LX_O_EXCL 0x80
#define LX_O_NOCTTY 0x100
#define LX_O_TRUNC 0x200
#define LX_O_APPEND 0x400
#define LX_O_NONBLOCK 0x800
#define LX_O_DIRECTORY 0x4000
#define LX_O_NOFOLLOW 0x8000
#define LX_O_CLOEXEC 0x80000

int kl_open_flags(int lx) {
    int d = lx & 0x3;                            // O_RDONLY/WRONLY/RDWR agree
    if (lx & LX_O_CREAT)     d |= O_CREAT;
    if (lx & LX_O_EXCL)      d |= O_EXCL;
    if (lx & LX_O_NOCTTY)    d |= O_NOCTTY;
    if (lx & LX_O_TRUNC)     d |= O_TRUNC;
    if (lx & LX_O_APPEND)    d |= O_APPEND;
    if (lx & LX_O_NONBLOCK)  d |= O_NONBLOCK;
    if (lx & LX_O_DIRECTORY) d |= O_DIRECTORY;
    if (lx & LX_O_NOFOLLOW)  d |= O_NOFOLLOW;
    if (lx & LX_O_CLOEXEC)   d |= O_CLOEXEC;
    return d;
}
int klb_madvise(void *a, size_t n, int advice) {
    if (advice == 8) advice = MADV_FREE;         // Linux MADV_FREE
    return madvise(a, n, advice);
}
