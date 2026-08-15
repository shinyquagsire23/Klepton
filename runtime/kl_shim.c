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
#include <fenv.h>        // feclearexcept/fetestexcept: SUPERHOT's libOVRLipSync
                         // imports them, and a direct forward needs the
                         // DECLARATION here, not just the symbol in libSystem
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <setjmp.h>
#include <locale.h>
#include <xlocale.h>
#include <libgen.h>
#include <fnmatch.h>    // libunity (1.40) — every FNM_* number matches Linux's
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
#include "kl_aaudio.h"
#include "kl_vulkan.h"
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
    // SO_BROADCAST is the guest DECLARING that it is about to broadcast, and it
    // is the only signal that survives whichever send call it then uses. Said
    // out loud because "no fan-out line in the log" has two completely
    // different causes — the sweep missed the send, or discovery never ran —
    // and without this they are the same silence. (0x20 on both platforms.)
    if (glevel == SOL_SOCKET && opt == SO_BROADCAST) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [sock] the guest enabled SO_BROADCAST on fd %d (%s) — "
                            "discovery is starting, so a fan-out line must follow\n",
                    fd, r ? strerror(errno) : "ok");
    }
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
// A datagram socket that has been connect()ed, being sendto()'d with an
// explicit destination anyway. **Linux allows it and Darwin does not** — BSD's
// udp_send refuses with EISCONN, Linux's udp_sendmsg just uses the address it
// was handed. That is a behavioural divergence rather than a struct layout or
// a flag number, which is what made it invisible until a guest depended on it:
// Steam Link's SVL transport connects its UDP socket and then sends through
// sendto() forever, so every packet failed, the client counted 51 in a row and
// tore the link down — "TX Failure Streak Hit 51; Aborting and reconnecting",
// looping for the whole run with no video. Nothing in the log said "sendto",
// and the natural reading was that the host had stopped listening.
//
// The destination is checked against the connected peer rather than assumed:
// same peer means send() is exactly what Linux would have done, and a
// different one is a real divergence we cannot paper over, so it keeps the
// error and says so by name.
static ssize_t kl_sendto_connected(int fd, const void *buf, size_t n, int dflags,
                                   const struct sockaddr *sa) {
    struct sockaddr_storage peer;
    socklen_t plen = sizeof peer;
    if (getpeername(fd, (struct sockaddr *)&peer, &plen) != 0) { errno = EISCONN; return -1; }

    int same = 0;
    if (peer.ss_family == sa->sa_family) {
        if (peer.ss_family == AF_INET) {
            const struct sockaddr_in *p = (const struct sockaddr_in *)&peer;
            const struct sockaddr_in *d = (const struct sockaddr_in *)sa;
            same = p->sin_port == d->sin_port &&
                   p->sin_addr.s_addr == d->sin_addr.s_addr;
        } else if (peer.ss_family == AF_INET6) {
            const struct sockaddr_in6 *p = (const struct sockaddr_in6 *)&peer;
            const struct sockaddr_in6 *d = (const struct sockaddr_in6 *)sa;
            same = p->sin6_port == d->sin6_port &&
                   memcmp(&p->sin6_addr, &d->sin6_addr, sizeof p->sin6_addr) == 0;
        }
    }
    if (!same) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [net] sendto() to an address other than the connected "
                            "peer on fd %d: Darwin refuses this and Linux allows it\n", fd);
        errno = EISCONN;
        return -1;
    }
    static int said;
    if (!said++)
        fprintf(stderr, "  [net] sendto() on a connected socket -> send() "
                        "(Linux permits the destination, BSD returns EISCONN)\n");
    return send(fd, buf, n, dflags);
}

// ---- broadcast, on a platform that will not let an app broadcast ----------
//
// Steam Link finds hosts by UDP broadcast to 255.255.255.255:27036 (SL-5). On
// visionOS that needs `com.apple.developer.networking.multicast`, which Apple
// grants by REQUEST rather than by enabling a capability — so it is not a build
// setting, it is a wait. Without it the computer list stays empty.
//
// It does not have to be. A broadcast is a destination address and nothing
// else: the same datagram sent to each host on the subnet reaches the same
// listeners, and unicast to a LAN address needs only the local-network
// permission (Info.plist's NSLocalNetworkUsageDescription), which is free. The
// replies come back unicast to the port the guest already bound, so nothing
// downstream of this changes at all.
//
// **Both are sent, and that is deliberate.** The real broadcast is attempted
// first because on a host run it simply works and is one packet instead of 254.
// Its result is NOT used to decide whether to fan out, because the failure mode
// we are working around is a send that *succeeds* and is dropped — Apple's
// filtering is not required to surface as an errno, and "silent zeros are worse
// than errors" (trap 6d) cuts both ways: we must not read a successful send as
// evidence the packet arrived. Duplicate probes are harmless here; a discovery
// reply is idempotent and the guest already merges hosts by id.
//
// KL_NET_BCAST_FANOUT=0 turns it off (the A/B, and what a host run wants once
// this is understood). KL_SLINK_HOST=<ip>[,<ip>…] replaces the sweep with an
// explicit list, which is the direct-connect case and costs one packet.
// 1022 hosts, which is a /22 — measured, because that is what this network
// turned out to be and a 512 cap swept exactly half of it. The PC happened to
// be in the half that was covered, which is the kind of luck that makes a bug
// look like a feature working.
#define KLNET_FANOUT_MAX 1024

