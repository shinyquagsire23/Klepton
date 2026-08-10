// The half of the bionic->Darwin surface Beat Saber never asked for.
//
// PLANNING §11.4 predicted that the ELF loader, the bionic shim and the pthread
// layer would "carry over essentially unchanged" for Steam Link. They did — but
// the shim's *coverage* is a function of what one title imports, and SDL3 plus
// Valve's own C++ reach for a different set: locale-aware wide character I/O,
// epoll, the FORTIFY `_chk` family, fenv, and CPU feature detection.
//
// Everything here is hand-written because a direct forward would be wrong, and
// the reasons are the same three that recur across this project:
//
//   * divergent struct layouts (trap 7). Measured against the NDK's own
//     headers rather than assumed — see the table above each function.
//   * a name that exists on both platforms with a different signature
//     (`sendfile`), which is trap 6b waiting to happen.
//   * a Linux facility with no Darwin equivalent (`epoll`, `getauxval`).
//
// Direct forwards for this target live in the generated table as usual.
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <poll.h>
#include <stdatomic.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/un.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <wchar.h>
#include "klepton.h"
#include "kl_va.h"

FILE *kl_host_file(void *guest);            // kl_shim.c
int   kl_open_flags(int linux_flags);       // kl_libc.c
const char *kl_guest_path(const char *path, char *buf, size_t cap);
void  kl_fatal_prepare(void);

#define GUEST_PATH(p) char _kp[1024]; const char *_p = kl_guest_path((p), _kp, sizeof _kp)

// ---------------------------------------------------------------- getauxval
//
// There is no auxiliary vector on Darwin, so the values have to be synthesised.
// The only one that matters here is AT_HWCAP: SDL3 reads it in
// SDL_GetCPUFeatures, and libc++ / BoringSSL read it to pick CPU-specific code
// paths — a wrong bit there does not degrade gracefully, it selects an
// instruction sequence the CPU may not have.
//
// So the bits are *measured*, not declared. `hw.optional.*` is Darwin's own
// answer to the same question, and this is the same posture as the synthetic
// /proc (trap 6d), which reports the host's real cores rather than a plausible
// number. A feature we cannot confirm is reported absent, which costs
// performance and never correctness.
#define AT_PAGESZ   6
#define AT_PLATFORM 15
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_HWCAP2   26

// Linux aarch64 HWCAP bit numbers (uapi/asm/hwcap.h).
#define HWCAP_FP        (1UL << 0)
#define HWCAP_ASIMD     (1UL << 1)
#define HWCAP_AES       (1UL << 3)
#define HWCAP_PMULL     (1UL << 4)
#define HWCAP_SHA1      (1UL << 5)
#define HWCAP_SHA2      (1UL << 6)
#define HWCAP_CRC32     (1UL << 7)
#define HWCAP_ATOMICS   (1UL << 8)
#define HWCAP_FPHP      (1UL << 9)
#define HWCAP_ASIMDHP   (1UL << 10)
#define HWCAP_ASIMDRDM  (1UL << 12)
#define HWCAP_JSCVT     (1UL << 13)
#define HWCAP_FCMA      (1UL << 14)
#define HWCAP_LRCPC     (1UL << 15)
#define HWCAP_SHA3      (1UL << 17)
#define HWCAP_SHA512    (1UL << 21)
#define HWCAP_ASIMDDP   (1UL << 20)
#define HWCAP_ASIMDFHM  (1UL << 23)
#define HWCAP_FLAGM     (1UL << 27)

static int hw_opt(const char *name) {
    int v = 0;
    size_t sz = sizeof v;
    if (sysctlbyname(name, &v, &sz, NULL, 0) != 0) return 0;
    return v != 0;
}

static unsigned long build_hwcap(void) {
    unsigned long c = 0;
    // FP and AdvSIMD are architectural on every arm64 Darwin device; the rest
    // are asked for by name. `hw.optional.arm.FEAT_*` is the modern spelling and
    // the older aliases are checked too, because a missing sysctl and a
    // false one are indistinguishable through sysctlbyname.
    c |= HWCAP_FP | HWCAP_ASIMD;
    if (hw_opt("hw.optional.arm.FEAT_AES")    || hw_opt("hw.optional.arm.crypto")) c |= HWCAP_AES | HWCAP_PMULL;
    if (hw_opt("hw.optional.arm.FEAT_SHA1")   || hw_opt("hw.optional.arm.crypto")) c |= HWCAP_SHA1;
    if (hw_opt("hw.optional.arm.FEAT_SHA256") || hw_opt("hw.optional.arm.crypto")) c |= HWCAP_SHA2;
    if (hw_opt("hw.optional.armv8_crc32"))     c |= HWCAP_CRC32;
    if (hw_opt("hw.optional.arm.FEAT_LSE")    || hw_opt("hw.optional.armv8_1_atomics")) c |= HWCAP_ATOMICS;
    if (hw_opt("hw.optional.arm.FEAT_FP16"))   c |= HWCAP_FPHP | HWCAP_ASIMDHP;
    if (hw_opt("hw.optional.arm.FEAT_RDM"))    c |= HWCAP_ASIMDRDM;
    if (hw_opt("hw.optional.arm.FEAT_JSCVT"))  c |= HWCAP_JSCVT;
    if (hw_opt("hw.optional.arm.FEAT_FCMA"))   c |= HWCAP_FCMA;
    if (hw_opt("hw.optional.arm.FEAT_LRCPC"))  c |= HWCAP_LRCPC;
    if (hw_opt("hw.optional.arm.FEAT_SHA3"))   c |= HWCAP_SHA3;
    if (hw_opt("hw.optional.arm.FEAT_SHA512")) c |= HWCAP_SHA512;
    if (hw_opt("hw.optional.arm.FEAT_DotProd"))c |= HWCAP_ASIMDDP;
    if (hw_opt("hw.optional.arm.FEAT_FHM"))    c |= HWCAP_ASIMDFHM;
    if (hw_opt("hw.optional.arm.FEAT_FlagM"))  c |= HWCAP_FLAGM;
    return c;
}

