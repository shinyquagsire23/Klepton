// bionic-layout pthread / semaphore shims.
//
// Guest objects are embedded in guest structs at bionic's sizes, so none of these
// can be forwarded to Darwin's larger types:
//
//   type              bionic (LP64)      Darwin
//   pthread_mutex_t    40 B / align 4      64 B / align 8
//   pthread_cond_t     48 B / align 4      48 B / align 8
//   pthread_rwlock_t   56 B / align 4     200 B / align 8
//   pthread_attr_t     56 B / align 8      64 B
//   pthread_once_t      4 B                16 B
//   pthread_key_t       4 B                 8 B
//   sem_t               4 B                opaque
//   pthread_t           8 B                 8 B  <- both pointer-sized, passes through
//
// Strategy: the guest's own storage holds a handle to a real Darwin object that we
// create lazily. bionic's static initialisers are all-zero, so 0 reliably means
// "not yet created" and PTHREAD_MUTEX_INITIALIZER keeps working.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <semaphore.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <time.h>
#include <dispatch/dispatch.h>
#include "klepton.h"
#include "kl_env.h"
#include "kl_x18.h"

// ---------- lazy handle in guest storage ----------
// CRITICAL: bionic's sync types are `int32_t __private[N]`, so guest objects are only
// **4-byte aligned**. Storing a 64-bit pointer in them and touching it with `ldar x`
// raises SIGBUS (EXC_ARM_DA_ALIGN) -- observed at libil2cpp's DT_INIT_ARRAY, where a
// pthread_cond_t landed on a 4-mod-8 address. So the guest slot holds a 32-bit index
// into a side table instead of a pointer.
//
//   type              bionic size / align
//   pthread_mutex_t    40 B / 4    (int32_t[10])
//   pthread_cond_t     48 B / 4    (int32_t[12])
//   pthread_rwlock_t   56 B / 4    (int32_t[14])
#define KL_MAX_SYNC 65536
// Table slots recycle: with the JNI pool leak fixed, long runs otherwise die
// here — Unity creates and destroys a mutex or two per frame, and a leaked
// slot per destroy is 65536 creates at ~frame 40k (observed). The kind object
// behind a recycled slot is replaced, not shared; the old one leaks (a
// destroy is the guest saying nobody holds it).
#define SYNC_TABLE(kind, tab, count, init_expr)                                  \
    static kind *tab[KL_MAX_SYNC];                                               \
    static _Atomic uint32_t count = 1;              /* 0 means uninitialised */  \
    static uint32_t tab##_free[KL_MAX_SYNC];                                     \
    static _Atomic uint32_t tab##_nfree;                                         \
    static kind *tab##_get(void *g) {                                            \
        _Atomic uint32_t *slot = (_Atomic uint32_t *)(g);                        \
        uint32_t idx = atomic_load(slot);                                        \
        if (idx) return tab[idx];                                                \
        kind *fresh = malloc(sizeof *fresh);                                     \
        init_expr;                                                               \
        uint32_t mine;                                                           \
        uint32_t nf = atomic_load(&tab##_nfree);                                 \
        if (nf && atomic_compare_exchange_strong(&tab##_nfree, &nf, nf - 1))     \
            mine = tab##_free[nf - 1];                                           \
        else {                                                                   \
            mine = atomic_fetch_add(&count, 1);                                  \
            if (mine >= KL_MAX_SYNC) abort();                                    \
        }                                                                        \
        tab[mine] = fresh;                                                       \
        uint32_t expect = 0;                                                     \
        if (!atomic_compare_exchange_strong(slot, &expect, mine)) {              \
            /* lost the race; leak this slot rather than free a live object */   \
            return tab[expect];                                                  \
        }                                                                        \
        return fresh;                                                            \
    }                                                                            \
    static void tab##_recycle(void *g) {                                         \
        _Atomic uint32_t *slot = (_Atomic uint32_t *)(g);                        \
        uint32_t idx = atomic_exchange(slot, 0);                                 \
        if (!idx || idx >= count) return;                                        \
        uint32_t nf = atomic_fetch_add(&tab##_nfree, 1);                         \
        if (nf < KL_MAX_SYNC) tab##_free[nf] = idx;                              \
    }

SYNC_TABLE(pthread_rwlock_t, g_rwl, g_rwl_n, pthread_rwlock_init(fresh, NULL))

// ---------- mutexes: address-keyed map ----------
// The slot-in-guest-storage design (shared SYNC_TABLE above) aliases two
// logical mutexes onto one host object whenever guest storage carries a
// stale slot index — memory freed without pthread_mutex_destroy and reused,
// or a live 40-byte bionic mutex memcpy'd into a moved struct. Both were
// observed in one run: the libunity static at libunity+0x1237EE4 and an
// arena object shared slot 23, and the render thread held it via one
// address while the main thread waited via the other — a manufactured
// deadlock (2026-08-06 capture hang). So mutexes are keyed by guest
// *address* instead: a copied or recycled address is the same mutex, a
// different address is a different mutex, and guest storage is never read.
// Keyed by guest ADDRESS, and slots are only reclaimed on an explicit
// pthread_mutex_destroy — which many guests never call (freeing the containing
// object is legal on Linux, where a pthread_mutex_t needs no destroy). So the
// table fills monotonically with the working set of distinct mutex addresses.
// TWD2's UE4 load creates ~32k mutexes without destroying any and overflowed the
// old 32768 table: mtx_entry_for's unbounded probe then spun FOREVER on the first
// insert into a full table (the "abort past it" below was never implemented), which
// looked like a hang with FAsyncLoading pinned 99% in mtx_entry_for. 8x the size so
// a large UE4 load fits, and mtx_entry_for now bounds its probe (see there).
#define MTX_MAP_SIZE 262144         // power of two; mtx_entry_for degrades (not hangs) if full
typedef struct {
    _Atomic(uintptr_t)   key;       // guest address; 0 = empty
    // Atomic, and published AFTER the key with release ordering — see
    // mtx_entry_for. A plain pointer here was a publication race and it was
    // reached: the claiming thread stored the key, and any other thread that
    // probed the same address in that window found k == want, returned the
    // entry and called pthread_mutex_lock(NULL).
    _Atomic(pthread_mutex_t *) m;
    _Atomic(void *)      owner;     // owner tracking, dumped by kl_pthread_report
    _Atomic(void *)      locksite;
    // How deep the current owner is in. Our host mutexes are RECURSIVE, and a
    // recursive mutex held more than once cannot be handed to
    // pthread_cond_wait: Darwin's droplock refuses and the wait returns EINVAL
    // without sleeping, which the guest reads as a spurious wake and retries
    // forever. Counting is the only way to see that from here.
    _Atomic(int)         depth;
} mtx_entry;
static mtx_entry g_mtx_map[MTX_MAP_SIZE];

static unsigned mtx_hash(uintptr_t a) {
    return (unsigned)(((a >> 4) * 0x9E3779B97F4A7C15ULL) >> 49);
}

static pthread_mutex_t *mtx_make(void) {
    pthread_mutex_t *fresh = malloc(sizeof *fresh);
    if (!fresh) return NULL;
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(fresh, &a); pthread_mutexattr_destroy(&a);
    return fresh;
}

// ---------- condition variables: address-keyed, for the mutex map's reason ----
// Keyed on the guest ADDRESS, like the mutex map above and for the same reason:
// a slot index stored in guest storage aliases two logical objects onto one host
// object whenever that storage is stale or copied, which for a mutex is a
// manufactured deadlock.
//
// For a cond it is worse, because Darwin latches the mutex a
// condvar is waited on with: two guest condvars sharing one host condvar are
// waited on with two different host mutexes, the second `pthread_cond_wait`
// returns EINVAL **without sleeping**, and the guest's `do { wait } while
// (!triggered)` loop spins forever — starving the thread trying to signal it.
// It presents as a hang three subsystems away, with the spinning thread
// reported as asleep. Linux never sees it: NPTL does not latch the mutex.
//
// Same shape as the mutex map: create-on-demand, destroy leaks (a host cond is
// small, and freeing one a guest might still signal is worse), and guest
// storage is never read or written.
typedef struct {
    _Atomic(uintptr_t)       key;
    _Atomic(pthread_cond_t *) c;
    // The HOST mutex this cond was last waited on with, in the entry itself
    // because that is the only place it can mean anything. Indexed
    // by a slot read out of the guest's own storage (`g_cnd_mutex[*(uint32_t *)c]`)
    // it reads bionic's private cond state instead: a number with no relation to
    // this condvar, so the one check that can recognise the aliasing bug answers
    // about an unrelated entry.
    _Atomic(void *)          last_mutex;
    // Cond-deadlock instrumentation (KL_TRACE_CONDDUMP). `waiters` is how many
    // threads are asleep in this cond right now; `signals` counts every
    // signal+broadcast delivered to it; `last_sig_ns` is when the last one landed.
    // At a hang, waiters>0 with signals still climbing = a lost wakeup (the signal
    // reached a DIFFERENT host cond than the sleeper is on — aliasing/copy);
    // waiters>0 with signals frozen = nobody is signalling it at all.
    _Atomic(int)             waiters;
    _Atomic(uint64_t)        signals;
    _Atomic(uint64_t)        last_sig_ns;
    _Atomic(void *)          waiter_ra;   // guest call site of the last waiter here
    _Atomic(void *)          waiter_ra2;  // one frame deeper: the caller of that
} cnd_entry;
static cnd_entry g_cnd_map[MTX_MAP_SIZE];

static uint64_t cnd_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static pthread_cond_t *cnd_make(void) {
    pthread_cond_t *fresh = malloc(sizeof *fresh);
    if (fresh) pthread_cond_init(fresh, NULL);
    return fresh;
}

// The map ENTRY, for callers that need more than the host cond (the guard wants
// last_mutex). cnd() is this and one field.
static cnd_entry *cnd_entry_for(void *g) {
    uintptr_t want = (uintptr_t)g;
    pthread_cond_t *fresh = NULL;
    for (unsigned i = mtx_hash(want) & (MTX_MAP_SIZE - 1); ; i = (i + 1) & (MTX_MAP_SIZE - 1)) {
        uintptr_t k = atomic_load(&g_cnd_map[i].key);
        if (k == want) {
            if (fresh) { pthread_cond_destroy(fresh); free(fresh); }
            return &g_cnd_map[i];
        }
        if (k) continue;
        if (!fresh) fresh = cnd_make();
        uintptr_t expect = 0;
        if (!atomic_compare_exchange_strong(&g_cnd_map[i].key, &expect, want))
            { i--; continue; }
        atomic_store(&g_cnd_map[i].c, fresh);
        return &g_cnd_map[i];
    }
}

// The host cond behind an entry, with the mutex map's publication wait: the key
// is claimed before the cond is stored, so a reader that arrives between the
// two waits rather than dereferencing NULL.
static pthread_cond_t *cnd_host(cnd_entry *e) {
    pthread_cond_t *c = atomic_load(&e->c);
    while (!c) { sched_yield(); c = atomic_load(&e->c); }
    return c;
}
static pthread_cond_t *cnd(void *g) { return cnd_host(cnd_entry_for(g)); }


// The host mutex behind an entry. Separate from the lookup because the entry
// may be visible for a few instructions before its mutex is: the creator
// publishes the key with the CAS and the mutex immediately after, so a reader
// that arrives between the two waits rather than dereferencing NULL. Bounded by
// a handful of instructions on the creating thread, so a yield loop is right.
static pthread_mutex_t *mtx_host(mtx_entry *e) {
    pthread_mutex_t *m = atomic_load(&e->m);
    while (!m) { sched_yield(); m = atomic_load(&e->m); }
    return m;
}

// Find-or-create the host mutex for guest address g. Linear probe.
//
// The mutex is constructed BEFORE the key is claimed. This is the same ordering
// bug klb_sem_init already carries a comment about — fill the slot, then
// publish the index, never the other way round — and here it presented as
// libphonon's worker pool dying in pthread_mutex_lock(NULL) three frames into
// guest code, which reads as the guest's own null-mutex bug and is not.
// Several threads first-locking one brand-new mutex at once is exactly what a
// thread pool coming up does, which is why it was intermittent and why it
// picked the guest with the most thread pools.
// A single shared fallback used only when the whole table is full — better a few
// guest mutexes serialising on one recursive host mutex than an infinite probe
// spin. With MTX_MAP_SIZE at 262144 this should never be reached in practice; it
// exists so a pathological guest degrades instead of hanging (which is exactly
// what TWD2 did at the old 32768).
static mtx_entry g_mtx_overflow;
static mtx_entry *mtx_overflow(void) {
    if (!atomic_load(&g_mtx_overflow.m)) {
        pthread_mutex_t *m = mtx_make();
        pthread_mutex_t *expect = NULL;
        if (!atomic_compare_exchange_strong(&g_mtx_overflow.m, &expect, m) && m) {
            pthread_mutex_destroy(m); free(m);
        }
    }
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) == 0)
        fprintf(stderr, "  [klb] mutex table FULL (%d slots) — sharing one fallback "
                        "mutex; sync correctness degraded (raise MTX_MAP_SIZE)\n",
                MTX_MAP_SIZE);
    return &g_mtx_overflow;
}
static mtx_entry *mtx_entry_for(void *g) {
    uintptr_t want = (uintptr_t)g;
    pthread_mutex_t *fresh = NULL;
    unsigned probes = 0;
    for (unsigned i = mtx_hash(want) & (MTX_MAP_SIZE - 1); ; i = (i + 1) & (MTX_MAP_SIZE - 1)) {
        uintptr_t k = atomic_load(&g_mtx_map[i].key);
        if (k == want) {
            if (fresh) { pthread_mutex_destroy(fresh); free(fresh); }
            return &g_mtx_map[i];
        }
        if (k) {
            // Bound the probe: a full table (every slot occupied by a DIFFERENT
            // key) would otherwise loop forever. One full sweep proves it full.
            if (++probes >= MTX_MAP_SIZE) {
                if (fresh) { pthread_mutex_destroy(fresh); free(fresh); }
                return mtx_overflow();
            }
            continue;
        }
        // empty: build the mutex first, so claiming the key publishes a
        // complete entry. Kept across a lost race rather than rebuilt.
        if (!fresh) fresh = mtx_make();
        uintptr_t expect = 0;
        if (!atomic_compare_exchange_strong(&g_mtx_map[i].key, &expect, want))
            { i--; continue; }          // lost the race; re-read this slot
        atomic_store(&g_mtx_map[i].m, fresh);
        return &g_mtx_map[i];
    }
}