static int kl_fanout_enabled(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_NET_BCAST_FANOUT", 1);
    return on;
}

// Every IPv4 address this datagram would have reached, resolved once.
//
// Sized rather than unbounded: a /16 is 65,534 sends per discovery tick, which
// is a denial of service against our own network rather than a sweep. Anything
// wider than /22 is refused BY NAME — the answer there is KL_SLINK_HOST, not a
// bigger buffer.
static unsigned kl_fanout_targets(uint32_t *out, unsigned max) {
    static uint32_t cache[KLNET_FANOUT_MAX];
    static unsigned cached = 0;
    static int done = 0;
    if (done) {
        unsigned k = cached < max ? cached : max;
        memcpy(out, cache, k * sizeof *out);
        return k;
    }
    done = 1;

    // An explicit list wins outright: this is the direct-connect path and the
    // point of it is not to sweep.
    const char *only = kl_env_str("KL_SLINK_HOST", NULL);
    if (only && *only) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", only);
        for (char *p = strtok(tmp, ","); p && cached < KLNET_FANOUT_MAX; p = strtok(NULL, ",")) {
            struct in_addr a;
            while (*p == ' ') p++;
            if (inet_pton(AF_INET, p, &a) == 1) cache[cached++] = a.s_addr;
            else fprintf(stderr, "  [net] KL_SLINK_HOST: '%s' is not an IPv4 address\n", p);
        }
        fprintf(stderr, "  [net] broadcast fan-out: %u host(s) from KL_SLINK_HOST\n", cached);
        unsigned k = cached < max ? cached : max;
        memcpy(out, cache, k * sizeof *out);
        return k;
    }

    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) {
        fprintf(stderr, "  [net] broadcast fan-out: getifaddrs failed (%s)\n", strerror(errno));
        return 0;
    }
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        if (!p->ifa_netmask) continue;
        uint32_t addr = ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr);
        uint32_t mask = ntohl(((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr);
        if (!mask || mask == 0xFFFFFFFFu) continue;          // /32: nothing to sweep
        uint32_t hosts = ~mask;
        if (hosts > KLNET_FANOUT_MAX - 1) {
            fprintf(stderr, "  [net] broadcast fan-out: %s is a /%d — %u hosts is too "
                            "many to sweep; set KL_SLINK_HOST=<ip> instead\n",
                    p->ifa_name, 32 - __builtin_popcount(hosts), hosts - 1);
            continue;
        }
        uint32_t net = addr & mask;
        uint32_t want = 0;
        for (uint32_t h = 1; h < hosts; h++) {
            uint32_t t = net | h;
            if (t == addr) continue;                          // ourselves
            want++;
            if (cached < KLNET_FANOUT_MAX) cache[cached++] = htonl(t);
        }
        // Never silently. A truncated sweep finds the hosts at the bottom of
        // the range and not the ones above it, which presents as "discovery
        // works, except for that one machine" — and the first version of this
        // capped a /22 at 512 without a word.
        if (want > cached)
            fprintf(stderr, "  [net] broadcast fan-out: %s has %u hosts and the cap "
                            "is %u — %u address(es) NOT swept; use KL_SLINK_HOST=<ip> "
                            "if the machine you want is above %u in the range\n",
                    p->ifa_name, want, (unsigned)KLNET_FANOUT_MAX, want - cached, cached);
        fprintf(stderr, "  [net] broadcast fan-out: %s %u.%u.%u.%u/%d -> %u unicast "
                        "target(s)\n", p->ifa_name,
                (addr >> 24) & 0xff, (addr >> 16) & 0xff, (addr >> 8) & 0xff, addr & 0xff,
                32 - __builtin_popcount(hosts), cached);
    }
    freeifaddrs(ifa);
    if (!cached)
        fprintf(stderr, "  [net] broadcast fan-out: no usable IPv4 interface — "
                        "discovery will only see what the real broadcast reaches\n");
    unsigned k = cached < max ? cached : max;
    memcpy(out, cache, k * sizeof *out);
    return k;
}

