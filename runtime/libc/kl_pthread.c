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
// The side table is CHUNKED and grows on demand: a fixed directory of
// pointers to fixed-size chunks, each allocated on first use and never moved
// or freed, so a slot index (the only thing guest storage holds) and any host
// pointer read out of a slot stay valid for the life of the process. It used
// to be a flat static [65536] that aborted when full. Init no longer recycles
// the slot index it finds in guest storage (see klb_pthread_barrier_init: an
// engine that memmoves arrays leaves a LIVE index behind in the freed block),
// so a re-init without destroy leaks its slot by design — and Deadpool VR's
// Unreal 4.27 runtime did ~5 such re-inits a second, which reaches 65536 in
// 3-4 hours of play. Cost now: 32 KB of
// directory per kind up front, then 48 KB per 4096 slots actually used
// (pointer + free-list word each). The hard ceiling is 2^24 slots, ~3 days of
// that churn; a log at each power of two past 65536 shows the climb long before.
#define KL_SYNC_OLD_CEILING 65536u
#define KL_SYNC_CHUNK_BITS  12
#define KL_SYNC_CHUNK       (1u << KL_SYNC_CHUNK_BITS)
#define KL_SYNC_DIR         4096u
#define KL_MAX_SYNC         (KL_SYNC_CHUNK * KL_SYNC_DIR)      /* 2^24 */
// Element `idx` of a chunked array; allocates (zeroed) the chunk on first
// touch. The loser of a racing allocation frees its copy and uses the winner's.
static void *kl_sync_cell(_Atomic(void *) *dir, uint32_t idx, size_t elem) {
    _Atomic(void *) *d = &dir[idx >> KL_SYNC_CHUNK_BITS];
    char *c = atomic_load_explicit(d, memory_order_acquire);
    if (!c) {
        char *mine = calloc(KL_SYNC_CHUNK, elem);
        if (!mine) {
            fprintf(stderr, "  [klb] FATAL: out of memory growing the sync side table "
                    "(slot %u)\n", idx);
            abort();
        }
        void *expect = NULL;
        if (atomic_compare_exchange_strong(d, &expect, mine)) c = mine;
        else { free(mine); c = expect; }
    }
    return c + (size_t)(idx & (KL_SYNC_CHUNK - 1)) * elem;
}
static void kl_sync_grew(const char *name, uint32_t mine) {
    if (mine >= KL_MAX_SYNC) {
        fprintf(stderr, "  [klb] FATAL: sync side table %s is full (%u slots, ~%u MB); "
                "something is leaking a slot per call far faster than any guest "
                "observed so far — see the stale-init counters\n",
                name, KL_MAX_SYNC, (unsigned)((KL_MAX_SYNC * 12ull) >> 20));
        abort();
    }
    if (mine >= KL_SYNC_OLD_CEILING && (mine & (mine - 1)) == 0)
        fprintf(stderr, "  [klb] sync side table %s grew past %u slots (the old fixed "
                "ceiling was %u; leaked stale-init slots are the usual cause)\n",
                name, mine, KL_SYNC_OLD_CEILING);
}
// Table slots recycle: with the JNI pool leak fixed, long runs otherwise die
// here — Unity creates and destroys a mutex or two per frame, and a leaked
// slot per destroy is 65536 creates at ~frame 40k (observed). The kind object
// behind a recycled slot is replaced, not shared; the old one leaks (a
// destroy is the guest saying nobody holds it).
#define SYNC_TABLE(kind, tab, count, init_expr)                                  \
    static _Atomic(void *) tab##_dir[KL_SYNC_DIR];                               \
    static _Atomic uint32_t count = 1;              /* 0 means uninitialised */  \
    static _Atomic(void *) tab##_free_dir[KL_SYNC_DIR];                          \
    static _Atomic uint32_t tab##_nfree;                                         \
    static kind **tab##_at(uint32_t i) {                                         \
        return (kind **)kl_sync_cell(tab##_dir, i, sizeof(kind *));              \
    }                                                                            \
    static uint32_t *tab##_free_at(uint32_t i) {                                 \
        return (uint32_t *)kl_sync_cell(tab##_free_dir, i, sizeof(uint32_t));    \
    }                                                                            \
    static kind *tab##_get(void *g) {                                            \
        _Atomic uint32_t *slot = (_Atomic uint32_t *)(g);                        \
        uint32_t idx = atomic_load(slot);                                        \
        if (idx) return *tab##_at(idx);                                          \
        kind *fresh = malloc(sizeof *fresh);                                     \
        init_expr;                                                               \
        uint32_t mine;                                                           \
        uint32_t nf = atomic_load(&tab##_nfree);                                 \
        if (nf && atomic_compare_exchange_strong(&tab##_nfree, &nf, nf - 1))     \
            mine = *tab##_free_at(nf - 1);                                       \
        else {                                                                   \
            mine = atomic_fetch_add(&count, 1);                                  \
            kl_sync_grew(#tab, mine);                                            \
        }                                                                        \
        *tab##_at(mine) = fresh;                                                 \
        uint32_t expect = 0;                                                     \
        if (!atomic_compare_exchange_strong(slot, &expect, mine)) {              \
            /* lost the race; leak this slot rather than free a live object */   \
            return *tab##_at(expect);                                            \
        }                                                                        \
        return fresh;                                                            \
    }                                                                            \
    static void tab##_recycle(void *g) {                                         \
        _Atomic uint32_t *slot = (_Atomic uint32_t *)(g);                        \
        uint32_t idx = atomic_exchange(slot, 0);                                 \
        if (!idx || idx >= count) return;                                        \
        uint32_t nf = atomic_fetch_add(&tab##_nfree, 1);                         \
        if (nf < KL_MAX_SYNC) *tab##_free_at(nf) = idx;                          \
    }

// Rwlocks are not on this table: see the rwl_entry map below. Refusing to
// recycle at init covers a re-init over a relocated block, but not storage
// that is reused WITHOUT any init (90 of 101 lock sites in Pinball FX's
// libUnreal never call pthread_rwlock_init) where the stale slot index is
// simply believed. Address keying covers both.

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
// Power of two. TWD2's UE4 load creates ~32k mutexes without destroying any,
// which overflowed a 32768-entry table; 2^18 leaves a large UE4 load room, and
// with tombstones (below) occupancy tracks LIVE mutexes rather than every
// address ever used as one. If it does fill, mtx_entry_for degrades to a shared
// fallback mutex rather than hanging.
#define MTX_MAP_BITS 18
#define MTX_MAP_SIZE (1u << MTX_MAP_BITS)
// Key sentinels. A guest mutex address is a real pointer — page-aligned storage
// at minimum, never 0, 1 or 2 — so three low values are free to mean something.
//
// **Why the tombstone.** This is an open-addressed table with LINEAR PROBING,
// and `destroy` used to
// write 0 — a true empty — into the middle of a live probe chain. Every entry
// behind that hole became unreachable: the next lookup of such a key stopped at
// the hole, inserted a DUPLICATE there, and orphaned the original for the life
// of the process. Orphans only accumulate, so the table filled monotonically
// under mutex churn until the probe found no empty slot at all. Worse, a
// duplicate is a LOST LOCK: the thread holding the original host mutex and the
// thread that resolved the duplicate do not exclude each other. On a build
// whose probe was unbounded this was Pinball FX's table-load stall: the lobby
// ran 2.5 minutes, the table load churned enough mutexes to top the table off,
// and the render thread never returned from `pthread_mutex_init` — no crash,
// no log, one core at 100%.
#define MTX_EMPTY  ((uintptr_t)0)   // never used; probing stops here
#define MTX_TOMB   ((uintptr_t)1)   // destroyed; probing CONTINUES through it
#define MTX_CLAIM  ((uintptr_t)2)   // reserved, contents not yet published
typedef struct {
    _Atomic(uintptr_t)   key;       // guest address, or one of the sentinels
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

// Fibonacci hashing: the top MTX_MAP_BITS of the multiply, not a fixed 15. The
// shift was hard-coded at 49 when the table was 2^15, and the table grew
// without it: every key folded into the first 32768 slots, and the probe
// chains were eight times longer than the table needs.
static unsigned mtx_hash(uintptr_t a) {
    return (unsigned)(((a >> 4) * 0x9E3779B97F4A7C15ULL) >> (64 - MTX_MAP_BITS));
}

// KL_MUTEX_POLICY=fairshare hands a guest mutex to its waiter on unlock
// instead of Darwin's default first-fit, where the unlocking thread that
// immediately re-locks wins the race nearly every time. Radical's radLoad
// (The Simpsons: Hit & Run) runs its loader on a second thread and shares ONE
// mutex with the game thread as a baton -- `Unlock(); Sleep(0); Lock();` on
// both sides -- so under first-fit the baton changes hands about once per
// frame and a 1.4 MB zone takes 2+ s to stream (the P3D loader logged
// "Starved for 79 frames, 1250 ms" on a 192 KB block whose fread took under
// 10 ms). Fair handoff is what bionic's futex wake gave that code on the
// Quest. Off unless the environment asks; `firstfit` is the A/B.
#ifndef _PTHREAD_MUTEX_POLICY_FAIRSHARE
#define _PTHREAD_MUTEX_POLICY_FAIRSHARE 1
#endif
static int mtx_policy_fairshare(void) {
    static int on = -1;
    if (on < 0) {
        const char *s = kl_env_str("KL_MUTEX_POLICY", NULL);
        on = (s && !strcmp(s, "fairshare")) ? 1 : 0;
        if (on) fprintf(stderr, "  [thr] guest mutexes use the FAIRSHARE policy "
                                "(KL_MUTEX_POLICY=firstfit is the A/B)\n");
    }
    return on;
}
static pthread_mutex_t *mtx_make(void) {
    pthread_mutex_t *fresh = malloc(sizeof *fresh);
    if (!fresh) return NULL;
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    if (mtx_policy_fairshare())
        pthread_mutexattr_setpolicy_np(&a, _PTHREAD_MUTEX_POLICY_FAIRSHARE);
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
    // BOUNDED, for the mutex map's reason. This map never tombstones —
    // cond_destroy is deliberately a no-op, because a host cond the guest might
    // still signal is worse to free than to leak — so it only ever grows, and
    // an unbounded probe over a full table is an infinite 100%-CPU spin with no
    // crash and no log. If this ever fires, the answer is a bigger table, and
    // the abort is what makes that visible instead of a frozen headset.
    for (unsigned n = 0; n < MTX_MAP_SIZE; n++) {
        unsigned i = (mtx_hash(want) + n) & (MTX_MAP_SIZE - 1);
        uintptr_t k = atomic_load(&g_cnd_map[i].key);
        if (k == want) {
            if (fresh) { pthread_cond_destroy(fresh); free(fresh); }
            return &g_cnd_map[i];
        }
        if (k) continue;
        if (!fresh) fresh = cnd_make();
        uintptr_t expect = 0;
        if (!atomic_compare_exchange_strong(&g_cnd_map[i].key, &expect, want))
            { n--; continue; }
        atomic_store(&g_cnd_map[i].c, fresh);
        return &g_cnd_map[i];
    }
    if (fresh) { pthread_cond_destroy(fresh); free(fresh); }
    fprintf(stderr,
            "  [klb] FATAL: the condvar map is full — %u slots, all taken, "
            "probing for guest %p. Condvars are never reclaimed by design; "
            "this guest made more than the table holds.\n",
            (unsigned)MTX_MAP_SIZE, (void *)want);
    kl_pthread_report(stderr);
    abort();
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
// How many slots are not MTX_EMPTY. Tombstones are reusable, so this is the
// high-water mark of the PROBE, not of live mutexes — it is what says how close
// the table is to the state that used to hang, and it is the only number worth
// printing about this map.
static _Atomic(unsigned) g_mtx_used;

// Exposed for `t_mtx`, and only for it. The gate's subject is whether destroyed
// slots come BACK, and "the process did not hang" is far too weak a way to ask:
// with destroy writing an empty, 500,000 churn cycles through 64 addresses
// consumed 98,304 of 131,072 slots and still finished. Occupancy is the number
// that tells the two behaviours apart at a glance.
unsigned kl_pthread_mtx_used(void) { return atomic_load(&g_mtx_used); }

// A single shared fallback used only when the whole table is full — better a few
// guest mutexes serialising on one recursive host mutex than an infinite probe
// spin. With tombstones reclaiming destroyed slots this needs more LIVE mutexes
// than the table holds; it exists so a pathological guest degrades instead of
// hanging (which is what TWD2 did at the old 32768).
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
                        "mutex; sync correctness degraded (raise MTX_MAP_BITS)\n",
                MTX_MAP_SIZE);
    return &g_mtx_overflow;
}

static mtx_entry *mtx_entry_for(void *g) {
    uintptr_t want = (uintptr_t)g;
    pthread_mutex_t *fresh = NULL;
    for (;;) {
        unsigned start = mtx_hash(want) & (MTX_MAP_SIZE - 1);
        long tomb = -1;                 // first reusable slot seen on this pass
        unsigned n = 0;
        for (; n < MTX_MAP_SIZE; n++) {
            unsigned i = (start + n) & (MTX_MAP_SIZE - 1);
            uintptr_t k = atomic_load(&g_mtx_map[i].key);
            if (k == want) {
                if (fresh) { pthread_mutex_destroy(fresh); free(fresh); }
                return &g_mtx_map[i];
            }
            // A tombstone is a candidate to reuse, but the scan does NOT stop
            // here: `want` may still be live further along a chain that ran
            // through this slot before it was destroyed. Stopping early is how
            // duplicates are made.
            if (k == MTX_TOMB) { if (tomb < 0) tomb = (long)i; continue; }
            // A slot mid-claim must be WAITED ON, never stepped over. The
            // claim may be for `want` itself, and a prober that walks past it
            // finds the next empty and inserts a DUPLICATE — two entries, two
            // host mutexes, one guest address. That is not a slow path, it is
            // a lost lock: the thread that took host mutex A resolves the same
            // address to host mutex B on the way out and unlocks the wrong one,
            // leaving A held forever. (Measured, not theorised — it deadlocked
            // t_mtx's first-touch race on the first run.)
            //
            // The wait cannot cycle: a thread holding a claim never waits on
            // another one — its own duplicate re-scan below skips claims rather
            // than blocking on them — so every claim resolves in a bounded
            // number of instructions.
            if (k == MTX_CLAIM) { sched_yield(); n--; continue; }   // re-read
            // Live, and not ours — unless this is the last slot of a full lap
            // and a tombstone was passed: a table with no empty left but
            // reclaimed slots in it still has room, in the tombstone.
            if (k != MTX_EMPTY && !(n == MTX_MAP_SIZE - 1 && tomb >= 0)) continue;
            // A true empty ends the chain: `want` is definitively absent, so
            // this is the insert. Prefer the first tombstone — that is what
            // keeps occupancy bounded by LIVE mutexes rather than by every
            // address ever used as one.
            if (!fresh) fresh = mtx_make();
            unsigned at = (tomb >= 0) ? (unsigned)tomb : i;
            uintptr_t expect = (tomb >= 0) ? MTX_TOMB : MTX_EMPTY;
            // Reserve before publishing. The old code stored the key first and
            // the mutex after, and any thread probing in that window got the
            // entry with m == NULL; MTX_CLAIM is that window made explicit, and
            // it is skipped by every other prober because it is not MTX_EMPTY.
            if (!atomic_compare_exchange_strong(&g_mtx_map[at].key, &expect,
                                                MTX_CLAIM))
                break;                  // lost the slot; rescan from the top
            mtx_entry *e = &g_mtx_map[at];
            // Re-scan for a live `want` before committing. Only reachable on
            // the insert path, and it closes the one window the tombstone
            // reuse opens: another thread may have inserted `want` between our
            // sighting of this tombstone and our arrival at the empty.
            mtx_entry *dup = NULL;
            for (unsigned j = 0; j < MTX_MAP_SIZE; j++) {
                unsigned p = (start + j) & (MTX_MAP_SIZE - 1);
                if (p == at) continue;
                uintptr_t kk = atomic_load(&g_mtx_map[p].key);
                if (kk == want) { dup = &g_mtx_map[p]; break; }
                if (kk == MTX_EMPTY) break;
            }
            if (dup) {                  // someone beat us; give the slot back
                atomic_store(&e->key, expect);
                if (fresh) { pthread_mutex_destroy(fresh); free(fresh); }
                return dup;
            }
            if (expect == MTX_EMPTY) {
                unsigned used = atomic_fetch_add(&g_mtx_used, 1) + 1;
                // Say so ONCE, well before it fills. A guest that legitimately
                // needs three quarters of the table is a guest whose next
                // version needs all of it, and this is the only warning that
                // can be given while the run is still healthy.
                if (used == (MTX_MAP_SIZE / 4) * 3)
                    fprintf(stderr,
                            "  [klb] the mutex map is %u of %u slots deep — "
                            "not fatal, but the table was sized for far less; "
                            "raise MTX_MAP_BITS before it fills.\n",
                            used, (unsigned)MTX_MAP_SIZE);
            }
            // A recycled slot still points at its previous life's host mutex.
            // That object is deliberately leaked rather than freed — destroy
            // cannot know whether the guest still holds it — but it must not be
            // INHERITED by a different guest address, and the stale
            // owner/depth must not be either.
            atomic_store(&e->m, fresh);
            atomic_store(&e->owner, NULL);
            atomic_store(&e->locksite, NULL);
            atomic_store(&e->depth, 0);
            atomic_store(&e->key, want);        // publish last
            return e;
        }
        if (n == MTX_MAP_SIZE) {
            // Neither a match, nor an empty, nor a tombstone in a full lap.
            if (fresh) { pthread_mutex_destroy(fresh); free(fresh); }
            return mtx_overflow();
        }
    }
}

static mtx_entry *mtx_find(void *g) {
    uintptr_t want = (uintptr_t)g;
    for (unsigned n = 0; n < MTX_MAP_SIZE; n++) {
        unsigned i = (mtx_hash(want) + n) & (MTX_MAP_SIZE - 1);
        uintptr_t k = atomic_load(&g_mtx_map[i].key);
        if (k == want) return &g_mtx_map[i];
        if (k == MTX_EMPTY) return NULL;    // tombstones do NOT end the chain
    }
    return NULL;
}

// ---------- rwlocks: address-keyed, for the mutex map's reason ----------
//
// Rwlocks were the last sync type still on the slot-in-guest-storage SYNC_TABLE
// after mutexes and condvars were moved off it, and they failed for exactly the
// reason the mutex map's comment predicts: a slot index living in guest storage
// aliases two logical objects onto one host object whenever that storage is
// stale or copied.
//
// It was measured on Pinball FX's table load. The guest's array
// entries are 0x50 bytes with a `pthread_rwlock_t` at +8, walked by a linear
// scan — a TArray, which UE reallocates and **memcpy**s wholesale when it
// grows. A moved entry carries its slot index into the new storage, and the
// freed storage is handed back out for a new entry that has never called
// `pthread_rwlock_init` — bionic's static initialiser is all-zero, and 90 of
// this guest's 101 lock sites never init at all, so the stale index is simply
// believed. Two unrelated guest rwlocks, one host rwlock.
//
// The stall dump caught the consequence exactly: ONE guest thread blocked in
// `pthread_rwlock_wrlock`, and no other thread anywhere in the process inside a
// lock call — every other guest thread idle in a cond wait or a usleep. Nobody
// was holding it legitimately, because the thread blocking on it was the thread
// holding it: it took a READ lock on entry A at one call site and a WRITE lock
// on entry B four instructions later, and A and B were the same host object. On
// Android they are two, and the guest runs.
//
// So: keyed on the guest ADDRESS, guest storage never read or written. A copied
// or recycled address is the same rwlock; a different address is a different
// rwlock. Same shape as the cond map — create-on-demand, destroy leaks (a host
// rwlock is 200 bytes, and freeing one the guest might still lock is worse than
// keeping it), bounded probe with an abort rather than an unbounded spin.
typedef struct {
    _Atomic(uintptr_t)          key;
    _Atomic(pthread_rwlock_t *) l;
    // Who holds it, so a stall dump can NAME the holder instead of leaving a
    // blocked thread with nothing on the other end of the wait. Without it the
    // dump above could say only "blocked in wrlock"; the whole diagnosis had
    // to come from what was NOT in the dump.
    _Atomic(void *)             writer;   // tid holding it for write, or NULL
    _Atomic(void *)             wsite;    // where that write lock was taken
    _Atomic(int)                readers;  // read holders, across all threads
} rwl_entry;
static rwl_entry g_rwl_map[MTX_MAP_SIZE];

static struct { _Atomic(void *) tid, guest, ra; _Atomic(int) write; }
    g_rwl_waiters[64];

static pthread_rwlock_t *rwl_make(void) {
    pthread_rwlock_t *fresh = malloc(sizeof *fresh);
    if (fresh) pthread_rwlock_init(fresh, NULL);
    return fresh;
}

static rwl_entry *rwl_entry_for(void *g) {
    uintptr_t want = (uintptr_t)g;
    pthread_rwlock_t *fresh = NULL;
    for (unsigned n = 0; n < MTX_MAP_SIZE; n++) {
        unsigned i = (mtx_hash(want) + n) & (MTX_MAP_SIZE - 1);
        uintptr_t k = atomic_load(&g_rwl_map[i].key);
        if (k == want) {
            if (fresh) { pthread_rwlock_destroy(fresh); free(fresh); }
            return &g_rwl_map[i];
        }
        if (k) continue;
        if (!fresh) fresh = rwl_make();
        uintptr_t expect = 0;
        if (!atomic_compare_exchange_strong(&g_rwl_map[i].key, &expect, want))
            { n--; continue; }
        atomic_store(&g_rwl_map[i].l, fresh);
        return &g_rwl_map[i];
    }
    if (fresh) { pthread_rwlock_destroy(fresh); free(fresh); }
    fprintf(stderr,
            "  [klb] FATAL: the rwlock map is full — %u slots, all taken, "
            "probing for guest %p. Rwlocks are never reclaimed by design; "
            "this guest made more than the table holds.\n",
            (unsigned)MTX_MAP_SIZE, (void *)want);
    kl_pthread_report(stderr);
    abort();
}

// The key is published before the lock is stored, so a reader that arrives in
// that window waits rather than dereferencing NULL — the mutex map's rule.
static pthread_rwlock_t *rwl_host(rwl_entry *e) {
    pthread_rwlock_t *l = atomic_load(&e->l);
    while (!l) { sched_yield(); l = atomic_load(&e->l); }
    return l;
}
// (There is deliberately no one-shot `rwl(g)` helper any more: every entry
// point below needs the ENTRY, not just the host lock, because ownership is
// recorded on it — and a helper that returned only the lock is how the previous
// design made the holder unnameable in a stall dump.)

// Lookup that never creates and never aborts — the report's form. This map has
// no tombstones (destroy is a no-op), so an empty slot really does end a chain.
static rwl_entry *rwl_find(void *g) {
    uintptr_t want = (uintptr_t)g;
    for (unsigned n = 0; n < MTX_MAP_SIZE; n++) {
        unsigned i = (mtx_hash(want) + n) & (MTX_MAP_SIZE - 1);
        uintptr_t k = atomic_load(&g_rwl_map[i].key);
        if (k == want) return &g_rwl_map[i];
        if (!k) return NULL;
    }
    return NULL;
}

static void rwl_wait_enter(void *guest, void *ra, int write) {
    void *self = (void *)pthread_self();
    for (int i = 0; i < 64; i++) {
        void *expect = NULL;
        if (atomic_compare_exchange_strong(&g_rwl_waiters[i].tid, &expect, self)) {
            atomic_store(&g_rwl_waiters[i].guest, guest);
            atomic_store(&g_rwl_waiters[i].ra, ra);
            atomic_store(&g_rwl_waiters[i].write, write);
            return;
        }
    }
}
static void rwl_wait_leave(void) {
    void *self = (void *)pthread_self();
    for (int i = 0; i < 64; i++) {
        void *expect = self;
        if (atomic_compare_exchange_strong(&g_rwl_waiters[i].tid, &expect, NULL))
            return;
    }
}

// A thread that already holds this rwlock for WRITE and asks for it again is
// about to block forever against itself. With the aliasing gone that can only
// be the guest's own lock order, so say so before the process goes quiet.
static void rwl_self_check(void *g, void *ra, void *wsite) {
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) >= 4) return;
    size_t off = 0;
    const char *img = kl_addr_image(ra, &off);
    fprintf(stderr, "  [klb] pthread_rwlock_wrlock on guest %p by the thread "
                    "that already WRITE-holds it (taken at %p) — this will "
                    "block forever; from ", g, wsite);
    if (img) fprintf(stderr, "%s+0x%zx\n", img, off); else fprintf(stderr, "%p\n", ra);
}


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
    // TOMBSTONE, not empty. Writing 0 here is what broke every probe chain
    // running through this slot and, over a long session, filled the table and
    // hung the guest — see the sentinel comment at the top of the file. The
    // host mutex is still leaked rather than freed (the guest may hold it); it
    // is released when the slot is next claimed.
    if (e) atomic_store(&e->key, MTX_TOMB);
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
    // Rwlocks, on the same terms. Pinball FX's stall was a thread blocked in
    // pthread_rwlock_wrlock with NOTHING on the other end of the report — the
    // holder had to be inferred from the absence of any other blocked thread.
    // Holder and waiter together are what say a run is STUCK rather than busy,
    // and a WRITE-holder that is also the waiter names the bug outright.
    unsigned rwl_shown = 0;
    for (uint32_t i = 0; i < MTX_MAP_SIZE; i++) {
        void *w = atomic_load(&g_rwl_map[i].writer);
        int rd = atomic_load(&g_rwl_map[i].readers);
        if (!w && rd <= 0) continue;
        void *key = (void *)atomic_load(&g_rwl_map[i].key);
        if (w) {
            const char *nm = thread_name_of(w);
            fprintf(out, "  guest rwlock %p: WRITE-held by tid %p%s%s%s%s "
                         "(locked from %p), %d reader(s) counted\n",
                    key, w, nm ? " [" : "", nm ? nm : "", nm ? "]" : "",
                    thread_is_alive(w) ? "" : "  ** EXITED **",
                    atomic_load(&g_rwl_map[i].wsite), rd);
            dump_thread_stack(out, w);
        } else {
            fprintf(out, "  guest rwlock %p: %d reader(s) counted, no writer\n",
                    key, rd);
        }
        rwl_shown++;
        if (rwl_shown >= 64) { fprintf(out, "  ...(more rwlocks held)\n"); break; }
    }
    for (int i = 0; i < 64; i++) {
        void *tid = atomic_load(&g_rwl_waiters[i].tid);
        if (!tid) continue;
        const char *nm = thread_name_of(tid);
        void *guest = atomic_load(&g_rwl_waiters[i].guest);
        void *ra = atomic_load(&g_rwl_waiters[i].ra);
        int write = atomic_load(&g_rwl_waiters[i].write);
        // find, never create: this report is also what the map-full abort
        // prints, and a creating lookup there would recurse into that abort.
        rwl_entry *e = rwl_find(guest);
        void *holder = e ? atomic_load(&e->writer) : NULL;
        size_t off = 0;
        const char *img = kl_addr_image(ra, &off);
        fprintf(out, "  waiter tid %p%s%s%s wants guest rwlock %p for %s (from ",
                tid, nm ? " [" : "", nm ? nm : "", nm ? "]" : "", guest,
                write ? "WRITE" : "read");
        if (img) fprintf(out, "%s+0x%zx)", img, off); else fprintf(out, "%p)", ra);
        if (holder == tid)
            fprintf(out, "  ** AND ALREADY WRITE-HOLDS IT — self-deadlock **");
        else if (holder)
            fprintf(out, "  [write-held by tid %p]", holder);
        fprintf(out, "\n");
        dump_thread_stack(out, tid);
    }
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
// ones ~1.7e9), which classifies both clocks correctly. Any other clock is
// EINVAL, as bionic answers (Linux numbering: REALTIME 0, MONOTONIC 1).
int klb_pthread_cond_clockwait(void *c, void *m, int clk, const struct timespec *ts) {
    if (clk != 0 && clk != 1) return kl_errno_to_linux(EINVAL);
    return klb_pthread_cond_timedwait(c, m, ts);
}
int klb_pthread_condattr_init(long *a)                 { *a = 0; return 0; }
int klb_pthread_condattr_destroy(long *a)              { (void)a; return 0; }
int klb_pthread_condattr_setclock(long *a, int clk)    { (void)a; (void)clk; return 0; }

// ---------- rwlock ----------
// Address-keyed; see the rwl_entry map above for why guest storage is never
// read. `l == NULL` would match the map's empty-slot sentinel, so it is refused
// the way a NULL mutex is rather than handed back an entry with no lock in it.
static int rwl_null(const char *what) {
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) < 4)
        fprintf(stderr, "  [klb] pthread_rwlock_%s(NULL) from the guest — "
                        "refusing with EINVAL (its own bug)\n", what);
    return kl_errno_to_linux(EINVAL);
}

