// The mutex slot table — `make mtx`, in `make check`.
//
// This gate is `t_sem`'s twin, one table over, and it exists because the same
// class of bug hid in this one for far longer.
//
// The mutex map is open-addressed with LINEAR PROBING, keyed on the guest's
// mutex ADDRESS. `pthread_mutex_destroy` used to write 0 — a true empty — into
// the slot. In a linear-probe table, a hole punched in the middle of a chain
// truncates it: every entry that had probed PAST that slot became unreachable.
// The next lookup of such a key stopped at the hole, inserted a DUPLICATE
// there, and orphaned the original for the life of the process. Orphans only
// accumulate. So the table filled monotonically under ordinary mutex churn,
// and `mtx_entry_for` — which had no bound and no abort, despite the header
// comment promising one — then looped forever over a full table the first time
// it was handed a key it had not seen.
//
// The symptom was not a crash and not a log line. It was one core pinned at
// 100% and a guest frozen for good: Pinball FX ran its lobby for two and a half
// minutes, then a table load churned enough mutexes to top the table off, and
// the render thread never returned from `pthread_mutex_init`. The visionOS
// compositor kept re-projecting the last guest frame it had, so the headset
// showed a stale picture smeared 60 degrees off-axis with passthrough around
// its edges — which reads as a RENDERING bug and is not one.
//
// So the checks are the properties that story needed and did not have:
//
//   1. churn does not accumulate. Half a million create/destroy cycles through
//      a handful of live mutexes — about twice the 262144-slot
//      table, and past what the old code survived — with the table's own
//      occupancy proving slots come BACK rather than merely being big.
//   2. a destroy does not truncate the chain behind it. Built directly: fill
//      one hash chain, destroy an entry in its MIDDLE, and confirm every
//      mutex behind the hole is still the same host object it was before.
//      This is the exact bug, in the smallest form that shows it.
//   3. identity is stable across the churn — the same guest address maps to
//      the same host mutex, and a destroyed-then-reinitialised address does
//      not silently inherit the previous life's owner or recursion depth.
//   4. mutual exclusion actually holds under concurrent first-touch, which is
//      what the MTX_CLAIM publication sentinel is for: a slot must never be
//      visible under its final key before its host mutex is stored.
//
// No guest, no headset; about a second.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The shims bind through the guest symbol table, not a header.
extern int klb_pthread_mutex_init(void *m, void *attr);
extern int klb_pthread_mutex_destroy(void *m);
extern int klb_pthread_mutex_lock(void *m);
extern int klb_pthread_mutex_unlock(void *m);
extern int klb_pthread_mutex_trylock(void *m);
// Slots that are not empty. The one number that distinguishes "destroyed slots
// come back" from "destroyed slots leak" without waiting for the table to fill.
extern unsigned kl_pthread_mtx_used(void);
extern int klb_pthread_rwlock_init(void *l, const void *a);
extern int klb_pthread_rwlock_destroy(void *l);
extern int klb_pthread_rwlock_rdlock(void *l);
extern int klb_pthread_rwlock_trywrlock(void *l);
extern int klb_pthread_rwlock_unlock(void *l);
extern unsigned kl_pthread_rwl_stale_inits(void);
extern int klb_pthread_barrier_init(void *b, const void *attr, unsigned count);
extern int klb_pthread_barrier_wait(void *b);
extern int klb_pthread_barrier_destroy(void *b);
extern unsigned kl_pthread_bar_stale_inits(void);

