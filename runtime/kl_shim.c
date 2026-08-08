// bionic -> Darwin symbol table.
//
// Three tiers:
//   1. kl_libc_table.h  — generated direct forwards (signature + layout both match)
//   2. klb_* / klv_*    — hand-written: divergent layouts (kl_libc.c, kl_pthread.c,
//                          kl_dl.c) and AAPCS64 variadic thunks (kl_va_thunks.S)
//   3. local statics    — small bionic-isms defined right here
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <setjmp.h>
#include <locale.h>
#include <xlocale.h>
#include <libgen.h>
#include <pthread.h>
#include <sched.h>
#include <poll.h>
#include <termios.h>
#include <malloc/malloc.h>
// <sys/random.h> does not exist in the xrOS or xrsimulator SDKs, but the
// symbol does — _getentropy is exported from libSystem on both. Only the
// declaration is missing, so supply it rather than substituting a different
// generator: the guest imports getentropy by name and the shim forwards it
// directly, so the implementation must stay the platform's own.
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#else
int getentropy(void *buffer, size_t size);
#endif
#include <utime.h>
#include <pwd.h>
#include <zlib.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <net/if.h>
#include "klepton.h"
#include "kl_va.h"
#include "kl_ndk.h"
#include "kl_egl.h"
#include "kl_opensl.h"
#include "kl_ovrp.h"
#include "kl_ovrplat.h"
#include "kl_jni.h"

// ---------- S0.1: bionic stack-guard canary in Darwin TSD slot 5 ----------
#define TLS_SLOT_STACK_GUARD 5
static uint64_t g_canary = 0x0000BEEFCAFE0000ULL;
void kl_thread_init(void) {
    uint64_t tp; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    ((uint64_t *)tp)[TLS_SLOT_STACK_GUARD] = g_canary;
}

// The guest installs its own fatal-signal handlers — IL2CPP does this for crash
// reporting — and they expect a Linux `ucontext_t`. Ours hands them a Darwin one,
// so a diagnostic abort() is caught by guest code that then wanders off and hangs
// instead of dying. Restoring the default disposition first means our own failure
// reports actually terminate the process. Without this every diagnostic below
// turns into a 20-minute mystery hang.
void kl_fatal_prepare(void) {
    // Whatever we are dying of, the graphics surface is context — and unlike the
    // JNI report, plenty of fatal paths (a failed sem_wait, an unresolved
    // import) have no reason to know EGL exists. Idempotent, so paths that
    // already printed it are unaffected.
    kl_egl_report(stderr);
    kl_opensl_report(stderr);
    kl_ovrp_report(stderr);
    kl_ovrplat_report(stderr);
    static const int sigs[] = {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) signal(sigs[i], SIG_DFL);
    fflush(NULL);
}

__attribute__((noreturn)) static void die(const char *what) {
    fprintf(stderr, "[klepton] fatal: %s\n", what);
    // Whatever we died of, the JNI surface is the context: it says how far the
    // guest got and what it had asked for by then. Without it these paths print
    // one line and lose the whole run, which reads as a much earlier failure.
    kl_jni_report(stderr);
    kl_fatal_prepare();
    abort();
}
__attribute__((noreturn)) static void kl_unresolved(void) { die("called an unresolved import"); }

// The named form, reached through the per-symbol trampolines kl_image.c builds.
// Only imports that never got a stub — an allocation failure — still land on the
// anonymous kl_unresolved above.
__attribute__((noreturn)) void kl_unresolved_named(const char *name) {
    char msg[256];
    snprintf(msg, sizeof msg, "called unresolved import '%s'", name ? name : "?");
    die(msg);
}
__attribute__((noreturn)) static void kl_stack_chk_fail(void) { die("stack smashing detected"); }

// ---------- knobs ----------
// See klepton.h for why this reads the value and not just the presence.
int kl_env_on(const char *name, int dflt) {
    const char *v = getenv(name);
    if (!v) return dflt;
    return !(!*v || !strcmp(v, "0") || !strcasecmp(v, "no")
             || !strcasecmp(v, "off") || !strcasecmp(v, "false"));
}

