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
#include <grp.h>        // getgrgid — Qt6Core's QFileSystemEngine asks for it
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
#include <sys/sem.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include "klepton.h"
#include "kl_env.h"
#include "kl_va.h"
#include "kl_ndk.h"
#include "kl_egl.h"
#include "kl_opensl.h"
#include "kl_openxr.h"
#include "kl_mediandk.h"
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
// The guest's -fstack-protector epilogue calls this when the canary it read at
// entry no longer matches. Naming the caller is most of the diagnosis and costs
// nothing: bionic keeps the canary in TLS slot 5, which we squat (trap 1), so
// "stack smashing detected" here has two quite different causes — a genuine
// overflow of a guest buffer (often one WE filled), or slot 5 changing under a
// frame that was holding a copy. Which function it was usually separates them,
// and a bare one-line abort separates nothing.
__attribute__((noreturn)) static void kl_stack_chk_fail(void) {
    char msg[256];
    void *ra = __builtin_return_address(0);
    size_t off = 0;
    const char *img = kl_addr_image(ra, &off);
    if (img) snprintf(msg, sizeof msg, "stack smashing detected in %s+0x%zx", img, off);
    else     snprintf(msg, sizeof msg, "stack smashing detected, called from %p", ra);
    die(msg);
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
    if (on < 0) on = kl_env_on("KL_TRACE_IO", 0);
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
    default: return opt;
    }
}

// ...and the IPPROTO_IP / IPPROTO_IPV6 levels diverge too, which an earlier
// comment here denied. They are not a harmless subset: the numbers OVERLAP, so
// a forwarded option lands on a DIFFERENT real option rather than on none.
// Linux IP_TOS is 1 and Darwin's 1 is IP_OPTIONS; Linux IP_TTL is 2 and
// Darwin's 2 is IP_HDRINCL. Setting a TTL of 4 would ask Darwin to enable raw
// IP headers — a call that SUCCEEDS and means something else entirely, which is
// strictly worse than the EINVAL that made trap 4 visible.
//
// Found through Steam Link: IPV6_V6ONLY is 26 on Linux and 27 on Darwin, so its
// dual-stack UDP socket failed "Protocol not available" on every start. The
// multicast entries are the ones that matter for LAN host discovery.
//
// Only well-established pairs are translated. Anything unrecognised is passed
// through and named once — guessing is what this whole comment is about.
static int kl_ip_optname(int opt) {
    switch (opt) {                          // bionic IPPROTO_IP numbers
    case 1:  return IP_TOS;
    case 2:  return IP_TTL;
    case 3:  return IP_HDRINCL;
    case 4:  return IP_OPTIONS;
    case 6:  return IP_RECVOPTS;
    case 7:  return IP_RETOPTS;
    case 32: return IP_MULTICAST_IF;
    case 33: return IP_MULTICAST_TTL;
    case 34: return IP_MULTICAST_LOOP;
    case 35: return IP_ADD_MEMBERSHIP;
    case 36: return IP_DROP_MEMBERSHIP;
    case 8:  return IP_PKTINFO;
    default: return opt;
    }
}
static int kl_ipv6_optname(int opt) {
    switch (opt) {                          // bionic IPPROTO_IPV6 numbers
    case 16: return IPV6_UNICAST_HOPS;
    case 17: return IPV6_MULTICAST_IF;
    case 18: return IPV6_MULTICAST_HOPS;
    case 19: return IPV6_MULTICAST_LOOP;
    case 20: return IPV6_JOIN_GROUP;        // Linux IPV6_ADD_MEMBERSHIP
    case 21: return IPV6_LEAVE_GROUP;       // Linux IPV6_DROP_MEMBERSHIP
    case 26: return IPV6_V6ONLY;
    // Darwin only declares these two under __APPLE_USE_RFC_3542, which this
    // file does not set (it changes other IPv6 semantics wholesale). The wire
    // numbers are stable ABI, so spell them rather than pass the Linux ones
    // through onto whatever Darwin option happens to share the number.
    case 49: return 61;                     // IPV6_RECVPKTINFO
    case 50: return 46;                     // IPV6_PKTINFO
    case 67: return IPV6_TCLASS;
    default: return opt;
    }
}
// ---------- interface 0 means different things on the two kernels ----------
// Linux takes ipv6mr_interface == 0 (and sin6_scope_id == 0) as "you pick";
// Darwin takes it literally and rejects the operation, because a link-local
// IPv6 destination is meaningless without a link. Steam Link's discovery
// socket joins ff02::1 with interface 0 and got EADDRNOTAVAIL — the guest
// printed "Couldn't set IPV6_ADD_MEMBERSHIP on discovery network socket" —
// and its later sendto to [ff02::1]:27036 failed ENETUNREACH for the same
// reason. So the choice Linux would have made is made here instead: the first
// up, multicast-capable, non-loopback interface.
static unsigned kl_default_mcast_if(void) {
    static unsigned idx; static int done;
    if (done) return idx;
    done = 1;
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) return 0;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || !(p->ifa_flags & IFF_MULTICAST)) continue;
        if (p->ifa_flags & IFF_LOOPBACK) continue;
        unsigned i = if_nametoindex(p->ifa_name);
        if (i) { idx = i; break; }
    }
    freeifaddrs(ifa);
    if (!idx) fprintf(stderr, "  [klepton] no multicast-capable interface found; "
                              "IPv6 multicast will stay unavailable to the guest\n");
    return idx;
}