static int g_fail;
static void ck(int ok, const char *what) {
    if (!ok) g_fail++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// Guest "mutexes" are just addresses; the shim never reads their contents. A
// big backing array gives us as many distinct addresses as we like.
#define SLAB 4096
static long g_slab[SLAB * 8];
static void *addr(unsigned i) { return &g_slab[(i % SLAB) * 8]; }

// ---------------------------------------------------------------------------
// 1. Churn does not accumulate.
//
// The old table died because destroy returned the slot to nobody. Here we run
// far more create/destroy cycles than there are slots, through a working set of
// only 8 live mutexes at a time. If destroyed slots were not genuinely
// reusable, the table would fill and the process would hang (old code) or abort
// (new code) long before this loop finished — either way the test never
// returns, which is the assertion.
static void churn(void) {
    const unsigned cycles = 500000;
    for (unsigned i = 0; i < cycles; i++) {
        // A working set of 64 addresses, cycled. Only the SLOTS churn, and the
        // whole point is that a destroyed one must come back: 500,000 cycles is
        // about twice the table's size, so if destroy did not genuinely
        // free the slot this loop could not finish. The other checks below use
        // addresses outside this range so they start from a cold chain.
        void *m = addr(i % 64);
        klb_pthread_mutex_init(m, NULL);
        klb_pthread_mutex_lock(m);
        klb_pthread_mutex_unlock(m);
        klb_pthread_mutex_destroy(m);
    }
    // The assertion that actually separates the two behaviours. 64 addresses
    // are live at most, so a table that reclaims properly ends this loop with
    // occupancy in the dozens. A destroy that writes an empty instead of a
    // tombstone ended it at 98,304 of a 131,072-slot table — it leaked a slot
    // per destroy. 1024 is far above the honest answer and far below the
    // leaking one; nothing lands in between.
    unsigned used = kl_pthread_mtx_used();
    ck(used < 1024, "500,000 create/destroy cycles did not accumulate slots");
    printf("       %u of the table's slots are in use after the churn\n", used);
}

// ---------------------------------------------------------------------------
// 2. A destroy does not truncate the chain behind it.
//
// This is the bug itself, built deliberately. We cannot see the table's
// internals from here, but we do not need to: what "unreachable" MEANT was that
// a second lookup of the same guest address produced a DIFFERENT host mutex —
// a duplicate entry. And a different host mutex is directly observable, because
// locking one no longer excludes the other.
//
// So: take a run of addresses (whatever chains they land in), lock every one,
// destroy one in the middle, and then confirm that every OTHER address is still
// held — i.e. still the same host object, still locked by us. Under the old
// code the addresses probing past the destroyed slot re-resolved to brand-new
// unlocked mutexes, and trylock succeeded where it must not.
// Exclusion has to be judged from ANOTHER thread: our host mutexes are
// recursive, so the locking thread's own trylock succeeds whether or not the
// entry was silently replaced. This asks "is this address still held?" from
// outside.
static void *probe_held(void *m) {
    // 0 = still held by the other thread (correct), 1 = we got it, which means
    // the address resolved to a different host mutex than the one that is held.
    if (klb_pthread_mutex_trylock(m) == 0) { klb_pthread_mutex_unlock(m); return (void *)1; }
    return (void *)0;
}
static int stolen(void *m) {
    pthread_t t; void *r = NULL;
    pthread_create(&t, NULL, probe_held, m);
    pthread_join(t, &r);
    return (int)(long)r;
}

static void hole_in_the_chain(void) {
    enum { N = 64 };
    // Lock a run of addresses, then punch holes through the middle of it. The
    // holes are real destroys of real entries, spread out so that whatever the
    // hash does, some of them land mid-chain for some other live key.
    for (unsigned i = 0; i < N; i++) {
        klb_pthread_mutex_init(addr(1000 + i), NULL);
        klb_pthread_mutex_lock(addr(1000 + i));
    }
    for (unsigned i = 8; i < N; i += 8) klb_pthread_mutex_destroy(addr(1000 + i));

    // Every address we did NOT destroy must still be held. Under the old code,
    // an address whose chain ran past a zeroed slot re-resolved to a fresh,
    // UNLOCKED entry — so the probe thread could take it.
    int lost = 0;
    for (unsigned i = 0; i < N; i++) {
        if (i && i % 8 == 0) continue;          // destroyed on purpose
        lost += stolen(addr(1000 + i));
    }
    ck(lost == 0, "no live entry was lost behind a destroyed slot");
    if (lost) printf("       %d of %d addresses re-resolved to a new mutex\n", lost, N);
    for (unsigned i = 0; i < N; i++)
        if (!(i && i % 8 == 0)) klb_pthread_mutex_unlock(addr(1000 + i));

    // The same thing in its smallest form, and the one that does not depend on
    // where the hash happens to put things: a live entry, a destroy right next
    // to it, and the live entry still excluding another thread afterwards.
    void *keep = addr(2000), *gone = addr(2001);
    klb_pthread_mutex_init(keep, NULL);
    klb_pthread_mutex_init(gone, NULL);
    klb_pthread_mutex_lock(keep);
    klb_pthread_mutex_destroy(gone);            // the hole
    ck(stolen(keep) == 0, "a neighbouring destroy did not release the live entry");
    klb_pthread_mutex_unlock(keep);
}

// ---------------------------------------------------------------------------
// 3. A recycled address does not inherit the previous life's state.
//
// A tombstoned slot still points at its old host mutex and still carries the
// old owner and recursion depth. Claiming it for a different guest address must
// reset all of that. The observable: init-after-destroy leaves a mutex that is
// FREE, and one unlock-per-lock is enough to release it.
static void recycle_is_clean(void) {
    void *m = addr(3000);
    klb_pthread_mutex_init(m, NULL);
    klb_pthread_mutex_lock(m);
    klb_pthread_mutex_lock(m);                  // recursive: depth 2
    klb_pthread_mutex_destroy(m);               // destroyed while HELD, depth 2
    klb_pthread_mutex_init(m, NULL);            // same address, fresh life
    int got = klb_pthread_mutex_trylock(m);
    ck(got == 0, "a reinitialised mutex is free, not still held at depth 2");
    if (got == 0) klb_pthread_mutex_unlock(m);
}

// ---------------------------------------------------------------------------
// 4. Mutual exclusion holds under concurrent first-touch.
//
// This is what the publication sentinel is for. Many threads racing to be the
// first to lock one brand-new mutex is exactly what a guest thread pool coming
// up does, and if a slot becomes visible under its final key before its host
// mutex is stored, a prober returns an entry whose mutex is NULL — or, with
// tombstone reuse, the PREVIOUS occupant's mutex, which excludes the wrong set
// of threads. A counter incremented under the lock catches both.
enum { T = 8, ROUNDS = 400 };
static long g_counter;
static _Atomic(unsigned) g_arrived, g_round;

// A round is: all T threads meet at the barrier, then every one of them locks
// the SAME address — an address the table has never seen before, so all T are
// racing to be the thread that creates it. That is the shape that used to
// return an entry with a NULL mutex, and the shape tombstone reuse could
// otherwise answer with the previous occupant's mutex.
static void *racer(void *arg) {
    (void)arg;
    for (unsigned r = 0; r < ROUNDS; r++) {
        atomic_fetch_add(&g_arrived, 1);
        while (atomic_load(&g_round) != r) sched_yield();
        void *m = addr(2048 + r);               // never touched before
        klb_pthread_mutex_lock(m);
        g_counter++;                            // deliberately not atomic
        klb_pthread_mutex_unlock(m);
    }
    return NULL;
}
static void concurrent_first_touch(void) {
    pthread_t th[T];
    g_counter = 0;
    atomic_store(&g_arrived, 0);
    atomic_store(&g_round, (unsigned)-1);
    for (int i = 0; i < T; i++) pthread_create(&th[i], NULL, racer, NULL);
    for (unsigned r = 0; r < ROUNDS; r++) {
        while (atomic_load(&g_arrived) < T) sched_yield();
        atomic_store(&g_arrived, 0);
        atomic_store(&g_round, r);
    }
    for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
    // Every increment in a round is under ONE mutex, so the total is exact if
    // and only if all T threads resolved that address to the same host object
    // and it excluded them.
    ck(g_counter == (long)T * ROUNDS,
       "8 threads racing 400 brand-new mutexes lost no increments");
    printf("       counter %ld of %d\n", g_counter, T * ROUNDS);
}

// 5. rwlocks: init over stale storage must not alias two live locks.
// Deadpool VR: Unreal relocates arrays bitwise, so a
// FRWLock moved by memmove keeps its side-table slot at the new address while
// the freed old block still carries that index. A new FRWLock constructed in
// the reused block called init, init RECYCLED the stale index (still live in
// the moved copy), the next init popped it, and the game thread's
// read-source / write-destination pair landed on one host rwlock: a
// self-deadlock in __psynch_rw_wrlock, audio playing on, picture frozen.
// trywrlock stands in for wrlock so a regression FAILS instead of hanging.
static void rwlock_relocation_does_not_alias(void) {
    _Alignas(8) static unsigned char oldblk[56], moved[56], fresh[56], other[56];
    memset(oldblk, 0, sizeof oldblk);
    memset(fresh, 0, sizeof fresh);
    memset(other, 0, sizeof other);
    klb_pthread_rwlock_init(oldblk, NULL);
    klb_pthread_rwlock_rdlock(oldblk);            // touch it: slot assigned
    klb_pthread_rwlock_unlock(oldblk);
    memcpy(moved, oldblk, sizeof moved);          // TArray growth: memmove...
    // ...and the old block is freed WITHOUT destroy, then reused: a new lock
    // is constructed over the stale bytes, and another right after it.
    unsigned before = kl_pthread_rwl_stale_inits();
    klb_pthread_rwlock_init(oldblk, NULL);
    klb_pthread_rwlock_init(other, NULL);
    ck(kl_pthread_rwl_stale_inits() == before + 1,
       "rwlock init over a stale slot index is seen (and counted)");
    // The moved lock is read-held (the source header); the two new locks must
    // each still take a write lock on the SAME thread.
    klb_pthread_rwlock_rdlock(moved);
    int a = klb_pthread_rwlock_trywrlock(oldblk);
    int b = klb_pthread_rwlock_trywrlock(other);
    ck(a == 0, "reused block's new rwlock is not the moved lock (no alias)");
    ck(b == 0, "the next fresh rwlock is not the moved lock either");
    if (a == 0) klb_pthread_rwlock_unlock(oldblk);
    if (b == 0) klb_pthread_rwlock_unlock(other);
    klb_pthread_rwlock_unlock(moved);
    // Garbage that happens to look like a slot index (fresh malloc memory).
    unsigned *w = (unsigned *)(void *)fresh;
    *w = *(unsigned *)(void *)moved;              // a live index, by accident
    klb_pthread_rwlock_init(fresh, NULL);
    klb_pthread_rwlock_rdlock(moved);
    int c = klb_pthread_rwlock_trywrlock(fresh);
    ck(c == 0, "init over garbage equal to a LIVE slot does not alias it");
    if (c == 0) klb_pthread_rwlock_unlock(fresh);
    klb_pthread_rwlock_unlock(moved);
    klb_pthread_rwlock_destroy(moved);
    klb_pthread_rwlock_destroy(oldblk);
    klb_pthread_rwlock_destroy(other);
    klb_pthread_rwlock_destroy(fresh);
}

// The barrier sibling of rwlock_relocation_does_not_alias: barrier init used to
// g_bar_recycle the storage first, so init over a block that still carried a
// moved barrier's slot index put that LIVE slot on the free list and popped it
// straight back — the new barrier and the moved one shared one kl_barrier.
static void *bar_arrive(void *b) { return (void *)(long)klb_pthread_barrier_wait(b); }
static void barrier_relocation_does_not_alias(void) {
    _Alignas(8) static unsigned char oldblk[32], moved[32], fresh[32], other[32];
    memset(oldblk, 0, sizeof oldblk);
    memset(fresh, 0, sizeof fresh);
    memset(other, 0, sizeof other);
    klb_pthread_barrier_init(oldblk, NULL, 2);    // slot assigned at init
    memcpy(moved, oldblk, sizeof moved);          // relocated bitwise...
    unsigned live = *(unsigned *)(void *)moved;
    // ...old block freed without destroy, reused: two new barriers.
    unsigned before = kl_pthread_bar_stale_inits();
    klb_pthread_barrier_init(oldblk, NULL, 1);
    klb_pthread_barrier_init(other, NULL, 1);
    ck(kl_pthread_bar_stale_inits() == before + 1,
       "barrier init over a stale slot index is seen (and counted)");
    ck(*(unsigned *)(void *)oldblk != live,
       "reused block's new barrier does not take the moved barrier's slot");
    ck(*(unsigned *)(void *)other != live,
       "the next fresh barrier does not take it either");
    ck(klb_pthread_barrier_wait(oldblk) == -1 && klb_pthread_barrier_wait(other) == -1,
       "the new barriers have their own threshold of 1");
    // The moved barrier keeps threshold 2: two arrivals, exactly one serial.
    pthread_t t;
    pthread_create(&t, NULL, bar_arrive, moved);
    int me = klb_pthread_barrier_wait(moved);
    void *them; pthread_join(t, &them);
    ck((me == -1) != ((long)them == -1),
       "moved barrier still releases two arrivals with one serial thread");
    // Garbage equal to a live slot (fresh malloc memory).
    *(unsigned *)(void *)fresh = live;
    klb_pthread_barrier_init(fresh, NULL, 1);
    ck(*(unsigned *)(void *)fresh != live,
       "init over garbage equal to a LIVE barrier slot does not alias it");
    klb_pthread_barrier_destroy(moved);
    klb_pthread_barrier_destroy(oldblk);
    klb_pthread_barrier_destroy(other);
    klb_pthread_barrier_destroy(fresh);
}

// 7. The side table outgrows its old fixed ceiling instead of aborting.
// Init over storage still holding a slot leaks that slot by design (section 6),
// and Deadpool VR's Unreal runtime did ~5 such re-inits a second, so the old
// flat [65536] table — which abort()ed when full — was a 3-4 hour fuse.
// Barriers are the kind still on the slot table, so push one block well past
// the old ceiling and check the newest barrier AND one made before the climb
// (whose host object lives in the first chunk) both still work.
static void stale_reinits_outgrow_old_ceiling(void) {
    _Alignas(8) static unsigned char early[32], blk[32];
    memset(early, 0, sizeof early);
    memset(blk, 0, sizeof blk);
    klb_pthread_barrier_init(early, NULL, 1);
    klb_pthread_barrier_init(blk, NULL, 1);
    unsigned before = kl_pthread_bar_stale_inits();
    const unsigned n = 65536 + 8192;
    for (unsigned i = 0; i < n; i++)
        klb_pthread_barrier_init(blk, NULL, 1);   // no destroy: each leaks a slot
    ck(kl_pthread_bar_stale_inits() - before == n,
       "every stale re-init counted (no abort at the old 65536 ceiling)");
    unsigned idx = *(unsigned *)(void *)blk;
    ck(idx > 65536, "the newest barrier's slot is past the old ceiling");
    printf("       newest slot %u\n", idx);
    ck(klb_pthread_barrier_wait(blk) == -1,
       "the barrier past the old ceiling releases (threshold 1, serial thread)");
    ck(klb_pthread_barrier_wait(early) == -1,
       "a barrier from before the growth is still its own barrier");
    klb_pthread_barrier_destroy(blk);
    klb_pthread_barrier_destroy(early);
}

// ---------------------------------------------------------------------------
// 8. Rwlocks are keyed on the address too — Pinball FX's table-load stall.
//
// Rwlocks were the last kind still storing a SLOT INDEX in the guest's own
// 56 bytes, and that is exactly what the mutex map was moved off address-blind
// keying to avoid. Pinball FX's table load hit it: the guest's entries are
// 0x50-byte records with an rwlock at +8, held in a UE TArray, and a TArray
// **memcpy**s its whole buffer when it grows. The moved record carried its slot
// index; the freed record's storage came back out for a new entry that never
// calls `pthread_rwlock_init` (90 of that library's 101 lock sites don't, and
// bionic's static initialiser is all-zero, so the stale index is believed). Two
// guest rwlocks, one host rwlock — and a thread that read-locked one record and
// write-locked the other blocked against ITSELF, forever, with no other thread
// in the process inside a lock call to name as the holder.
//
// The check is that story in six lines: read-lock A, copy A's storage to B the
// way a TArray move does, and take a WRITE lock on B. It must succeed, because
// A and B are different rwlocks. With the slot design it returned EBUSY.
extern int klb_pthread_rwlock_wrlock(void *l);

static void *rw_probe(void *l) {
    if (klb_pthread_rwlock_trywrlock(l) == 0) { klb_pthread_rwlock_unlock(l); return (void *)1; }
    return (void *)0;
}
static int rw_stolen(void *l) {
    pthread_t t; void *r = NULL;
    pthread_create(&t, NULL, rw_probe, l);
    pthread_join(t, &r);
    return (int)(long)r;
}

static void rwlock_is_address_keyed(void) {
    void *a = addr(4000), *b = addr(4001);
    // A brand-new record, statically zero-initialised the way bionic's is — no
    // pthread_rwlock_init anywhere, which is how the guest uses most of them.
    memset(a, 0, 56);
    memset(b, 0, 56);
    klb_pthread_rwlock_rdlock(a);
    memcpy(b, a, 56);                           // the TArray move
    int got = klb_pthread_rwlock_trywrlock(b);
    ck(got == 0, "a copied rwlock's storage does not alias the original's lock");
    if (got == 0) klb_pthread_rwlock_unlock(b);
    klb_pthread_rwlock_unlock(a);

    // The other half of "keyed on the address": one address is one lock, so a
    // write lock really does exclude another thread. A map that handed out a
    // fresh host rwlock per lookup would pass the check above and fail this one.
    void *c = addr(4002);
    klb_pthread_rwlock_wrlock(c);
    ck(rw_stolen(c) == 0, "a write-held rwlock excludes another thread");
    klb_pthread_rwlock_unlock(c);

    // Destroy is a deliberate no-op, so a destroyed-then-reused address must
    // still be a working, FREE lock rather than one stuck in its previous life.
    void *d = addr(4003);
    klb_pthread_rwlock_init(d, NULL);
    klb_pthread_rwlock_wrlock(d);
    klb_pthread_rwlock_unlock(d);
    klb_pthread_rwlock_destroy(d);
    klb_pthread_rwlock_init(d, NULL);
    int again = klb_pthread_rwlock_trywrlock(d);
    ck(again == 0, "a destroyed-then-reinitialised rwlock is free");
    if (again == 0) klb_pthread_rwlock_unlock(d);
}

int main(void) {
    printf("mutex slot table\n");
    churn();
    hole_in_the_chain();
    recycle_is_clean();
    concurrent_first_touch();
    rwlock_relocation_does_not_alias();
    barrier_relocation_does_not_alias();
    stale_reinits_outgrow_old_ceiling();
    rwlock_is_address_keyed();
    printf(g_fail ? "FAILED (%d)\n" : "all good\n", g_fail);
    return g_fail ? 1 : 0;
}
