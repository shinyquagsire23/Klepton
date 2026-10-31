// The guest-mutex table under create/destroy churn. `make mtxtable`, in `make check`.
//
// This gate exists because the failure it guards has no diagnostic of its own
// and no fixed place: a game stops in a loading screen with every thread
// waiting, at a different point each time, and runs through on the next try.
// kl_pthread.c keeps one host mutex per guest ADDRESS in a table it probes
// linearly. pthread_mutex_destroy used to clear the slot, which cuts the chain
// of every mutex stored behind it: that mutex is then not found, its next
// unlock goes to a second entry made in the gap, and the first host mutex
// stays locked under a key that is still in the table. Once the gap is filled
// again, a NEW mutex at that address is the stranded one — locked by a thread
// that released it long ago.
//
// It needs nothing but the table, so it is driven here through the same klb_*
// entry points a guest calls, on "guest" addresses in a plain arena:
//
//   1. the sequence itself: a mutex held while the one made just before it is
//      destroyed, then released and destroyed, both addresses used again — and
//      no new mutex may start out locked;
//   2. the same through a cond wait, which is how it happens to a thread that
//      does nothing but sleep on an event: the wait re-takes the mutex when it
//      wakes, and the unlock after it is the call that misses;
//   3. a mutex destroyed while HELD does not hand its lock to the next mutex
//      at that address;
//   4. mutual exclusion while other threads destroy and re-create older
//      mutexes around the ones in use;
//   5. a destroyed mutex gives its place back: a million made and destroyed
//      one after another must not leave a million entries (a table that only
//      grows is full after an evening of an Unreal game);
//   6. threads that first-lock one brand-new mutex at the same moment all get
//      the SAME one — the place where a freed entry is taken over is also the
//      place where two could be made.
//
// A stranded lock shows up in 4 as a thread that never comes back, so the run
// is bounded by an alarm: dying on SIGALRM is the failure.
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int klb_pthread_mutex_init(void *m, const void *a);
int klb_pthread_mutex_lock(void *m);
int klb_pthread_mutex_unlock(void *m);
int klb_pthread_mutex_trylock(void *m);
int klb_pthread_mutex_destroy(void *p);
int klb_pthread_cond_init(void *c, const void *a);
int klb_pthread_cond_wait(void *c, void *m);
int klb_pthread_cond_broadcast(void *c);
unsigned kl_pthread_mutex_entries(void);