// Re-init KEEPS the host rwlock, for the reason klb_pthread_mutex_init spells
// out: a recycled address is the same lock, and swapping the host object under
// a guest that still holds or waits on it is the manufactured deadlock this map
// exists to prevent.
// kl_pthread_rwl_stale_inits (counted for t_mtx): under address keying a
// "stale init" is an init at an address that already has an entry — a re-init
// without destroy, or a reused block. It keeps its host rwlock (see above).
static _Atomic unsigned g_rwl_stale_inits;
unsigned kl_pthread_rwl_stale_inits(void) { return atomic_load(&g_rwl_stale_inits); }
int klb_pthread_rwlock_init(void *l, const void *a) {
    (void)a;
    if (!l) return rwl_null("init");
    if (rwl_find(l)) atomic_fetch_add(&g_rwl_stale_inits, 1);
    rwl_entry *e = rwl_entry_for(l);
    rwl_host(e);
    atomic_store(&e->writer, NULL);
    atomic_store(&e->wsite, NULL);
    atomic_store(&e->readers, 0);
    return 0;
}
// Deliberately a no-op, exactly like klb_pthread_cond_destroy: the entry is
// keyed on an address the guest may hand straight back to a new object, and a
// host rwlock is far cheaper to leak than to free under a guest that still
// locks it.
int klb_pthread_rwlock_destroy(void *p) { (void)p; return 0; }

