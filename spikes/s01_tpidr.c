// S0.1 — Darwin arm64 thread-pointer semantics probe
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <sched.h>

static inline uint64_t rd_tpidr(void)   { uint64_t v; __asm__ volatile("mrs %0, tpidr_el0"  :"=r"(v)); return v; }
static inline uint64_t rd_tpidrro(void) { uint64_t v; __asm__ volatile("mrs %0, tpidrro_el0":"=r"(v)); return v; }
static inline void     wr_tpidr(uint64_t v){ __asm__ volatile("msr tpidr_el0, %0" :: "r"(v)); }

static void *thr(void *arg) {
    printf("  thread %s: pthread_self=%016llx  tpidr_el0=%016llx  tpidrro_el0=%016llx\n",
           (char*)arg, (unsigned long long)(uintptr_t)pthread_self(),
           (unsigned long long)rd_tpidr(), (unsigned long long)rd_tpidrro());
    return NULL;
}

// run fn in a child so a fault is contained; returns 0 ok, else signal/exit code
static int sandbox(void (*fn)(void)) {
    pid_t p = fork();
    if (p == 0) { fn(); fflush(NULL); _exit(0); }
    int st; waitpid(p, &st, 0);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return WEXITSTATUS(st);
}

#define SENTINEL 0xDEADBEEF12340000ULL

static void t_readback(void) {
    uint64_t orig = rd_tpidr();
    wr_tpidr(SENTINEL);
    uint64_t got = rd_tpidr();
    wr_tpidr(orig);
    printf("  wrote %016llx -> read back %016llx  %s\n",
           (unsigned long long)SENTINEL, (unsigned long long)got,
           got == SENTINEL ? "[WRITABLE]" : "[NOT WRITABLE / IGNORED]");
}

static void t_ctxswitch(void) {
    uint64_t orig = rd_tpidr();
    wr_tpidr(SENTINEL);
    // force scheduler round-trips without touching libSystem TLS
    for (int i = 0; i < 5; i++) sched_yield();
    usleep(2000);
    uint64_t got = rd_tpidr();
    wr_tpidr(orig);
    printf("  after yields+sleep: %016llx  %s\n", (unsigned long long)got,
           got == SENTINEL ? "[PRESERVED ACROSS CONTEXT SWITCH]" : "[CLOBBERED BY KERNEL]");
}

static void t_libsystem(void) {
    // clobber and then exercise libSystem without restoring
    wr_tpidr(SENTINEL);
    void *m = malloc(64);            // allocator TLS caches
    memset(m, 0xAB, 64);
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    pthread_mutex_unlock(&mu);
    volatile int e = errno;          // errno is TLS
    (void)e;
    free(m);
    pthread_self();
    fprintf(stderr, "  libSystem survived TPIDR_EL0 clobber\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== A: which register carries thread identity ==\n");
    thr("main");
    pthread_t a, b;
    pthread_create(&a, NULL, thr, "A"); pthread_join(a, NULL);
    pthread_create(&b, NULL, thr, "B"); pthread_join(b, NULL);

    printf("\n== B: is TPIDR_EL0 writable from EL0 ==\n");
    int r = sandbox(t_readback);
    if (r) printf("  child died rc=%d (%s)\n", r, r==128+4?"SIGILL - trapped":"fault");

    printf("\n== C: does the kernel preserve a user-written TPIDR_EL0 ==\n");
    r = sandbox(t_ctxswitch);
    if (r) printf("  child died rc=%d\n", r);

    printf("\n== D: does libSystem break if TPIDR_EL0 is clobbered ==\n");
    r = sandbox(t_libsystem);
    printf("  -> rc=%d %s\n", r, r==0 ? "[libSystem INDEPENDENT of TPIDR_EL0]"
                                      : "[libSystem DEPENDS on TPIDR_EL0]");
    return 0;
}