static mtx_entry *mtx_find(void *g) {
    uintptr_t want = (uintptr_t)g;
    for (unsigned i = mtx_hash(want) & (MTX_MAP_SIZE - 1); ; i = (i + 1) & (MTX_MAP_SIZE - 1)) {
        uintptr_t k = atomic_load(&g_mtx_map[i].key);
        if (k == want) return &g_mtx_map[i];
        if (!k) return NULL;
    }
}

static pthread_rwlock_t *rwl(void *g) { return g_rwl_get(g); }

// KL_TRACE_MUTEX=1: lifecycle of translated mutexes — init, destroy, and
// lazy creation — so aliasing (two guest addresses, one host object) can be
// attributed: copied storage keeps the same slot; freed-without-destroy
// storage keeps a stale one.
static int t_mtx(void) { static int t = -1; if (t < 0) t = kl_env_on("KL_TRACE_MUTEX", 0); return t; }
static void mtx_log(const char *what, void *g, uint32_t idx) {
    if (!t_mtx()) return;
    static _Atomic int n;
    if (atomic_fetch_add(&n, 1) < 200)
        fprintf(stderr, "  [klb] mutex %s: guest %p slot %u (tid %p, ra %p)\n",
                what, g, idx, (void *)pthread_self(),
                __builtin_return_address(0));
}

// ---------- mutex ----------
static struct { _Atomic(void *) tid, guest, ra; } g_mtx_waiters[64];

// ...and the same census for threads asleep in a CONDITION wait. They appear
// nowhere else: a cond wait releases its mutex, so such a thread is neither a
// holder nor a waiter, and the report could name every blocked thread except
// the one they were all blocked behind. RE4's engine boot is where that cost
// real time — two threads named, and the third, the one actually holding the
// boot open, invisible.
static struct { _Atomic(void *) tid, cond, guest, ra; } g_cnd_sleepers[64];

static void cnd_sleep_enter(void *c, void *guest, void *ra) {
    void *self = (void *)pthread_self();
    for (int i = 0; i < 64; i++) {
        void *expect = NULL;
        if (atomic_compare_exchange_strong(&g_cnd_sleepers[i].tid, &expect, self)) {
            atomic_store(&g_cnd_sleepers[i].cond, c);
            atomic_store(&g_cnd_sleepers[i].guest, guest);
            atomic_store(&g_cnd_sleepers[i].ra, ra);
            return;
        }
    }
}
// A cond wait that FAILS is always a bug and is otherwise perfectly silent: the
// guest sees a non-zero return, loops, and waits again — so the symptom is a
// thread that appears to be sleeping and is in fact spinning, which starves
// whoever is trying to signal it and reads as a deadlock somewhere else
// entirely. Named once per (error, site).
// Which mutex each host cond was last waited on with lives in the cond map's
// own entry (cnd_entry.last_mutex). Darwin latches the mutex into the condvar
// and refuses a second one with EINVAL; Linux does not, so a guest that reuses
// one condvar with two mutexes is legal there and fatal here, and this is what
// tells the two cases apart.

static void cnd_wait_failed(int err, void *c, void *m, void *ra, void *was) {
    if (!err) return;
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) >= 8) return;
    size_t off = 0;
    const char *img = kl_addr_image(ra, &off);
    fprintf(stderr, "  [klb] pthread_cond_wait FAILED: %s (%d) on guest cond %p "
                    "mutex %p, from ", strerror(err), err, c, m);
    if (img) fprintf(stderr, "%s+0x%zx", img, off); else fprintf(stderr, "%p", ra);
    fprintf(stderr, " [held %d deep]", atomic_load(&mtx_entry_for(m)->depth));
    void *now = (void *)mtx_host(mtx_entry_for(m));
    if (was && was != now)
        fprintf(stderr, " — this condvar was last waited on with HOST mutex %p "
                        "and is now given %p; Darwin refuses a second one", was, now);
    fprintf(stderr, "\n");
}

static void cnd_sleep_leave(void) {
    void *self = (void *)pthread_self();
    for (int i = 0; i < 64; i++) {
        void *expect = self;
        if (atomic_compare_exchange_strong(&g_cnd_sleepers[i].tid, &expect, NULL))
            return;
    }
}

static void mtx_wait_enter(void *guest, void *ra) {
    void *self = (void *)pthread_self();
    for (int i = 0; i < 64; i++) {
        void *expect = NULL;
        if (atomic_compare_exchange_strong(&g_mtx_waiters[i].tid, &expect, self)) {
            atomic_store(&g_mtx_waiters[i].guest, guest);
            atomic_store(&g_mtx_waiters[i].ra, ra);
            return;
        }
    }
}
static void mtx_wait_leave(void) {
    void *self = (void *)pthread_self();
    for (int i = 0; i < 64; i++) {
        void *expect = self;
        if (atomic_compare_exchange_strong(&g_mtx_waiters[i].tid, &expect, NULL))
            return;
    }
}

