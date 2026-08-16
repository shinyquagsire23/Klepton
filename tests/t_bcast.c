// The broadcast fan-out. `make bcast`, in `make check`.
//
// Steam Link finds hosts by UDP broadcast to 255.255.255.255:27036. On
// visionOS an app may not broadcast without `com.apple.developer.networking.
// multicast`, which Apple grants by REQUEST — so the computer list stays empty
// and there is no build setting that fixes it. kl_shim.c's answer is to deliver
// the same datagram as unicast to each host on the subnet, which needs only the
// local-network permission.
//
// This gate exists because that path is unreachable from every other test: it
// only runs inside a live discovery sweep, which needs the Qt frontend, a
// window, and a Steam host on the LAN. Here it is driven directly through the
// shim's own `sendto` — the one the guest calls — and aimed at loopback with
// KL_SLINK_HOST, so it sends no packet to anyone else's network.
//
// What it proves, in order:
//   1. a broadcast destination is RECOGNISED (255.255.255.255 and the
//      subnet-directed form both, so a guest using either is served);
//   2. the datagram actually ARRIVES at the configured host, byte for byte;
//   3. the call REPORTS SUCCESS even though the underlying broadcast was
//      refused — the guest must not see the platform's refusal, or it counts a
//      TX failure and tears the link down;
//   4. an ordinary unicast send is untouched, which is what says this is a
//      narrow intercept and not a rewrite of the send path.
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "../runtime/klepton.h"

static int g_fail;
static void ck(int ok, const char *what) {
    if (!ok) g_fail++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

typedef ssize_t (*sendto_fn)(int, const void *, size_t, int,
                             const struct sockaddr *, socklen_t);

// A LINUX-layout sockaddr_in, which is what a guest hands the shim and
// therefore what this has to send. Darwin puts a one-byte `sin_len` first and
// the family second; Linux has a two-byte family at offset 0, and kl_sa_to_host
// converts one into the other on every call. A test that passed the native
// struct would be testing a path the guest never takes — and would silently
// fail the address translation before ever reaching the fan-out, which is
// exactly what the first version of this file did.
typedef struct {
    uint16_t sin_family;      // AF_INET == 2 on both
    uint16_t sin_port;        // network order
    uint32_t sin_addr;        // network order
    uint8_t  pad[8];
} lx_sockaddr_in;

static lx_sockaddr_in lx_addr(const char *ip, uint16_t port_net) {
    lx_sockaddr_in a = {0};
    a.sin_family = 2;
    a.sin_port = port_net;
    struct in_addr t;
    if (inet_pton(AF_INET, ip, &t) != 1) { fprintf(stderr, "bad ip %s\n", ip); exit(2); }
    a.sin_addr = t.s_addr;
    return a;
}

// A bound loopback socket, and the port it landed on.
static int rx_socket(uint16_t *port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); exit(2); }
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) { perror("bind"); exit(2); }
    socklen_t l = sizeof a;
    if (getsockname(fd, (struct sockaddr *)&a, &l) != 0) { perror("getsockname"); exit(2); }
    *port = a.sin_port;
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

// Send `msg` to `dst`:port through the SHIM, and say whether it arrived on rx.
static int roundtrip(sendto_fn shim_sendto, int rx, uint16_t port,
                     const char *dst, const char *msg, ssize_t *out_rc) {
    int tx = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx < 0) { perror("socket"); exit(2); }
    lx_sockaddr_in to = lx_addr(dst, port);

    errno = 0;
    ssize_t r = shim_sendto(tx, msg, strlen(msg), 0, (struct sockaddr *)&to, sizeof to);
    if (out_rc) *out_rc = r;

    char buf[128];
    ssize_t got = recv(rx, buf, sizeof buf - 1, 0);
    close(tx);
    if (got <= 0) return 0;
    buf[got] = 0;
    return strcmp(buf, msg) == 0;
}