int klb_pthread_rwlock_rdlock(void *l) {
    if (!l) return rwl_null("rdlock");
    rwl_entry *e = rwl_entry_for(l);
    rwl_wait_enter(l, __builtin_return_address(0), 0);
    int r = pthread_rwlock_rdlock(rwl_host(e));
    rwl_wait_leave();
    if (!r) atomic_fetch_add(&e->readers, 1);
    return px(r);
}
int klb_pthread_rwlock_wrlock(void *l) {
    if (!l) return rwl_null("wrlock");
    rwl_entry *e = rwl_entry_for(l);
    void *self = (void *)pthread_self();
    void *held = atomic_load(&e->writer);
    if (held == self)
        rwl_self_check(l, __builtin_return_address(0), atomic_load(&e->wsite));
    rwl_wait_enter(l, __builtin_return_address(0), 1);
    int r = pthread_rwlock_wrlock(rwl_host(e));
    rwl_wait_leave();
    if (!r) {
        atomic_store(&e->writer, self);
        atomic_store(&e->wsite, __builtin_return_address(0));
    }
    return px(r);
}
int klb_pthread_rwlock_unlock(void *l) {
    if (!l) return rwl_null("unlock");
    rwl_entry *e = rwl_entry_for(l);
    void *self = (void *)pthread_self();
    void *expect = self;
    // Clear the ownership BEFORE the host unlock: a reader that wakes the
    // instant the lock drops must not find this entry still naming a writer.
    if (atomic_compare_exchange_strong(&e->writer, &expect, NULL))
        atomic_store(&e->wsite, NULL);
    else if (atomic_load(&e->readers) > 0)
        atomic_fetch_sub(&e->readers, 1);
    return px(pthread_rwlock_unlock(rwl_host(e)));
}
// SDL3 reaches for the try- forms; Beat Saber never did.
int klb_pthread_rwlock_tryrdlock(void *l) {
    if (!l) return rwl_null("tryrdlock");
    rwl_entry *e = rwl_entry_for(l);
    int r = pthread_rwlock_tryrdlock(rwl_host(e));
    if (!r) atomic_fetch_add(&e->readers, 1);
    return px(r);
}
int klb_pthread_rwlock_trywrlock(void *l) {
    if (!l) return rwl_null("trywrlock");
    rwl_entry *e = rwl_entry_for(l);
    int r = pthread_rwlock_trywrlock(rwl_host(e));
    if (!r) {
        atomic_store(&e->writer, (void *)pthread_self());
        atomic_store(&e->wsite, __builtin_return_address(0));
    }
    return px(r);
}

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
// No such call on Darwin. klb_pthread_create maps only the stack and detach
// state out of this attr — the scheduling policy is never honoured either way
// (Darwin gives an unprivileged process nothing worth promising a game
// thread) — so there is nothing for the flag to change. Validate the argument
// the way bionic does (0 = PTHREAD_INHERIT_SCHED, 1 = PTHREAD_EXPLICIT_SCHED)
// and otherwise accept: the usual attr-setter answer.
int klb_pthread_attr_setinheritsched(bionic_attr *a, int inherit) {
    (void)a;
    if (inherit < 0 || inherit > 1) return EINVAL;
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
    // QoS is set on the attr at creation now (guest_thread_qos, below), so the
    // thread starts in its class rather than switching into it here.
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
// The QoS class every guest thread is created with.
//
// **A plain pthread_attr gets QOS_CLASS_UNSPECIFIED**, which on Apple silicon
// is the legacy tier: the scheduler is free to put such a thread on an
// efficiency core, and a thread pool full of them is a load that takes minutes
// where the hardware could do it in less. kl_app.c already makes this argument
// for the guest's own frame thread and gives it USER_INTERACTIVE; every OTHER
// guest thread — UE4's render thread, its RHI thread, its task-graph workers,
// its streaming and audio threads — came through here and got nothing.
//
// USER_INITIATED rather than USER_INTERACTIVE, deliberately. Interactive is the
// frame producer's tier and the compositor's; a task-graph worker that shares
// it can preempt the thread whose deadline actually matters. Initiated is "the
// user is waiting for this", which is exactly what a loading screen is, and it
// still keeps the pool off the E-cores.
//
// This used to be the opt-in KL_GUEST_QOS=1 (a self-set at thread start, for
// Steam Link's UDP receive and decode threads, which a default-QoS schedule
// queued behind the compositor). It is the default now, set on the attr so the
// thread starts in its class. KL_GUEST_QOS names the class: `off` restores the
// unspecified behaviour, `1`/`on` keep meaning USER_INITIATED, and the class
// names select one. Applies to every target built from this runtime, which is
// why the off switch exists.
static qos_class_t guest_thread_qos(void) {
    static int resolved;
    static qos_class_t qos = QOS_CLASS_USER_INITIATED;
    if (resolved) return qos;
    resolved = 1;
    const char *s = kl_env_str("KL_GUEST_QOS", NULL);
    if (s) {
        if      (!strcmp(s, "off") || !strcmp(s, "0")) qos = QOS_CLASS_UNSPECIFIED;
        else if (!strcmp(s, "background"))  qos = QOS_CLASS_BACKGROUND;
        else if (!strcmp(s, "utility"))     qos = QOS_CLASS_UTILITY;
        else if (!strcmp(s, "default"))     qos = QOS_CLASS_DEFAULT;
        else if (!strcmp(s, "initiated") || !strcmp(s, "1") || !strcmp(s, "on"))
                                            qos = QOS_CLASS_USER_INITIATED;
        else if (!strcmp(s, "interactive")) qos = QOS_CLASS_USER_INTERACTIVE;
        else fprintf(stderr, "  [thr] KL_GUEST_QOS=%s is not a class name "
                             "(off/background/utility/default/initiated/"
                             "interactive) — using initiated\n", s);
    }
    fprintf(stderr, "  [thr] guest threads are created at QoS %s\n",
            qos == QOS_CLASS_UNSPECIFIED     ? "unspecified (KL_GUEST_QOS=off)" :
            qos == QOS_CLASS_BACKGROUND      ? "background" :
            qos == QOS_CLASS_UTILITY         ? "utility" :
            qos == QOS_CLASS_DEFAULT         ? "default" :
            qos == QOS_CLASS_USER_INTERACTIVE ? "user-interactive" :
                                                "user-initiated");
    return qos;
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
    qos_class_t qos = guest_thread_qos();
    if (qos != QOS_CLASS_UNSPECIFIED)
        pthread_attr_set_qos_class_np(&da, qos, 0);
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
    //
    // The QoS the thread was actually created at is on the line as well. The
    // one-shot "guest threads are created at QoS ..." banner
    // above says what the DEFAULT is; it cannot say that a particular thread
    // got it, and "the loader is starved" is a claim about a particular thread.
    static int trace = -1;
    if (trace < 0) trace = kl_env_on("KL_TRACE_THREADS", 0);
    if (trace)
        fprintf(stderr, "  [thr] pthread_create entry=%p arg=%p stack=%zu%s"
                        " qos=%s -> %d\n",
                (void *)fn, arg, ga ? ga->stack_size : 0,
                (ga && (ga->flags & BIONIC_ATTR_DETACHED)) ? " detached" : "",
                qos == QOS_CLASS_UNSPECIFIED      ? "unspecified" :
                qos == QOS_CLASS_BACKGROUND       ? "background"  :
                qos == QOS_CLASS_UTILITY          ? "utility"     :
                qos == QOS_CLASS_DEFAULT          ? "default"     :
                qos == QOS_CLASS_USER_INTERACTIVE ? "interactive" :
                                                    "initiated", rc);
    return rc;
}
int   klb_pthread_join(pthread_t t, void **r)  { return px(pthread_join(t, r)); }
int   klb_pthread_detach(pthread_t t)          { return px(pthread_detach(t)); }
void  klb_pthread_exit(void *r)                { thread_unregister(pthread_self()); pthread_exit(r); }
pthread_t klb_pthread_self(void)               { return pthread_self(); }
int   klb_pthread_equal(pthread_t a, pthread_t b) { return pthread_equal(a, b); }
int   klb_pthread_kill(pthread_t t, int sig)   {
    int r = px(pthread_kill(t, kl_sig_to_darwin(sig)));
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
    if (s) din = (sigset_t)kl_sigmask_to_darwin(*s);
    int r = pthread_sigmask(kl_sigmask_how(how), s ? &din : NULL, o ? &dout : NULL);
    if (!r && o) *o = kl_sigmask_to_darwin(dout);   // the swap is its own inverse
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
//
// **This table used to be a one-way bump allocator, and it was a fuse.** Every
// other kind in this file recycles its slots (SYNC_TABLE, above); semaphores
// alone counted up and never came back down, because sem_destroy only zeroed
// the guest's handle. Unity churns roughly a dozen semaphores a second in
// gameplay — job-system fences, async file reads, the audio mixer — so a
// 1024-slot table burned through in about eighty seconds of play, EVERY run,
// and then the cascade (a SUPERHOT VR session log):
//
//     "Failed to open a semaphore (No space left on device)"  x132
//   -> init returned -1 without writing *s, so the guest kept handle 0
//   -> every post/wait on it EINVAL'd silently
//   -> the async asset read never ran, and Unity parsed the buffer anyway
//   -> "The file sharedassets5.assets is corrupted!"          (a lie)
//   -> "GLSL link error: No compiled shaders."                (also a lie)
//   -> SIGTRAP FatalError, ~80 s in, blamed on the level that happened to load
//
// So: slots come back on destroy, the bound is checked before the bump, and a
// failed init writes a poison handle rather than leaving the guest's storage
// untouched — "this semaphore does not exist" must not be spelled the same way
// as "this semaphore was never created".
#define KL_MAX_SEMS       16384         // must fit KL_SEM_SLOT_BITS
#define KL_SEM_SLOT_BITS  14
#define KL_SEM_SLOT_MASK  ((1u << KL_SEM_SLOT_BITS) - 1)
#define KL_SEM_GEN_MASK   (0xFFFFFFFFu >> KL_SEM_SLOT_BITS)
// Slot 0 is never allocated, so a handle naming it can never resolve. That
// makes it the poison value: distinguishable from a pristine 0 handle, and
// EINVAL either way.
#define KL_SEM_POISON     (KL_SEM_GEN_MASK << KL_SEM_SLOT_BITS)

// The count is tracked alongside the dispatch semaphore rather than left to GCD.
// It has to be: GCD gives no way to read the count, and IL2CPP POLLS it, so a
// constant 0 from sem_getvalue is not an answer. The loop at libil2cpp+0x12d57d4
// is a barrier — usleep(3000), sem_getvalue, compare against a target of
// initial+N, repeat — so a value that never changes is an infinite spin with the
// main thread apparently just asleep — a silent zero read as an answer rather
// than as "unknown".
typedef struct {
    dispatch_semaphore_t d;          // NULL when the slot is free
    _Atomic int          count;
    int                  initial;    // what it was created with; see sem_retire
    unsigned             gen;        // bumped by destroy; the handle carries a copy
    unsigned             waiters;    // threads currently inside dispatch_semaphore_wait
    unsigned             retired;    // destroyed while waiters remained
} kl_sem;
static kl_sem   g_sems[KL_MAX_SEMS];
static uint32_t g_sem_free[KL_MAX_SEMS];
static unsigned g_sem_nfree;
static unsigned g_sem_high = 1;                 // bump pointer; slot 0 is reserved
// One lock for the whole table. It is never held across a blocking wait, and
// semaphore create/destroy runs at guest speed (tens per second), so the
// contention is noise next to being able to reason about the teardown at all.
static pthread_mutex_t g_sem_lock = PTHREAD_MUTEX_INITIALIZER;

// Caller holds g_sem_lock. A handle resolves only if its slot is live AND its
// generation still matches — which is what makes use-after-destroy fail at the
// call that is wrong rather than at some innocent later one that inherited the
// slot.
static kl_sem *sem_locked(uint32_t h) {
    unsigned slot = h & KL_SEM_SLOT_MASK;
    if (!slot || slot >= KL_MAX_SEMS) return NULL;
    kl_sem *k = &g_sems[slot];
    if (!k->d || k->gen != (h >> KL_SEM_SLOT_BITS)) return NULL;
    return k;
}

static void sem_complain(uint32_t h) {
    static _Atomic int logged;
    if (atomic_fetch_add(&logged, 1) >= 8) return;
    unsigned slot = h & KL_SEM_SLOT_MASK;
    const char *why =
        h == 0                    ? "it never initialised here" :
        h == KL_SEM_POISON        ? "whose creation failed (table was full)" :
        (slot && slot < KL_MAX_SEMS && g_sems[slot].d)
                                  ? "it already destroyed (stale generation)" :
                                    "that does not exist";
    fprintf(stderr, "  [klepton] sem: handle %#x (slot %u) — the guest is using "
                    "a semaphore %s\n", h, slot, why);
}

// Caller holds g_sem_lock, and k has no waiters left. libdispatch traps on a
// dispatch_semaphore_t released while its value sits below the value it was
// created with ("Semaphore object deallocated while in use"), so pay the count
// back first. With no waits in flight our mirror IS GCD's internal value.
static void sem_retire(kl_sem *k) {
    for (int have = atomic_load(&k->count); have < k->initial; have++)
        dispatch_semaphore_signal(k->d);
    dispatch_release(k->d);
    k->d = NULL;
    k->retired = 0;
    if (g_sem_nfree < KL_MAX_SEMS) g_sem_free[g_sem_nfree++] = (uint32_t)(k - g_sems);
}

int klb_sem_init(int *s, int pshared, unsigned value) {
    (void)pshared;
    pthread_mutex_lock(&g_sem_lock);
    unsigned slot = 0;
    if (g_sem_nfree)                 slot = g_sem_free[--g_sem_nfree];
    // Bound-checked BEFORE the bump. The old code fetch_add'd first and checked
    // after, so a full table kept counting up forever — past the cap, past the
    // point where any later free could have helped.
    else if (g_sem_high < KL_MAX_SEMS) slot = g_sem_high++;
    if (!slot) {
        pthread_mutex_unlock(&g_sem_lock);
        *s = (int)KL_SEM_POISON;
        errno = ENOSPC;
        return -1;
    }
    kl_sem *k = &g_sems[slot];
    // Fill the slot BEFORE publishing the index. The pre-lock version bumped the
    // counter first, which made the range check pass while the slot was still
    // empty — a waiter on another thread then got EINVAL from a semaphore that
    // had, as far as its owner was concerned, been initialised.
    atomic_store(&k->count, (int)value);
    k->initial = (int)value;
    k->waiters = 0;
    k->retired = 0;
    k->d = dispatch_semaphore_create((long)value);
    uint32_t h = (k->gen << KL_SEM_SLOT_BITS) | slot;
    // Occupancy is worth one line in the log: this table failing is not a
    // semaphore bug when it happens, it is a leak somewhere upstream, and the
    // last run gave no warning at all before the wall.
    static int warned;
    unsigned live = (g_sem_high - 1) - g_sem_nfree;
    if (!warned && live > (unsigned)(KL_MAX_SEMS / 4) * 3) {
        warned = 1;
        fprintf(stderr, "  [klepton] sem: %u of %d slots live — the table is 75%% "
                        "full; something is creating semaphores it never destroys\n",
                live, KL_MAX_SEMS);
    }
    pthread_mutex_unlock(&g_sem_lock);
    *s = (int)h;
    return 0;
}

// Resolve for the non-blocking operations, which run entirely under the lock so
// that the dispatch object cannot be released out from under them.
static kl_sem *sem_of(int *s) {
    kl_sem *k = sem_locked((uint32_t)*s);
    if (!k) sem_complain((uint32_t)*s);
    return k;
}

int klb_sem_destroy(int *s) {
    uint32_t h = (uint32_t)*s;
    pthread_mutex_lock(&g_sem_lock);
    kl_sem *k = sem_locked(h);
    if (k) {
        k->gen = (k->gen + 1) & KL_SEM_GEN_MASK;   // the old handle stops resolving here
        if (k->waiters) {
            // Releasing with threads parked inside dispatch_semaphore_wait is
            // the one thing libdispatch will kill the process over. Wake them
            // (as posts, so the count mirror stays faithful) and let the last
            // one out do the teardown.
            k->retired = 1;
            for (unsigned i = 0; i < k->waiters; i++) {
                atomic_fetch_add(&k->count, 1);
                dispatch_semaphore_signal(k->d);
            }
        } else {
            sem_retire(k);
        }
    }
    pthread_mutex_unlock(&g_sem_lock);
    *s = 0;
    return 0;
}

int klb_sem_post(int *s) {
    pthread_mutex_lock(&g_sem_lock);
    kl_sem *k = sem_of(s);
    if (!k) { pthread_mutex_unlock(&g_sem_lock); errno = EINVAL; return -1; }
    if (kl_env_on("KL_TRACE_SIG", 0)) {
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 12)
            fprintf(stderr, "  [sig] sem_post handle %#x (count now %d)\n",
                    (unsigned)*s, atomic_load(&k->count) + 1);
    }
    atomic_fetch_add(&k->count, 1);
    dispatch_semaphore_signal(k->d);
    pthread_mutex_unlock(&g_sem_lock);
    return 0;
}

// The blocking pair: registered as a waiter under the lock, block outside it,
// then settle up. The waiter count is what keeps a concurrent destroy from
// releasing the object while we are standing on it.
static int sem_block(int *s, dispatch_time_t when, int timeout_errno) {
    pthread_mutex_lock(&g_sem_lock);
    kl_sem *k = sem_of(s);
    if (!k) { pthread_mutex_unlock(&g_sem_lock); errno = EINVAL; return -1; }
    k->waiters++;
    pthread_mutex_unlock(&g_sem_lock);

    sem_wait_enter(s, __builtin_return_address(0));
    long timed_out = dispatch_semaphore_wait(k->d, when);
    sem_wait_leave();

    pthread_mutex_lock(&g_sem_lock);
    if (!timed_out) atomic_fetch_sub(&k->count, 1);
    if (--k->waiters == 0 && k->retired) sem_retire(k);
    pthread_mutex_unlock(&g_sem_lock);
    if (timed_out) { errno = timeout_errno; return -1; }
    return 0;
}

int klb_sem_wait(int *s) { return sem_block(s, DISPATCH_TIME_FOREVER, EINVAL); }

int klb_sem_trywait(int *s) {
    pthread_mutex_lock(&g_sem_lock);
    kl_sem *k = sem_of(s);
    if (!k) { pthread_mutex_unlock(&g_sem_lock); errno = EINVAL; return -1; }
    if (dispatch_semaphore_wait(k->d, DISPATCH_TIME_NOW) != 0) {
        pthread_mutex_unlock(&g_sem_lock);
        errno = EAGAIN;
        return -1;
    }
    atomic_fetch_sub(&k->count, 1);
    pthread_mutex_unlock(&g_sem_lock);
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
    return sem_block(s, dispatch_walltime(ts, 0), ETIMEDOUT);
}

// POSIX lets an implementation report 0 rather than a negative count when there
// are waiters, which is what Linux does — so clamp rather than exposing our
// internal debt.
int klb_sem_getvalue(int *s, int *out) {
    pthread_mutex_lock(&g_sem_lock);
    kl_sem *k = sem_of(s);
    if (!k) { pthread_mutex_unlock(&g_sem_lock); errno = EINVAL; return -1; }
    int v = atomic_load(&k->count);
    pthread_mutex_unlock(&g_sem_lock);
    *out = v > 0 ? v : 0;
    return 0;
}

// ---------- barriers: nothing to forward to ----------
//
// Every other kind in this file wraps a Darwin object of the same name. Barriers
// have none: POSIX makes them optional (_POSIX_BARRIERS) and Apple never shipped
// them, so `pthread_barrier_t` does not exist in the host headers and the object
// itself has to be built here out of a mutex and a condition variable.
//
// QuakeQuest is the target that needs them (its renderer parks worker threads on
// one); Beat Saber never calls a barrier, which is why this was absent until now.
//
// **The generation counter is the whole construction.** A barrier that only
// counted arrivals lets a thread that clears the barrier, loops, and arrives
// again be counted into the round that is still draining — the second round then
// trips early and the slow threads from the first are never released. So waiters
// block on the *generation* they arrived in rather than on the count, and the
// releasing thread bumps it exactly once under the lock.
//
// Guest storage is bionic's pthread_barrier_t (32 B, zero-initialised); the
// SYNC_TABLE slot index lives in its first four bytes like every other kind here.
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    unsigned        threshold;   // arrivals needed to release
    unsigned        count;       // arrivals so far this generation
    unsigned        generation;  // bumped once per release
} kl_barrier;