// ---------- bionic FILE ----------
// The guest computes stderr as &__sF[2] using bionic's sizeof(FILE), which we do not
// know, so anything landing inside the block routes to stderr.
// TODO(M3): recover the true stride from the GLOB_DAT addend.
static char g_sF[4096] __attribute__((aligned(16)));
FILE *kl_host_file(void *guest) {
    if ((char *)guest >= g_sF && (char *)guest < g_sF + sizeof g_sF) return stderr;
    // A FILE * small enough to be a page-zero offset is not a FILE * — it is an
    // argument that landed in the wrong slot (trap 6b) or an uninitialised one.
    // Passing it through means dying inside flockfile with no idea who called,
    // so it is named here instead and the call is dropped.
    // A FILE * small enough to be a page-zero offset is not a FILE * — it is an
    // argument that landed in the wrong slot (trap 6b) or an uninitialised one.
    // Passing it through dies inside flockfile with no clue who called; naming it
    // and routing to stderr keeps the run going, and a write that lands in the
    // log is a better outcome than a dropped one. Reads simply fail, which is
    // the honest answer for a handle that was never valid.
    if ((uintptr_t)guest < 0x10000) {
        static void *seen[8];
        static unsigned n;
        unsigned i = 0;
        for (; i < n; i++) if (seen[i] == guest) break;
        if (i == n && n < 8) {
            seen[n++] = guest;
            fprintf(stderr, "[klepton] bogus guest FILE * %p — routed to stderr\n", guest);
        }
        return stderr;
    }
    return (FILE *)guest;
}
static int kl_fputc(int c, void *f)                { return fputc(c, kl_host_file(f)); }
static int kl_fputs(const char *s, void *f)        { return fputs(s, kl_host_file(f)); }
static size_t kl_fwrite(const void *p, size_t a, size_t b, void *f) { return fwrite(p, a, b, kl_host_file(f)); }
// KL_TRACE_IO=1: one line a second with the running fread byte total — the
// async loader's throughput is the loading-screen pacing question. Bytes are
// attributed per open file (kl_file_path) so a slow trickle names its file.
extern const char *kl_file_path(void *f);
static size_t kl_fread(void *p, size_t a, size_t b, void *f) {
    FILE *hf = kl_host_file(f);
    size_t r = fread(p, a, b, hf);
    static int on = -1;
    if (on < 0) on = getenv("KL_TRACE_IO") != NULL;
    if (on) {
        static _Atomic unsigned long long bytes, lines;
        static _Atomic time_t last;
        static struct { char path[160]; unsigned long long bytes; } per[32];
        static int nper;
        if (r) {
            const char *path = kl_file_path(f);
            if (!path) path = "?";
            int i;
            for (i = 0; i < nper; i++) if (!strcmp(per[i].path, path)) break;
            if (i == nper && nper < 32) {
                snprintf(per[nper].path, sizeof per[nper].path, "%s", path);
                nper++;
            }
            if (i < nper) per[i].bytes += (unsigned long long)r * a;
        }
        unsigned long long nb = atomic_fetch_add(&bytes, r * a) + r * a;
        time_t now = time(NULL), prev = atomic_load(&last);
        if (now != prev && atomic_compare_exchange_strong(&last, &prev, now)) {
            fprintf(stderr, "  [io] fread total %.1f MB after %llu calls\n",
                    (double)nb / 1048576.0, (unsigned long long)atomic_fetch_add(&lines,0));
            for (int i = 0; i < nper && i < 6; i++)
                fprintf(stderr, "        %8.1f MB  %s\n",
                        (double)per[i].bytes / 1048576.0, per[i].path);
        }
        atomic_fetch_add(&lines, 1);
    }
    return r;
}
static int kl_fclose(void *f)                      { return fclose(kl_host_file(f)); }
static int kl_fflush(void *f)                      { return fflush(f ? kl_host_file(f) : NULL); }
static char *kl_fgets(char *s, int n, void *f)     { return fgets(s, n, kl_host_file(f)); }
static int kl_getc_(void *f)                       { return getc(kl_host_file(f)); }
static int kl_feof(void *f)                        { return feof(kl_host_file(f)); }
static int kl_ferror(void *f)                      { return ferror(kl_host_file(f)); }
static void kl_clearerr(void *f)                   { clearerr(kl_host_file(f)); }
static int kl_fseek(void *f, long o, int w)        { return fseek(kl_host_file(f), o, w); }
static int kl_fseeko(void *f, off_t o, int w)      { return fseeko(kl_host_file(f), o, w); }
static long kl_ftell(void *f)                      { return ftell(kl_host_file(f)); }
static off_t kl_ftello(void *f)                    { return ftello(kl_host_file(f)); }
static int kl_setvbuf(void *f, char *b, int m, size_t n) { return setvbuf(kl_host_file(f), b, m, n); }