// Semaphore-waiter tracking, mirroring g_mtx_waiters. A thread stuck in
// klb_sem_wait (dispatch_semaphore_wait FOREVER) is invisible to the cond and
// mutex dumps and often to the sampler (the game/main thread is not sampled), so
// this is the last place a stalled producer can hide. Populated for the duration
// of every blocking sem wait; the watchdog dumps whoever is still parked.
static struct { _Atomic(void *) tid, guest, ra; } g_sem_waiters[64];
static void sem_wait_enter(void *guest, void *ra) {
    void *self = (void *)pthread_self();
    for (int i = 0; i < 64; i++) {
        void *expect = NULL;
        if (atomic_compare_exchange_strong(&g_sem_waiters[i].tid, &expect, self)) {
            atomic_store(&g_sem_waiters[i].guest, guest);
            atomic_store(&g_sem_waiters[i].ra, ra);
            return;
        }
    }
}
static void sem_wait_leave(void) {
    void *self = (void *)pthread_self();
    for (int i = 0; i < 64; i++) {
        void *expect = self;
        if (atomic_compare_exchange_strong(&g_sem_waiters[i].tid, &expect, NULL))
            return;
    }
}

// Cond-deadlock watchdog. Every ~3s it dumps every cond that has sleepers, with
// its waiter count, total signals delivered, and how long since the last one.
// The read at a hang is the whole point: a cond with waiters>0 whose `signals`
// keeps CLIMBING is a lost wakeup (the signal reached a different host cond than
// the sleeper is parked on — an aliased/copied condvar); one whose `signals` is
// FROZEN (or zero) is never signalled at all (a missing signal path, or a wait
// with no corresponding wake). Default ON for the targets that deadlock in the
// all-cvwait state so no env is needed on device; KL_TRACE_CONDDUMP overrides.
static void *cnd_watchdog(void *arg) {
    (void)arg;
    for (;;) {
        struct timespec s = { .tv_sec = 3, .tv_nsec = 0 };
        nanosleep(&s, NULL);
        uint64_t now = cnd_now_ns();
        int shown = 0, total_waiters = 0;
        for (unsigned i = 0; i < MTX_MAP_SIZE; i++) {
            int w = atomic_load(&g_cnd_map[i].waiters);
            if (w <= 0) continue;
            total_waiters += w;
            uint64_t sig = atomic_load(&g_cnd_map[i].signals);
            uint64_t ls  = atomic_load(&g_cnd_map[i].last_sig_ns);
            double age = ls ? (double)(now - ls) / 1e9 : -1.0;
            // A cond is a deadlock SUSPECT when it has a sleeper but nothing is
            // waking it: never signalled, or the last signal is old. Those get
            // logged unconditionally (with the waiter's call site to symbolise),
            // healthy re-waiting pools only up to a cap.
            int stuck = (sig == 0) || (ls && (now - ls) > 5000000000ull);
            if (stuck || shown < 24) {
                void *ra  = atomic_load(&g_cnd_map[i].waiter_ra);
                void *ra2 = atomic_load(&g_cnd_map[i].waiter_ra2);
                fprintf(stderr, "  [cnd] %#llx waiters=%d signals=%llu "
                        "last_signal=%.1fs waiter_ra=%p caller=%p%s\n",
                        (unsigned long long)atomic_load(&g_cnd_map[i].key), w,
                        (unsigned long long)sig, age, ra, ra2,
                        stuck ? "   <== STUCK (no wake coming)" : "");
            }
            shown++;
        }
        if (shown)
            fprintf(stderr, "  [cnd] --- %d cond(s) sleeping, %d waiter(s) total "
                    "(signals climbing => lost wakeup; frozen => never signalled)\n",
                    shown, total_waiters);
        // Threads currently BLOCKED on a guest mutex (g_mtx_waiters is populated
        // for the duration of every pthread_mutex_lock that has to wait). This is
        // where a producer thread hides when it is not on a cond or a semaphore —
        // e.g. the game thread stalled trying to take a lock a stuck thread holds.
        // For each, name the waiter's call site and the mutex's current owner +
        // the site that took it, so a lock-order / held-forever deadlock is legible.
        int mshown = 0;
        for (int i = 0; i < 64; i++) {
            void *tid = atomic_load(&g_mtx_waiters[i].tid);
            if (!tid) continue;
            void *g  = atomic_load(&g_mtx_waiters[i].guest);
            void *ra = atomic_load(&g_mtx_waiters[i].ra);
            void *owner = NULL, *locksite = NULL; int depth = 0;
            if (g) { mtx_entry *me = mtx_entry_for(g);
                     owner = atomic_load(&me->owner);
                     locksite = atomic_load(&me->locksite);
                     depth = atomic_load(&me->depth); }
            fprintf(stderr, "  [mtx] tid=%p BLOCKED on %p (ra=%p) — held by owner=%p "
                    "locksite=%p depth=%d\n", tid, g, ra, owner, locksite, depth);
            mshown++;
        }
        if (mshown)
            fprintf(stderr, "  [mtx] --- %d thread(s) blocked on a mutex "
                    "(waiter ra + holder locksite name the lock-order deadlock)\n",
                    mshown);
        // ...and threads parked in a blocking sem_wait (see g_sem_waiters).
        for (int i = 0; i < 64; i++) {
            void *tid = atomic_load(&g_sem_waiters[i].tid);
            if (!tid) continue;
            fprintf(stderr, "  [sem] tid=%p BLOCKED in sem_wait on %p (ra=%p)\n",
                    tid, atomic_load(&g_sem_waiters[i].guest),
                    atomic_load(&g_sem_waiters[i].ra));
        }
    }
    return NULL;
}
static void cnd_watchdog_launch(void) {
    extern const char *kl_driver_target_name(void);
    const char *t = kl_driver_target_name();
    int dflt = t && (strcmp(t, "wrath2") == 0 || strcmp(t, "wanderer") == 0 ||
                     strcmp(t, "twd2") == 0);
    if (!kl_env_on("KL_TRACE_CONDDUMP", dflt)) return;
    pthread_t th;
    if (pthread_create(&th, NULL, cnd_watchdog, NULL) == 0) pthread_detach(th);
}
static void cnd_watchdog_start(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, cnd_watchdog_launch);
}

// ---------- the return-code convention, and a trap it hides ----------
//
// pthread functions do NOT set errno — they RETURN the error code. So the errno
// translation that klb_errno() performs for the rest of libc never applied
// here, and the codes diverge above 34 exactly as they do everywhere else.
//
// It bit at Qt's QWaitCondition. pthread_cond_timedwait timed out legitimately,
// we returned Darwin's ETIMEDOUT (60), Qt compared against Linux's (110), did
// not recognise it as a timeout and reported it as a hard failure —
// `QWaitCondition::wait(): cv wait failure (Operation timed out)`, printed by
// its own strerror of the number it was handed, over and over — byte for byte
// IL2CPP's sem_timedwait failure in the neighbouring API.
//
// The three that actually differ and are reachable from a pthread call:
//   EDEADLK   Darwin 11  -> Linux 35   (pthread_join on self, recursive lock)
//   EAGAIN    Darwin 35  -> Linux 11   (pthread_create out of resources)
//   ETIMEDOUT Darwin 60  -> Linux 110  (the timed waits)
// EBUSY/EINVAL/EPERM/ENOMEM are below 34 and identical, which is why this went
// unnoticed for so long: the common failures all happened to agree.
static inline int px(int r) { return r ? kl_errno_to_linux(r) : 0; }

// A NULL mutex is the guest's bug, and it must not become ours. It also
// collides with the map's own sentinel: `key == 0` means "empty slot", so
// mtx_entry_for(NULL) matches the first empty slot it probes and hands back an
// entry with no mutex in it — the crash the caller was already heading for,
// relocated into this file and blamed on it.
//
// EINVAL is what POSIX says and what bionic returns, so a guest that checks
// gets the answer it expects; one that does not is no worse off than on
// Android. Named once, because the fact that a guest is doing this at all is
// the finding — libphonon's HRTF failure path leaves its worker pool with an
// uninitialised mutex, and the abort looked exactly like a runtime bug of ours.
static int mtx_null(const char *what) {
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) < 4)
        fprintf(stderr, "  [klb] pthread_mutex_%s(NULL) from the guest — refusing "
                        "with EINVAL (its own bug; on Android this is a crash too)\n",
                what);
    return kl_errno_to_linux(EINVAL);
}