unsigned long klb_getauxval(unsigned long type) {
    static unsigned long hwcap;
    static int done;
    switch (type) {
    case AT_HWCAP:
        if (!done) {
            hwcap = build_hwcap();
            // KL_HWCAP=<hex>: override the measured value outright. This is an
            // A/B, not a tuning knob — the bits select whole hand-written
            // assembly implementations (BoringSSL's aes_hw_* / gcm_*_v8 are
            // gated on AES|PMULL), so clearing them moves the guest onto its
            // portable C path. That is the one measurement that separates "our
            // runtime mishandles that assembly" from "the fault is elsewhere".
            const char *o = getenv("KL_HWCAP");
            if (o && *o) {
                hwcap = strtoul(o, NULL, 0);
                fprintf(stderr, "  [klepton] getauxval(AT_HWCAP) forced to 0x%lx "
                                "by KL_HWCAP\n", hwcap);
            }
            done = 1;
        }
        return hwcap;
    case AT_HWCAP2:  return 0;               // nothing in HWCAP2 is measurable here
    case AT_PAGESZ:  return (unsigned long)getpagesize();
    case AT_CLKTCK:  return 100;             // Linux CONFIG_HZ; what bionic reports
    case AT_SECURE:  return 0;
    case AT_PLATFORM: return (unsigned long)(uintptr_t)"aarch64";
    default:
        // Unknown keys really do return 0 on Linux, so this is the honest
        // answer rather than a silent zero (trap 6d) — errno is set to match.
        errno = ENOENT;
        return 0;
    }
}

// ------------------------------------------------------------------- fenv
//
//   fenv_t   bionic 8 bytes   Darwin 16 bytes      (measured with the NDK's
//                                                   own headers, see PLANNING)
//
// The guest allocates the fenv_t — SDL3 does it on the stack around float
// conversions — so calling Darwin's fegetenv on it writes eight bytes past the
// end. That is trap 7 with a stack smash attached, and the failure would land
// in whatever local followed it.
//
// The contents are two system registers either way, so rather than translate
// between two libc representations we read and write FPCR/FPSR directly into
// bionic's layout. That is also what bionic itself does.
typedef struct { uint32_t fpcr, fpsr; } bionic_fenv;

static inline uint64_t rd_fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static inline uint64_t rd_fpsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
static inline void wr_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
static inline void wr_fpsr(uint64_t v) { __asm__ volatile("msr fpsr, %0" :: "r"(v)); }

int klb_fegetenv(void *env) {
    bionic_fenv *e = env;
    e->fpcr = (uint32_t)rd_fpcr();
    e->fpsr = (uint32_t)rd_fpsr();
    return 0;
}

int klb_fesetenv(const void *env) {
    const bionic_fenv *e = env;
    wr_fpcr(e->fpcr);
    wr_fpsr(e->fpsr);
    return 0;
}

int klb_feholdexcept(void *env) {
    bionic_fenv *e = env;
    uint64_t fpcr = rd_fpcr(), fpsr = rd_fpsr();
    e->fpcr = (uint32_t)fpcr;
    e->fpsr = (uint32_t)fpsr;
    wr_fpsr(fpsr & ~0x1fULL);          // clear the sticky exception flags
    wr_fpcr(fpcr & ~(0x1fULL << 8));   // and mask every exception trap
    return 0;
}

int klb_feupdateenv(const void *env) {
    const bionic_fenv *e = env;
    uint64_t raised = rd_fpsr() & 0x1f;
    wr_fpcr(e->fpcr);
    wr_fpsr(e->fpsr | raised);
    return 0;
}

// ----------------------------------------------------------------- statvfs
//
//   struct statvfs   bionic 112 bytes   Darwin 64 bytes
//   f_bavail         bionic  @32        Darwin @24   (Darwin's fsblkcnt_t is 32-bit)
//
// Every count field is a different width and every offset after f_blocks
// disagrees, so this is filled by hand from statfs(2). Same shape as klb_statfs
// in kl_libc.c, and the same reason it exists: 120 zero bytes reads as a full
// disk, not as "unknown" (trap 6d).
typedef struct {
    unsigned long f_bsize, f_frsize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail;
    unsigned long f_fsid, f_flag, f_namemax;
    uint32_t __reserved[6];
} bionic_statvfs;