// ---------- sockets: bionic -> Darwin option constants ----------
// socket()/bind()/connect() forward verbatim — the address families, socket
// types and protocol numbers agree. The SOL_SOCKET *level* and its option
// numbers do not: bionic SOL_SOCKET is 1, Darwin's is 0xffff, and the option
// numbers diverge (SO_RCVTIMEO 20 vs 0x1006 — forwarded raw it lands on no
// valid option, setsockopt fails EINVAL, and Unity's Ping prints "Error
// setting socket options" forever). Payload layouts (int, timeval, linger)
// match, so a pure constant remap suffices.
static int kl_sock_optname(int opt) {
    switch (opt) {                          // bionic SOL_SOCKET numbers
    case 1:  return SO_DEBUG;
    case 2:  return SO_REUSEADDR;
    case 3:  return SO_TYPE;
    case 4:  return SO_ERROR;
    case 5:  return SO_DONTROUTE;
    case 6:  return SO_BROADCAST;
    case 7:  return SO_SNDBUF;
    case 8:  return SO_RCVBUF;
    case 9:  return SO_KEEPALIVE;
    case 10: return SO_OOBINLINE;
    case 13: return SO_LINGER;
    case 15: return SO_REUSEPORT;
    case 18: return SO_RCVLOWAT;
    case 19: return SO_SNDLOWAT;
    case 20: case 66: return SO_RCVTIMEO;   // 66 = SO_RCVTIMEO_NEW
    case 21: case 67: return SO_SNDTIMEO;   // 67 = SO_SNDTIMEO_NEW
    default: return opt;                    // IPPROTO_* options: numbers agree
    }
}
static int kl_setsockopt(int fd, int level, int opt, const void *val, socklen_t len) {
    if (level == 1) level = SOL_SOCKET;     // bionic SOL_SOCKET -> Darwin's
    int ropt = level == SOL_SOCKET ? kl_sock_optname(opt) : opt;
    int r = setsockopt(fd, level, ropt, val, len);
    if (r) fprintf(stderr, "  [sock] setsockopt(fd=%d level=%d opt=%d->%d) FAILED: %s\n",
                   fd, level, opt, ropt, strerror(errno));
    return r;
}
static int kl_getsockopt(int fd, int level, int opt, void *val, socklen_t *len) {
    if (level == 1) level = SOL_SOCKET;
    int ropt = level == SOL_SOCKET ? kl_sock_optname(opt) : opt;
    return getsockopt(fd, level, ropt, val, len);
}