// One place, so set and get cannot drift into disagreeing about an option.
static int kl_sock_level_opt(int *level, int opt) {
    if (*level == 1) { *level = SOL_SOCKET; return kl_sock_optname(opt); }
    if (*level == SOL_SOCKET)  return kl_sock_optname(opt);
    if (*level == IPPROTO_IP)  return kl_ip_optname(opt);
    if (*level == IPPROTO_IPV6) return kl_ipv6_optname(opt);
    return opt;                             // IPPROTO_TCP: NODELAY agrees at 1
}
static int kl_setsockopt(int fd, int level, int opt, const void *val, socklen_t len) {
    int glevel = level;
    int ropt = kl_sock_level_opt(&level, opt);
    // struct ipv6_mreq matches on both platforms (in6_addr then a u32 index),
    // so only the zero index has to be filled in; the guest's buffer is const
    // and stays untouched.
    struct ipv6_mreq mreq;
    if (glevel == IPPROTO_IPV6 && (opt == 20 || opt == 21) &&
        val && len == sizeof mreq) {
        memcpy(&mreq, val, sizeof mreq);
        if (mreq.ipv6mr_interface == 0) {
            mreq.ipv6mr_interface = kl_default_mcast_if();
            val = &mreq;
        }
    }
    int r = setsockopt(fd, level, ropt, val, len);
    if (r) fprintf(stderr, "  [sock] setsockopt(fd=%d level=%d opt=%d->%d) FAILED: %s\n",
                   fd, level, opt, ropt, strerror(errno));
    return r;
}
static int kl_getsockopt(int fd, int level, int opt, void *val, socklen_t *len) {
    int ropt = kl_sock_level_opt(&level, opt);
    return getsockopt(fd, level, ropt, val, len);
}

// KL_TRACE_NET=1: name resolution and connect attempts, with durations. The
// loading bar creeps for 20k+ frames with zero asset I/O — sequential network
// timeouts are the remaining clock that could pace it.
static int kl_net_trace(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_TRACE_NET", 0);
    return on;
}
// KL_NET_OFFLINE=1: present a headset with no network at all — a valid real
// config, and the honest one here: anything certificate/TEE-backed (Oculus
// platform, leaderboards) can never work, and failing fast keeps the guest on
// its offline path instead of paying TCP timeouts to unreachable endpoints.
static int kl_net_offline(void) {
    static int off = -1;
    if (off < 0) off = kl_env_on("KL_NET_OFFLINE", 0);
    return off;
}
// KL_TRACE_NET_HEX=<n>: the first n bytes of every payload, alongside the
// [net] line. A discovery probe and its own broadcast echo are the same length,
// and a TLS record is identified by its first five bytes — neither question can
// be answered from counts.
static int kl_net_hex(void) {
    static int n = -1;
    if (n < 0) n = kl_env_int("KL_TRACE_NET_HEX", 0);
    return n;
}
static void kl_hexdump(const char *tag, const void *buf, ssize_t n) {
    int lim = kl_net_hex();
    if (lim <= 0 || n <= 0 || !buf) return;
    if (n > lim) n = lim;
    const uint8_t *p = buf;
    fprintf(stderr, "       %s ", tag);
    for (ssize_t i = 0; i < n; i++) fprintf(stderr, "%02x", p[i]);
    fprintf(stderr, "  |");
    for (ssize_t i = 0; i < n; i++) fputc(p[i] >= 0x20 && p[i] < 0x7f ? p[i] : '.', stderr);
    fprintf(stderr, "|\n");
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
    // The other half of the interface-0 divergence: a link-local IPv6
    // destination (ff02::/16 multicast, fe80::/10 unicast) carries its link in
    // sin6_scope_id, and Linux fills a zero in from the routing table where
    // Darwin returns ENETUNREACH. Only link-local addresses are touched —
    // a scope on a global address would be wrong, not merely unhelpful.
    if (fam == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)dst;
        const uint8_t *a = in6->sin6_addr.s6_addr;
        int linklocal = (a[0] == 0xff && (a[1] & 0x0f) == 0x02) ||
                        (a[0] == 0xfe && (a[1] & 0xc0) == 0x80);
        if (linklocal && in6->sin6_scope_id == 0)
            in6->sin6_scope_id = kl_default_mcast_if();
    }
    return 0;
}
static void kl_sa_to_guest(struct sockaddr *sa, socklen_t *len) {
    if (!sa || !len || *len < 2) return;   // memmove below reads *len - 2
    unsigned fam = sa->sa_family;
    if (fam == AF_INET6) fam = 10;
    memmove((uint8_t *)sa + 2, (const uint8_t *)sa + 2, (size_t)*len - 2);
    *(uint16_t *)sa = (uint16_t)fam;
}
// A HOST sockaddr, printed. Shared by every traced call so one address never
// formats two ways.
static void kl_sa_fmt(char *out, size_t n, const struct sockaddr *sa) {
    if (!sa || !n) return;
    snprintf(out, n, "?");
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &in->sin_addr, ip, sizeof ip);
        snprintf(out, n, "%s:%d", ip, ntohs(in->sin_port));
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        char ip[INET6_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof ip);
        snprintf(out, n, "[%s]:%d", ip, ntohs(in6->sin6_port));
    }
}