static int fill_statvfs(const struct statfs *s, bionic_statvfs *b) {
    memset(b, 0, sizeof *b);
    b->f_bsize   = s->f_bsize;
    b->f_frsize  = s->f_bsize;
    b->f_blocks  = s->f_blocks;
    b->f_bfree   = s->f_bfree;
    b->f_bavail  = s->f_bavail;
    b->f_files   = s->f_files;
    b->f_ffree   = s->f_ffree;
    b->f_favail  = s->f_ffree;
    b->f_fsid    = (unsigned long)s->f_fsid.val[0];
    b->f_flag    = (s->f_flags & MNT_RDONLY) ? 1 : 0;   // ST_RDONLY
    b->f_namemax = 255;
    return 0;
}

int klb_statvfs(const char *path, void *out) {
    GUEST_PATH(path);
    struct statfs s;
    if (statfs(_p, &s) != 0) return -1;
    return fill_statvfs(&s, out);
}

int klb_fstatvfs(int fd, void *out) {
    struct statfs s;
    if (fstatfs(fd, &s) != 0) return -1;
    return fill_statvfs(&s, out);
}

// ---------------------------------------------------------------- sendfile
//
// The name exists on both platforms and means different things:
//
//   Linux   ssize_t sendfile(int out, int in, off_t *offset, size_t count)
//   Darwin  int     sendfile(int fd, int s, off_t offset, off_t *len,
//                            struct sf_hdtr *hdtr, int flags)
//
// Forwarding it would put `offset` (a pointer) in Darwin's `s` (an int) and
// call it a socket — exactly trap 6b, and this one would be a wild read rather
// than a wrong number. Darwin's also requires the *destination* to be a socket,
// which Linux's does not, so even with the arguments untangled it is not a
// substitute. A copy loop is.
ssize_t klb_sendfile(int out_fd, int in_fd, off_t *offset, size_t count) {
    char buf[65536];
    size_t done = 0;
    off_t pos = offset ? *offset : 0;
    while (done < count) {
        size_t want = count - done;
        if (want > sizeof buf) want = sizeof buf;
        ssize_t n = offset ? pread(in_fd, buf, want, pos) : read(in_fd, buf, want);
        if (n < 0) return done ? (ssize_t)done : -1;
        if (n == 0) break;
        ssize_t w = write(out_fd, buf, (size_t)n);
        if (w < 0) return done ? (ssize_t)done : -1;
        done += (size_t)w;
        pos  += w;
        if (w < n) break;
    }
    if (offset) *offset = pos;
    return (ssize_t)done;
}

// ------------------------------------------------------------------- epoll
//
// No Darwin equivalent, so it is built on kqueue. SDL3 and Valve's socket code
// both use it for readiness on a small number of fds, which is the case kqueue
// maps onto cleanly; what does not map is epoll's *level/edge* distinction and
// EPOLLONESHOT, which are not used here and are refused rather than faked.
//
// The lesson from `futex` (trap 6e) applies directly: refusing this does not
// fail loudly, it makes the caller spin. So it is implemented rather than
// stubbed, and anything it cannot express says so by name.
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLLIN     0x001
#define EPOLLPRI    0x002
#define EPOLLOUT    0x004
#define EPOLLERR    0x008
#define EPOLLHUP    0x010
#define EPOLLRDHUP  0x2000

// Linux's struct is PACKED on x86 but not on aarch64: 4-byte events then an
// 8-byte union at offset 8. Measured 16 bytes with the NDK headers.
typedef struct { uint32_t events; uint32_t __pad; uint64_t data; } linux_epoll_event;

int klb_epoll_create1(int flags) {
    int kq = kqueue();
    if (kq < 0) return -1;
    if (flags) fcntl(kq, F_SETFD, FD_CLOEXEC);   // EPOLL_CLOEXEC is the only flag
    return kq;
}

int klb_epoll_create(int size) { (void)size; return klb_epoll_create1(0); }

