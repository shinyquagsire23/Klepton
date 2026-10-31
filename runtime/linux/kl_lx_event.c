// eventfd, with its counter.
//
// The Android personality's eventfd (kl_libc_slink.c) is a self-connected
// datagram socket: one descriptor, readable once written. That is all Qt asks of
// it. Source 2 asks for the rest — its semaphores ARE eventfds, opened with
// EFD_SEMAPHORE, where each read takes exactly one and a write of N admits N
// readers — so here the counter is real.
//
// The descriptor the guest holds is the read end of a pipe, kept holding exactly
// one byte whenever the counter is non-zero. That one invariant is what makes
// poll, ppoll and epoll on an eventfd work with no knowledge of it: "readable"
// already means "the counter is not zero".
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kl_lx.h"

#define EFD_SEMAPHORE 1
#define EFD_NONBLOCK  0x800
#define EFD_CLOEXEC   0x80000

struct kl_lx_eventfd {
    pthread_mutex_t lock;
    uint64_t        count;
    int             wfd, sem;
};

int kl_lx_eventfd(unsigned initval, int flags) {
    int p[2];
    if (pipe(p) != 0) return -1;
    kl_lx_fdinfo *fi = kl_lx_fd(p[0]);
    struct kl_lx_eventfd *e = fi ? calloc(1, sizeof *e) : NULL;
    if (!e) { close(p[0]); close(p[1]); errno = EMFILE; return -1; }
    pthread_mutex_init(&e->lock, NULL);
    e->wfd = p[1];
    e->sem = flags & EFD_SEMAPHORE;
    e->count = initval;
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    if (flags & EFD_CLOEXEC)  fcntl(p[0], F_SETFD, FD_CLOEXEC);
    if (flags & EFD_NONBLOCK) fcntl(p[0], F_SETFL, O_NONBLOCK);
    if (initval && write(p[1], "", 1) != 1) { /* a fresh pipe takes one byte */ }
    memset(fi, 0, sizeof *fi);
    strlcpy(fi->guest, "anon_inode:[eventfd]", sizeof fi->guest);
    fi->efd = e;
    return p[0];
}

void kl_lx_eventfd_close(struct kl_lx_eventfd *e) {
    close(e->wfd);
    pthread_mutex_destroy(&e->lock);
    free(e);
}

int64_t kl_lx_eventfd_write(struct kl_lx_eventfd *e, const void *buf, size_t n) {
    uint64_t v;
    if (n < 8) { errno = EINVAL; return -1; }
    memcpy(&v, buf, 8);
    if (v == UINT64_MAX) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&e->lock);
    int was_zero = e->count == 0;
    e->count += v;
    if (was_zero && e->count && write(e->wfd, "", 1) != 1) { /* cannot fill: holds 0 bytes */ }
    pthread_mutex_unlock(&e->lock);
    return 8;
}

int64_t kl_lx_eventfd_read(int fd, struct kl_lx_eventfd *e, void *buf, size_t n) {
    if (n < 8) { errno = EINVAL; return -1; }
    for (;;) {
        pthread_mutex_lock(&e->lock);
        if (e->count) {
            uint64_t v = e->sem ? 1 : e->count;
            e->count -= v;
            char c;
            if (!e->count && read(fd, &c, 1) != 1) { /* the byte is ours and is there */ }
            pthread_mutex_unlock(&e->lock);
            memcpy(buf, &v, 8);
            return 8;
        }
        pthread_mutex_unlock(&e->lock);
        if (fcntl(fd, F_GETFL) & O_NONBLOCK) { errno = EAGAIN; return -1; }
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        if (poll(&pf, 1, -1) < 0 && errno != EINTR) return -1;
    }
}