// ---------- MSG_* flags: trap 16b's class, on the send/recv path ----------
// The four message-passing calls take a flag word, and the two platforms
// number it differently AND OVERLAPPINGLY, so a forwarded flag does not fail —
// it asks for a different thing:
//
//   Linux MSG_DONTWAIT 0x40   == Darwin MSG_WAITALL 0x40     (worst: a poll
//                                                             becomes a block)
//   Linux MSG_EOR      0x80   == Darwin MSG_DONTWAIT 0x80
//   Linux MSG_WAITALL  0x100  == Darwin MSG_EOF     0x100    (shuts the socket
//                                                             down on send)
//   Linux MSG_CTRUNC   0x8    == Darwin MSG_EOR     0x8
//   Linux MSG_TRUNC    0x20   == Darwin MSG_CTRUNC  0x20
//   Linux MSG_NOSIGNAL 0x4000 == Darwin MSG_RCVMORE 0x4000
//
// Only the bits with a real Darwin meaning are carried; MSG_NOSIGNAL is
// DROPPED and answered by SO_NOSIGPIPE on the fd instead (Darwin's equivalent
// is a socket option, not a per-call flag). Anything unrecognised is dropped
// and named once — passing it through is what this comment is about.
#define LX_MSG_OOB       0x1
#define LX_MSG_PEEK      0x2
#define LX_MSG_DONTROUTE 0x4
#define LX_MSG_CTRUNC    0x8
#define LX_MSG_TRUNC     0x20
#define LX_MSG_DONTWAIT  0x40
#define LX_MSG_EOR       0x80
#define LX_MSG_WAITALL   0x100
#define LX_MSG_CONFIRM   0x800
#define LX_MSG_NOSIGNAL  0x4000
#define LX_MSG_MORE      0x8000

static void kl_net_warn_once(const char *fn, long v) {
    static struct { const char *fn; long v; } seen[32]; static int n;
    for (int i = 0; i < n; i++) if (seen[i].fn == fn && seen[i].v == v) return;
    if (n < 32) seen[n++] = (typeof(seen[0])){fn, v};
    fprintf(stderr, "  [klepton] %s: unrecognised Linux flag/value 0x%lx — dropped "
                    "(add a translation rather than passing it through)\n", fn, v);
}

// Returns the Darwin flag word; *nosignal is set when the caller asked for
// MSG_NOSIGNAL, which the fd-level SO_NOSIGPIPE answers.
static int kl_msg_flags(int lx, int *nosignal) {
    int d = 0;
    if (nosignal) *nosignal = 0;
    if (lx & LX_MSG_OOB)       { d |= MSG_OOB;       lx &= ~LX_MSG_OOB; }
    if (lx & LX_MSG_PEEK)      { d |= MSG_PEEK;      lx &= ~LX_MSG_PEEK; }
    if (lx & LX_MSG_DONTROUTE) { d |= MSG_DONTROUTE; lx &= ~LX_MSG_DONTROUTE; }
    if (lx & LX_MSG_CTRUNC)    { d |= MSG_CTRUNC;    lx &= ~LX_MSG_CTRUNC; }
    if (lx & LX_MSG_TRUNC)     { d |= MSG_TRUNC;     lx &= ~LX_MSG_TRUNC; }
    if (lx & LX_MSG_DONTWAIT)  { d |= MSG_DONTWAIT;  lx &= ~LX_MSG_DONTWAIT; }
    if (lx & LX_MSG_EOR)       { d |= MSG_EOR;       lx &= ~LX_MSG_EOR; }
    if (lx & LX_MSG_WAITALL)   { d |= MSG_WAITALL;   lx &= ~LX_MSG_WAITALL; }
    if (lx & LX_MSG_NOSIGNAL)  { if (nosignal) *nosignal = 1; lx &= ~LX_MSG_NOSIGNAL; }
    // MSG_CONFIRM is a Linux ARP hint and MSG_MORE a corking hint; both are
    // advisory, and dropping them changes throughput, never correctness.
    lx &= ~(LX_MSG_CONFIRM | LX_MSG_MORE);
    if (lx) kl_net_warn_once("msg flags", lx);
    return d;
}
// Darwin has no per-call MSG_NOSIGNAL; the fd carries it. Set once per socket
// so a guest that wrote MSG_NOSIGNAL gets what it asked for — a failed write
// rather than a SIGPIPE that kills the process.
static void kl_no_sigpipe(int fd) {
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
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
        kl_sa_fmt(host, sizeof host, (const struct sockaddr *)&hs);
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
// A guest binding a fixed port is normally alone on the device. Here it is not:
// developing against a Steam host on the SAME Mac puts Steam's own UDP *:27036
// under Steam Link's [::]:27036, and because both ask for address reuse the
// bind SUCCEEDS. Every unicast reply to that port is then delivered to one
// socket of the two, and losing that draw reads as "no computers on the
// network" — a conclusion about the LAN drawn from a conflict on localhost.
//
// So it is detected and named. The probe is a throwaway socket bound WITHOUT
// reuse: if that fails EADDRINUSE the port already has an owner. It runs
// before the real bind, because afterwards our own socket is an owner and the
// probe could no longer tell the two apart.
static void kl_bind_conflict_check(int fd, const struct sockaddr *sa, socklen_t len) {
    int type = 0; socklen_t tl = sizeof type;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0) return;
    int port = 0;
    if (sa->sa_family == AF_INET)  port = ntohs(((const struct sockaddr_in *)sa)->sin_port);
    if (sa->sa_family == AF_INET6) port = ntohs(((const struct sockaddr_in6 *)sa)->sin6_port);
    if (!port) return;                          // ephemeral: nothing to collide with
    int busy = 0;
    int p = socket(sa->sa_family, type, 0);
    if (p >= 0) {
        busy = bind(p, sa, len) != 0 && errno == EADDRINUSE;
        close(p);
    }
    // ...and the same port in the OTHER family, when the guest is binding the
    // IPv6 wildcard. Darwin lets [::] and 0.0.0.0 hold one port side by side
    // even with no reuse flags, so a family-matched probe cannot see an IPv4
    // owner — but an incoming v4 packet still goes to the AF_INET socket as the
    // more specific match, which is precisely how the replies were lost. The
    // first version of this check probed only the guest's own family and was
    // therefore silent about the one case it exists for.
    if (!busy && sa->sa_family == AF_INET6 &&
        memcmp(&((const struct sockaddr_in6 *)sa)->sin6_addr, &in6addr_any,
               sizeof in6addr_any) == 0) {
        int v6o = 1; socklen_t vl = sizeof v6o;
        getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6o, &vl);
        if (!v6o) {                             // dual-stack: v4 traffic is in scope
            struct sockaddr_in v4;
            memset(&v4, 0, sizeof v4);
            v4.sin_len = sizeof v4;
            v4.sin_family = AF_INET;
            v4.sin_port = htons((uint16_t)port);
            v4.sin_addr.s_addr = INADDR_ANY;
            int q = socket(AF_INET, type, 0);
            if (q >= 0) {
                busy = bind(q, (struct sockaddr *)&v4, sizeof v4) != 0 && errno == EADDRINUSE;
                close(q);
            }
        }
    }
    if (!busy) return;
    char host[80] = "?";
    kl_sa_fmt(host, sizeof host, sa);
    static int said[8]; static unsigned nsaid;
    for (unsigned i = 0; i < nsaid; i++) if (said[i] == port) return;
    if (nsaid < 8) said[nsaid++] = port;
    fprintf(stderr, "  [klepton] bind(%s): ANOTHER PROCESS ON THIS HOST ALREADY HOLDS "
                    "THAT PORT. This bind still succeeds — Darwin lets the two "
                    "wildcards coexist — but an arriving IPv4 packet goes to the "
                    "AF_INET socket as the more specific match, so the guest will "
                    "silently miss replies.%s\n",
            host, port == 27036 ? "  Port 27036 is Steam's own discovery port: quit "
                                  "the local Steam client, or set KL_NET_BIND_REMAP="
                                  "27036:27136 to move the guest off it." : "");
}

