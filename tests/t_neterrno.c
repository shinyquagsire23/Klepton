// What the guest reads in errno after a failed send/recv. `make neterrno`, in
// `make check`.
//
// klb_errno resolves "Darwin's errno did not move since the slot was filled" as
// a guest write. A client reading a non-blocking socket in a loop trips it: the
// first recv fails EAGAIN, the TLS layer clears errno (a guest write), and the
// second recv fails EAGAIN again -- Darwin's errno is still the EAGAIN of the
// first call, so the guest's 0 was kept and curl saw "no error" on a failed
// read, reset the stream and reported "Failed to receive data". The shim's send
// and recv now publish THIS call's errno through kl_errno_set, so the guest
// reads the failure it just had.
//
// Since #36 the same holds for connect, sendto, sendmsg, recvfrom, recvmsg and
// read, and since then also for write, readv, writev, accept, accept4, pread and
// pread64: each case below is the guest clearing errno and then the same failure
// twice, and it must read the failure both times.
//
// Driven through the shim's own entries and __errno, the ones the guest calls,
// on local sockets, pipes and a temporary file only.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include "../runtime/klepton.h"

#define LX_EAGAIN 11          // Linux's value; Darwin's is 35
#define LX_EPIPE  32
#define LX_MSG_NOSIGNAL 0x4000
#define LX_ESPIPE 29
#define LX_ECONNREFUSED 111
#define LX_AF_INET 2

// The guest's own layouts (Linux arm64), as the shim reads them.
typedef struct { uint16_t family, port; uint32_t addr; uint8_t zero[8]; } lx_sockaddr_in;
typedef struct {
    void *name; uint32_t namelen, _pad; struct iovec *iov; uint64_t iovlen;
    void *control; uint64_t controllen; int32_t flags; uint32_t _pad2;
} lx_msghdr;

static int g_fail;
static void ck(int ok, const char *what) {
    if (!ok) g_fail++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

typedef ssize_t (*recv_fn)(int, void *, size_t, int);
typedef ssize_t (*send_fn)(int, const void *, size_t, int);
typedef int *(*errno_fn)(void);
typedef ssize_t (*rw_fn)(int, void *, size_t);
typedef ssize_t (*iov_fn)(int, const struct iovec *, int);
typedef ssize_t (*pread_fn)(int, void *, size_t, off_t);
typedef ssize_t (*sendto_fn)(int, const void *, size_t, int, const void *, socklen_t);
typedef ssize_t (*recvfrom_fn)(int, void *, size_t, int, void *, socklen_t *);
typedef ssize_t (*msg_fn)(int, void *, int);
typedef int (*connect_fn)(int, const void *, socklen_t);
typedef int (*accept_fn)(int, void *, socklen_t *);
typedef int (*accept4_fn)(int, void *, socklen_t *, int);

static errno_fn g_errno;
// Clear, fail, read; twice. `call` returns <0 on failure.
#define TWICE(want, what, call) do {                                              \
        for (int k_ = 0; k_ < 2; k_++) {                                          \
            *g_errno() = 0;                                                        \
            ssize_t r_ = (ssize_t)(call);                                          \
            int e_ = *g_errno();                                                   \
            ck(r_ < 0 && e_ == (want), k_ ? "...and again, after the guest cleared errno" : (what)); \
            if (r_ >= 0 || e_ != (want)) printf("       (returned %zd, errno %d)\n", r_, e_); \
        }                                                                          \
    } while (0)