int klb_pthread_mutex_init(void *m, const void *a) {
    (void)a;
    if (!m) return mtx_null("init");
    // Re-init of an address we already have a host mutex for KEEPS that mutex.
    //
    // Installing a fresh one instead — POSIX calls re-initialising a live mutex
    // undefined — is true of the GUEST's object and false of this one: a
    // condition variable latches the mutex it is waited on with, and on Darwin,
    // unlike Linux, presenting a second one is EINVAL. Swapping the host mutex
    // under a live condvar makes every later `pthread_cond_wait` fail INSTANTLY
    // without sleeping, which the guest reads as a spurious wake and retries
    // forever: a thread that looks asleep, is spinning, and starves the thread
    // trying to signal it.
    //
    // UE4 pools its `FPThreadEvent`s and re-inits the mutex of a recycled one
    // without re-initing its condvar, so RE4's engine boot wedges in
    // `FEngineLoop::PreInitPreStartupScreen` with the shader library's read task
    // waiting on an event nothing can trigger.
    // Keeping the object is also what the address-keyed map already says
    // everywhere else: a recycled address is the same mutex.
    mtx_entry *e = mtx_entry_for(m);
    if (!atomic_load(&e->m)) atomic_store(&e->m, mtx_make());
    atomic_store(&e->owner, NULL);
    atomic_store(&e->locksite, NULL);
    atomic_store(&e->depth, 0);
    mtx_log("init", m, (uint32_t)(e - g_mtx_map));
    return 0;
}
int klb_pthread_mutex_lock(void *m) {
    if (!m) return mtx_null("lock");
    mtx_entry *e = mtx_entry_for(m);
    mtx_wait_enter(m, __builtin_return_address(0));
    int r = px(pthread_mutex_lock(mtx_host(e)));
    mtx_wait_leave();
    if (r == 0) atomic_fetch_add(&e->depth, 1);
    if (r == 0) {
        atomic_store(&e->locksite, __builtin_return_address(0));
        atomic_store(&e->owner, (void *)pthread_self());
    }
    return r;
}
int klb_pthread_mutex_unlock(void *m)  {
    if (!m) return mtx_null("unlock");
    mtx_entry *e = mtx_entry_for(m);
    // Our host mutexes are RECURSIVE, so one unlock of a depth>1 hold does NOT
    // release the host mutex — clearing owner/locksite here would then report a
    // still-held lock as free (owner=0x0), which is exactly what hid the holder
    // of the deadlocked allocator lock in the [mtx] dump. Only clear the owner
    // when this unlock actually drops the last level.
    int newdepth = atomic_fetch_sub(&e->depth, 1) - 1;
    if (newdepth <= 0) {
        atomic_store(&e->owner, NULL);
        atomic_store(&e->locksite, NULL);
    }
    return px(pthread_mutex_unlock(mtx_host(e)));
}
int klb_pthread_mutex_trylock(void *m) {
    if (!m) return mtx_null("trylock");
    mtx_entry *e = mtx_entry_for(m);
    int r = px(pthread_mutex_trylock(mtx_host(e)));
    if (r == 0) atomic_fetch_add(&e->depth, 1);
    if (r == 0) {
        atomic_store(&e->locksite, __builtin_return_address(0));
        atomic_store(&e->owner, (void *)pthread_self());
    }
    return r;
}
int klb_pthread_mutex_destroy(void *p) {
    mtx_log("destroy", p, 0);
    mtx_entry *e = mtx_find(p);
    if (e) atomic_store(&e->key, 0);    // host leaks; it may be held
    return 0;
}

static int thread_is_alive(void *self);        // threads section, below
static const char *thread_name_of(void *self); // threads section, below

#include <mach/mach.h>
#include <mach/vm_map.h>
#include <dlfcn.h>

// Current backtrace of a mutex holder, captured from the fault handler so a
// deadlock report shows where the *owner* is parked, not just who waits.
static void dump_thread_stack(FILE *out, void *pt) {
    mach_port_t act = pthread_mach_thread_np((pthread_t)pt);
    if (!act) { fprintf(out, "    (no mach thread)\n"); return; }
    arm_thread_state64_t ts;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(act, ARM_THREAD_STATE64, (thread_state_t)&ts,
                         &cnt) != KERN_SUCCESS) {
        fprintf(out, "    (thread_get_state failed)\n");
        return;
    }
    void *pc = (void *)ts.__pc;
    uint64_t fp = ts.__fp;
    for (int d = 0; d < 12 && pc; d++) {
        size_t off = 0;
        const char *img = kl_addr_image(pc, &off);
        if (img) fprintf(out, "    #%-2d %s+0x%zx\n", d, img, off);
        else {
            Dl_info di;
            if (dladdr(pc, &di) && di.dli_sname)
                fprintf(out, "    #%-2d %s+0x%tx\n", d, di.dli_sname,
                        (const char *)pc - (const char *)di.dli_saddr);
            else fprintf(out, "    #%-2d %p\n", d, pc);
        }
        if (!fp || (fp & 7)) break;
        uint64_t pair[2];
        vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), fp, sizeof pair,
                              (vm_address_t)pair, &got) != KERN_SUCCESS ||
            got != sizeof pair || !pair[1] || pair[0] <= fp)
            break;
        pc = (void *)pair[1];
        fp = pair[0];
    }
}

static void kl_tsd_report(FILE *out);    // defined with the key table below

void kl_pthread_report(FILE *out) {
    // The TSD ceiling, first, because a guest that hit it is already broken in
    // a way nothing else in this dump names: pthread_key_create hands back
    // EAGAIN, the guest stores a sentinel key, and the fault lands hundreds of
    // frames later on a NULL from getspecific. Asgard's Wrath 2 died exactly
    // that way (FPhysXCPUDispatcher::submitTask, key -1, SIGSEGV at 0x84).
    kl_tsd_report(out);
    fprintf(out, "-- mutex owners (kl_pthread address map) --\n");
    unsigned shown = 0;
    for (uint32_t i = 0; i < MTX_MAP_SIZE; i++) {
        void *owner = atomic_load(&g_mtx_map[i].owner);
        if (!owner) continue;
        const char *nm = thread_name_of(owner);
        fprintf(out, "  guest %p: held by tid %p%s%s%s (locked from %p)\n",
                (void *)atomic_load(&g_mtx_map[i].key), owner,
                nm ? " [" : "", nm ? nm : "", nm ? "]" : "",
                thread_is_alive(owner) ? "" : "  ** EXITED **",
                atomic_load(&g_mtx_map[i].locksite));
        dump_thread_stack(out, owner);
        shown++;
    }
    if (!shown) fprintf(out, "  (no tracked holders)\n");
    for (int i = 0; i < 64; i++) {
        void *tid = atomic_load(&g_mtx_waiters[i].tid);
        if (!tid) continue;
        const char *nm = thread_name_of(tid);
        // The call site, and then the whole stack. A holder without a waiter is
        // a lock that is merely held; the pair is what says a run is STUCK, and
        // until both ends were symbolized this report could name the holder's
        // frames and left the waiter as a bare address in an image the reader
        // then had to find the load base of by hand.
        void *ra = atomic_load(&g_mtx_waiters[i].ra);
        size_t off = 0;
        const char *img = kl_addr_image(ra, &off);
        if (img)
            fprintf(out, "  waiter tid %p%s%s%s wants guest %p (from %s+0x%zx)\n",
                    tid, nm ? " [" : "", nm ? nm : "", nm ? "]" : "",
                    atomic_load(&g_mtx_waiters[i].guest), img, off);
        else
            fprintf(out, "  waiter tid %p%s%s%s wants guest %p (from %p)\n", tid,
                    nm ? " [" : "", nm ? nm : "", nm ? "]" : "",
                    atomic_load(&g_mtx_waiters[i].guest), ra);
        dump_thread_stack(out, tid);
    }
    for (int i = 0; i < 64; i++) {
        void *tid = atomic_load(&g_cnd_sleepers[i].tid);
        if (!tid) continue;
        const char *nm = thread_name_of(tid);
        void *ra = atomic_load(&g_cnd_sleepers[i].ra);
        size_t off = 0;
        const char *img = kl_addr_image(ra, &off);
        fprintf(out, "  sleeper tid %p%s%s%s in a cond wait on guest cond %p "
                "(mutex %p) from ", tid, nm ? " [" : "", nm ? nm : "",
                nm ? "]" : "", atomic_load(&g_cnd_sleepers[i].cond),
                atomic_load(&g_cnd_sleepers[i].guest));
        if (img) fprintf(out, "%s+0x%zx\n", img, off); else fprintf(out, "%p\n", ra);
        dump_thread_stack(out, tid);
    }
}
// bionic pthread_mutexattr_t is a plain int holding the type.
int klb_pthread_mutexattr_init(int *a)            { *a = 0; return 0; }
int klb_pthread_mutexattr_destroy(int *a)         { (void)a; return 0; }
int klb_pthread_mutexattr_settype(int *a, int t)  { *a = t; return 0; }

// ---------- condition variable ----------
// Init KEEPS an existing host cond for the same address, exactly as
// klb_pthread_mutex_init does and for the same reason: a guest that re-inits a
// pooled object must not have the host object swapped under a thread that is
// already waiting on it.
int klb_pthread_cond_init(void *c, const void *a)  { (void)a; cnd(c); return 0; }
int klb_pthread_cond_destroy(void *p) { (void)p; return 0; }
int klb_pthread_cond_signal(void *c) {
    cnd_entry *ce = cnd_entry_for(c);
    atomic_fetch_add(&ce->signals, 1);
    atomic_store(&ce->last_sig_ns, cnd_now_ns());
    return px(pthread_cond_signal(cnd_host(ce)));
}
int klb_pthread_cond_broadcast(void *c) {
    cnd_entry *ce = cnd_entry_for(c);
    atomic_fetch_add(&ce->signals, 1);
    atomic_store(&ce->last_sig_ns, cnd_now_ns());
    return px(pthread_cond_broadcast(cnd_host(ce)));
}
// Validated one-record frame walk — see klepton.h. `my_frame` is the caller's
// __builtin_frame_address(0) (== its x29). Our prologue stored the caller's own
// frame record at [my_frame] = { caller's saved fp, caller's return address }, so
// one hop up and +8 is the caller's caller's return address, i.e. what
// __builtin_return_address(1) would compute — but bounded to this thread's stack
// so a guest frame that left junk in x29 (batman: 0x18) yields NULL, not a fault.
void *kl_caller_ra2(void *my_frame) {
    uintptr_t fp = (uintptr_t)my_frame;
    if (!fp || (fp & 7)) return NULL;
    uintptr_t hi = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
    uintptr_t lo = hi - pthread_get_stacksize_np(pthread_self());
    if (fp < lo || fp + 16 > hi) return NULL;          // [fp],[fp+8] must be in-stack
    uintptr_t cfp = *(uintptr_t *)fp;                  // caller's saved fp (guest x29)
    if (cfp <= fp || (cfp & 7) || cfp < lo || cfp + 16 > hi) return NULL;
    return *(void **)(cfp + 8);                         // caller's caller return addr
}