int klb_epoll_ctl(int epfd, int op, int fd, void *ev) {
    const linux_epoll_event *e = ev;
    struct kevent ch[2];
    int n = 0;
    // The udata rides on every filter so epoll_wait can hand back exactly what
    // the caller registered, which is what its consumers key on.
    void *ud = e ? (void *)(uintptr_t)e->data : NULL;
    uint32_t want = e ? e->events : 0;

    if (op == EPOLL_CTL_DEL) {
        EV_SET(&ch[n++], fd, EVFILT_READ,  EV_DELETE, 0, 0, NULL);
        EV_SET(&ch[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(epfd, ch, n, NULL, 0, NULL);      // ENOENT on a filter never added
        return 0;                                 // is not an error to epoll
    }
    if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD) { errno = EINVAL; return -1; }

    EV_SET(&ch[n++], fd, EVFILT_READ,
           (want & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, ud);
    EV_SET(&ch[n++], fd, EVFILT_WRITE,
           (want & EPOLLOUT) ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, ud);
    struct kevent out[2];
    int r = kevent(epfd, ch, n, out, n, &(struct timespec){0, 0});
    if (r < 0) return -1;
    // A delete of a filter that was never added reports ENOENT through the
    // eventlist rather than the return value. That is expected here — MOD
    // turning a direction off looks exactly like it — so only real errors count.
    for (int i = 0; i < r; i++)
        if ((out[i].flags & EV_ERROR) && out[i].data && out[i].data != ENOENT) {
            errno = (int)out[i].data;
            return -1;
        }
    return 0;
}

int klb_epoll_wait(int epfd, void *events, int maxevents, int timeout_ms) {
    if (maxevents <= 0) { errno = EINVAL; return -1; }
    struct kevent *kev = calloc((size_t)maxevents, sizeof *kev);
    if (!kev) { errno = ENOMEM; return -1; }
    struct timespec ts, *tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }
    int r = kevent(epfd, NULL, 0, kev, maxevents, tsp);
    if (r < 0) { free(kev); return -1; }

    // kqueue reports one event per (fd, filter); epoll reports one per fd with
    // the directions OR'd. Collapsing them matters: a caller that sees the same
    // fd twice may close it on the first and act on a stale one on the second.
    linux_epoll_event *out = events;
    int n = 0;
    for (int i = 0; i < r; i++) {
        uint32_t bits = 0;
        if (kev[i].filter == EVFILT_READ)  bits |= EPOLLIN;
        if (kev[i].filter == EVFILT_WRITE) bits |= EPOLLOUT;
        if (kev[i].flags & EV_EOF)   bits |= EPOLLHUP | EPOLLRDHUP;
        if (kev[i].flags & EV_ERROR) bits |= EPOLLERR;
        uint64_t data = (uint64_t)(uintptr_t)kev[i].udata;
        int j = 0;
        for (; j < n; j++) if (out[j].data == data) { out[j].events |= bits; break; }
        if (j == n) { out[n].events = bits; out[n].__pad = 0; out[n].data = data; n++; }
    }
    free(kev);
    return n;
}

// ------------------------------------------------------- *at() file syscalls
//
// Fixed-arity on purpose, not variadic. AAPCS64 passes the first eight
// arguments of a variadic call in x0-x7, which is exactly where a fixed-arity
// Darwin function reads its own, so `mode` arrives correctly with no thunk —
// the same reasoning that made klb_syscall fixed-arity (trap 6e).
//
// The flag translation is the point (trap 4): O_* differ between the platforms,
// and openat without it is the same bug that made every guest anonymous mmap a
// file mapping of fd -1.
int klb_openat(int dirfd, const char *path, int flags, int mode) {
    GUEST_PATH(path);
    return openat(dirfd, _p, kl_open_flags(flags), (mode_t)mode);
}

// bionic's __open_2 is FORTIFY's non-variadic open: it exists precisely to
// assert that no mode was passed, so O_CREAT is a caller bug rather than
// something to handle.
int klb___open_2(const char *path, int flags) {
    GUEST_PATH(path);
    return open(_p, kl_open_flags(flags));
}

// --------------------------------------------------------------- FORTIFY
//
// The `_chk` family is bionic's compile-time-bounds-checked libc: same
// behaviour as the plain function plus a trailing "size of the destination
// object" argument, and an abort if the copy would exceed it. Keeping the
// check rather than dropping the argument is deliberate — these fire on real
// bugs, and a guest that trips one should stop here with a name rather than
// corrupt memory and stop somewhere else.
static void chk_fail(const char *who, size_t len, size_t cap) {
    fprintf(stderr, "[klepton] fatal: %s: %zu bytes into a %zu-byte buffer "
                    "(bionic FORTIFY would have aborted here too)\n", who, len, cap);
    kl_fatal_prepare();
    abort();
}

// (__memcpy_chk, __memset_chk, __strcpy_chk, __strlen_chk, __strchr_chk and
// __vsnprintf_chk already exist in kl_shim.c — Beat Saber reached those. Only
// the ones this target adds are here.)
void *klb___memmove_chk(void *d, const void *s, size_t n, size_t cap) {
    if (n > cap) chk_fail("__memmove_chk", n, cap);
    return memmove(d, s, n);
}
char *klb___strncpy_chk(char *d, const char *s, size_t n, size_t cap) {
    if (n > cap) chk_fail("__strncpy_chk", n, cap);
    return strncpy(d, s, n);
}
// The two-size form additionally knows the source's declared length, which it
// uses only to bound the traversal; the copy semantics are strncpy's.
char *klb___strncpy_chk2(char *d, const char *s, size_t n, size_t dcap, size_t scap) {
    if (n > dcap) chk_fail("__strncpy_chk2", n, dcap);
    if (strnlen(s, scap) >= scap && n > scap) chk_fail("__strncpy_chk2 (src)", n, scap);
    return strncpy(d, s, n);
}
char *klb___strcat_chk(char *d, const char *s, size_t cap) {
    size_t n = strlen(d) + strlen(s) + 1;
    if (n > cap) chk_fail("__strcat_chk", n, cap);
    return strcat(d, s);
}
ssize_t klb___read_chk(int fd, void *buf, size_t n, size_t cap) {
    if (n > cap) chk_fail("__read_chk", n, cap);
    return read(fd, buf, n);
}

// __vsprintf_chk takes an already-materialised va_list, which on AAPCS64 is a
// 32-byte descriptor passed *by reference* — so a kl_va* parameter receives it
// directly and no asm thunk is needed, exactly as klh_android_log_print does
// (trap 6b). flags/cap are FORTIFY's and do not participate in the format.
//
// Bounding the write by `cap` rather than ignoring it is what makes this the
// checked variant it claims to be: bionic aborts here, and vsprintf into a
// buffer the caller told us the size of would otherwise be the one place we
// silently drop the guarantee the guest was compiled expecting.
int klb___vsprintf_chk(char *d, int flags, size_t cap, const char *fmt, kl_va *va) {
    (void)flags;
    char _m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, va, _m, sizeof _m, KL_VA_PRINTF) == (size_t)-1) return -1;
    int n = vsnprintf(d, cap == (size_t)-1 ? (size_t)INT32_MAX : cap, fmt, (va_list)_m);
    if (cap != (size_t)-1 && n >= 0 && (size_t)n >= cap) chk_fail("__vsprintf_chk", (size_t)n + 1, cap);
    return n;
}