// KL_TRACE_NET=1: name resolution and connect attempts, with durations. The
// loading bar creeps for 20k+ frames with zero asset I/O — sequential network
// timeouts are the remaining clock that could pace it.
static int kl_net_trace(void) {
    static int on = -1;
    if (on < 0) on = getenv("KL_TRACE_NET") != NULL;
    return on;
}
// KL_NET_OFFLINE=1: present a headset with no network at all — a valid real
// config, and the honest one here: anything certificate/TEE-backed (Oculus
// platform, leaderboards) can never work, and failing fast keeps the guest on
// its offline path instead of paying TCP timeouts to unreachable endpoints.
static int kl_net_offline(void) {
    static int off = -1;
    if (off < 0) off = getenv("KL_NET_OFFLINE") != NULL;
    return off;
}
static void kl_sa_to_guest(struct sockaddr *sa, socklen_t *len);
static int kl_getaddrinfo(const char *node, const char *serv,
                          const void *hints, void *res) {
    if (kl_net_offline()) {
        if (kl_net_trace())
            fprintf(stderr, "  [net] getaddrinfo(\"%s\") -> EAI_NONAME (offline)\n",
                    node ? node : "(null)");
        return EAI_NONAME;
    }
    struct addrinfo *hres = NULL;
    int r = getaddrinfo(node, serv, hints, &hres);
    if (kl_net_trace())
        fprintf(stderr, "  [net] getaddrinfo(\"%s\",\"%s\") -> %d\n",
                node ? node : "(null)", serv ? serv : "", r);
    // bionic and Darwin share the addrinfo layout (canonname before addr —
    // proven by the Ping callsite, which reads ai_addr at +0x20); only the
    // sockaddr inside differs (Darwin's sa_len). Convert each in place and
    // hand the host's own list over; kl_freeaddrinfo is plain freeaddrinfo.
    for (struct addrinfo *a = hres; a; a = a->ai_next) {
        if (a->ai_addr && a->ai_addrlen) {
            socklen_t gl = (socklen_t)a->ai_addrlen;
            kl_sa_to_guest(a->ai_addr, &gl);
        }
    }
    *(void **)res = hres;
    return r;
}
// sockaddr layout differs past the family: bionic has no sa_len byte, Darwin
// leads with it — so a guest-built sockaddr_in reads as family 0 here, and
// every forwarded connect/sendto failed EINVAL (the "Ping" storm is one).
// Convert through a local: bionic family (u16 at 0) -> Darwin sa_len+sa_family,
// and AF_INET6 10 -> 30 on top of that. Port/addr offsets are identical.
static int kl_sa_to_host(struct sockaddr_storage *dst, const struct sockaddr *sa,
                         socklen_t len) {
    if (!sa || len > sizeof *dst) return -1;
    memcpy(dst, sa, len);
    unsigned fam = *(const uint16_t *)sa;
    if (fam == 10) fam = AF_INET6;
    memmove((uint8_t *)dst + 2, (const uint8_t *)sa + 2, (size_t)len - 2);
    dst->ss_family = (uint8_t)fam;
    dst->ss_len = (uint8_t)len;
    return 0;
}
static void kl_sa_to_guest(struct sockaddr *sa, socklen_t *len) {
    if (!sa || !len) return;
    unsigned fam = sa->sa_family;
    if (fam == AF_INET6) fam = 10;
    memmove((uint8_t *)sa + 2, (const uint8_t *)sa + 2, (size_t)*len - 2);
    *(uint16_t *)sa = (uint16_t)fam;
}
static int kl_socket(int domain, int type, int protocol) {
    // bionic AF_INET6=10 -> Darwin 30; SOCK_* types agree.
    if (domain == 10) domain = AF_INET6;
    int fd = socket(domain, type, protocol);
    if (kl_net_trace())
        fprintf(stderr, "  [net] socket(dom=%d type=%d proto=%d) -> fd %d%s\n",
                domain, type, protocol, fd, fd < 0 ? strerror(errno) : "");
    return fd;
}
static int kl_connect(int fd, const struct sockaddr *sa, socklen_t len) {
    if (kl_net_offline()) {
        if (kl_net_trace()) fprintf(stderr, "  [net] connect() -> ENETUNREACH (offline)\n");
        errno = ENETUNREACH;
        return -1;
    }
    char host[80] = "?";
    if (!sa) {
        static int said;
        if (!said++) fprintf(stderr, "  [net] connect(fd=%d, NULL, %d) — caller pc=%p\n",
                             fd, (int)len, __builtin_return_address(0));
    }
    struct sockaddr_storage hs;
    if (kl_sa_to_host(&hs, sa, len) == 0) {
        sa = (struct sockaddr *)&hs;
        if (hs.ss_family == AF_INET) {
            const struct sockaddr_in *in = (const struct sockaddr_in *)&hs;
            inet_ntop(AF_INET, &in->sin_addr, host, sizeof host);
            char *hp = host + strlen(host);
            snprintf(hp, sizeof host - (hp - host), ":%d", ntohs(in->sin_port));
        } else if (hs.ss_family == AF_INET6) {
            const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)&hs;
            host[0] = '[';
            inet_ntop(AF_INET6, &in6->sin6_addr, host + 1, sizeof host - 20);
            char *hp = host + strlen(host);
            snprintf(hp, sizeof host - (hp - host), "]:%d", ntohs(in6->sin6_port));
        }
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = connect(fd, sa, len);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (kl_net_trace())
        fprintf(stderr, "  [net] connect(%s) -> %d (%s) in %.2fs [fam=%d len=%d]\n", host, r,
                r ? strerror(errno) : "ok",
                (double)(t1.tv_sec - t0.tv_sec) + 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec),
                sa ? ((const struct sockaddr_storage *)sa)->ss_family : -1, (int)len);
    return r;
}
// The other sockaddr carriers, same conversion. msg_name inside msghdr is
// left alone until a guest proves it uses sendmsg/recvmsg with an address.
static int kl_bind(int fd, const struct sockaddr *sa, socklen_t len) {
    struct sockaddr_storage hs;
    if (kl_sa_to_host(&hs, sa, len) == 0) { sa = (struct sockaddr *)&hs; }
    return bind(fd, sa, len);
}
static ssize_t kl_sendto(int fd, const void *buf, size_t n, int flags,
                         const struct sockaddr *sa, socklen_t len) {
    struct sockaddr_storage hs;
    if (sa && kl_sa_to_host(&hs, sa, len) == 0) { sa = (struct sockaddr *)&hs; }
    return sendto(fd, buf, n, flags, sa, len);
}
static int kl_accept(int fd, struct sockaddr *sa, socklen_t *len) {
    int r = accept(fd, sa, len);
    if (r >= 0) kl_sa_to_guest(sa, len);
    return r;
}
static ssize_t kl_recvfrom(int fd, void *buf, size_t n, int flags,
                           struct sockaddr *sa, socklen_t *len) {
    ssize_t r = recvfrom(fd, buf, n, flags, sa, len);
    if (r >= 0) kl_sa_to_guest(sa, len);
    return r;
}
static int kl_getpeername(int fd, struct sockaddr *sa, socklen_t *len) {
    int r = getpeername(fd, sa, len);
    if (r == 0) kl_sa_to_guest(sa, len);
    return r;
}
static int kl_getsockname(int fd, struct sockaddr *sa, socklen_t *len) {
    int r = getsockname(fd, sa, len);
    if (r == 0) kl_sa_to_guest(sa, len);
    return r;
}
// Same throughput meter as kl_fread, for the raw-read path. KL_TRACE_IO=1.
static ssize_t kl_read(int fd, void *buf, size_t n) {
    ssize_t r = read(fd, buf, n);
    static int on = -1;
    if (on < 0) on = getenv("KL_TRACE_IO") != NULL;
    if (on && r > 0) {
        static _Atomic unsigned long long bytes;
        static _Atomic time_t last;
        unsigned long long nb = atomic_fetch_add(&bytes, (unsigned long long)r) + (unsigned long long)r;
        time_t now = time(NULL), prev = atomic_load(&last);
        if (now != prev && atomic_compare_exchange_strong(&last, &prev, now))
            fprintf(stderr, "  [io] read total %.1f MB\n", (double)nb / 1048576.0);
    }
    return r;
}