static int g_fail;
static void ck(int ok, const char *what) {
    if (!ok) g_fail++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// "Guest" mutexes: bionic's pthread_mutex_t is 40 bytes, 4-aligned, and the
// table never reads it — only where it is matters.
#define STEP 64
static char *g_arena;
static void *at(unsigned i) { return g_arena + (size_t)i * STEP; }

// How many of [0, n) another thread finds locked. A mutex nobody holds must
// answer 0 to a trylock from any thread; the host mutexes are recursive, so
// the asking has to be done from a thread that is not the one under test.
struct probe { unsigned from, n, step, busy; };
static void *probe_run(void *arg) {
    struct probe *p = arg;
    for (unsigned i = p->from; i < p->n; i += p->step) {
        if (klb_pthread_mutex_trylock(at(i)) != 0) { p->busy++; continue; }
        klb_pthread_mutex_unlock(at(i));
    }
    return NULL;
}
static unsigned busy_among(unsigned from, unsigned n, unsigned step) {
    struct probe p = { from, n, step, 0 };
    pthread_t t;
    pthread_create(&t, NULL, probe_run, &p);
    pthread_join(t, NULL);
    return p.busy;
}

#define PAIRS 50000
#define CHURN 1000000

// ---- 2: sleepers on a cond while the mutex in front of theirs goes away ----
#define SLEEPERS 16
static _Atomic int g_asleep;
static _Atomic int g_go;
struct sleeper { void *m, *c; };
static void *sleeper_run(void *arg) {
    struct sleeper *s = arg;
    klb_pthread_mutex_lock(s->m);
    atomic_fetch_add(&g_asleep, 1);
    while (!atomic_load(&g_go)) klb_pthread_cond_wait(s->c, s->m);
    klb_pthread_mutex_unlock(s->m);
    return NULL;
}

// ---- 4: exclusion under churn ----
#define SHARED   64
#define WORKERS  8
#define ROUNDS   20000
#define PRIVATE  4096
static unsigned g_shared_base, g_private_base;
static long g_counter[SHARED];              // plain: the mutex is what guards it
static void *worker_run(void *arg) {
    unsigned w = (unsigned)(uintptr_t)arg, seed = 0x9E3779B9u * (w + 1);
    for (unsigned r = 0; r < ROUNDS; r++) {
        seed = seed * 1664525u + 1013904223u;
        unsigned s = (seed >> 8) % SHARED;
        // One of this thread's own mutexes — all of them older than the shared
        // ones — goes away and comes back around each use of a shared one, as
        // an object with a critical section does in a guest.
        void *mine = at(g_private_base + w * PRIVATE + (seed >> 16) % PRIVATE);
        klb_pthread_mutex_lock(mine);
        klb_pthread_mutex_lock(at(g_shared_base + s));
        long v = g_counter[s];
        if ((r & 63) == 0) sched_yield();
        g_counter[s] = v + 1;
        klb_pthread_mutex_unlock(at(g_shared_base + s));
        klb_pthread_mutex_unlock(mine);
        klb_pthread_mutex_destroy(mine);
        klb_pthread_mutex_init(mine, NULL);
    }
    return NULL;
}

// ---- 6: one brand-new mutex, first-locked by every thread at once ----
#define FIRSTS 3000
static unsigned g_first_base;
static _Atomic int g_round = -1, g_round_done;
static long g_first_counter[FIRSTS];
static void *first_run(void *arg) {
    (void)arg;
    for (int r = 0; r < FIRSTS; r++) {
        while (atomic_load(&g_round) != r) sched_yield();
        void *m = at(g_first_base + (unsigned)r);
        klb_pthread_mutex_lock(m);              // never initialised: made by whoever is first
        long v = g_first_counter[r];
        if ((r & 7) == 0) sched_yield();
        g_first_counter[r] = v + 1;
        klb_pthread_mutex_unlock(m);
        atomic_fetch_add(&g_round_done, 1);
    }
    return NULL;
}

int main(void) {
    g_arena = malloc((size_t)(2 * PAIRS + 2 * SLEEPERS + WORKERS * PRIVATE + SHARED + CHURN + FIRSTS) * STEP);
    if (!g_arena) { printf("FAIL: no arena\n"); return 1; }
    setvbuf(stdout, NULL, _IOLBF, 0);       // a run the alarm ends still says how far it got
    alarm(120);
    printf("=== guest-mutex table under create/destroy ===\n");
    char msg[240];

    // 1. The sequence, pair after pair: B is held while A — made just before
    // it, so wherever the table chains, A is the one in front — is destroyed;
    // B is released and destroyed; both addresses are used again. The pairs
    // stay behind, so the table fills as it does in a long session.
    unsigned busy = 0;
    for (unsigned k = 0; k < PAIRS; k++) {
        void *A = at(2 * k), *B = at(2 * k + 1);
        klb_pthread_mutex_init(A, NULL);
        klb_pthread_mutex_init(B, NULL);
        klb_pthread_mutex_lock(B);
        klb_pthread_mutex_destroy(A);
        klb_pthread_mutex_unlock(B);
        klb_pthread_mutex_destroy(B);
        klb_pthread_mutex_init(A, NULL);
        klb_pthread_mutex_init(B, NULL);
    }
    busy = busy_among(0, 2 * PAIRS, 1);
    snprintf(msg, sizeof msg, "%d pairs, one destroyed while the other was held: %u "
             "of the mutexes made at those addresses afterwards start out locked",
             PAIRS, busy);
    ck(busy == 0, msg);

    // 2. The same through a cond wait: a sleeper on each B, every A destroyed
    // while they sleep.
    unsigned base = 2 * PAIRS;
    static struct sleeper sl[SLEEPERS];
    static char conds[SLEEPERS][STEP];
    pthread_t st[SLEEPERS];
    for (unsigned k = 0; k < SLEEPERS; k++) {
        klb_pthread_mutex_init(at(base + 2 * k), NULL);
        klb_pthread_mutex_init(at(base + 2 * k + 1), NULL);
        sl[k].m = at(base + 2 * k + 1);
        sl[k].c = conds[k];
        klb_pthread_cond_init(sl[k].c, NULL);
        pthread_create(&st[k], NULL, sleeper_run, &sl[k]);
    }
    while (atomic_load(&g_asleep) < SLEEPERS) usleep(1000);
    usleep(20000);                              // ...and into the wait itself
    for (unsigned k = 0; k < SLEEPERS; k++) klb_pthread_mutex_destroy(at(base + 2 * k));
    for (unsigned k = 0; k < SLEEPERS; k++) {
        klb_pthread_mutex_lock(sl[k].m);
        atomic_store(&g_go, 1);
        klb_pthread_cond_broadcast(sl[k].c);
        klb_pthread_mutex_unlock(sl[k].m);
    }
    for (unsigned k = 0; k < SLEEPERS; k++) pthread_join(st[k], NULL);
    for (unsigned k = SLEEPERS; k-- > 0; ) klb_pthread_mutex_destroy(sl[k].m);
    for (unsigned k = 0; k < SLEEPERS; k++) klb_pthread_mutex_init(at(base + 2 * k), NULL);
    for (unsigned k = 0; k < SLEEPERS; k++) klb_pthread_mutex_init(sl[k].m, NULL);
    busy = busy_among(base + 1, base + 2 * SLEEPERS, 2);
    snprintf(msg, sizeof msg, "%d sleepers woke after the mutex in front of theirs was "
             "destroyed: %u of their mutexes stayed locked behind them", SLEEPERS, busy);
    ck(busy == 0, msg);

    // 3. Destroyed while held.
    void *held = at(0);
    klb_pthread_mutex_lock(held);
    klb_pthread_mutex_destroy(held);
    klb_pthread_mutex_init(held, NULL);
    ck(busy_among(0, 1, 1) == 0, "a mutex destroyed while held leaves the next one "
                                 "at its address unlocked");

    // 4. Exclusion while older mutexes come and go around the ones in use.
    g_private_base = base + 2 * SLEEPERS;
    g_shared_base = g_private_base + WORKERS * PRIVATE;
    for (unsigned i = 0; i < WORKERS * PRIVATE; i++)
        klb_pthread_mutex_init(at(g_private_base + i), NULL);
    for (unsigned s = 0; s < SHARED; s++) klb_pthread_mutex_init(at(g_shared_base + s), NULL);
    pthread_t wt[WORKERS];
    for (unsigned w = 0; w < WORKERS; w++)
        pthread_create(&wt[w], NULL, worker_run, (void *)(uintptr_t)w);
    for (unsigned w = 0; w < WORKERS; w++) pthread_join(wt[w], NULL);
    long total = 0;
    for (unsigned s = 0; s < SHARED; s++) total += g_counter[s];
    snprintf(msg, sizeof msg, "%d threads, %d rounds each, a mutex destroyed and made "
             "again per round: %ld of %ld increments kept", WORKERS, ROUNDS, total,
             (long)WORKERS * ROUNDS);
    ck(total == (long)WORKERS * ROUNDS, msg);
    busy = busy_among(g_shared_base, g_shared_base + SHARED, 1);
    snprintf(msg, sizeof msg, "...and %u of the %d shared mutexes are left locked",
             busy, SHARED);
    ck(busy == 0, msg);

    // 5. A million mutexes, each made, used and destroyed before the next.
    unsigned churn_base = g_shared_base + SHARED, had = kl_pthread_mutex_entries();
    for (unsigned i = 0; i < CHURN; i++) {
        void *m = at(churn_base + i);
        klb_pthread_mutex_init(m, NULL);
        klb_pthread_mutex_lock(m);
        klb_pthread_mutex_unlock(m);
        klb_pthread_mutex_destroy(m);
    }
    unsigned grew = kl_pthread_mutex_entries() - had;
    snprintf(msg, sizeof msg, "%d mutexes made and destroyed one after another: the "
             "table grew by %u entries", CHURN, grew);
    ck(grew < CHURN / 4, msg);

    // 6. First lock of a brand-new mutex from every thread at once, on
    // addresses whose buckets hold freed entries from 5.
    g_first_base = churn_base + CHURN;
    pthread_t ft[WORKERS];
    for (unsigned w = 0; w < WORKERS; w++) pthread_create(&ft[w], NULL, first_run, NULL);
    for (int r = 0; r < FIRSTS; r++) {
        atomic_store(&g_round_done, 0);
        atomic_store(&g_round, r);
        while (atomic_load(&g_round_done) < WORKERS) sched_yield();
        klb_pthread_mutex_destroy(at(g_first_base + (unsigned)r));
    }
    for (unsigned w = 0; w < WORKERS; w++) pthread_join(ft[w], NULL);
    unsigned wrong = 0;
    for (int r = 0; r < FIRSTS; r++) if (g_first_counter[r] != WORKERS) wrong++;
    snprintf(msg, sizeof msg, "%d brand-new mutexes first-locked by %d threads at once: "
             "%u of them did not keep the threads apart", FIRSTS, WORKERS, wrong);
    ck(wrong == 0, msg);

    printf("%s: guest-mutex table\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