// Is this destination one the platform may refuse?
//
// **Measured on device, and it is not what the IPv4 shape suggested.** Steam
// Link's discovery opens an IPv6 socket, binds `[::]:27036` dual-stack, and
// sends its 31-byte probe twice:
//
//     sendto(fd, 31 B, [::ffff:255.255.255.255]:27036) -> No route to host
//     sendto(fd, 31 B, [ff02::1]:27036)                -> No route to host
//
// So the IPv4 broadcast arrives as an IPv4-MAPPED IPv6 address, and there is a
// second probe to the IPv6 all-nodes link-local multicast group. A family test
// of `AF_INET` matches neither, which is why the first fan-out fired on nothing
// and why SO_BROADCAST never appeared in the log: the guest never sets it,
// because it is not using an IPv4 broadcast socket at all.
//
// Both are treated as broadcast-like. The reply that matters is IPv4 — Steam's
// discovery protocol is — so both fan out over the same IPv4 host list, sent
// back through whichever family the socket is.
#define KL_V4MAPPED(a) (!memcmp((a), "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12))

static int kl_is_broadcast(const struct sockaddr *sa) {
    if (!sa) return 0;
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;
        const uint8_t *a = s6->sin6_addr.s6_addr;
        // ff02::1 — all nodes on this link. The v6 counterpart of a broadcast,
        // and refused here for the same reason.
        if (a[0] == 0xff && a[1] == 0x02 &&
            !memcmp(a + 2, "\0\0\0\0\0\0\0\0\0\0\0\0\0", 13) && a[15] == 1) return 1;
        if (!KL_V4MAPPED(a)) return 0;
        struct sockaddr_in v4 = { .sin_family = AF_INET };
        memcpy(&v4.sin_addr, a + 12, 4);
        return kl_is_broadcast((const struct sockaddr *)&v4);
    }
    if (sa->sa_family != AF_INET) return 0;
    uint32_t d = ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr);
    if (d == 0xFFFFFFFFu) return 1;
    struct ifaddrs *ifa = NULL;
    int hit = 0;
    if (getifaddrs(&ifa) != 0) return 0;
    for (struct ifaddrs *p = ifa; p && !hit; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !p->ifa_netmask) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        uint32_t a = ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr);
        uint32_t m = ntohl(((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr);
        if (m && d == (a | ~m)) hit = 1;
    }
    freeifaddrs(ifa);
    return hit;
}

// One destination, in the family the guest's socket actually is.
//
// The socket that broadcasts here is IPv6 (measured), so sending a plain
// sockaddr_in on it fails with EAFNOSUPPORT and the whole fan-out is a no-op
// that logs success. The IPv4 host goes back as an IPv4-mapped address, which
// a dual-stack socket routes to the real IPv4 host.
static socklen_t kl_fanout_addr(int family, uint16_t port_net, uint32_t v4,
                                struct sockaddr_storage *out) {
    memset(out, 0, sizeof *out);
    if (family == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)out;
        s6->sin6_family = AF_INET6;
        s6->sin6_len = sizeof *s6;
        s6->sin6_port = port_net;
        s6->sin6_addr.s6_addr[10] = 0xff;
        s6->sin6_addr.s6_addr[11] = 0xff;
        memcpy(s6->sin6_addr.s6_addr + 12, &v4, 4);
        return sizeof *s6;
    }
    struct sockaddr_in *s4 = (struct sockaddr_in *)out;
    s4->sin_family = AF_INET;
    s4->sin_len = sizeof *s4;
    s4->sin_port = port_net;
    s4->sin_addr.s_addr = v4;
    return sizeof *s4;
}

static uint16_t kl_sa_port(const struct sockaddr *sa) {
    return sa->sa_family == AF_INET6 ? ((const struct sockaddr_in6 *)sa)->sin6_port
                                     : ((const struct sockaddr_in *)sa)->sin_port;
}