// The loader thread was caught polling: guest code -> usleep, 99% of its
// samples. KL_TRACE_SLEEP=1 histograms the requested durations once a second
// to say what the poll period actually is.
static int kl_usleep(unsigned usec) {
    static int on = -1;
    if (on < 0) on = getenv("KL_TRACE_SLEEP") != NULL;
    if (on) {
        static _Atomic unsigned long long cnt, usec_sum, usec_max;
        static _Atomic time_t last;
        unsigned long long c = atomic_fetch_add(&cnt, 1) + 1;
        unsigned long long s = atomic_fetch_add(&usec_sum, usec) + usec;
        unsigned long long m = atomic_load(&usec_max);
        while (usec > m && !atomic_compare_exchange_strong(&usec_max, &m, usec)) {}
        time_t now = time(NULL), prev = atomic_load(&last);
        if (now != prev && atomic_compare_exchange_strong(&last, &prev, now))
            fprintf(stderr, "  [sleep] usleep x%llu, mean %llu us, max %llu us\n",
                    c, s / (c ? c : 1), (unsigned long long)atomic_load(&usec_max));
    }
    // KL_USLEEP_CAP=<usec>: clamp the guest's sleep. The loader thread polls
    // with ~5ms usleeps; if that poll IS the loading pace, capping it to
    // 50us moves the bottleneck somewhere measurable.
    static int cap = -1;
    if (cap < 0) {
        const char *e = getenv("KL_USLEEP_CAP");
        cap = e ? atoi(e) : 0;
    }
    if (cap > 0 && (int)usec > cap) usec = (unsigned)cap;
    return usleep(usec);
}

// ---------- guest va_list consumers that live here ----------
static int kl_vfprintf(void *f, const char *fmt, void *gva) {
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, (kl_va *)gva, m, sizeof m, KL_VA_PRINTF) == (size_t)-1) return -1;
    return vfprintf(kl_host_file(f), fmt, (va_list)m);
}
static int kl_vsnprintf(char *b, size_t n, const char *fmt, void *gva) {
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, (kl_va *)gva, m, sizeof m, KL_VA_PRINTF) == (size_t)-1) return -1;
    return vsnprintf(b, n, fmt, (va_list)m);
}
static int kl_vasprintf(char **o, const char *fmt, void *gva) {
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, (kl_va *)gva, m, sizeof m, KL_VA_PRINTF) == (size_t)-1) return -1;
    return vasprintf(o, fmt, (va_list)m);
}

// ---------- fortify (_chk) variants ----------
static void *kl_memcpy_chk(void *d, const void *s, size_t n, size_t dl) {
    if (n > dl) die("__memcpy_chk overflow"); return memcpy(d, s, n); }
static void *kl_memset_chk(void *d, int c, size_t n, size_t dl) {
    if (n > dl) die("__memset_chk overflow"); return memset(d, c, n); }
static char *kl_strcpy_chk(char *d, const char *s, size_t dl) {
    if (strlen(s) + 1 > dl) die("__strcpy_chk overflow"); return strcpy(d, s); }
static size_t kl_strlen_chk(const char *s, size_t dl) {
    size_t n = strlen(s);
    if (n >= dl) die("__strlen_chk overflow");
    return n;
}
static char *kl_strchr_chk(char *s, int ch, size_t dl) {
    kl_strlen_chk(s, dl);                  // proves the object is dl bytes
    return strchr(s, ch);
}
// bionic: __vsnprintf_chk(dest, supplied_size, flags, compiler_size, fmt, va).
static int kl_vsnprintf_chk(char *d, size_t n, int flags, size_t dl,
                            const char *fmt, void *gva) {
    (void)flags;
    if (n > dl) die("__vsnprintf_chk overflow");
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, (kl_va *)gva, m, sizeof m, KL_VA_PRINTF) == (size_t)-1) return -1;
    return vsnprintf(d, n, fmt, (va_list)m);
}
static size_t kl_malloc_usable_size(const void *p) { return malloc_size(p); }
// execv would replace the HOST process with an Android binary — the one
// outcome worse than refusing. execl already answers ENOSYS; match it.
static int kl_execv(const char *p, char *const argv[]) {
    (void)p; (void)argv;
    static int warned;
    if (!warned++) fprintf(stderr, "  [klepton] execv() refused — ENOSYS\n");
    errno = ENOSYS;
    return -1;
}