// KL_NET_BIND_REMAP=<from>:<to>[,<from>:<to>...] — bind a listening port
// somewhere else. A HOST-DEVELOPMENT knob and off by default: on the headset
// the guest is alone and nothing collides. It exists because the collision
// above has no other way out — Steam's discovery responder answers to whatever
// source port asked (measured), so moving the guest off 27036 keeps discovery
// working while a Steam host runs on the same Mac. It does forfeit Steam's
// unsolicited broadcasts to 27036, which is why it is not the default.
static int kl_bind_remap(int port) {
    static char buf[128]; static int loaded;
    if (!loaded) {
        loaded = 1;
        const char *e = kl_env_str("KL_NET_BIND_REMAP", NULL);
        if (e) snprintf(buf, sizeof buf, "%s", e);
    }
    if (!buf[0]) return port;
    for (const char *p = buf; *p; ) {
        int from = 0, to = 0, n = 0;
        if (sscanf(p, "%d:%d%n", &from, &to, &n) == 2 && from == port) {
            static int said;
            if (!said++) fprintf(stderr, "  [klepton] KL_NET_BIND_REMAP: guest port "
                                         "%d bound as %d instead\n", from, to);
            return to;
        }
        const char *c = strchr(p, ',');
        if (!c) break;
        p = c + 1;
    }
    return port;
}