static void kl_barrier_fresh(kl_barrier *b) {
    pthread_mutex_init(&b->m, NULL);
    pthread_cond_init(&b->c, NULL);
    b->threshold = b->count = b->generation = 0;
}

SYNC_TABLE(kl_barrier, g_bar, g_bar_n, kl_barrier_fresh(fresh))

// bionic returns this to exactly one waiter per round; it is -1 there, and
// Darwin has no spelling for it because it has no barriers.
#define KLB_BARRIER_SERIAL_THREAD (-1)

// init takes a FRESH slot and never recycles what the guest storage holds —
// the rule rwlocks needed first (a Deadpool VR freeze: Unreal relocates arrays
// bitwise, and two live rwlocks ended up on one host object). A non-zero word
// in the storage is usually NOT this
// barrier's own earlier slot: it is malloc garbage, or the stale index left in
// a block that was memmove'd elsewhere and freed without destroy, and the moved
// copy still owns that slot. Recycling it at init put a live slot on the free
// list, and the very next get (this one) popped it straight back — two live
// guest barriers on one kl_barrier, whose threshold/count this init then reset
// under the other's waiters. The cost is a leaked slot on a genuine re-init
// without destroy; destroy still recycles.
static _Atomic unsigned g_bar_stale_inits;
unsigned kl_pthread_bar_stale_inits(void) { return atomic_load(&g_bar_stale_inits); }
int klb_pthread_barrier_init(void *b, const void *attr, unsigned count) {
    (void)attr;                                   // process-shared is the only
    if (!b || count == 0) return EINVAL;          // attribute, and we are one process
    uint32_t old = atomic_exchange((_Atomic uint32_t *)b, 0);
    if (old) {
        unsigned n = atomic_fetch_add(&g_bar_stale_inits, 1);
        if (n < 4 || (n & (n - 1)) == 0)
            fprintf(stderr, "  [klb] barrier init over storage already holding slot %u "
                    "(guest %p, from %p): NOT recycled — a fresh slot instead "
                    "(%u such init(s) so far; same rule as rwlock init)\n", old, b,
                    __builtin_return_address(0), n + 1);
    }
    kl_barrier *kb = g_bar_get(b);
    pthread_mutex_lock(&kb->m);
    kb->threshold  = count;
    kb->count      = 0;
    kb->generation = 0;
    pthread_mutex_unlock(&kb->m);
    return 0;
}

int klb_pthread_barrier_wait(void *b) {
    if (!b) return EINVAL;
    kl_barrier *kb = g_bar_get(b);
    pthread_mutex_lock(&kb->m);
    // A barrier the guest waits on without initialising: threshold 0 would make
    // every arrival the last one AND never release anybody, depending on which
    // way the comparison fell. Treat it as a barrier of one — the guest is
    // already wrong, and a silent hang here is far harder to read than a
    // barrier that simply does not block.
    if (kb->threshold == 0) kb->threshold = 1;
    unsigned gen = kb->generation;
    if (++kb->count >= kb->threshold) {
        kb->count = 0;
        kb->generation++;
        pthread_cond_broadcast(&kb->c);
        pthread_mutex_unlock(&kb->m);
        return KLB_BARRIER_SERIAL_THREAD;
    }
    // Loop on the generation, not on a flag: pthread_cond_wait may wake
    // spuriously, and the release is edge-triggered.
    while (gen == kb->generation)
        pthread_cond_wait(&kb->c, &kb->m);
    pthread_mutex_unlock(&kb->m);
    return 0;
}

int klb_pthread_barrier_destroy(void *b) {
    if (!b) return EINVAL;
    g_bar_recycle(b);
    return 0;
}