// ---------- small bionic-isms ----------
static void kl_set_abort_message(const char *m) { fprintf(stderr, "[guest abort] %s\n", m ? m : ""); }
// Real implementation in kl_dl.c — it owns the image registry. This is the
// guest unwinder's FDE lookup, not a bionic nicety; see the comment there.
extern int kl_dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *d);
static void kl_openlog(const char *a, int b, int c) { (void)a; (void)b; (void)c; }
static void kl_closelog(void) {}
static int  kl_cxa_atexit(void (*f)(void *), void *a, void *d) { (void)f;(void)a;(void)d; return 0; }
static void kl_cxa_finalize(void *d) { (void)d; }
static void kl_android_log_write(int p, const char *t, const char *m) {
    fprintf(stderr, "[%d/%s] %s\n", p, t ? t : "", m ? m : ""); }

// ---------- externs ----------
#define X(n) extern int n(void);
X(klv_printf) X(klv_fprintf) X(klv_sprintf) X(klv_snprintf) X(klv_asprintf)
X(klv_dprintf) X(klv_syslog) X(klv_android_log_print) X(klv_sscanf) X(klv_fscanf)
X(klv_open) X(klv_fcntl) X(klv_ioctl) X(klv_strtold) X(klv_wcstold) X(klv_strtold_l)
X(klb_errno) X(klb_gettid) X(klb_sysprop_find) X(klb_sysprop_get) X(klb_sysprop_read)
X(klb_prctl) X(klb_sched_getaffinity) X(klb_sched_setaffinity)
X(klb_stat) X(klb_lstat) X(klb_fstat) X(klb_statfs) X(klb_uname) X(klb_sigaction)
X(klb_opendir) X(klb_readdir) X(klb_closedir)
X(klb_FD_ISSET_chk) X(klb_FD_SET_chk) X(klb_ctype_mb_cur_max) X(klb_lseek64)
X(klb_sysconf) X(klb_fopen) X(klb_access) X(klb_mkdir) X(klb_unlink) X(klb_rename)
X(klh_android_log_print)
X(klb_getpwuid) X(klb_getpwuid_r) X(klb_execl) X(klb_syscall) X(klb_swprintf)
X(klb_vprintf) X(klb_vsscanf) X(klb_memrchr) X(klb_memalign)
X(klb_mmap) X(klb_madvise)
X(klb_pthread_mutex_init) X(klb_pthread_mutex_lock) X(klb_pthread_mutex_unlock)
X(klb_pthread_mutex_trylock) X(klb_pthread_mutex_destroy)
X(klb_pthread_mutexattr_init) X(klb_pthread_mutexattr_destroy) X(klb_pthread_mutexattr_settype)
X(klb_pthread_cond_init) X(klb_pthread_cond_destroy) X(klb_pthread_cond_signal)
X(klb_pthread_cond_broadcast) X(klb_pthread_cond_wait) X(klb_pthread_cond_timedwait)
X(klb_pthread_condattr_init) X(klb_pthread_condattr_destroy) X(klb_pthread_condattr_setclock)
X(klb_pthread_rwlock_init) X(klb_pthread_rwlock_destroy) X(klb_pthread_rwlock_rdlock)
X(klb_pthread_rwlock_wrlock) X(klb_pthread_rwlock_unlock)
X(klb_pthread_attr_init) X(klb_pthread_attr_destroy) X(klb_pthread_attr_setstacksize)
X(klb_pthread_attr_setdetachstate) X(klb_pthread_attr_getstack) X(klb_pthread_getattr_np)
X(klb_pthread_key_create) X(klb_pthread_key_delete) X(klb_pthread_getspecific)
X(klb_pthread_setspecific) X(klb_pthread_once)
X(klb_pthread_create) X(klb_pthread_join) X(klb_pthread_detach) X(klb_pthread_exit)
X(klb_pthread_self) X(klb_pthread_equal) X(klb_pthread_kill) X(klb_pthread_sigmask)
X(klb_sigprocmask)
X(klb_pthread_setname_np) X(klb_pthread_atfork)
X(klb_sem_init) X(klb_sem_destroy) X(klb_sem_post) X(klb_sem_wait)
X(klb_sem_timedwait) X(klb_sem_getvalue) X(klb_sem_trywait)
X(klb_sem_open) X(klb_sem_close)
X(klb_pthread_getschedparam) X(klb_pthread_setschedparam)
X(klb_dlopen) X(klb_dlsym) X(klb_dlclose) X(klb_dlerror) X(klb_dladdr)
X(klb_clock_gettime) X(klb_clock_getres) X(klb_gettimeofday)
#undef X
extern char **environ;

// ---------- table ----------
typedef struct { const char *name; void *fn; } kl_entry;
#define E(n, f)   { n, (void *)(f) }
#define KL_FWD(n) { #n, (void *)(n) },