static void kl_sendto_fanout(int fd, const void *buf, size_t n, int dflags,
                             const struct sockaddr *sa) {
    static uint32_t tgt[KLNET_FANOUT_MAX];
    static unsigned ntgt;
    static int init;
    if (!init) { init = 1; ntgt = kl_fanout_targets(tgt, KLNET_FANOUT_MAX); }
    if (!ntgt) return;

    uint16_t port = kl_sa_port(sa);
    unsigned ok = 0;
    int last = 0;
    for (unsigned i = 0; i < ntgt; i++) {
        struct sockaddr_storage to;
        socklen_t tl = kl_fanout_addr(sa->sa_family, port, tgt[i], &to);
        if (sendto(fd, buf, n, dflags, (struct sockaddr *)&to, tl) >= 0) ok++;
        else last = errno;
    }
    static int said;
    if (!said++)
        fprintf(stderr, "  [net] broadcast fan-out: %zu B to port %u delivered as %u "
                        "unicast of %u%s%s (KL_NET_BCAST_FANOUT=0 to stop, "
                        "KL_SLINK_HOST to aim it)\n", n, ntohs(port), ok, ntgt,
                ok < ntgt ? ", last error " : "", ok < ntgt ? strerror(last) : "");
}

// The msghdr form of the same thing. Shares the target list, so a run that
// discovers through one call and one that discovers through the other reach
// exactly the same hosts.
static void kl_sendmsg_fanout(int fd, struct msghdr *h, int dflags) {
    static uint32_t tgt[KLNET_FANOUT_MAX];
    static unsigned ntgt;
    static int init;
    if (!init) { init = 1; ntgt = kl_fanout_targets(tgt, KLNET_FANOUT_MAX); }
    if (!ntgt) return;

    uint16_t port = kl_sa_port(h->msg_name);
    void *saved = h->msg_name;
    socklen_t savedlen = h->msg_namelen;
    struct sockaddr_storage to;
    unsigned ok = 0;
    for (unsigned i = 0; i < ntgt; i++) {
        socklen_t tl = kl_fanout_addr(((struct sockaddr *)saved)->sa_family,
                                      port, tgt[i], &to);
        h->msg_name = &to;
        h->msg_namelen = tl;
        if (sendmsg(fd, h, dflags) >= 0) ok++;
    }
    h->msg_name = saved;
    h->msg_namelen = savedlen;
    static int said;
    if (!said++)
        fprintf(stderr, "  [net] broadcast fan-out (sendmsg): port %u delivered as "
                        "%u unicast of %u\n", ntohs(port), ok, ntgt);
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
    if (r < 0 && errno == EISCONN && sa)
        r = kl_sendto_connected(fd, buf, n, dflags, sa);
    // ...and again, one host at a time, where the platform may have dropped it.
    // Deliberately not conditional on `r` — see kl_sendto_fanout above.
    if (sa && kl_fanout_enabled() && kl_is_broadcast(sa)) {
        int e = errno;                          // the fan-out must not rewrite it
        kl_sendto_fanout(fd, buf, n, dflags, sa);
        errno = e;
        if (r < 0) r = (ssize_t)n;              // the datagram did go out
    }
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
    if (r < 0 && errno == EISCONN && h.msg_name) {
        // Same divergence as kl_sendto's — a named send on a connected socket.
        // Retry with the name dropped, which is what Linux effectively did.
        h.msg_name = NULL; h.msg_namelen = 0;
        r = sendmsg(fd, &h, dflags);
    }
    // ...and the fan-out, for the same reason and on the same terms as
    // kl_sendto's. libshell imports BOTH sendto and sendmsg, so covering one
    // and not the other is a fan-out that works or does not depending on which
    // call the guest happened to use — which is indistinguishable, from the
    // log, from a fan-out that is broken.
    if (h.msg_name && kl_fanout_enabled() && kl_is_broadcast(h.msg_name)) {
        int e = errno;
        kl_sendmsg_fanout(fd, &h, dflags);
        errno = e;
        if (r < 0) {
            r = 0;
            for (int i = 0; i < (int)h.msg_iovlen; i++) r += (ssize_t)h.msg_iov[i].iov_len;
        }
    }
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
// Unity 2018.4 (Beat Saber 1.6.0) imports the unbounded form as well; 2019.4
// did not. Same trap 2 marshalling as its bounded sibling -- the guest's
// va_list is an AAPCS64 descriptor and cannot be handed to Darwin's vsprintf.
static int kl_vsprintf(char *b, const char *fmt, void *gva) {
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, (kl_va *)gva, m, sizeof m, KL_VA_PRINTF) == (size_t)-1) return -1;
    return vsprintf(b, fmt, (va_list)m);
}