// ---------------------------------------------------------------- odds and ends
//
// sincosf is a GNU extension with no Darwin declaration. Computing both is
// correct and the compiler folds it to the same pair of calls the guest would
// have made itself.
void klb_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
void klb_sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }

// Darwin defines putchar/putc/getchar as macros, so a direct forward would take
// the macro rather than a function address.
int klb_putchar(int c) { return fputc(c, stdout); }
int klb_getchar(void)  { return fgetc(stdin); }

// Darwin has no fdatasync; fsync is a strict superset (it also flushes
// metadata), so this is slower than asked for and never weaker.
int klb_fdatasync(int fd) { return fsync(fd); }

// (stat64/lstat64/fstat64 are bionic's LP64 aliases for stat and are bound
// straight to klb_stat & co. in kl_shim.c's table — not to Darwin's stat, whose
// struct layout differs (trap 7). No wrapper here on purpose: another prototype
// for those would be a second copy of a layout that must not drift.)

// Linux's cmsghdr leads with a size_t; Darwin's with a 32-bit socklen_t, so the
// header is 16 bytes on one and 12 on the other and CMSG_NXTHDR cannot be
// shared. bionic exports this as a real function (the macro calls it), so it is
// implemented against the guest's layout.
typedef struct { size_t cmsg_len; int cmsg_level, cmsg_type; } linux_cmsghdr;
typedef struct {
    void *msg_name; uint32_t msg_namelen; uint32_t __pad;
    void *msg_iov;  size_t msg_iovlen;
    void *msg_control; size_t msg_controllen;
    int msg_flags;
} linux_msghdr;

void *klb___cmsg_nxthdr(void *mhdr, void *cmsg) {
    linux_msghdr  *m = mhdr;
    linux_cmsghdr *c = cmsg;
    if (c->cmsg_len < sizeof *c) return NULL;
    size_t step = (c->cmsg_len + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1);
    linux_cmsghdr *next = (linux_cmsghdr *)((char *)c + step);
    char *end = (char *)m->msg_control + m->msg_controllen;
    if ((char *)next + sizeof *next > end) return NULL;
    if ((char *)next + next->cmsg_len > end) return NULL;
    return next;
}

// C++11 thread_local destructors. Darwin's own facility is dyld's
// `_tlv_atexit`, not libc++abi's `__cxa_thread_atexit` — the latter happens to
// be exported on macOS and is NOT on xrOS, which is a link error rather than a
// runtime one, and is the second half of what broke `make xros`.
//
// `_tlv_atexit` takes no DSO handle. Nothing is lost: the handle exists so a
// runtime can run a library's thread-locals' destructors when that library is
// unloaded early, and guest images are never unloaded here.
extern void _tlv_atexit(void (*fn)(void *), void *obj);
int klb___cxa_thread_atexit_impl(void (*fn)(void *), void *obj, void *dso) {
    (void)dso;
    _tlv_atexit(fn, obj);
    return 0;
}

// bionic exports the three standard streams as `FILE *` VARIABLES, so what the
// shim must supply is the address of a pointer, not the stream. Darwin's own
// `stdout`/`stderr` are variables too, and kl_host_file passes any pointer
// outside the __sF stand-in array straight through, so the guest's writes land
// where they should. (Beat Saber only ever reached __sF, hence these arriving
// with the second target.)

// ------------------------------------------------------------------ _ctype_
//
// bionic exports `_ctype_` as a 1+256 byte table and its <ctype.h> macros index
// it directly as `_ctype_[c + 1]` — so the guest reads this array itself rather
// than calling isupper(). Darwin has no `_ctype_` at all (it uses
// _DefaultRuneLocale with a different shape and different bit values), which is
// why a direct forward does not merely differ, it does not link.
//
// Built from the host's own classifiers at startup rather than transcribed:
// a hand-copied table is a second copy of a fact, and this way the answers
// cannot disagree with the isupper() the same guest may also call. The bit
// values are bionic's, and those DO have to be written down.
// Prefixed, because Darwin's own <ctype.h> defines _CTYPE_U and friends with
// DIFFERENT values (0x00008000L, not 0x01). Reusing those names compiled with a
// warning and would have built the table out of the host's bit assignments
// while the guest read it with bionic's — a silent wrong answer of exactly the
// kind trap 4 describes, caught only because the redefinition was noisy.
#define KLB_CT_U 0x01   /* upper */
#define KLB_CT_L 0x02   /* lower */
#define KLB_CT_D 0x04   /* digit */
#define KLB_CT_S 0x08   /* space */
#define KLB_CT_P 0x10   /* punct */
#define KLB_CT_C 0x20   /* control */
#define KLB_CT_X 0x40   /* hex digit */
#define KLB_CT_B 0x80   /* blank */

static unsigned char g_ctype[257];
const unsigned char *klb_ctype_ptr = g_ctype + 1;   // the table entry is &this