// The other sockaddr carriers, same conversion.
static int kl_bind(int fd, const struct sockaddr *sa, socklen_t len) {
    struct sockaddr_storage hs;
    char host[80] = "?";
    if (kl_sa_to_host(&hs, sa, len) == 0) {
        sa = (struct sockaddr *)&hs;
        if (hs.ss_family == AF_INET) {
            struct sockaddr_in *v4 = (struct sockaddr_in *)&hs;
            v4->sin_port = htons((uint16_t)kl_bind_remap(ntohs(v4->sin_port)));
        } else if (hs.ss_family == AF_INET6) {
            struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&hs;
            v6->sin6_port = htons((uint16_t)kl_bind_remap(ntohs(v6->sin6_port)));
        }
        kl_sa_fmt(host, sizeof host, (const struct sockaddr *)&hs);
        kl_bind_conflict_check(fd, sa, len);
    }
    int r = bind(fd, sa, len);
    if (kl_net_trace())
        fprintf(stderr, "  [net] bind(fd=%d, %s) -> %d (%s)\n",
                fd, host, r, r ? strerror(errno) : "ok");
    return r;
}
static ssize_t kl_sendto(int fd, const void *buf, size_t n, int flags,
                         const struct sockaddr *sa, socklen_t len) {
    struct sockaddr_storage hs;
    char host[80] = "-";
    if (sa && kl_sa_to_host(&hs, sa, len) == 0) {
        sa = (struct sockaddr *)&hs;
        kl_sa_fmt(host, sizeof host, (const struct sockaddr *)&hs);
    }
    int nosig = 0, dflags = kl_msg_flags(flags, &nosig);
    if (nosig) kl_no_sigpipe(fd);
    ssize_t r = sendto(fd, buf, n, dflags, sa, len);
    if (kl_net_trace())
        fprintf(stderr, "  [net] sendto(fd=%d, %zu B, flags=0x%x->0x%x, %s) -> %zd%s\n",
                fd, n, flags, dflags, host, r, r < 0 ? strerror(errno) : "");
    kl_hexdump("->", buf, r);
    return r;
}
// send/recv carry the same flag word and were forwarded raw. curl's every
// write is send(..., MSG_NOSIGNAL) on Linux, which arrived here as Darwin's
// MSG_RCVMORE.
static ssize_t kl_send(int fd, const void *buf, size_t n, int flags) {
    int nosig = 0, dflags = kl_msg_flags(flags, &nosig);
    if (nosig) kl_no_sigpipe(fd);
    ssize_t r = send(fd, buf, n, dflags);
    if (kl_net_trace())
        fprintf(stderr, "  [net] send(fd=%d, %zu B, flags=0x%x->0x%x) -> %zd%s\n",
                fd, n, flags, dflags, r, r < 0 ? strerror(errno) : "");
    kl_hexdump("->", buf, r);
    return r;
}
static ssize_t kl_recv(int fd, void *buf, size_t n, int flags) {
    int dflags = kl_msg_flags(flags, NULL);
    ssize_t r = recv(fd, buf, n, dflags);
    if (kl_net_trace())
        fprintf(stderr, "  [net] recv(fd=%d, %zu B, flags=0x%x->0x%x) -> %zd%s\n",
                fd, n, flags, dflags, r, r < 0 ? strerror(errno) : "");
    kl_hexdump("<-", buf, r);
    return r;
}
static int kl_accept(int fd, struct sockaddr *sa, socklen_t *len) {
    int r = accept(fd, sa, len);
    if (r >= 0) kl_sa_to_guest(sa, len);
    return r;
}
static ssize_t kl_recvfrom(int fd, void *buf, size_t n, int flags,
                           struct sockaddr *sa, socklen_t *len) {
    int dflags = kl_msg_flags(flags, NULL);
    ssize_t r = recvfrom(fd, buf, n, dflags, sa, len);
    char host[80] = "-";
    if (r >= 0 && sa && len) {
        kl_sa_fmt(host, sizeof host, sa);
        kl_sa_to_guest(sa, len);
    }
    if (kl_net_trace())
        fprintf(stderr, "  [net] recvfrom(fd=%d, %zu B, flags=0x%x->0x%x) -> %zd from %s%s\n",
                fd, n, flags, dflags, r, host, r < 0 ? strerror(errno) : "");
    kl_hexdump("<-", buf, r);
    return r;
}

// ---------- sendmsg / recvmsg: struct msghdr diverges ----------
// These were plain forwards, and the layouts do not match. Linux (LP64) makes
// msg_iovlen and msg_controllen size_t and puts msg_flags at +48; Darwin makes
// both int and puts msg_flags at +44. Little-endian hides the first two for
// small values — but msg_flags does not overlap at all, so a forwarded recvmsg
// writes MSG_TRUNC/MSG_CTRUNC into the guest's padding and the guest reads
// whatever was at +48.
//
// The ancillary data diverges the same way one level down: Linux cmsghdr leads
// with a size_t cmsg_len (16-byte header, 8-byte alignment), Darwin with a
// socklen_t (12-byte header, 4-byte alignment) — and the option TYPE numbers
// differ on top of that (IP_PKTINFO is 8 on Linux, 26 here). So the control
// buffer is rebuilt, not aliased. This is the path Steam's UDP discovery uses
// to learn which local interface a reply arrived on.
typedef struct {
    void     *msg_name;
    uint32_t  msg_namelen;
    uint32_t  _pad;
    struct iovec *msg_iov;
    uint64_t  msg_iovlen;
    void     *msg_control;
    uint64_t  msg_controllen;
    int32_t   msg_flags;
    uint32_t  _pad2;
} kl_lx_msghdr;
typedef struct { uint64_t cmsg_len; int32_t cmsg_level; int32_t cmsg_type; } kl_lx_cmsghdr;
#define KL_LX_CMSG_ALIGN(n) (((n) + 7u) & ~7u)