int klb_pthread_cond_wait(void *c, void *m) {
    // A cond wait releases the mutex while sleeping; reflect that in the
    // owner table or every sleeper reads as a holder.
    mtx_entry *e = mtx_entry_for(m);
    atomic_store(&e->owner, NULL);
    cnd_sleep_enter(c, m, __builtin_return_address(0));
    cnd_entry *ce = cnd_entry_for(c);
    void *prev_mtx = atomic_exchange(&ce->last_mutex, (void *)mtx_host(e));
    cnd_watchdog_start();
    atomic_store(&ce->waiter_ra, __builtin_return_address(0));
    atomic_store(&ce->waiter_ra2, kl_caller_ra2(__builtin_frame_address(0)));
    atomic_fetch_add(&ce->waiters, 1);
    int raw = pthread_cond_wait(cnd_host(ce), mtx_host(e));
    atomic_fetch_sub(&ce->waiters, 1);
    cnd_sleep_leave();
    cnd_wait_failed(raw, c, m, __builtin_return_address(0), prev_mtx);
    int r = px(raw);
    atomic_store(&e->locksite, __builtin_return_address(0));
    atomic_store(&e->owner, (void *)pthread_self());
    return r;
}
int klb_pthread_cond_timedwait(void *c, void *m, const struct timespec *ts) {
    // bionic's default cond clock is CLOCK_MONOTONIC; Darwin conds speak only
    // CLOCK_REALTIME. A guest abstime is monotonic-based (e.g. libil2cpp's
    // ConditionVariableImpl builds it from clock_gettime(CLOCK_MONOTONIC)),
    // so rebase it onto realtime before forwarding. Monotonic abstimes are
    // small (< ~4.5 years of uptime seconds); realtime ones are ~1.7e9.
    struct timespec rts;
    static int no_rebase = -1;
    if (no_rebase < 0) no_rebase = kl_env_on("KL_NO_REBASE", 0);
    if (!no_rebase && ts && ts->tv_sec < 100000000) {
        struct timespec rt, mo;
        clock_gettime(CLOCK_REALTIME, &rt);
        clock_gettime(CLOCK_MONOTONIC, &mo);
        int64_t dsec = (int64_t)rt.tv_sec - mo.tv_sec;
        long    dnsec = rt.tv_nsec - mo.tv_nsec;
        rts.tv_sec = ts->tv_sec + dsec;
        rts.tv_nsec = ts->tv_nsec + dnsec;
        while (rts.tv_nsec >= 1000000000L) { rts.tv_nsec -= 1000000000L; rts.tv_sec++; }
        while (rts.tv_nsec < 0)            { rts.tv_nsec += 1000000000L; rts.tv_sec--; }
        ts = &rts;
    }
    // KL_CONDWAIT_CAP_MS=<ms>: clamp the abstime to now+ms. Diagnostic for the
    // class-init deadlock: libil2cpp's Class::Init waiters sleep on a condvar
    // nobody signals after the cctor completes, and the abstime they compute
    // reads as ~71 minutes out (w23 = 4294967 ms ≈ UINT_MAX µs / 1000), so
    // they never wake to re-check the class state. Capping the wait proves
    // whether the stuck boot is exactly those waiters never re-checking.
    static int cap_ms = -1;
    static int trace = -1;
    if (cap_ms < 0) {
        cap_ms = kl_env_int("KL_CONDWAIT_CAP_MS", 0);
    }
    if (trace < 0) trace = kl_env_on("KL_TRACE_CONDWAIT", 0) ? 1 : 0;
    if (trace && ts) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        static _Atomic int tlogged;
        if (atomic_fetch_add(&tlogged, 1) < 60)
            fprintf(stderr, "  [klb] cond_timedwait tid=%p ra=%p: ts %lld.%09ld "
                    "(delta %.3fs)\n", (void *)pthread_self(),
                    __builtin_return_address(0),
                    (long long)ts->tv_sec, ts->tv_nsec,
                    (double)(ts->tv_sec - now.tv_sec) +
                    (double)(ts->tv_nsec - now.tv_nsec) / 1e9);
    }
    struct timespec cts;
    if (cap_ms && ts) {
        struct timespec now, lim;
        clock_gettime(CLOCK_REALTIME, &now);
        lim = now;
        lim.tv_sec += cap_ms / 1000;
        lim.tv_nsec += (cap_ms % 1000) * 1000000L;
        if (lim.tv_nsec >= 1000000000L) { lim.tv_sec++; lim.tv_nsec -= 1000000000L; }
        if (ts->tv_sec > lim.tv_sec ||
            (ts->tv_sec == lim.tv_sec && ts->tv_nsec > lim.tv_nsec)) {
            static _Atomic int logged;
            if (atomic_fetch_add(&logged, 1) < 10)
                fprintf(stderr, "  [klb] cond_timedwait clamped: guest ts was "
                        "%lld.%09ld (%.3fs out), now+%dms\n",
                        (long long)ts->tv_sec, ts->tv_nsec,
                        (double)(ts->tv_sec - now.tv_sec) +
                        (double)(ts->tv_nsec - now.tv_nsec) / 1e9, cap_ms);
            cts = lim;
            ts = &cts;
        }
    }
    // A cond wait releases the mutex while sleeping; reflect that in the
    // owner table or every sleeper reads as a holder.
    mtx_entry *e = mtx_entry_for(m);
    atomic_store(&e->owner, NULL);
    cnd_sleep_enter(c, m, __builtin_return_address(0));
    cnd_entry *ce = cnd_entry_for(c);
    cnd_watchdog_start();
    atomic_store(&ce->waiter_ra, __builtin_return_address(0));
    atomic_store(&ce->waiter_ra2, kl_caller_ra2(__builtin_frame_address(0)));
    atomic_fetch_add(&ce->waiters, 1);
    int raw = pthread_cond_timedwait(cnd_host(ce), mtx_host(e), ts);
    atomic_fetch_sub(&ce->waiters, 1);
    cnd_sleep_leave();
    if (raw != ETIMEDOUT) cnd_wait_failed(raw, c, m, __builtin_return_address(0), NULL);
    int r = px(raw);
    atomic_store(&e->locksite, __builtin_return_address(0));
    atomic_store(&e->owner, (void *)pthread_self());
    return r;
}
// pthread_cond_clockwait(cond, mutex, clockid, abstime) — bionic's API-30 form
// that names the clock the abstime is in (CLOCK_MONOTONIC or CLOCK_REALTIME)
// instead of relying on the cond's configured clock. olar (UE5) calls it and it
// was unresolved -> fatal. Delegate to klb_pthread_cond_timedwait: its abstime
// rebase already keys off the magnitude (monotonic abstimes are small, realtime
// ones ~1.7e9), which classifies both clocks correctly, so the clockid is
// redundant here. Reuses all the timedwait handling (rebase, cap, owner table).
int klb_pthread_cond_clockwait(void *c, void *m, int clk, const struct timespec *ts) {
    (void)clk;
    return klb_pthread_cond_timedwait(c, m, ts);
}
int klb_pthread_condattr_init(long *a)                 { *a = 0; return 0; }
int klb_pthread_condattr_destroy(long *a)              { (void)a; return 0; }
int klb_pthread_condattr_setclock(long *a, int clk)    { (void)a; (void)clk; return 0; }

// ---------- rwlock ----------
int klb_pthread_rwlock_init(void *l, const void *a) { (void)a; g_rwl_recycle(l); rwl(l); return 0; }
int klb_pthread_rwlock_destroy(void *p) { g_rwl_recycle(p); return 0; }
int klb_pthread_rwlock_rdlock(void *l) { return px(pthread_rwlock_rdlock(rwl(l))); }
int klb_pthread_rwlock_wrlock(void *l) { return px(pthread_rwlock_wrlock(rwl(l))); }
int klb_pthread_rwlock_unlock(void *l) { return px(pthread_rwlock_unlock(rwl(l))); }
// SDL3 reaches for the try- forms; Beat Saber never did. Same side-table
// indirection as the blocking pair — a bionic rwlock is 56 bytes of 4-byte
// aligned int32, so what lives in it is an index, not a handle.
int klb_pthread_rwlock_tryrdlock(void *l) { return px(pthread_rwlock_tryrdlock(rwl(l))); }
int klb_pthread_rwlock_trywrlock(void *l) { return px(pthread_rwlock_trywrlock(rwl(l))); }

// ---------- attributes ----------
// bionic pthread_attr_t: { uint32 flags; void* stack_base; size_t stack_size;
//                          size_t guard_size; int32 policy; int32 prio; char pad[16] }
typedef struct { uint32_t flags; void *stack_base; size_t stack_size, guard_size;
                 int32_t policy, prio; char pad[16]; } bionic_attr;
#define BIONIC_ATTR_DETACHED 1

int klb_pthread_attr_init(bionic_attr *a) {
    memset(a, 0, sizeof *a);
    a->stack_size = 1024 * 1024;
    a->guard_size = 4096;
    return 0;
}
int klb_pthread_attr_destroy(bionic_attr *a) { (void)a; return 0; }
int klb_pthread_attr_setstacksize(bionic_attr *a, size_t n) { a->stack_size = n; return 0; }
int klb_pthread_attr_setdetachstate(bionic_attr *a, int st) {
    if (st) a->flags |= BIONIC_ATTR_DETACHED; else a->flags &= ~BIONIC_ATTR_DETACHED;
    return 0;
}
int klb_pthread_attr_getstack(const bionic_attr *a, void **base, size_t *size) {
    *base = a->stack_base; *size = a->stack_size; return 0;
}
int klb_pthread_getattr_np(pthread_t t, bionic_attr *a) {
    klb_pthread_attr_init(a);
    a->stack_size = pthread_get_stacksize_np(t);
    void *hi = pthread_get_stackaddr_np(t);              // Darwin returns the HIGH address
    a->stack_base = (char *)hi - a->stack_size;          // bionic wants the LOW address
    return 0;
}

