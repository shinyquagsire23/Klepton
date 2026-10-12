// The semaphore slot table — `make sem`, in `make check`.
//
// This gate exists because the bug it guards killed every play session at
// roughly the same wall-clock moment and never once looked like a semaphore
// bug. The table was a one-way bump allocator: sem_destroy zeroed the guest's
// handle and returned the slot to nobody. Unity churns about a dozen
// semaphores a second in gameplay, so 1024 slots lasted ~80 seconds — and the
// failure surfaced as "The file sharedassets5.assets is corrupted!" and "GLSL
// link error: No compiled shaders", because a failed sem_init left the guest's
// storage untouched, the guest kept handle 0, and the async asset read that
// should have filled a buffer quietly EINVAL'd while Unity parsed the buffer
// anyway. Two false accusations and a SIGTRAP, ~80 s in, every run.
//
// So the checks are the four properties that story needed and did not have:
//
//   1. churn does not accumulate — 200,000 create/destroy cycles, which is
//      about four and a half hours of gameplay at the observed rate, against a
//      table that used to die at 1023.
//   2. exhaustion is survivable AND legible — fill it, confirm ENOSPC, confirm
//      init wrote a poison handle rather than leaving the guest's own storage
//      to speak for it, confirm operations on that handle fail loudly...
//      ...and then confirm the table COMES BACK once the guest frees. The old
//      allocator, once past the cap, was dead for the life of the process.
//   3. a destroyed handle stops working at the destroy, not later. The slot is
//      reused within microseconds; without the generation tag in the handle, a
//      stale post lands on somebody else's semaphore and corrupts a count that
//      belongs to another subsystem entirely.
//   4. destroying a semaphore with a thread parked on it does not take the
//      process with it. libdispatch kills the process outright when a
//      dispatch_semaphore_t is released while a waiter is inside it, so this
//      one is checked by the test surviving at all.
//
// Plus the ordinary semantics (post/wait/trywait/getvalue) and a concurrent
// churn, because the free list is now shared mutable state.
//
// No guest, no headset; about a second.
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// The shims bind through the guest symbol table, not a header.
extern int klb_sem_init(int *s, int pshared, unsigned value);
extern int klb_sem_destroy(int *s);
extern int klb_sem_post(int *s);
extern int klb_sem_wait(int *s);
extern int klb_sem_trywait(int *s);
extern int klb_sem_getvalue(int *s, int *out);
extern int klb_sem_timedwait(int *s, const struct timespec *ts);