__attribute__((constructor)) static void kl_build_ctype(void) {
    for (int c = -1; c < 256; c++) {
        unsigned char f = 0;
        if (c >= 0 && c < 128) {                    // ASCII only, as bionic's is
            if (isupper(c))  f |= KLB_CT_U;
            if (islower(c))  f |= KLB_CT_L;
            if (isdigit(c))  f |= KLB_CT_D;
            if (isspace(c))  f |= KLB_CT_S;
            if (ispunct(c))  f |= KLB_CT_P;
            if (iscntrl(c))  f |= KLB_CT_C;
            if (isxdigit(c)) f |= KLB_CT_X;
            if (isblank(c))  f |= KLB_CT_B;
        }
        g_ctype[c + 1] = f;
    }
}

// FORTIFY's write, the counterpart to __read_chk above.
ssize_t klb___write_chk(int fd, const void *buf, size_t n, size_t cap) {
    if (n > cap) chk_fail("__write_chk", n, cap);
    return write(fd, buf, n);
}

// bionic's atfork registration, which libc++ and the pthread shims funnel
// through. The 4th argument is the caller's DSO handle, used only so bionic can
// unregister handlers when a library is unloaded — we never unload a guest
// image, so dropping it is exact rather than approximate.
int klb___register_atfork(void (*prep)(void), void (*parent)(void),
                          void (*child)(void), void *dso) {
    (void)dso;
    return pthread_atfork(prep, parent, child);
}

// The GNU flavour of strerror_r returns char* and may ignore the buffer; the
// POSIX one Darwin has returns int. Getting these two confused is trap 6b in
// miniature — the caller would read an errno as a pointer.
char *klb___gnu_strerror_r(int err, char *buf, size_t cap) {
    if (strerror_r(err, buf, cap) != 0) snprintf(buf, cap, "Unknown error %d", err);
    return buf;
}

// ---------------------------------------------------------------- stdio
//
// Anything taking a guest FILE* has to go through kl_host_file, which
// demultiplexes bionic's __sF array (kl_shim.c). Forwarding these directly
// would hand Darwin's stdio a pointer into our stand-in array.
int   klb_fileno(void *f)                  { return fileno(kl_host_file(f)); }
int   klb_fgetc(void *f)                   { return fgetc(kl_host_file(f)); }
int   klb_ungetc(int c, void *f)           { return ungetc(c, kl_host_file(f)); }
wint_t klb_getwc(void *f)                  { return fgetwc(kl_host_file(f)); }
wint_t klb_fgetwc(void *f)                 { return fgetwc(kl_host_file(f)); }
wint_t klb_ungetwc(wint_t c, void *f)      { return ungetwc(c, kl_host_file(f)); }
wint_t klb_fputwc(wchar_t c, void *f)      { return fputwc(c, kl_host_file(f)); }
wint_t klb_putwc(wchar_t c, void *f)       { return fputwc(c, kl_host_file(f)); }
int   klb_fwide(void *f, int mode)         { return fwide(kl_host_file(f), mode); }

// ---------------------------------------------------------------- Qt / libshell
//
// The 2D configuration frontend (libshell + Qt6) reaches a fourth category
// beyond the three in this file's header: Linux SYSCALL WRAPPERS that Darwin
// simply does not have. Qt's event dispatcher, file watcher and process code
// are written against them directly.
//
// The rule below is the project's usual one — answer honestly, never with a
// silent zero (trap 6d). Where Darwin has the facility under another shape we
// build it; where it genuinely has no equivalent we fail with the errno Qt
// already handles, because Qt has a fallback for every one of these and taking
// the fallback is a better outcome than pretending.

// ---- eventfd ----
//
// Qt uses it for exactly one thing: waking a blocked event dispatcher from
// another thread (QEventDispatcherUNIXPrivate's wakeup channel). That is the
// self-pipe trick with a nicer API, so a self-connected AF_UNIX datagram socket
// reproduces it faithfully with the property that matters — ONE descriptor,
// readable as soon as it has been written, closable with plain close().
//
// The socketpair alternative needs a side table to find the write end and leaks
// the partner when the guest closes the fd; this has neither problem.
//
// What is NOT reproduced is the 64-bit counter: eventfd accumulates, this
// queues datagrams. eventfd_read below drains everything pending and sums it,
// which matches for every use Qt makes of it. A guest using eventfd as a
// SEMAPHORE (EFD_SEMAPHORE) would see different behaviour — none does, and the
// flag is refused below rather than ignored.
#define LX_EFD_SEMAPHORE 1
#define LX_EFD_CLOEXEC   0x80000
#define LX_EFD_NONBLOCK  0x800

int klb_eventfd(unsigned int initval, int flags) {
    if (flags & LX_EFD_SEMAPHORE) {   // see above: we do not implement counting
        errno = EINVAL;
        return -1;
    }
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0) return -1;

    // A name is needed only long enough to bind and connect; the connection
    // outlives the unlink, so nothing is left in the filesystem.
    static _Atomic unsigned seq;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    const char *tmp = getenv("TMPDIR"); if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(a.sun_path, sizeof a.sun_path, "%s/kl-efd-%d-%u",
             tmp, (int)getpid(), atomic_fetch_add(&seq, 1));
    unlink(a.sun_path);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0
        || connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        int e = errno; unlink(a.sun_path); close(s); errno = e;
        return -1;
    }
    unlink(a.sun_path);

    if (flags & LX_EFD_CLOEXEC)  fcntl(s, F_SETFD, FD_CLOEXEC);
    if (flags & LX_EFD_NONBLOCK) fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    for (unsigned i = 0; i < initval; i++) {   // initval is 0 in every real use
        uint64_t one = 1;
        if (send(s, &one, sizeof one, 0) < 0) break;
    }
    return s;
}