int main(void) {
    printf("=== the guest's errno after a failed send/recv ===\n");
    signal(SIGPIPE, SIG_IGN);      // a send to a closed peer, the way a guest with MSG_NOSIGNAL sees it
    recv_fn g_recv = (recv_fn)kl_shim_lookup("recv");
    send_fn g_send = (send_fn)kl_shim_lookup("send");
    g_errno = (errno_fn)kl_shim_lookup("__errno");
    if (!g_recv || !g_send || !g_errno) { printf("FAIL: the shim lacks recv, send or __errno\n"); return 1; }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 2; }
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    char buf[16];

    // The first failure is translated to Linux's number.
    ssize_t r = g_recv(sv[0], buf, sizeof buf, 0);
    ck(r < 0 && *g_errno() == LX_EAGAIN, "an empty non-blocking recv fails with EAGAIN (Linux 11)");

    // The TLS layer clears errno; the next recv fails the same way. The guest
    // must read EAGAIN again, not its own 0.
    for (int i = 0; i < 3; i++) {
        *g_errno() = 0;
        r = g_recv(sv[0], buf, sizeof buf, 0);
        int e = *g_errno();
        ck(r < 0 && e == LX_EAGAIN, "...cleared by the guest, the repeated EAGAIN is EAGAIN again");
        if (e != LX_EAGAIN) printf("       (read %d)\n", e);
    }

    // The same for a second, different value after a success in between.
    ck(g_send(sv[1], "x", 1, 0) == 1, "a send on the pair succeeds");
    ck(g_recv(sv[0], buf, sizeof buf, 0) == 1, "...and the recv gets the byte");
    *g_errno() = 0;
    r = g_recv(sv[0], buf, sizeof buf, 0);
    ck(r < 0 && *g_errno() == LX_EAGAIN, "EAGAIN after a success is EAGAIN");

    // send: the peer is gone, so it fails EPIPE, twice, with a clear between.
    close(sv[0]);
    r = g_send(sv[1], "x", 1, LX_MSG_NOSIGNAL);
    ck(r < 0 && *g_errno() == LX_EPIPE, "send to a closed peer fails with EPIPE");
    *g_errno() = 0;
    r = g_send(sv[1], "x", 1, LX_MSG_NOSIGNAL);
    ck(r < 0 && *g_errno() == LX_EPIPE, "...cleared by the guest, the repeated EPIPE is EPIPE again");
    close(sv[1]);

    // A success leaves the guest's own errno alone.
    int sv2[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv2);
    *g_errno() = 1234;
    ck(g_send(sv2[0], "y", 1, 0) == 1 && *g_errno() == 1234, "a successful send does not touch the guest's errno");
    ck(g_recv(sv2[1], buf, sizeof buf, 0) == 1 && *g_errno() == 1234, "a successful recv does not touch it either");
    close(sv2[0]); close(sv2[1]);

    // The rest of the calls #36 and its follow-up cover.
    rw_fn g_read = (rw_fn)kl_shim_lookup("read");
    rw_fn g_write = (rw_fn)kl_shim_lookup("write");
    iov_fn g_readv = (iov_fn)kl_shim_lookup("readv");
    iov_fn g_writev = (iov_fn)kl_shim_lookup("writev");
    pread_fn g_pread = (pread_fn)kl_shim_lookup("pread");
    pread_fn g_pread64 = (pread_fn)kl_shim_lookup("pread64");
    sendto_fn g_sendto = (sendto_fn)kl_shim_lookup("sendto");
    recvfrom_fn g_recvfrom = (recvfrom_fn)kl_shim_lookup("recvfrom");
    msg_fn g_sendmsg = (msg_fn)kl_shim_lookup("sendmsg");
    msg_fn g_recvmsg = (msg_fn)kl_shim_lookup("recvmsg");
    connect_fn g_connect = (connect_fn)kl_shim_lookup("connect");
    accept_fn g_accept = (accept_fn)kl_shim_lookup("accept");
    accept4_fn g_accept4 = (accept4_fn)kl_shim_lookup("accept4");
    if (!g_read || !g_write || !g_readv || !g_writev || !g_pread || !g_pread64 || !g_sendto ||
        !g_recvfrom || !g_sendmsg || !g_recvmsg || !g_connect || !g_accept || !g_accept4) {
        printf("FAIL: the shim lacks one of the calls below\n");
        return 1;
    }

    // Pipes: read and readv on an empty non-blocking one, write and writev on a
    // full one, write to one whose reader is gone, pread on one at all.
    int pp[2];
    if (pipe(pp) != 0) { perror("pipe"); return 2; }
    fcntl(pp[0], F_SETFL, fcntl(pp[0], F_GETFL) | O_NONBLOCK);
    fcntl(pp[1], F_SETFL, fcntl(pp[1], F_GETFL) | O_NONBLOCK);
    struct iovec iv = { buf, sizeof buf };
    TWICE(LX_EAGAIN, "read on an empty non-blocking pipe fails with EAGAIN", g_read(pp[0], buf, sizeof buf));
    TWICE(LX_EAGAIN, "readv on it fails with EAGAIN", g_readv(pp[0], &iv, 1));
    TWICE(LX_ESPIPE, "pread on a pipe fails with ESPIPE (Linux 29)", g_pread(pp[0], buf, sizeof buf, 0));
    TWICE(LX_ESPIPE, "pread64 on it too", g_pread64(pp[0], buf, sizeof buf, 0));
    static char fill[4096];
    while (write(pp[1], fill, sizeof fill) > 0) {}
    TWICE(LX_EAGAIN, "write on a full non-blocking pipe fails with EAGAIN", g_write(pp[1], fill, sizeof fill));
    struct iovec wv = { fill, sizeof fill };
    TWICE(LX_EAGAIN, "writev on it fails with EAGAIN", g_writev(pp[1], &wv, 1));
    close(pp[0]);
    TWICE(LX_EPIPE, "write to a pipe whose reader is gone fails with EPIPE", g_write(pp[1], "x", 1));
    close(pp[1]);

    // A datagram pair: recvfrom and recvmsg on an empty non-blocking one.
    int dp[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, dp) != 0) { perror("socketpair"); return 2; }
    fcntl(dp[0], F_SETFL, fcntl(dp[0], F_GETFL) | O_NONBLOCK);
    TWICE(LX_EAGAIN, "recvfrom on an empty non-blocking socket fails with EAGAIN",
          g_recvfrom(dp[0], buf, sizeof buf, 0, NULL, NULL));
    lx_msghdr m = { .iov = &iv, .iovlen = 1 };
    TWICE(LX_EAGAIN, "recvmsg on it fails with EAGAIN", g_recvmsg(dp[0], &m, 0));
    close(dp[0]); close(dp[1]);

    // A stream pair whose peer is gone: sendto and sendmsg fail with EPIPE.
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) { perror("socketpair"); return 2; }
    close(sp[0]);
    TWICE(LX_EPIPE, "sendto to a closed peer fails with EPIPE",
          g_sendto(sp[1], "x", 1, LX_MSG_NOSIGNAL, NULL, 0));
    struct iovec one = { "x", 1 };
    lx_msghdr sm = { .iov = &one, .iovlen = 1 };
    TWICE(LX_EPIPE, "sendmsg to a closed peer fails with EPIPE", g_sendmsg(sp[1], &sm, LX_MSG_NOSIGNAL));
    close(sp[1]);

    // TCP on the loopback: accept and accept4 on a non-blocking listener with
    // nobody waiting, and connect to a port that was just let go.
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in la = { .sin_len = sizeof la, .sin_family = AF_INET,
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t ll = sizeof la;
    if (ls < 0 || bind(ls, (struct sockaddr *)&la, sizeof la) != 0 || listen(ls, 1) != 0 ||
        getsockname(ls, (struct sockaddr *)&la, &ll) != 0) { perror("listen"); return 2; }
    fcntl(ls, F_SETFL, fcntl(ls, F_GETFL) | O_NONBLOCK);
    TWICE(LX_EAGAIN, "accept with nobody waiting fails with EAGAIN", g_accept(ls, NULL, NULL));
    TWICE(LX_EAGAIN, "accept4 too", g_accept4(ls, NULL, NULL, 0));
    lx_sockaddr_in to = { .family = LX_AF_INET, .port = la.sin_port, .addr = htonl(INADDR_LOOPBACK) };
    close(ls);                                   // nothing listens on that port now
    for (int k = 0; k < 2; k++) {
        int c = socket(AF_INET, SOCK_STREAM, 0);
        *g_errno() = 0;
        int r2 = g_connect(c, &to, sizeof to);
        int e2 = *g_errno();
        ck(r2 < 0 && e2 == LX_ECONNREFUSED,
           k ? "...and again, after the guest cleared errno" : "connect to a closed port fails with ECONNREFUSED (Linux 111)");
        if (r2 >= 0 || e2 != LX_ECONNREFUSED) printf("       (returned %d, errno %d)\n", r2, e2);
        close(c);
    }

    // A success leaves the guest's errno alone here too.
    char path[] = "/tmp/t_neterrno.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 2; }
    unlink(path);
    *g_errno() = 1234;
    ck(g_write(fd, "abc", 3) == 3 && *g_errno() == 1234, "a successful write does not touch the guest's errno");
    ck(g_pread(fd, buf, 3, 0) == 3 && *g_errno() == 1234, "a successful pread does not either");
    ck(g_read(fd, buf, sizeof buf) == 0 && *g_errno() == 1234, "nor a read at end of file");
    close(fd);

    printf("neterrno: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
