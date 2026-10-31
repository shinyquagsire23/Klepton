// tgkill/tkill with the null signal — `make tgkill`, in `make check`.
//
// Half-Life: Alyx's libvideo keeps a per-thread registry that probes every
// registered thread with tgkill(getpid(), tid, 0) and reclaims the slot of any
// thread the probe calls dead. The raw-syscall door used to refuse 131, so
// every live thread read as dead, the second VideoPlayer Decoder thread tore
// down the first one's state, and the first faulted on a NULL.
// The probe must answer "alive" for a live thread and ESRCH for a gone one,
// with tids from the same door libc++'s guards read (raw syscall 178).
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

long klb_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6);
#define SYS_TKILL 130
#define SYS_TGKILL 131
#define SYS_GETTID 178

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
    else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  c = PTHREAD_COND_INITIALIZER;
static int go, child_tid;

static void *child(void *arg) {
    (void)arg;
    pthread_mutex_lock(&m);
    child_tid = (int)klb_syscall(SYS_GETTID, 0, 0, 0, 0, 0, 0);
    pthread_cond_broadcast(&c);
    while (!go) pthread_cond_wait(&c, &m);
    pthread_mutex_unlock(&m);
    return NULL;
}

static long tg(long pid, long tid, long sig) { errno = 0; return klb_syscall(SYS_TGKILL, pid, tid, sig, 0, 0, 0); }

int main(void) {
    printf("=== tgkill/tkill null-signal liveness probe ===\n");
    int self = (int)klb_syscall(SYS_GETTID, 0, 0, 0, 0, 0, 0);
    CHECK(tg(getpid(), self, 0) == 0, "tgkill(getpid(), self, 0) == 0");
    CHECK(klb_syscall(SYS_TKILL, self, 0, 0, 0, 0, 0) == 0, "tkill(self, 0) == 0");

    pthread_t t;
    pthread_create(&t, NULL, child, NULL);
    pthread_mutex_lock(&m);
    while (!child_tid) pthread_cond_wait(&c, &m);
    pthread_mutex_unlock(&m);
    CHECK(child_tid != self, "the child has its own tid (%d vs %d)", child_tid, self);
    CHECK(tg(getpid(), child_tid, 0) == 0, "a live sibling thread reads as alive");

    pthread_mutex_lock(&m); go = 1; pthread_cond_broadcast(&c); pthread_mutex_unlock(&m);
    pthread_join(t, NULL);
    long r = -2;
    // The joined thread's kernel thread can outlive pthread_join by a moment.
    for (int i = 0; i < 200 && (r = tg(getpid(), child_tid, 0)) == 0; i++) usleep(1000);
    CHECK(r == -1 && errno == ESRCH, "an exited thread reads as gone (ESRCH), r=%ld errno=%d", r, errno);

    CHECK(tg(getpid() + 1, self, 0) == -1 && errno == ESRCH, "another tgid is ESRCH");
    CHECK(tg(getpid(), 0, 0) == -1 && errno == EINVAL, "tid 0 is EINVAL");
    // A real signal is still refused (a Darwin default action would kill us).
    CHECK(tg(getpid(), self, 10) == -1 && errno == ENOSYS, "a real signal (Linux SIGUSR1) is still refused");

    printf(fails ? "tgkill: %d FAILED\n" : "tgkill: all passed\n", fails);
    return fails != 0;
}