// ---------- keys ----------
//
// Guest keys are OURS, not Darwin's, and the difference is the whole point.
//
// The obvious shim — one real pthread_key_create per guest key — spends a
// scarce host resource on an unbounded guest demand, and Darwin's supply is
// smaller than it looks: external keys are handed out from 258 up to 767, so a
// bare process gets 510, and this one is not bare. SwiftUI, Metal, ANGLE and
// MoltenVK all claim keys during dyld's initialisation of THEIR images, before
// any guest code runs, and kl_x18 deliberately claims a high one for itself
// (KLX_TSD_SLOT). Whatever is left is what a whole Android game engine gets.
//
// It is not enough. Asgard's Wrath 2 exhausted it during its first map load,
// and the way that failure presents is the reason this table exists rather
// than a bigger ceiling: a guest does not handle EAGAIN from pthread_key_create
// in any useful way. UE4's FPhysScene_PhysX::InitPhysScene stores -1 for the
// failed key and carries on, and FPhysXCPUDispatcher::submitTask then does
//
//     ldrsw x8, [x0, #0x84]     // x0 = pthread_getspecific(-1) == NULL
//
// on every nested PhysX task — SIGSEGV at 0x84, three minutes in, in guest code
// a hundred thousand instructions away from the call that actually failed.
// Returning EAGAIN is a correct answer that gets a process killed, so the fix
// is to stop running out.
//
// The old mapping also LEAKED: the index came from a counter that only ever
// went up, so key_delete returned the Darwin key to Darwin and kept the guest
// index forever. A guest that cycles keys — and UE4 cycles one per physics
// scene — walks the ceiling down on its own.
//
// So: one Darwin key holds a per-thread slot array, indices come from a free
// list, and a delete makes every outstanding value stale at once by bumping the
// key's sequence number (bionic's design, and for bionic's reason: POSIX says
// values do not survive a delete, and a recycled index must not hand the next
// owner the last one's pointer).
#define KL_MAX_KEYS 512
#define KL_TSD_DTOR_ITERS 4          // POSIX PTHREAD_DESTRUCTOR_ITERATIONS

typedef struct { uint32_t seq; void *val; } kl_tsd_slot;

static pthread_key_t   g_tsd_key;               // the ONE Darwin key we spend
static pthread_once_t  g_tsd_once = PTHREAD_ONCE_INIT;
static _Atomic uint32_t g_key_seq[KL_MAX_KEYS]; // odd = live, even = free
static void (*g_key_dtor[KL_MAX_KEYS])(void *);
static _Atomic int     g_nkeys_hw;              // high-water, for the report
static pthread_mutex_t g_key_lock = PTHREAD_MUTEX_INITIALIZER;

// Run the guest's destructors for this thread, POSIX-style: repeat while a
// destructor plants a new value, up to KL_TSD_DTOR_ITERS rounds. Darwin has
// already cleared its own slot by the time this runs, so re-register the table
// for the duration or a destructor's own getspecific answers NULL.
static void tsd_thread_exit(void *p) {
    kl_tsd_slot *s = p;
    pthread_setspecific(g_tsd_key, s);
    for (int round = 0; round < KL_TSD_DTOR_ITERS; round++) {
        int more = 0;
        for (int k = 0; k < KL_MAX_KEYS; k++) {
            void *v = s[k].val;
            if (!v || s[k].seq != atomic_load(&g_key_seq[k])) { s[k].val = NULL; continue; }
            void (*d)(void *) = g_key_dtor[k];
            s[k].val = NULL;
            if (d) { d(v); more = 1; }
        }
        if (!more) break;
    }
    pthread_setspecific(g_tsd_key, NULL);
    free(s);
}
static void tsd_init(void) { pthread_key_create(&g_tsd_key, tsd_thread_exit); }
// No pthread_once here on purpose: every caller has already passed key_live(),
// and a key cannot be live until klb_pthread_key_create has run the once. This
// path is hot — UE4's binned allocator reads TLS on every malloc — so it is
// worth not paying for a second barrier the caller has already crossed.
static kl_tsd_slot *tsd_table(int make) {
    kl_tsd_slot *s = pthread_getspecific(g_tsd_key);
    if (!s && make) {
        s = calloc(KL_MAX_KEYS, sizeof *s);
        if (s) pthread_setspecific(g_tsd_key, s);
    }
    return s;
}
// A key is usable iff its sequence is odd; `seq` also tags the values, so a
// stale value from before a delete never reads back through a recycled index.
static inline int key_live(int k, uint32_t *seq) {
    if (k < 0 || k >= KL_MAX_KEYS) return 0;
    uint32_t s = atomic_load(&g_key_seq[k]);
    if (!(s & 1)) return 0;
    if (seq) *seq = s;
    return 1;
}
int klb_pthread_key_create(int *out, void (*dtor)(void *)) {
    pthread_once(&g_tsd_once, tsd_init);
    pthread_mutex_lock(&g_key_lock);
    for (int k = 0; k < KL_MAX_KEYS; k++) {
        if (atomic_load(&g_key_seq[k]) & 1) continue;
        g_key_dtor[k] = dtor;
        atomic_fetch_add(&g_key_seq[k], 1);          // even -> odd: live
        int live = 0;
        for (int j = 0; j < KL_MAX_KEYS; j++) live += (atomic_load(&g_key_seq[j]) & 1);
        if (live > atomic_load(&g_nkeys_hw)) atomic_store(&g_nkeys_hw, live);
        pthread_mutex_unlock(&g_key_lock);
        *out = k;                                    // ...only ever on success
        static int trace = -1;
        if (trace < 0) trace = kl_env_on("KL_TRACE_TSD", 0);
        if (trace)
            fprintf(stderr, "  [tsd] key_create -> %d (dtor %p, %d live)\n",
                    k, (void *)dtor, live);
        return 0;
    }
    pthread_mutex_unlock(&g_key_lock);
    // Loud, because the guest will not be: EAGAIN here comes back as a NULL
    // from getspecific hundreds of frames later, in guest code that never
    // checked. See the header comment.
    fprintf(stderr, "  [tsd] pthread_key_create EXHAUSTED at %d keys — the guest "
                    "gets EAGAIN, and a guest that ignores it will fault on a "
                    "NULL getspecific later\n", KL_MAX_KEYS);
    return kl_errno_to_linux(EAGAIN);
}
int klb_pthread_key_delete(int k) {
    pthread_mutex_lock(&g_key_lock);
    if (!key_live(k, NULL)) { pthread_mutex_unlock(&g_key_lock); return kl_errno_to_linux(EINVAL); }
    g_key_dtor[k] = NULL;
    atomic_fetch_add(&g_key_seq[k], 1);              // odd -> even: free, and
    pthread_mutex_unlock(&g_key_lock);               // every value now stale
    return 0;
}
void *klb_pthread_getspecific(int k) {
    uint32_t seq;
    if (!key_live(k, &seq)) return NULL;
    kl_tsd_slot *s = tsd_table(0);
    return (s && s[k].seq == seq) ? s[k].val : NULL;
}
int klb_pthread_setspecific(int k, const void *v) {
    uint32_t seq;
    if (!key_live(k, &seq)) return kl_errno_to_linux(EINVAL);
    kl_tsd_slot *s = tsd_table(1);
    if (!s) return kl_errno_to_linux(ENOMEM);
    s[k].seq = seq; s[k].val = (void *)v;
    return 0;
}
// How close the guest came to the ceiling — printed by kl_pthread_report,
// because "how many keys does this guest want" is a number nobody has, and it
// is the one that decides whether KL_MAX_KEYS is enough.
static void kl_tsd_report(FILE *out) {
    int live = 0;
    for (int k = 0; k < KL_MAX_KEYS; k++) live += (atomic_load(&g_key_seq[k]) & 1);
    fprintf(out, "-- guest TSD: %d keys live, %d at high water, ceiling %d --\n",
            live, atomic_load(&g_nkeys_hw), KL_MAX_KEYS);
}
int klb_pthread_once(int *ctl, void (*fn)(void)) {
    if (atomic_load((_Atomic int *)ctl) == 2) return 0;
    int expect = 0;
    if (atomic_compare_exchange_strong((_Atomic int *)ctl, &expect, 1)) {
        fn(); atomic_store((_Atomic int *)ctl, 2); return 0;
    }
    while (atomic_load((_Atomic int *)ctl) != 2) sched_yield();
    return 0;
}