static const kl_entry g_shim[] = {
#include "kl_libc_table.h"

    // stdio needing guest-FILE translation
    E("__sF", g_sF),
    E("fputc", kl_fputc), E("fputs", kl_fputs), E("fwrite", kl_fwrite), E("fread", kl_fread),
    E("fclose", kl_fclose), E("fflush", kl_fflush), E("fgets", kl_fgets), E("getc", kl_getc_),
    E("feof", kl_feof), E("ferror", kl_ferror), E("clearerr", kl_clearerr),
    E("fseek", kl_fseek), E("fseeko", kl_fseeko), E("ftell", kl_ftell), E("ftello", kl_ftello),
    E("setvbuf", kl_setvbuf),

    // AAPCS64 variadic thunks
    E("printf", klv_printf), E("fprintf", klv_fprintf), E("sprintf", klv_sprintf),
    E("snprintf", klv_snprintf), E("asprintf", klv_asprintf), E("dprintf", klv_dprintf),
    E("sscanf", klv_sscanf), E("fscanf", klv_fscanf), E("syslog", klv_syslog),
    E("__android_log_print", klv_android_log_print),
    E("open", klv_open), E("fcntl", klv_fcntl), E("ioctl", klv_ioctl),
    E("setsockopt", kl_setsockopt), E("getsockopt", kl_getsockopt),
    E("read", kl_read), E("usleep", kl_usleep),
    E("getaddrinfo", kl_getaddrinfo), E("connect", kl_connect),
    E("socket", kl_socket),
    E("bind", kl_bind), E("sendto", kl_sendto), E("accept", kl_accept),
    E("recvfrom", kl_recvfrom), E("getpeername", kl_getpeername),
    E("getsockname", kl_getsockname),
    E("strtold", klv_strtold), E("wcstold", klv_wcstold), E("strtold_l", klv_strtold_l),
    E("swprintf", klb_swprintf), E("execl", klb_execl), E("syscall", klb_syscall),

    // guest va_list consumers
    E("vfprintf", kl_vfprintf), E("vsnprintf", kl_vsnprintf), E("vasprintf", kl_vasprintf),
    E("vprintf", klb_vprintf), E("vsscanf", klb_vsscanf),

    // fortify
    E("__memcpy_chk", kl_memcpy_chk), E("__memset_chk", kl_memset_chk),
    E("__strcpy_chk", kl_strcpy_chk), E("__stack_chk_fail", kl_stack_chk_fail),
    E("__FD_ISSET_chk", klb_FD_ISSET_chk), E("__FD_SET_chk", klb_FD_SET_chk),
    E("__strlen_chk", kl_strlen_chk), E("__strchr_chk", kl_strchr_chk),
    E("__vsnprintf_chk", kl_vsnprintf_chk),

    // divergent layouts / Android-only
    E("__errno", klb_errno), E("environ", &environ), E("gettid", klb_gettid),
    E("stat", klb_stat), E("lstat", klb_lstat), E("fstat", klb_fstat), E("statfs", klb_statfs),
    E("uname", klb_uname), E("sigaction", klb_sigaction),
    E("sysconf", klb_sysconf),
    E("fopen", klb_fopen), E("access", klb_access),
    E("mkdir", klb_mkdir), E("unlink", klb_unlink), E("rename", klb_rename),
    E("opendir", klb_opendir), E("readdir", klb_readdir), E("closedir", klb_closedir),
    E("lseek64", klb_lseek64), E("__ctype_get_mb_cur_max", klb_ctype_mb_cur_max),
    E("getpwuid", klb_getpwuid), E("getpwuid_r", klb_getpwuid_r),
    E("prctl", klb_prctl),
    E("memrchr", klb_memrchr), E("memalign", klb_memalign),
    E("mmap", klb_mmap), E("madvise", klb_madvise),
    E("sched_getaffinity", klb_sched_getaffinity), E("sched_setaffinity", klb_sched_setaffinity),
    E("__system_property_find", klb_sysprop_find),
    E("__system_property_get", klb_sysprop_get),
    E("__system_property_read", klb_sysprop_read),

    // setjmp/longjmp forward directly: bionic's jmp_buf (256B) is LARGER than
    // Darwin's (192B), so the guest's buffer is safe, and registering the host
    // function itself avoids interposing a stack frame that longjmp would destroy.
    E("setjmp", setjmp), E("longjmp", longjmp),

    // pthread / sem (bionic layouts)
    E("pthread_mutex_init", klb_pthread_mutex_init),
    E("pthread_mutex_lock", klb_pthread_mutex_lock),
    E("pthread_mutex_unlock", klb_pthread_mutex_unlock),
    E("pthread_mutex_trylock", klb_pthread_mutex_trylock),
    E("pthread_mutex_destroy", klb_pthread_mutex_destroy),
    E("pthread_mutexattr_init", klb_pthread_mutexattr_init),
    E("pthread_mutexattr_destroy", klb_pthread_mutexattr_destroy),
    E("pthread_mutexattr_settype", klb_pthread_mutexattr_settype),
    E("pthread_cond_init", klb_pthread_cond_init),
    E("pthread_cond_destroy", klb_pthread_cond_destroy),
    E("pthread_cond_signal", klb_pthread_cond_signal),
    E("pthread_cond_broadcast", klb_pthread_cond_broadcast),
    E("pthread_cond_wait", klb_pthread_cond_wait),
    E("pthread_cond_timedwait", klb_pthread_cond_timedwait),
    E("pthread_condattr_init", klb_pthread_condattr_init),
    E("pthread_condattr_destroy", klb_pthread_condattr_destroy),
    E("pthread_condattr_setclock", klb_pthread_condattr_setclock),
    E("pthread_rwlock_init", klb_pthread_rwlock_init),
    E("pthread_rwlock_destroy", klb_pthread_rwlock_destroy),
    E("pthread_rwlock_rdlock", klb_pthread_rwlock_rdlock),
    E("pthread_rwlock_wrlock", klb_pthread_rwlock_wrlock),
    E("pthread_rwlock_unlock", klb_pthread_rwlock_unlock),
    E("pthread_attr_init", klb_pthread_attr_init),
    E("pthread_attr_destroy", klb_pthread_attr_destroy),
    E("pthread_attr_setstacksize", klb_pthread_attr_setstacksize),
    E("pthread_attr_setdetachstate", klb_pthread_attr_setdetachstate),
    E("pthread_attr_getstack", klb_pthread_attr_getstack),
    E("pthread_getattr_np", klb_pthread_getattr_np),
    E("pthread_key_create", klb_pthread_key_create),
    E("pthread_key_delete", klb_pthread_key_delete),
    E("pthread_getspecific", klb_pthread_getspecific),
    E("pthread_setspecific", klb_pthread_setspecific),
    E("pthread_once", klb_pthread_once),
    E("pthread_create", klb_pthread_create), E("pthread_join", klb_pthread_join),
    E("pthread_detach", klb_pthread_detach), E("pthread_exit", klb_pthread_exit),
    E("pthread_self", klb_pthread_self), E("pthread_equal", klb_pthread_equal),
    E("pthread_kill", klb_pthread_kill), E("pthread_sigmask", klb_pthread_sigmask),
    E("sigprocmask", klb_sigprocmask),
    E("pthread_setname_np", klb_pthread_setname_np), E("pthread_atfork", klb_pthread_atfork),
    E("sem_init", klb_sem_init), E("sem_destroy", klb_sem_destroy),
    E("sem_post", klb_sem_post), E("sem_wait", klb_sem_wait),
    E("sem_timedwait", klb_sem_timedwait), E("sem_getvalue", klb_sem_getvalue),
    E("sem_trywait", klb_sem_trywait),
    E("sem_open", klb_sem_open), E("sem_close", klb_sem_close),
    E("pthread_getschedparam", klb_pthread_getschedparam),
    E("pthread_setschedparam", klb_pthread_setschedparam),

    // dl
    E("dlopen", klb_dlopen), E("dlsym", klb_dlsym), E("dlclose", klb_dlclose),
    E("dlerror", klb_dlerror), E("dladdr", klb_dladdr),
    E("dl_iterate_phdr", kl_dl_iterate_phdr),

    // logging / misc
    E("__android_log_write", kl_android_log_write),
    // __android_log_vprint(prio, tag, fmt, va_list) — NOT vfprintf's shape. It
    // was bound to kl_vfprintf once, which put `prio` in the FILE* slot and
    // segfaulted in flockfile the first time Unity logged through it. The
    // variadic handler already has exactly this signature, because an AAPCS64
    // va_list is a 32-byte descriptor and so arrives by reference — which is
    // what a kl_va * is.
    E("__android_log_vprint", klh_android_log_print),
    E("android_set_abort_message", kl_set_abort_message),
    E("openlog", kl_openlog), E("closelog", kl_closelog),
    E("__cxa_atexit", kl_cxa_atexit), E("__cxa_finalize", kl_cxa_finalize),

    // Steam Link (SDL3) additions — plain forwards plus the small wrappers above
    E("getentropy", getentropy), E("tzset", tzset), E("strncasecmp", strncasecmp),
    E("malloc_usable_size", kl_malloc_usable_size), E("execv", kl_execv),

    E("__klepton_unresolved", kl_unresolved),
};

void *kl_shim_lookup(const char *name) {
    for (size_t i = 0; i < sizeof g_shim / sizeof g_shim[0]; i++)
        if (strcmp(g_shim[i].name, name) == 0) return g_shim[i].fn;
    // Tier 4: the NDK surface (M3) lives in its own table — it is a different
    // API family with its own lifetimes, not more bionic libc.
    void *ndk = kl_ndk_lookup(name);
    if (ndk) return ndk;
    // Tier 5: EGL (M5). Same reasoning again, and it is the door to GLES —
    // eglGetProcAddress hands out the rest of the graphics surface.
    return kl_egl_lookup(name);
}