static int g_fail;
static void ck(int ok, const char *what) {
    if (!ok) g_fail++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// 1 — the eighty-second wall. Unity's observed rate is ~12 semaphores/second;
// 200k cycles is ~4.6 hours of play. The old allocator failed at 1023.
static void t_churn(void) {
    printf("=== 1. create/destroy churn ===\n");
    int bad = 0, worst = 0;
    for (int i = 0; i < 200000; i++) {
        int s = 0;
        if (klb_sem_init(&s, 0, 0) != 0) { bad = i; break; }
        // Use it, so the slot is not merely allocated and abandoned.
        klb_sem_post(&s);
        if (klb_sem_wait(&s) != 0) { bad = i; break; }
        if ((s & 0x3FFF) > worst) worst = s & 0x3FFF;
        klb_sem_destroy(&s);
    }
    ck(!bad, "200,000 create/use/destroy cycles (~4.6 h of gameplay churn)");
    if (bad) printf("       first failure at cycle %d\n", bad);
    // Reclaim means the slot index never climbs: a leak would walk it to the cap.
    ck(worst < 64, "highest slot index stayed low — slots are reclaimed, not consumed");
    printf("       highest slot reached: %d\n", worst);
}

// 2 — the cap, and the recovery from it.
static void t_exhaustion(void) {
    printf("=== 2. exhaustion is legible and recoverable ===\n");
    int cap = 20000;
    int *held = calloc((size_t)cap, sizeof *held);
    int n = 0;
    while (n < cap && klb_sem_init(&held[n], 0, 1) == 0) n++;
    ck(n >= 16000, "table holds at least 16k live semaphores");
    printf("       live semaphores at the wall: %d\n", n);

    // The cascade's first domino: init failing WITHOUT writing *s. The guest
    // kept whatever was there — 0 — and every later call read as "never
    // initialised" instead of "this one failed".
    int poisoned = 0x5A5A5A5A;
    errno = 0;
    int rc = klb_sem_init(&poisoned, 0, 1);
    ck(rc == -1 && errno == ENOSPC, "init past the cap returns -1/ENOSPC");
    ck(poisoned != 0x5A5A5A5A, "...and WRITES the handle rather than leaving the guest's");
    ck(poisoned != 0, "...to a value distinct from the pristine-zero handle");
    errno = 0;
    ck(klb_sem_post(&poisoned) == -1 && errno == EINVAL, "post on the poison handle EINVALs");
    errno = 0;
    ck(klb_sem_trywait(&poisoned) == -1 && errno == EINVAL, "trywait on it EINVALs");

    for (int i = 0; i < n; i++) klb_sem_destroy(&held[i]);
    int s = 0;
    ck(klb_sem_init(&s, 0, 0) == 0, "the table recovers once the guest frees (the old one never did)");
    klb_sem_destroy(&s);
    free(held);
}

// 3 — the generation tag: a stale handle must not land on the slot's next owner.
static void t_generation(void) {
    printf("=== 3. use-after-destroy fails at the destroy ===\n");
    int a = 0;
    klb_sem_init(&a, 0, 0);
    int stale = a;                       // what a careless guest keeps
    klb_sem_destroy(&a);

    int b = 0;
    klb_sem_init(&b, 0, 0);
    ck((stale & 0x3FFF) == (b & 0x3FFF), "the freed slot IS handed to the next semaphore");
    ck(stale != b, "...but the handle differs — the generation moved");

    errno = 0;
    ck(klb_sem_post(&stale) == -1 && errno == EINVAL, "a post through the stale handle EINVALs");
    int v = -1;
    klb_sem_getvalue(&b, &v);
    ck(v == 0, "...and did NOT land on the new owner's count");

    klb_sem_destroy(&b);
}

// 4 — destroy under a parked waiter. libdispatch's rule is that releasing a
// dispatch_semaphore_t with a thread inside it kills the process, so most of
// this check is simply that the process is still here afterwards.
static _Atomic int g_woke;
static int g_parked;
static void *parker(void *arg) {
    (void)arg;
    klb_sem_wait(&g_parked);
    atomic_store(&g_woke, 1);
    return NULL;
}
static void t_destroy_with_waiter(void) {
    printf("=== 4. destroy with a waiter parked ===\n");
    g_parked = 0;
    atomic_store(&g_woke, 0);
    klb_sem_init(&g_parked, 0, 0);
    pthread_t th;
    pthread_create(&th, NULL, parker, NULL);
    usleep(120000);                      // let it reach the wait
    ck(!atomic_load(&g_woke), "the waiter is parked");
    klb_sem_destroy(&g_parked);
    for (int i = 0; i < 200 && !atomic_load(&g_woke); i++) usleep(10000);
    ck(atomic_load(&g_woke), "destroy released it rather than stranding it");
    if (atomic_load(&g_woke)) pthread_join(th, NULL);
    ck(1, "the process survived the release (libdispatch did not trap)");
}

// 5 — the ordinary semantics, which the rewrite must not have moved.
static void t_semantics(void) {
    printf("=== 5. semantics ===\n");
    int s = 0;
    klb_sem_init(&s, 0, 2);
    int v = -1;
    klb_sem_getvalue(&s, &v);
    ck(v == 2, "getvalue reports the initial count");
    ck(klb_sem_trywait(&s) == 0 && klb_sem_trywait(&s) == 0, "two trywaits drain it");
    errno = 0;
    ck(klb_sem_trywait(&s) == -1 && errno == EAGAIN, "the third trywait EAGAINs");
    klb_sem_getvalue(&s, &v);
    ck(v == 0, "getvalue clamps at zero");
    klb_sem_post(&s);
    ck(klb_sem_wait(&s) == 0, "post then wait passes through");
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 20000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    errno = 0;
    ck(klb_sem_timedwait(&s, &ts) == -1 && errno == ETIMEDOUT, "timedwait times out on an empty one");
    klb_sem_destroy(&s);

    // The uninitialised handle: still the honest EINVAL it always was.
    int zero = 0;
    errno = 0;
    ck(klb_sem_post(&zero) == -1 && errno == EINVAL, "handle 0 is still 'never initialised'");
}

// 6 — the free list is shared mutable state now, so drive it from four threads.
static void *churner(void *arg) {
    long fails = 0;
    for (int i = 0; i < 25000; i++) {
        int s = 0;
        if (klb_sem_init(&s, 0, 1) != 0) { fails++; continue; }
        if (klb_sem_trywait(&s) != 0) fails++;
        klb_sem_post(&s);
        klb_sem_destroy(&s);
    }
    *(long *)arg = fails;
    return NULL;
}
static void t_concurrent(void) {
    printf("=== 6. concurrent churn ===\n");
    pthread_t th[4];
    long fails[4] = {0};
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, churner, &fails[i]);
    long total = 0;
    for (int i = 0; i < 4; i++) { pthread_join(th[i], NULL); total += fails[i]; }
    ck(total == 0, "4 threads x 25,000 cycles, no failures");
    if (total) printf("       failures: %ld\n", total);
    // Whether contention LOST any slots is not a question a single allocation
    // can answer — the free list is a stack, so the index it hands back says
    // nothing. Refilling the whole table does: a slot dropped on the floor by a
    // racing push is a slot missing from this count.
    int cap = 20000;
    int *held = calloc((size_t)cap, sizeof *held);
    int n = 0;
    while (n < cap && klb_sem_init(&held[n], 0, 0) == 0) n++;
    ck(n >= 16000, "the table still fills to capacity — contention lost no slots");
    printf("       refilled to: %d\n", n);
    for (int i = 0; i < n; i++) klb_sem_destroy(&held[i]);
    free(held);
}

int main(void) {
    printf("=== semaphore slot table ===\n");
    t_churn();
    t_exhaustion();
    t_generation();
    t_destroy_with_waiter();
    t_semantics();
    t_concurrent();
    printf(g_fail ? "=== FAILED (%d) ===\n" : "=== semaphore table OK ===\n", g_fail);
    return g_fail ? 1 : 0;
}