// ---------- Android-only libc instrumentation ----------
// bionic exports these as a pair of no-op hooks that Google's internal build
// interposes to account for time spent in blocking calls; the stock
// implementation does nothing at all. Unity 2018.4's libunity and libil2cpp
// both call them, so they are on the ordinary path rather than a diagnostic
// one, and refusing them would abort a run that has nothing wrong with it.
// Nothing here can observe the accounting, so a no-op IS the behaviour --
// unlike the silent-zero cases in trap 6d, there is no answer being invented.
static void kl_blocking_region_begin(void) { }
static void kl_blocking_region_end(void) { }

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
X(klb_getrandom) X(klb_isnan)
X(klb_mmap) X(klb_mprotect) X(klb_madvise) X(klb_sysinfo)
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
    E("vprintf", klb_vprintf), E("vsscanf", klb_vsscanf), E("vsprintf", kl_vsprintf),

    // Android-only libc instrumentation (no-ops in stock bionic)
    E("__google_potentially_blocking_region_begin", kl_blocking_region_begin),
    E("__google_potentially_blocking_region_end", kl_blocking_region_end),

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
    // Beat Saber 1.40: getrandom is Linux-only and getentropy is NOT a drop-in
    // (no flags, 256-byte cap, 0/-1 rather than a count); isnan is a macro on
    // Darwin, so there is no symbol to forward. Both in kl_libc.c.
    E("getrandom", klb_getrandom), E("isnan", klb_isnan),
    E("mmap", klb_mmap), E("mprotect", klb_mprotect), E("madvise", klb_madvise),
    // Linux-only, and the third door onto the memory budget — UE4 asks here
    // where every Unity title reads /proc/meminfo. See klb_sysinfo.
    E("sysinfo", klb_sysinfo),
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

// Tier 0: a diagnostic's own answer, consulted before everything else and NULL
// unless something host-only installs one. It exists because an instrument that
// has to see a libc call the guest makes has no other door — the generated table
// forwards `read` straight to Darwin's, and adding a permanent wrapper to the
// shim for the sake of one investigation is how a diagnostic becomes a tax on
// every guest. A lookup is not on any hot path; a call through the answer is
// the diagnostic's own business.
void *(*kl_shim_override)(const char *name);

void *kl_shim_lookup(const char *name) {
    if (kl_shim_override) { void *o = kl_shim_override(name); if (o) return o; }
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

    // Tier 9: libaaudio (SL-11) — the audio half of the same client. Another
    // DT_NEEDED bound at relocation time, and another lookup that says no to
    // what it does not serve. "AAudio" cannot collide with tier 8's prefixes,
    // so the order of these two tests does not matter.
    if (!strncmp(name, "AAudio", 6))
        return kl_aaudio_lookup(name);

    // Tier 10: Vulkan (BONELAB). libvulkan.so is REPLACED for the same reason
    // libopenxr_loader is — the real one is Android's loader, looking for an ICD
    // through driver plumbing that does not exist here, and MoltenVK IS the ICD.
    // libunity reaches it through dlopen/dlsym, but libSLZQuestNative names six
    // vk* symbols directly, so they have to bind at relocation time too.
    //
    // The third character is checked as well as the first two: `vk` alone would
    // claim any guest symbol starting with those letters, and this tier runs
    // before the miss falls through to the unresolved report.
    if (name[0] == 'v' && name[1] == 'k' && name[2] >= 'A' && name[2] <= 'Z')
        return kl_vulkan_lookup(name);

    // Tier 11: the Oculus Platform SDK (RE4). libovrplatformloader.so is
    // REPLACED like the three above it, and until now every guest reached it
    // the way Unity does — dlopen, then dlsym, which kl_ovrplat_sym has always
    // answered. **Unreal Engine 4 LINKS it**: libovrplatformloader is in
    // libUE4's DT_NEEDED and its hundred-odd `ovr_*` calls are bound at
    // RELOCATION time, so without this tier every one of them is an unresolved
    // import — the platform surface present and unreachable, aborting by name
    // on the first achievement or entitlement call.
    //
    // Same door as tiers 7-10, and the same discipline: kl_ovrplat_sym returns
    // NULL for a name it does not know, so a platform call we do not serve
    // still reaches the unresolved report rather than being swallowed.
    //
    // **This does not widen the DRM line by one name.** kl_ovrplat_sym is the
    // same function dlsym goes through, and it classifies before it dispatches
    // — the entitlement/IAP/delivery family still aborts unconditionally and
    // still ignores KL_PERMISSIVE. What changes is only WHERE the guest was
    // standing when it asked.
    //
    // `ovrp_` is OVRPlugin's prefix and a different library (kl_ovrp), so the
    // fourth character is checked: `ovr_` and `ovrID` are the platform's, and
    // `ovrp_` must not be claimed here.
    if (!strncmp(name, "ovr_", 4) || !strncmp(name, "ovrID", 5))
        return kl_ovrplat_sym(name);
    return NULL;
}