// ---------- threads ----------
// Every guest thread needs the bionic stack-guard canary in Darwin TSD slot 5.
//
// Liveness registry for the mutex-owner dump: a holder whose tid is dead is
// the signature of a thread that exited holding a mutex (which reads exactly
// like a deadlock). Registered in the tramp, unregistered on return/exit.
#define KL_MAX_THREADS_LIVE 256
static _Atomic(void *) g_live_threads[KL_MAX_THREADS_LIVE];
static char            g_live_names[KL_MAX_THREADS_LIVE][24];
// Parallel to g_live_threads: the mach thread id (pthread_threadid_np) of each
// live guest thread, so a thread named by tid (XR_KHR_android_thread_settings)
// can be found and its QoS raised. Same index as g_live_threads.
static _Atomic(uint64_t) g_live_tids[KL_MAX_THREADS_LIVE];
static void thread_register(void *self) {
    for (int i = 0; i < KL_MAX_THREADS_LIVE; i++) {
        void *expect = NULL;
        if (atomic_compare_exchange_strong(&g_live_threads[i], &expect, self)) {
            pthread_getname_np(pthread_self(), g_live_names[i],
                               sizeof g_live_names[i]);
            uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
            atomic_store(&g_live_tids[i], tid);
            return;
        }
    }
}
static void thread_unregister(void *self) {
    for (int i = 0; i < KL_MAX_THREADS_LIVE; i++) {
        void *expect = self;
        if (atomic_compare_exchange_strong(&g_live_threads[i], &expect, NULL)) {
            atomic_store(&g_live_tids[i], 0);
            return;
        }
    }
}
static const char *thread_name_of(void *self) {
    for (int i = 0; i < KL_MAX_THREADS_LIVE; i++)
        if (atomic_load(&g_live_threads[i]) == self) return g_live_names[i];
    return NULL;
}
static int thread_is_alive(void *self) {
    for (int i = 0; i < KL_MAX_THREADS_LIVE; i++)
        if (atomic_load(&g_live_threads[i]) == self) return 1;
    return 0;
}
// XR_KHR_android_thread_settings: the guest names a thread (its renderer- or
// app-main) as scheduling-critical. On visionOS a default-QoS guest thread is
// scheduled BEHIND the compositor, and for a streaming guest like Steam Link
// that thread set carries the UDP-receive / AV-decode / submit pipeline — a gap
// there reads to the sender as network jitter and the bitrate controller backs
// off. ALVR's visionOS client pins exactly these threads to userInteractive;
// this does the same, only for the threads the guest explicitly flags, so the
// compositor is not starved by boosting everything (that is KL_GUEST_QOS's blunt
// job).
//
// Darwin CAN raise another thread's QoS, the old "would be to lie" note in
// kl_openxr.c notwithstanding: a thread flagging ITSELF (the common case) takes
// the self door; a thread flagging another takes
// pthread_override_qos_class_start_np through the live-thread registry above.
// The override handle is intentionally not kept — the boost is meant to last the
// thread's life — and a given thread is boosted at most once so overrides do not
// stack.
void kl_pthread_boost_qos(uint64_t threadid) {
#ifdef __APPLE__
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_XR_THREAD_QOS", 1);
    if (!on) return;

    uint64_t self_tid = 0; pthread_threadid_np(NULL, &self_tid);
    if (threadid == self_tid) {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        fprintf(stderr, "  [thr] raised self (tid %llu) to USER_INTERACTIVE QoS "
                        "for a guest-flagged critical thread\n",
                (unsigned long long)threadid);
        return;
    }

    // Dedup: override a given thread at most once.
    static _Atomic(uint64_t) boosted[32];
    for (int i = 0; i < 32; i++) if (atomic_load(&boosted[i]) == threadid) return;

    for (int i = 0; i < KL_MAX_THREADS_LIVE; i++) {
        if (atomic_load(&g_live_tids[i]) != threadid) continue;
        void *pt = atomic_load(&g_live_threads[i]);
        if (!pt) return;
        pthread_override_t ov = pthread_override_qos_class_start_np(
                                    (pthread_t)pt, QOS_CLASS_USER_INTERACTIVE, 0);
        if (ov) {
            for (int k = 0; k < 32; k++) {
                uint64_t e = 0;
                if (atomic_compare_exchange_strong(&boosted[k], &e, threadid)) break;
            }
            fprintf(stderr, "  [thr] raised tid %llu to USER_INTERACTIVE QoS "
                            "(cross-thread override) for a guest-flagged thread\n",
                    (unsigned long long)threadid);
        }
        return;
    }
    // Not created through our trampoline (e.g. the process main thread) — the
    // self door already covers the common case, so this is only a note.
    fprintf(stderr, "  [thr] guest flagged tid %llu, not in the live registry "
                    "(main thread?) — QoS left as is\n",
            (unsigned long long)threadid);
#else
    (void)threadid;
#endif
}

typedef struct { void *(*fn)(void *); void *arg; } tramp;
static void *thread_tramp(void *p) {
    tramp t = *(tramp *)p; free(p);
    kl_thread_init();
    // KL_GUEST_QOS=1: every guest thread at USER_INITIATED instead of default.
    // On visionOS a default-QoS thread is scheduled behind the compositor's
    // work, and for Steam Link that thread set includes the UDP receive and
    // AV-decode pipeline - a scheduling gap there queues packets, Steam reads
    // the queueing as network jitter, and the bitrate controller backs off. A
    // stream that stutters on a clean link is often this, not the link.
    // Default OFF: it changes scheduling for every guest, so it is opt-in.
#ifdef __APPLE__
    {
        static int q = -1;
        if (q < 0) {
            q = kl_env_on("KL_GUEST_QOS", 0);
            // Named ONCE either way: this knob's whole use is as an A/B, and a
            // scheduling change leaves no other trace in a log — the first QoS
            // run came back unverifiable because nothing said whether the knob
            // was in force.
            fprintf(stderr, "  [thr] guest threads at %s QoS (KL_GUEST_QOS=%d)\n",
                    q ? "USER_INITIATED" : "default", q);
        }
        if (q) pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    }
#endif
    thread_register(pthread_self());
    // KL_TRACE_THREADS: log this guest thread's tid ON the thread (klb_pthread_create
    // logs the entry pointer at creation but cannot know the tid yet). The x18
    // veneer keeps the guest x18 in tsd[KLX_TSD_SLOT] read by a RAW thread-pointer
    // offset; that slot is inline on some threads and in Darwin's extended TSD on
    // others, and a raw read of an extended slot faults. Tying a fault's tid back
    // to the entry pointer here names which pool the broken thread came from, and
    // the raw-vs-API check says whether this thread is one the veneer can serve.
    {
        static int trace = -1;
        if (trace < 0) trace = kl_env_on("KL_TRACE_THREADS", 0);
        if (trace) {
            uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
            pthread_setspecific(KLX_TSD_SLOT, (void *)(uintptr_t)0xC0DEC0DEU);
            uint64_t tp; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
            void *api = pthread_getspecific(KLX_TSD_SLOT);
            fprintf(stderr, "  [thr] guest tramp tid=%llu entry=%p tp=%p "
                            "tsd[%d] api=%p (raw-offset check deferred to fault)\n",
                    (unsigned long long)tid, (void *)t.fn, (void *)tp,
                    (int)KLX_TSD_SLOT, api);
            pthread_setspecific(KLX_TSD_SLOT, NULL);
        }
    }
    void *r = t.fn(t.arg);
    thread_unregister(pthread_self());
    return r;
}
int klb_pthread_create(pthread_t *out, const bionic_attr *ga,
                       void *(*fn)(void *), void *arg) {
    // A NULL start routine becomes a jump to 0x0 the instant the thread runs,
    // which visionOS's AMFI kills as a CODESIGNING "invalid page" (an uncatchable
    // SIGKILL) rather than a signal the fault handler could recover. Refuse it
    // here — with the entry logged — so a guest that resolved its thread routine
    // to NULL fails a create call it can check instead of taking the process down.
    if (!fn) {
        fprintf(stderr, "  [thr] REFUSED pthread_create with a NULL start routine "
                        "(arg=%p) — this would jump to 0x0 and AMFI would SIGKILL "
                        "the process; returning EINVAL instead\n", arg);
        return 22;   // EINVAL
    }
    pthread_attr_t da;
    pthread_attr_init(&da);
    if (ga) {
        if (ga->stack_size) pthread_attr_setstacksize(&da, ga->stack_size);
        if (ga->flags & BIONIC_ATTR_DETACHED)
            pthread_attr_setdetachstate(&da, PTHREAD_CREATE_DETACHED);
    }
    tramp *t = malloc(sizeof *t);
    t->fn = fn; t->arg = arg;
    int rc = px(pthread_create(out, &da, thread_tramp, t));
    pthread_attr_destroy(&da);
    if (rc) free(t);
    // KL_TRACE_THREADS=1. "Which guest threads exist" is the question a hang
    // asks first, and `sample` only answers it for threads that were actually
    // created — a thread the guest never started leaves no trace at all, which
    // reads identically to a thread that exited. The entry pointer is printed
    // because it symbolises against the chain's load addresses each door
    // prints at load, and so names WHICH pool or subsystem it was.
    static int trace = -1;
    if (trace < 0) trace = kl_env_on("KL_TRACE_THREADS", 0);
    if (trace)
        fprintf(stderr, "  [thr] pthread_create entry=%p arg=%p stack=%zu%s -> %d\n",
                (void *)fn, arg, ga ? ga->stack_size : 0,
                (ga && (ga->flags & BIONIC_ATTR_DETACHED)) ? " detached" : "", rc);
    return rc;
}
int   klb_pthread_join(pthread_t t, void **r)  { return px(pthread_join(t, r)); }
int   klb_pthread_detach(pthread_t t)          { return px(pthread_detach(t)); }
void  klb_pthread_exit(void *r)                { thread_unregister(pthread_self()); pthread_exit(r); }
pthread_t klb_pthread_self(void)               { return pthread_self(); }
int   klb_pthread_equal(pthread_t a, pthread_t b) { return pthread_equal(a, b); }
int   klb_pthread_kill(pthread_t t, int sig)   {
    int r = px(pthread_kill(t, sig));
    if (kl_env_on("KL_TRACE_SIG", 0))
        fprintf(stderr, "  [sig] pthread_kill(%p, %d) -> %d%s\n", (void *)t, sig, r,
                r ? " FAILED" : "");
    return r;
}
// Same names, different numbers, and the consequence is invisible: Linux
// numbers SIG_BLOCK/UNBLOCK/SETMASK 0/1/2, Darwin 1/2/3. Forwarded raw, a guest
// asking to UNBLOCK a signal (1) is asking Darwin to BLOCK it, and the only
// symptom is a signal that pthread_kill accepts and the handler never sees.
// That is how Boehm's GC suspend stalled: handler installed, 163 signals sent,
// zero acknowledgements, and the collector spinning on sem_getvalue forever.
//
// bionic's sigset_t is 8 bytes on LP64 against Darwin's 4, but both number bit
// (sig-1), so reading the low word is correct for signals 1..32 — which is all
// Darwin has.
#define KL_LINUX_SIG_BLOCK   0
#define KL_LINUX_SIG_UNBLOCK 1
#define KL_LINUX_SIG_SETMASK 2

static int kl_sigmask_how(int linux_how) {
    switch (linux_how) {
    case KL_LINUX_SIG_BLOCK:   return SIG_BLOCK;
    case KL_LINUX_SIG_UNBLOCK: return SIG_UNBLOCK;
    case KL_LINUX_SIG_SETMASK: return SIG_SETMASK;
    default:                   return linux_how;
    }
}