int klb_eventfd_read(int fd, uint64_t *value) {
    uint64_t total = 0, v;
    ssize_t r = recv(fd, &v, sizeof v, 0);       // honours the fd's blocking mode
    if (r != (ssize_t)sizeof v) return -1;
    total = v;
    // Drain the rest without blocking, so one read clears the channel the way a
    // real eventfd does rather than leaving it spuriously readable.
    for (;;) {
        r = recv(fd, &v, sizeof v, MSG_DONTWAIT);
        if (r != (ssize_t)sizeof v) break;
        total += v;
    }
    if (value) *value = total;
    return 0;
}

int klb_eventfd_write(int fd, uint64_t value) {
    ssize_t w = send(fd, &value, sizeof value, 0);
    return w == (ssize_t)sizeof value ? 0 : -1;
}

// ---- ppoll ----
//
// poll() with a timespec and an optional signal mask. The mask swap is NOT
// atomic with the wait here, which is the whole point of ppoll on Linux — but
// the race it closes is "a signal arrives between unblocking and polling", and
// nothing in this process delivers signals to the guest's event threads (the
// guest's own fatal handlers are not installed by default, trap 5). Qt passes
// NULL for the mask in the dispatcher path; the mask is honoured for the sake
// of a caller that does pass one, rather than being silently dropped.
int klb_ppoll(struct pollfd *fds, unsigned long nfds, const struct timespec *ts,
              const void *sigmask) {
    int ms = -1;
    if (ts) {
        long long m = (long long)ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
        // A non-zero timespec below a millisecond must not round to "return
        // immediately" — Qt uses sub-ms timeouts and a 0 here would spin.
        if (m == 0 && (ts->tv_sec || ts->tv_nsec)) m = 1;
        ms = m > INT_MAX ? INT_MAX : (int)m;
    }
    if (!sigmask) return poll(fds, (nfds_t)nfds, ms);

    sigset_t saved;
    pthread_sigmask(SIG_SETMASK, (const sigset_t *)sigmask, &saved);
    int r = poll(fds, (nfds_t)nfds, ms);
    int e = errno;
    pthread_sigmask(SIG_SETMASK, &saved, NULL);
    errno = e;
    return r;
}

// ---- the three "...and set these flags atomically" variants ----
// Darwin has the base call and F_SETFD/F_SETFL, so these are the base call plus
// a follow-up. Not atomic against fork() in another thread; nothing here forks.
// The flag words are the GUEST's (Linux) numbering — trap 4, and the same
// values that made fcntl() wrong until it was fixed.
#define LX_SOCK_NONBLOCK 0x800
#define LX_SOCK_CLOEXEC  0x80000