// Linux cmsg_type -> Darwin, per level. Same table shape as kl_ip_optname and
// for the same reason: the numbers overlap, so an untranslated type is a
// different message rather than an unknown one.
static int kl_cmsg_type_to_host(int level, int type) {
    if (level == IPPROTO_IP) {
        switch (type) {
        case 8:  return IP_PKTINFO;             // Linux IP_PKTINFO
        case 1:  return IP_TOS;
        case 2:  return IP_TTL;
        default: kl_net_warn_once("cmsg IPPROTO_IP type", type); return -1;
        }
    }
    if (level == IPPROTO_IPV6) {
        switch (type) {
        case 50: return 46;                     // IPV6_PKTINFO
        case 52: return 36;                     // IPV6_HOPLIMIT
        case 67: return IPV6_TCLASS;
        default: kl_net_warn_once("cmsg IPPROTO_IPV6 type", type); return -1;
        }
    }
    kl_net_warn_once("cmsg level", level);
    return -1;
}
static int kl_cmsg_type_to_guest(int level, int type) {
    if (level == IPPROTO_IP) {
        if (type == IP_PKTINFO) return 8;
        if (type == IP_TOS)     return 1;
        if (type == IP_TTL)     return 2;
    } else if (level == IPPROTO_IPV6) {
        if (type == 46) return 50;
        if (type == 36) return 52;
        if (type == IPV6_TCLASS) return 67;
    }
    return type;
}
// The payloads themselves agree: struct in_pktinfo and struct in6_pktinfo have
// the same members in the same order on both platforms, so only the header and
// the type number are rewritten.
static ssize_t kl_sendmsg(int fd, const void *gmsg, int flags) {
    const kl_lx_msghdr *g = gmsg;
    if (!g) { errno = EFAULT; return -1; }
    struct msghdr h;
    memset(&h, 0, sizeof h);
    struct sockaddr_storage hs;
    char host[80] = "-";
    if (g->msg_name && g->msg_namelen &&
        kl_sa_to_host(&hs, g->msg_name, g->msg_namelen) == 0) {
        h.msg_name = &hs;
        h.msg_namelen = g->msg_namelen;
        kl_sa_fmt(host, sizeof host, (const struct sockaddr *)&hs);
    }
    h.msg_iov = g->msg_iov;                     // struct iovec agrees
    h.msg_iovlen = (int)g->msg_iovlen;

    char cbuf[512];
    if (g->msg_control && g->msg_controllen) {
        size_t out = 0;
        const uint8_t *p = g->msg_control;
        const uint8_t *end = p + g->msg_controllen;
        while (p + sizeof(kl_lx_cmsghdr) <= end) {
            const kl_lx_cmsghdr *lc = (const kl_lx_cmsghdr *)p;
            if (lc->cmsg_len < sizeof *lc || p + lc->cmsg_len > end) break;
            size_t dlen = (size_t)lc->cmsg_len - sizeof *lc;
            int dtype = kl_cmsg_type_to_host(lc->cmsg_level, lc->cmsg_type);
            if (dtype >= 0 && out + CMSG_SPACE(dlen) > sizeof cbuf)
                kl_net_warn_once("sendmsg control buffer overflow, cmsg dropped",
                                 (long)dlen);
            if (dtype >= 0 && out + CMSG_SPACE(dlen) <= sizeof cbuf) {
                struct cmsghdr *dc = (struct cmsghdr *)(cbuf + out);
                dc->cmsg_len = (socklen_t)CMSG_LEN(dlen);
                dc->cmsg_level = lc->cmsg_level;
                dc->cmsg_type = dtype;
                memcpy(CMSG_DATA(dc), p + sizeof *lc, dlen);
                out += CMSG_SPACE(dlen);
            }
            p += KL_LX_CMSG_ALIGN(lc->cmsg_len);
        }
        if (out) { h.msg_control = cbuf; h.msg_controllen = (socklen_t)out; }
    }
    int nosig = 0, dflags = kl_msg_flags(flags, &nosig);
    if (nosig) kl_no_sigpipe(fd);
    h.msg_flags = 0;
    ssize_t r = sendmsg(fd, &h, dflags);
    if (kl_net_trace())
        fprintf(stderr, "  [net] sendmsg(fd=%d, %d iov, %u B ctl, flags=0x%x->0x%x, %s) -> %zd%s\n",
                fd, (int)h.msg_iovlen, (unsigned)h.msg_controllen, flags, dflags,
                host, r, r < 0 ? strerror(errno) : "");
    return r;
}
static ssize_t kl_recvmsg(int fd, void *gmsg, int flags) {
    kl_lx_msghdr *g = gmsg;
    if (!g) { errno = EFAULT; return -1; }
    struct msghdr h;
    memset(&h, 0, sizeof h);
    struct sockaddr_storage hs;
    if (g->msg_name && g->msg_namelen) { h.msg_name = &hs; h.msg_namelen = sizeof hs; }
    h.msg_iov = g->msg_iov;
    h.msg_iovlen = (int)g->msg_iovlen;
    char cbuf[512];
    if (g->msg_control && g->msg_controllen) {
        h.msg_control = cbuf;
        h.msg_controllen = (socklen_t)(g->msg_controllen < sizeof cbuf
                                       ? g->msg_controllen : sizeof cbuf);
    }
    ssize_t r = recvmsg(fd, &h, kl_msg_flags(flags, NULL));
    char host[80] = "-";
    if (r >= 0) {
        if (h.msg_name && h.msg_namelen) {
            socklen_t nl = h.msg_namelen;
            kl_sa_fmt(host, sizeof host, (const struct sockaddr *)&hs);
            if (nl > g->msg_namelen) nl = g->msg_namelen;
            memcpy(g->msg_name, &hs, nl);
            kl_sa_to_guest(g->msg_name, &nl);
            g->msg_namelen = nl;
        } else {
            g->msg_namelen = 0;
        }
        size_t out = 0;
        for (struct cmsghdr *dc = h.msg_control ? CMSG_FIRSTHDR(&h) : NULL;
             dc; dc = CMSG_NXTHDR(&h, dc)) {
            size_t dlen = (size_t)dc->cmsg_len - (size_t)((uint8_t *)CMSG_DATA(dc) - (uint8_t *)dc);
            size_t need = KL_LX_CMSG_ALIGN(sizeof(kl_lx_cmsghdr) + dlen);
            if (out + need > g->msg_controllen) { h.msg_flags |= MSG_CTRUNC; break; }
            kl_lx_cmsghdr *lc = (kl_lx_cmsghdr *)((uint8_t *)g->msg_control + out);
            lc->cmsg_len = sizeof *lc + dlen;
            lc->cmsg_level = dc->cmsg_level;
            lc->cmsg_type = kl_cmsg_type_to_guest(dc->cmsg_level, dc->cmsg_type);
            memcpy(lc + 1, CMSG_DATA(dc), dlen);
            out += need;
        }
        g->msg_controllen = out;
        // ...and the output flags, at the offset the GUEST reads them from.
        int lf = 0;
        if (h.msg_flags & MSG_TRUNC)  lf |= LX_MSG_TRUNC;
        if (h.msg_flags & MSG_CTRUNC) lf |= LX_MSG_CTRUNC;
        if (h.msg_flags & MSG_OOB)    lf |= LX_MSG_OOB;
        if (h.msg_flags & MSG_EOR)    lf |= LX_MSG_EOR;
        g->msg_flags = lf;
    }
    if (kl_net_trace())
        fprintf(stderr, "  [net] recvmsg(fd=%d, flags=0x%x) -> %zd from %s%s\n",
                fd, flags, r, host, r < 0 ? strerror(errno) : "");
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
    if (on < 0) on = kl_env_on("KL_TRACE_IO", 0);
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
    if (on < 0) on = kl_env_on("KL_TRACE_SLEEP", 0);
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
    if (cap < 0) cap = kl_env_int("KL_USLEEP_CAP", 0);
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
X(klb_getpwuid) X(klb_getpwuid_r) X(klb_execl) X(klb_system) X(klb_syscall) X(klb_swprintf)
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
// kl_libc_slink.c — the surface Steam Link reaches for and Beat Saber does not.
X(klb_getauxval)
X(klb_fegetenv) X(klb_fesetenv) X(klb_feholdexcept) X(klb_feupdateenv)
X(klb_statvfs) X(klb_fstatvfs) X(klb_sendfile)
X(klb_epoll_create) X(klb_epoll_create1) X(klb_epoll_ctl) X(klb_epoll_wait)
X(klb_openat) X(klb___open_2)
X(klb___memmove_chk) X(klb___strncpy_chk) X(klb___strncpy_chk2) X(klb___strcat_chk)
X(klb___read_chk) X(klb___vsprintf_chk)
X(klb_sincosf) X(klb_sincos) X(klb_putchar) X(klb_getchar) X(klb_fdatasync)
X(klb___cmsg_nxthdr) X(klb___cxa_thread_atexit_impl)
X(klb_fileno) X(klb_fgetc) X(klb_ungetc) X(klb_getwc) X(klb_fgetwc)
X(klb_ungetwc) X(klb_fputwc) X(klb_putwc) X(klb_fwide)
X(klb_pthread_rwlock_tryrdlock) X(klb_pthread_rwlock_trywrlock)
X(klb___register_atfork) X(klb___gnu_strerror_r) X(klb___write_chk)
// ...and what the 2D frontend (libshell + Qt6) adds on top of it.
X(klb_eventfd) X(klb_eventfd_read) X(klb_eventfd_write) X(klb_ppoll)
X(klb_accept4) X(klb_pipe2) X(klb_dup2) X(klb_dup3) X(klb_memfd_create) X(klb_clone)
X(klb_inotify_init) X(klb_inotify_init1)
X(klb_inotify_add_watch) X(klb_inotify_rm_watch)
X(klb___assert2) X(klb___FD_CLR_chk) X(klb___fgets_chk)
X(klb___pthread_cleanup_push) X(klb___pthread_cleanup_pop)
#undef X
extern char **environ;
// A VARIABLE, not a function, so it cannot ride the X() macro above.
extern const unsigned char *klb_ctype_ptr;   // kl_libc_slink.c

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
    E("sendmsg", kl_sendmsg), E("recvmsg", kl_recvmsg),
    E("send", kl_send), E("recv", kl_recv),
    E("getsockname", kl_getsockname),
    E("strtold", klv_strtold), E("wcstold", klv_wcstold), E("strtold_l", klv_strtold_l),
    E("swprintf", klb_swprintf), E("execl", klb_execl), E("system", klb_system),
    E("syscall", klb_syscall),

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

    // The clocks are hand-written (kl_libc.c) and must stay that way: the
    // Choreographer, System.nanoTime and the engine's frame delta all have to
    // agree on CLOCK_MONOTONIC, and a frame delta measured across two different
    // clocks is the offset between them. These three lived as hand-edits inside
    // the GENERATED table until 2026-08-08, where the next `gen_libc_table.py`
    // run would have silently turned them back into direct forwards.
    E("clock_gettime", klb_clock_gettime), E("clock_getres", klb_clock_getres),
    E("gettimeofday", klb_gettimeofday),

    // ---- kl_libc_slink.c: the surface Steam Link adds (PLANNING §11.4) ----
    // Every one of these is hand-written for a reason recorded at its
    // definition — a divergent layout, a colliding signature, or no Darwin
    // equivalent at all. None of them may become a direct forward.
    E("getauxval", klb_getauxval),
    E("fegetenv", klb_fegetenv), E("fesetenv", klb_fesetenv),
    E("feholdexcept", klb_feholdexcept), E("feupdateenv", klb_feupdateenv),
    E("statvfs", klb_statvfs), E("fstatvfs", klb_fstatvfs),
    E("sendfile", klb_sendfile),
    E("epoll_create", klb_epoll_create), E("epoll_create1", klb_epoll_create1),
    E("epoll_ctl", klb_epoll_ctl), E("epoll_wait", klb_epoll_wait),
    E("openat", klb_openat), E("__open_2", klb___open_2),
    E("__memmove_chk", klb___memmove_chk), E("__strncpy_chk", klb___strncpy_chk),
    E("__strncpy_chk2", klb___strncpy_chk2), E("__strcat_chk", klb___strcat_chk),
    E("__read_chk", klb___read_chk), E("__vsprintf_chk", klb___vsprintf_chk),
    E("sincosf", klb_sincosf), E("sincos", klb_sincos),
    E("putchar", klb_putchar), E("getchar", klb_getchar),
    E("fdatasync", klb_fdatasync),
    E("__cmsg_nxthdr", klb___cmsg_nxthdr),
    E("__cxa_thread_atexit_impl", klb___cxa_thread_atexit_impl),
    E("fileno", klb_fileno), E("fgetc", klb_fgetc), E("ungetc", klb_ungetc),
    E("getwc", klb_getwc), E("fgetwc", klb_fgetwc), E("ungetwc", klb_ungetwc),
    E("fputwc", klb_fputwc), E("putwc", klb_putwc), E("fwide", klb_fwide),
    E("pthread_rwlock_tryrdlock", klb_pthread_rwlock_tryrdlock),
    E("pthread_rwlock_trywrlock", klb_pthread_rwlock_trywrlock),

    // ---- ...and what the 2D configuration frontend (libshell + Qt6) adds ----
    // Linux syscall wrappers with no Darwin name. Qt's event dispatcher, file
    // watcher and process code call them directly. Rationale per function in
    // kl_libc_slink.c; the short version is that each is either rebuilt out of
    // what Darwin does have, or refused with the errno Qt already has a
    // fallback for. None is a silent success.
    E("eventfd", klb_eventfd),
    E("eventfd_read", klb_eventfd_read), E("eventfd_write", klb_eventfd_write),
    E("ppoll", klb_ppoll),
    E("accept4", klb_accept4), E("pipe2", klb_pipe2), E("dup3", klb_dup3),
    E("dup2", klb_dup2),
    E("memfd_create", klb_memfd_create),
    E("clone", klb_clone),
    E("inotify_init", klb_inotify_init), E("inotify_init1", klb_inotify_init1),
    E("inotify_add_watch", klb_inotify_add_watch),
    E("inotify_rm_watch", klb_inotify_rm_watch),
    E("__assert2", klb___assert2),
    E("__FD_CLR_chk", klb___FD_CLR_chk), E("__fgets_chk", klb___fgets_chk),
    E("__pthread_cleanup_push", klb___pthread_cleanup_push),
    E("__pthread_cleanup_pop", klb___pthread_cleanup_pop),
    // The standard streams are `FILE *` VARIABLES in bionic, so the table entry
    // is the address OF the pointer — one level of indirection that, got wrong,
    // hands the guest a FILE whose first field is a function pointer. Same trap
    // kl_opensl.c records for the SL_IID_* interface ids.
    E("stdout", &stdout), E("stderr", &stderr), E("stdin", &stdin),
    E("__register_atfork", klb___register_atfork),
    E("__gnu_strerror_r", klb___gnu_strerror_r),
    E("__write_chk", klb___write_chk),
    // bionic indexes this table directly from its <ctype.h> macros, so what
    // the guest binds is the array, offset by one (index -1 is legal).
    E("_ctype_", &klb_ctype_ptr),
    // bionic's LP64 stat aliases must land on the translating shim, never on
    // Darwin's stat — same struct divergence as `stat` itself (trap 7).
    E("stat64", klb_stat), E("lstat64", klb_lstat), E("fstat64", klb_fstat),

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
    void *egl = kl_egl_lookup(name);
    if (egl) return egl;

    // Tier 6: GLES and OpenSL ES as *ELF imports*.
    //
    // Beat Saber reaches both through dlopen + dlsym — FMOD resolves
    // slCreateEngine at runtime, and Unity takes GL entry points from
    // eglGetProcAddress — so for that title nothing in the import list names
    // them and these gateways are only ever entered through kl_dl.c.
    //
    // Steam Link is the other shape: libSDL3.so DT_NEEDEDs libGLESv2.so and
    // libOpenSLES.so and imports 57 gl* and 6 SL_* symbols directly, so they
    // have to bind at RELOCATION time or 63 draws' worth of entry points become
    // unresolved stubs. Same implementations, different door.
    //
    // Both routes are gated on the name, because kl_egl_sym never returns NULL
    // — it manufactures a named stub for anything it does not know. Letting it
    // answer every miss would swallow the unresolved-import report that is this
    // project's entire work-list mechanism, and AMediaCodec_* would silently
    // become "resolved". A lookup is a measurement; it must still be able to
    // say no.
    if (name[0] == 'g' && name[1] == 'l' && name[2] >= 'A' && name[2] <= 'Z')
        return kl_egl_sym(name);
    if (!strncmp(name, "SL_IID_", 7) || !strcmp(name, "slCreateEngine"))
        return kl_opensl_sym(name);

    // Tier 7: OpenXR (SL-8). libopenxr_loader.so is REPLACED rather than
    // translated (kl_openxr.h), so libvrlink_scene's forty-six xr* imports have
    // to bind here at relocation time — the same door libSDL3's gl* imports use
    // above, for the same reason. kl_openxr_lookup returns NULL for a name it
    // does not know, so an xr* we do not serve still shows up in the
    // unresolved-import report instead of being silently swallowed.
    if (name[0] == 'x' && name[1] == 'r' && name[2] >= 'A' && name[2] <= 'Z')
        return kl_openxr_lookup(name);

    // Tier 8: libmediandk (SL-10) — AMediaCodec/AMediaFormat/AImageReader/AImage
    // and the AHardwareBuffer that joins them to the graphics side. Another
    // DT_NEEDED that has to bind at relocation time, and another lookup that
    // returns NULL for what it does not serve, so the unresolved report keeps
    // working. The name prefixes are checked rather than the whole table being
    // consulted for every miss, for the reason in the tier 6 note.
    if (name[0] == 'A' &&
        (!strncmp(name, "AMedia", 6) || !strncmp(name, "AImage", 6) ||
         !strncmp(name, "AHardwareBuffer_", 16) || !strncmp(name, "ATrace_", 7) ||
         !strncmp(name, "AMEDIAFORMAT_", 13)))
        return kl_mediandk_lookup(name);
    return NULL;
}