int klb_pthread_sigmask(int how, const uint64_t *s, uint64_t *o) {
    sigset_t din, dout;
    if (s) din = (sigset_t)(*s & 0xFFFFFFFFu);
    int r = pthread_sigmask(kl_sigmask_how(how), s ? &din : NULL, o ? &dout : NULL);
    if (!r && o) *o = dout;
    return r;
}

int klb_sigprocmask(int how, const uint64_t *s, uint64_t *o) {
    return klb_pthread_sigmask(how, s, o);
}
// bionic takes the thread; Darwin's only names the *current* thread.
int klb_pthread_setname_np(pthread_t t, const char *nm) {
    return pthread_equal(t, pthread_self()) ? pthread_setname_np(nm) : 0;
}
int klb_pthread_atfork(void (*p)(void), void (*c)(void), void (*ch)(void)) {
    return pthread_atfork(p, c, ch);
}

// ---------- semaphores ----------
// bionic sem_t is 4 bytes -- too small for a pointer, so it holds a table index.
// Darwin's POSIX sem_init is deprecated/unimplemented, so back them with GCD.
// Bionic has no limit of its own, and 1024 was not enough: Arizona Sunshine
// holds more than that LIVE while it loads (1,395 sem_init calls answered
// ENOSPC, every "Failed to open a semaphore" in Unity's log, then FMOD's
// Studio::System::initialize failing ERR_INTERNAL on the same shortage). 64K
// slots is 1 MiB of table; the high-water mark is logged at each power of two
// past 1024, so a genuine leak still shows itself.
#define KL_MAX_SEMS 65536

// The count is tracked alongside the dispatch semaphore rather than left to GCD.
// It has to be: GCD gives no way to read the count, and IL2CPP POLLS it, so a
// constant 0 from sem_getvalue is not an answer. The loop at libil2cpp+0x12d57d4
// is a barrier — usleep(3000), sem_getvalue, compare against a target of
// initial+N, repeat — so a value that never changes is an infinite spin with the
// main thread apparently just asleep — a silent zero read as an answer rather
// than as "unknown".
typedef struct {
    dispatch_semaphore_t d;
    _Atomic int          count;
    int                  initial;   // what it was created with, for the release
                                    // rule in klb_sem_destroy
} kl_sem;
static kl_sem g_sems[KL_MAX_SEMS];
static _Atomic int g_nsems = 1;                 // index 0 means "uninitialised"

// The free list. An atomic_fetch_add-only g_nsems against KL_MAX_SEMS makes the
// table a ONE-WAY budget for the life of the process: after 1024 sem_init calls
// every later one answers ENOSPC forever, whatever was destroyed in between —
// SUPERHOT's slot leak ~80 s into play, which lands at about the same point in a
// run as the OOM and is a different bug.
//
// A dead slot must keep `d` NULL, because sem_of() uses a live `d` as its
// validity test: a recycled slot holding a stale dispatch_semaphore_t would
// answer for a semaphore the guest destroyed.
static int      g_sem_free[KL_MAX_SEMS];
static int      g_sem_nfree;
static pthread_mutex_t g_sem_lock = PTHREAD_MUTEX_INITIALIZER;

int klb_sem_init(int *s, int pshared, unsigned value) {
    (void)pshared;
    int i = 0;
    pthread_mutex_lock(&g_sem_lock);
    if (g_sem_nfree) i = g_sem_free[--g_sem_nfree];
    pthread_mutex_unlock(&g_sem_lock);
    if (!i) {
        i = atomic_fetch_add(&g_nsems, 1);
        // A fresh slot means the free list was empty: i is the live high-water.
        if (i >= 1024 && (i & (i - 1)) == 0)
            fprintf(stderr, "  [klepton] sem: %d semaphores live at once (high-water; "
                            "table %d)\n", i, KL_MAX_SEMS);
    }
    if (i >= KL_MAX_SEMS) { errno = ENOSPC; return -1; }
    // Fill the slot BEFORE publishing the index. The old order bumped g_nsems
    // first, which made the range check pass while the slot was still empty — a
    // waiter on another thread then got EINVAL from a semaphore that had, as far
    // as its owner was concerned, been initialised.
    atomic_store(&g_sems[i].count, (int)value);
    g_sems[i].initial = (int)value;
    g_sems[i].d = dispatch_semaphore_create(value);
    atomic_thread_fence(memory_order_release);
    *s = i;
    return 0;
}

static kl_sem *sem_of(int *s) {
    int i = *s;
    if (i > 0 && i < KL_MAX_SEMS && g_sems[i].d) return &g_sems[i];
    static _Atomic int logged;
    if (atomic_fetch_add(&logged, 1) < 8)
        fprintf(stderr, "  [klepton] sem: no semaphore for slot %d — the guest is "
                        "waiting on one it never initialised here\n", i);
    return NULL;
}

int klb_sem_destroy(int *s) {
    int i = *s;
    *s = 0;
    if (i <= 0 || i >= KL_MAX_SEMS) return 0;
    // Clear `d` before the slot is offered back, and offer it back only once —
    // a double sem_destroy is guest UB, but it must not put the same index on
    // the free list twice and hand two live semaphores one slot.
    pthread_mutex_lock(&g_sem_lock);
    dispatch_semaphore_t d = g_sems[i].d;
    int releasable = d && atomic_load(&g_sems[i].count) >= g_sems[i].initial;
    if (d) {
        g_sems[i].d = NULL;
        atomic_store(&g_sems[i].count, 0);
        g_sems[i].initial = 0;
        if (g_sem_nfree < KL_MAX_SEMS) g_sem_free[g_sem_nfree++] = i;
    }
    pthread_mutex_unlock(&g_sem_lock);
    // GCD *aborts* if a dispatch semaphore is deallocated below the value it
    // was created with — "Semaphore object deallocated while in use" — which is
    // the guest destroying one that still has waiters, i.e. its bug, and not
    // one worth turning into a crash of ours. So it is released only when the
    // count is back where it started, and otherwise deliberately dropped: one
    // small object leaked against a crash inside a destructor.
    if (releasable) dispatch_release(d);
    return 0;
}

int klb_sem_post(int *s) {
    kl_sem *k = sem_of(s);
    if (!k) { errno = EINVAL; return -1; }
    if (kl_env_on("KL_TRACE_SIG", 0)) {
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 12)
            fprintf(stderr, "  [sig] sem_post slot %d (count now %d)\n",
                    *s, atomic_load(&k->count) + 1);
    }
    atomic_fetch_add(&k->count, 1);
    dispatch_semaphore_signal(k->d);
    return 0;
}

int klb_sem_wait(int *s) {
    kl_sem *k = sem_of(s);
    if (!k) { errno = EINVAL; return -1; }
    sem_wait_enter(s, __builtin_return_address(0));
    dispatch_semaphore_wait(k->d, DISPATCH_TIME_FOREVER);
    sem_wait_leave();
    atomic_fetch_sub(&k->count, 1);
    return 0;
}

int klb_sem_trywait(int *s) {
    kl_sem *k = sem_of(s);
    if (!k) { errno = EINVAL; return -1; }
    if (dispatch_semaphore_wait(k->d, DISPATCH_TIME_NOW) != 0) { errno = EAGAIN; return -1; }
    atomic_fetch_sub(&k->count, 1);
    return 0;
}

// sem_open/sem_close: bionic's named semaphores, modelled over the same slot
// table as sem_init. Darwin's own sem_open is persistent across processes,
// which is the wrong semantics here — the guest's names are process-scoped.
// The value argument only exists when O_CREAT (Linux 0x40 — NOT Darwin's
// 0x200) is set; the receive-side variadic read is the klb_execl pattern.
void *klb_sem_open(const char *name, int oflag, ...) {
    static struct { char name[128]; int slot; int used; } named[64];
    for (int i = 0; i < 64; i++)
        if (named[i].used && strcmp(named[i].name, name) == 0)
            return &named[i].slot;
    if (!(oflag & 0x40)) { errno = ENOENT; return (void *)-1; }  // SEM_FAILED
    va_list ap;
    va_start(ap, oflag);
    (void)va_arg(ap, int);              // mode_t mode — permission bits, moot
    unsigned value = va_arg(ap, unsigned);
    va_end(ap);
    for (int i = 0; i < 64; i++)
        if (!named[i].used) {
            if (klb_sem_init(&named[i].slot, 0, value) != 0) return (void *)-1;
            snprintf(named[i].name, sizeof named[i].name, "%s", name);
            named[i].used = 1;
            return &named[i].slot;
        }
    errno = ENOSPC;
    return (void *)-1;
}

// Deliberately not destroying the semaphore: a closed name that is re-opened
// must come back with its count intact. The table is bounded and tiny.
int klb_sem_close(void *s) { (void)s; return 0; }

// Scheduling policy is the host's business; the honest no-op is success.
int klb_pthread_getschedparam(void *t, int *policy, void *param) {
    (void)t;
    if (policy) *policy = 0;              // SCHED_OTHER
    if (param)  *(int *)param = 0;        // sched_param.sched_priority
    return 0;
}
int klb_pthread_setschedparam(void *t, int policy, const void *param) {
    (void)t; (void)policy; (void)param;
    return 0;
}

int klb_sem_timedwait(int *s, const struct timespec *ts) {
    kl_sem *k = sem_of(s);
    if (!k) { errno = EINVAL; return -1; }
    dispatch_time_t when = dispatch_walltime(ts, 0);
    if (dispatch_semaphore_wait(k->d, when) != 0) { errno = ETIMEDOUT; return -1; }
    atomic_fetch_sub(&k->count, 1);
    return 0;
}

// POSIX lets an implementation report 0 rather than a negative count when there
// are waiters, which is what Linux does — so clamp rather than exposing our
// internal debt.
int klb_sem_getvalue(int *s, int *out) {
    kl_sem *k = sem_of(s);
    if (!k) { errno = EINVAL; return -1; }
    int v = atomic_load(&k->count);
    *out = v > 0 ? v : 0;
    return 0;
}