static int apply_linux_fd_flags(int fd, int flags) {
    if (fd < 0) return fd;
    if (flags & LX_SOCK_CLOEXEC)  fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (flags & LX_SOCK_NONBLOCK) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

int klb_accept4(int s, struct sockaddr *addr, socklen_t *alen, int flags) {
    return apply_linux_fd_flags(accept(s, addr, alen), flags);
}

int klb_pipe2(int fds[2], int flags) {
    if (pipe(fds) < 0) return -1;
    apply_linux_fd_flags(fds[0], flags);
    apply_linux_fd_flags(fds[1], flags);
    return 0;
}

// ---- dup2/dup3, and the guest's attempt to own our stdout ----
//
// libvrlink_scene pipe()s and dup2()s the write end over BOTH fd 1 and fd 2 at
// startup (its `debug_capture_thread`), which is the ordinary Android idiom:
// an app's stdout and stderr go nowhere useful on Android, so a capture thread
// reads them back and re-emits each line through __android_log_write, where
// logd is the drain.
//
// Here that closes a cycle, and it wedged the first VR run completely.
// kl_android_log_write writes to *stderr* — which, after the dup2, IS the pipe
// the capture thread is reading. Every line it forwards it hands straight back
// to itself; 64 KB of pipe buffer later every writer blocks in write() forever.
// The abort that followed reported nothing, because the fault reporter's own
// output went into the same pipe. That is the whole measurement instrument
// disconnected — an M4 abort-by-name, the gap report, the fault reporter, all
// of it — at exactly the moment it is needed. Silence read as a hang.
//
// So the redirection is DECLINED, and both sides get what they were after: the
// guest wanted its stdout in the log, and our stderr already is the log. The
// capture thread then blocks on a read() that never returns, which is what a
// capture thread with nothing to capture should do.
//
// It reports success (dup2 returns the new descriptor) rather than an error,
// for klb_sigaction's reason — the guest believes it is installed, and a guest
// that checks the return value does not take an error path over a diagnostic
// convenience. Logged once by name: trap 6d, a silent refusal is worse than a
// loud one.
static int kl_fd_hijack_refused(int to) {
    if (to != STDOUT_FILENO && to != STDERR_FILENO) return 0;
    static int warned[2];
    if (!warned[to - 1]++)
        fprintf(stderr, "  [klepton] declining the guest's dup2() onto fd %d — "
                        "our diagnostics live there, and its capture thread "
                        "would be reading its own output back (see kl_libc_slink.c)\n",
                to);
    return 1;
}

int klb_dup2(int from, int to) {
    if (kl_fd_hijack_refused(to)) return to;
    return dup2(from, to);
}

int klb_dup3(int from, int to, int flags) {
    // dup3 differs from dup2 in one observable way besides the flags: it is an
    // error rather than a no-op when the two descriptors are equal.
    if (from == to) { errno = EINVAL; return -1; }
    if (kl_fd_hijack_refused(to)) return to;
    if (dup2(from, to) < 0) return -1;
    return apply_linux_fd_flags(to, flags);
}

// ---- memfd_create ----
// An anonymous file that can be ftruncate'd and mmap'd. shm_open gives exactly
// that once the name is unlinked; the fd keeps the object alive.
int klb_memfd_create(const char *name, unsigned int flags) {
    static _Atomic unsigned seq;
    char nm[64];
    snprintf(nm, sizeof nm, "/kl-memfd-%d-%u", (int)getpid(), atomic_fetch_add(&seq, 1));
    (void)name;                                    // Linux's is a debug label only
    int fd = shm_open(nm, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(nm);
    if (flags & LX_SOCK_CLOEXEC) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

// ---- inotify ----
// No Darwin equivalent that maps onto it (kqueue watches descriptors, not
// paths, and cannot watch a directory's contents the way inotify does). Qt's
// QFileSystemWatcher probes each backend and falls back to a polling engine
// when one is unavailable, so ENOSYS selects a working path — where a fake
// success would give it a watch that never fires.
int klb_inotify_init(void)               { errno = ENOSYS; return -1; }
int klb_inotify_init1(int f)             { (void)f; errno = ENOSYS; return -1; }
int klb_inotify_add_watch(int fd, const char *p, uint32_t m) {
    (void)fd; (void)p; (void)m; errno = ENOSYS; return -1;
}
int klb_inotify_rm_watch(int fd, int wd)  { (void)fd; (void)wd; errno = ENOSYS; return -1; }

// ---- clone ----
// The raw Linux thread/process primitive. Qt reaches it only through forkfd for
// QProcess, which has a fork()+pipe fallback. Refused rather than approximated:
// its argument shape (a child stack pointer and a flag word selecting what is
// shared) has no honest Darwin translation, and getting it half right would
// produce a process whose memory sharing is not what the caller asked for.
int klb_clone(void) { errno = ENOSYS; return -1; }

// ---- __assert2 ----
// bionic's assert. Fatal by definition, so it goes through kl_fatal_prepare for
// trap 5's reason: the guest's own signal handlers expect a Linux ucontext and
// hang on Darwin's.
void klb___assert2(const char *file, int line, const char *fn, const char *msg) {
    fprintf(stderr, "  [klepton] guest assertion failed: %s:%d: %s: %s\n",
            file ? file : "?", line, fn ? fn : "?", msg ? msg : "?");
    fflush(NULL);
    kl_fatal_prepare();
    abort();
}

// ---- two more of the FORTIFY family ----
// Same contract as the ones above: the compiler passes the destination's known
// size as an extra argument and the runtime is expected to trap an overflow
// rather than perform it.
void klb___FD_CLR_chk(int fd, fd_set *set, size_t set_size) {
    if (fd < 0 || (size_t)fd >= set_size * 8) {
        fprintf(stderr, "  [klepton] __FD_CLR_chk: fd %d out of range\n", fd);
        kl_fatal_prepare();
        abort();
    }
    FD_CLR(fd, set);
}

char *klb___fgets_chk(char *dst, int supplied, void *stream, size_t dst_len) {
    if (supplied < 0 || (size_t)supplied > dst_len) {
        fprintf(stderr, "  [klepton] __fgets_chk: %d bytes into a %zu-byte buffer\n",
                supplied, dst_len);
        kl_fatal_prepare();
        abort();
    }
    return fgets(dst, supplied, kl_host_file(stream));
}

// ---- __pthread_cleanup_push / _pop ----
//
// bionic's shape, and it is not glibc's: the cleanup record is allocated by the
// CALLER (on its stack, inside the pthread_cleanup_push macro) and bionic only
// links it into a per-thread list. So the layout below is part of the ABI and
// is transcribed from bionic's <pthread.h>, not invented.
//
// The list is maintained because pop(0) — pop without executing — is common and
// must still unlink. What we do not do is run these on pthread_cancel: nothing
// in this project cancels a thread, and Darwin's cancellation points would not
// walk our list anyway.
typedef struct kl_pthread_cleanup {
    struct kl_pthread_cleanup *prev;
    void (*routine)(void *);
    void *arg;
} kl_pthread_cleanup;

static _Thread_local kl_pthread_cleanup *g_cleanups;

void klb___pthread_cleanup_push(void *c, void (*routine)(void *), void *arg) {
    kl_pthread_cleanup *rec = (kl_pthread_cleanup *)c;
    if (!rec) return;
    rec->routine = routine;
    rec->arg     = arg;
    rec->prev    = g_cleanups;
    g_cleanups   = rec;
}

void klb___pthread_cleanup_pop(void *c, int execute) {
    kl_pthread_cleanup *rec = (kl_pthread_cleanup *)c;
    if (!rec) return;
    g_cleanups = rec->prev;
    if (execute && rec->routine) rec->routine(rec->arg);
}