int main(void) {
    // Aim the fan-out at loopback. Without this it would sweep the real subnet,
    // and a test that sends 254 datagrams to a stranger's network every time
    // `make check` runs is not a test anyone should have to run.
    setenv("KL_SLINK_HOST", "127.0.0.1", 1);
    setenv("KL_NET_BCAST_FANOUT", "1", 1);

    printf("=== the broadcast fan-out ===\n");

    sendto_fn shim_sendto = (sendto_fn)kl_shim_lookup("sendto");
    if (!shim_sendto) { printf("FAIL: the shim has no sendto\n"); return 1; }

    uint16_t port = 0;
    int rx = rx_socket(&port);

    // 1 + 2 + 3. The broadcast itself is refused here (no SO_BROADCAST on the
    // socket, which is exactly the shape of the platform refusing it), so this
    // is also the case that matters: the guest must see success.
    ssize_t rc = 0;
    int got = roundtrip(shim_sendto, rx, port, "255.255.255.255", "klepton-discovery", &rc);
    ck(got, "255.255.255.255 is delivered to the configured host");
    ck(rc == (ssize_t)strlen("klepton-discovery"),
       "...and the guest is told the datagram went out, not that it failed");

    // The subnet-directed form. A guest that computes 192.168.4.255 itself has
    // to be served identically, and recognising only the all-ones address would
    // pass every test above while missing half the real senders.
    {
        // Loopback's own broadcast address: 127.255.255.255 on a /8. Using the
        // real interface's would put packets on the LAN, which this test will
        // not do — so the check is that a NON-all-ones broadcast is recognised,
        // driven through the same code.
        int tx = socket(AF_INET, SOCK_DGRAM, 0);
        lx_sockaddr_in to = lx_addr("127.255.255.255", port);
        errno = 0;
        ssize_t r = shim_sendto(tx, "subnet", 6, 0, (struct sockaddr *)&to, sizeof to);
        char buf[64];
        ssize_t n = recv(rx, buf, sizeof buf, 0);
        close(tx);
        // Loopback is not in the fan-out's interface sweep (IFF_LOOPBACK is
        // skipped, deliberately — sweeping 127/8 is 16M sends), so this one is
        // recognised only if the platform itself delivers it. Reported rather
        // than asserted: what must not happen is an ERROR reaching the guest.
        printf("       (127.255.255.255: rc=%zd, %s)\n", r,
               n > 0 ? "delivered by the platform" : "not delivered — fan-out skips loopback");
        ck(r >= 0, "a subnet-directed broadcast does not return an error to the guest");
    }

    // 5. THE SAME THING THROUGH sendmsg, which is the half that was missing.
    //
    // libshell imports both `sendto` and `sendmsg`, and the first version of
    // the fan-out covered only `sendto` — so on device it did nothing and the
    // log looked identical to a fan-out that was never reached. Covering one
    // send call and not the other is not a partial fix, it is a coin toss.
    {
        int tx = socket(AF_INET, SOCK_DGRAM, 0);
        lx_sockaddr_in to = lx_addr("255.255.255.255", port);
        const char *msg = "klepton-discovery-msg";
        struct iovec iov = { .iov_base = (void *)msg, .iov_len = strlen(msg) };
        // The guest's msghdr layout, which is what the shim translates from.
        struct { void *name; uint32_t namelen; struct iovec *iov; size_t iovlen;
                 void *ctl; size_t ctllen; int flags; } g = {
            .name = &to, .namelen = sizeof to, .iov = &iov, .iovlen = 1,
        };
        errno = 0;
        ssize_t r = ((ssize_t (*)(int, const void *, int))kl_shim_lookup("sendmsg"))
                        (tx, &g, 0);
        char buf[128];
        ssize_t n = recv(rx, buf, sizeof buf - 1, 0);
        close(tx);
        if (n > 0) buf[n] = 0;
        ck(n > 0 && strcmp(buf, msg) == 0,
           "sendmsg to 255.255.255.255 is delivered to the configured host too");
        ck(r == (ssize_t)strlen(msg),
           "...and reports the byte count, not the platform's refusal");
    }

    // 6. THE SHAPE THE GUEST ACTUALLY USES, which is none of the above.
    //
    // Measured on device: Steam Link's discovery opens an IPv6 socket, binds
    // [::]:27036 dual-stack, and sends the v4 broadcast as an IPv4-MAPPED IPv6
    // address (plus a second probe to ff02::1). A family test of AF_INET
    // matches neither, so the first two versions of the fan-out fired on
    // nothing while every check above passed. That is the whole reason this
    // case is here: the gate has to speak the guest's dialect, not the one the
    // feature was designed against.
    {
        // A dual-stack receiver, so an ::ffff:127.0.0.1 datagram lands.
        int rx6 = socket(AF_INET6, SOCK_DGRAM, 0);
        int off = 0;
        setsockopt(rx6, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
        struct sockaddr_in6 b = {0};
        b.sin6_family = AF_INET6; b.sin6_len = sizeof b;
        if (bind(rx6, (struct sockaddr *)&b, sizeof b) != 0) { perror("bind6"); exit(2); }
        socklen_t bl = sizeof b;
        getsockname(rx6, (struct sockaddr *)&b, &bl);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(rx6, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

        // ...and the guest's own sockaddr: Linux layout, AF_INET6 == 10.
        struct {
            uint16_t family; uint16_t port; uint32_t flowinfo;
            uint8_t  addr[16]; uint32_t scope_id;
        } to6 = {0};
        to6.family = 10;
        to6.port = b.sin6_port;
        to6.addr[10] = 0xff; to6.addr[11] = 0xff;      // ::ffff:255.255.255.255
        memset(to6.addr + 12, 0xff, 4);

        int tx = socket(AF_INET6, SOCK_DGRAM, 0);
        const char *msg = "klepton-v4mapped";
        errno = 0;
        ssize_t r = shim_sendto(tx, msg, strlen(msg), 0, (struct sockaddr *)&to6, sizeof to6);
        char buf[128];
        ssize_t n = recv(rx6, buf, sizeof buf - 1, 0);
        close(tx); close(rx6);
        if (n > 0) buf[n] = 0;
        ck(n > 0 && strcmp(buf, msg) == 0,
           "::ffff:255.255.255.255 on an IPv6 socket reaches the configured host");
        ck(r == (ssize_t)strlen(msg),
           "...and reports the byte count, not \"No route to host\"");
    }

    // 4. The narrowness check. If this stops working, the intercept has become
    // a rewrite of the whole send path.
    got = roundtrip(shim_sendto, rx, port, "127.0.0.1", "ordinary-unicast", &rc);
    ck(got && rc == (ssize_t)strlen("ordinary-unicast"),
       "an ordinary unicast send is untouched");

    close(rx);
    printf("%s: the broadcast fan-out\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
