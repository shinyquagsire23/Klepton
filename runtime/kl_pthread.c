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
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <time.h>
#include <dispatch/dispatch.h>
#include "klepton.h"

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

SYNC_TABLE(pthread_cond_t,   g_cnd, g_cnd_n, pthread_cond_init(fresh, NULL))
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
#define MTX_MAP_SIZE 32768          // power of two; abort past it, KL_MAX_SYNC-style
typedef struct {
    _Atomic(uintptr_t)   key;       // guest address; 0 = empty
    pthread_mutex_t     *m;
    _Atomic(void *)      owner;     // owner tracking, dumped by kl_pthread_report
    _Atomic(void *)      locksite;
} mtx_entry;
static mtx_entry g_mtx_map[MTX_MAP_SIZE];

static unsigned mtx_hash(uintptr_t a) {
    return (unsigned)(((a >> 4) * 0x9E3779B97F4A7C15ULL) >> 49);
}

// Find-or-create the host mutex for guest address g. Linear probe.
static mtx_entry *mtx_entry_for(void *g) {
    uintptr_t want = (uintptr_t)g;
    for (unsigned i = mtx_hash(want) & (MTX_MAP_SIZE - 1); ; i = (i + 1) & (MTX_MAP_SIZE - 1)) {
        uintptr_t k = atomic_load(&g_mtx_map[i].key);
        if (k == want) return &g_mtx_map[i];
        if (k) continue;
        // empty: claim it
        uintptr_t expect = 0;
        if (!atomic_compare_exchange_strong(&g_mtx_map[i].key, &expect, want))
            { i--; continue; }          // lost the race; re-read this slot
        pthread_mutex_t *fresh = malloc(sizeof *fresh);
        pthread_mutexattr_t a; pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(fresh, &a); pthread_mutexattr_destroy(&a);
        g_mtx_map[i].m = fresh;
        return &g_mtx_map[i];
    }
}

static pthread_mutex_t *mtx(void *g) { return mtx_entry_for(g)->m; }

static mtx_entry *mtx_find(void *g) {
    uintptr_t want = (uintptr_t)g;
    for (unsigned i = mtx_hash(want) & (MTX_MAP_SIZE - 1); ; i = (i + 1) & (MTX_MAP_SIZE - 1)) {
        uintptr_t k = atomic_load(&g_mtx_map[i].key);
        if (k == want) return &g_mtx_map[i];
        if (!k) return NULL;
    }
}

static pthread_cond_t   *cnd(void *g) { return g_cnd_get(g); }
static pthread_rwlock_t *rwl(void *g) { return g_rwl_get(g); }

// KL_TRACE_MUTEX=1: lifecycle of translated mutexes — init, destroy, and
// lazy creation — so aliasing (two guest addresses, one host object) can be
// attributed: copied storage keeps the same slot; freed-without-destroy
// storage keeps a stale one.
static int t_mtx(void) { static int t = -1; if (t < 0) t = getenv("KL_TRACE_MUTEX") != NULL; return t; }
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

int klb_pthread_mutex_init(void *m, const void *a) {
    (void)a;
    // POSIX: init of an existing mutex is UB. Fresh host object either way;
    // a stale entry's host leaks (it might be held, and freeing is worse).
    mtx_entry *e = mtx_entry_for(m);
    pthread_mutex_t *fresh = malloc(sizeof *fresh);
    pthread_mutexattr_t at; pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(fresh, &at); pthread_mutexattr_destroy(&at);
    e->m = fresh;
    atomic_store(&e->owner, NULL);
    atomic_store(&e->locksite, NULL);
    mtx_log("init", m, (uint32_t)(e - g_mtx_map));
    return 0;
}
int klb_pthread_mutex_lock(void *m) {
    mtx_entry *e = mtx_entry_for(m);
    mtx_wait_enter(m, __builtin_return_address(0));
    int r = pthread_mutex_lock(e->m);
    mtx_wait_leave();
    if (r == 0) {
        atomic_store(&e->locksite, __builtin_return_address(0));
        atomic_store(&e->owner, (void *)pthread_self());
    }
    return r;
}
int klb_pthread_mutex_unlock(void *m)  {
    mtx_entry *e = mtx_entry_for(m);
    atomic_store(&e->owner, NULL);
    atomic_store(&e->locksite, NULL);
    return pthread_mutex_unlock(e->m);
}
int klb_pthread_mutex_trylock(void *m) {
    mtx_entry *e = mtx_entry_for(m);
    int r = pthread_mutex_trylock(e->m);
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

void kl_pthread_report(FILE *out) {
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
        fprintf(out, "  waiter tid %p%s%s%s wants guest %p (from %p)\n", tid,
                nm ? " [" : "", nm ? nm : "", nm ? "]" : "",
                atomic_load(&g_mtx_waiters[i].guest),
                atomic_load(&g_mtx_waiters[i].ra));
    }
}
// bionic pthread_mutexattr_t is a plain int holding the type.
int klb_pthread_mutexattr_init(int *a)            { *a = 0; return 0; }
int klb_pthread_mutexattr_destroy(int *a)         { (void)a; return 0; }
int klb_pthread_mutexattr_settype(int *a, int t)  { *a = t; return 0; }

// ---------- condition variable ----------
int klb_pthread_cond_init(void *c, const void *a)  { (void)a; g_cnd_recycle(c); cnd(c); return 0; }
int klb_pthread_cond_destroy(void *p) { g_cnd_recycle(p); return 0; }
int klb_pthread_cond_signal(void *c)    { return pthread_cond_signal(cnd(c)); }
int klb_pthread_cond_broadcast(void *c) { return pthread_cond_broadcast(cnd(c)); }
int klb_pthread_cond_wait(void *c, void *m) {
    // A cond wait releases the mutex while sleeping; reflect that in the
    // owner table or every sleeper reads as a holder.
    mtx_entry *e = mtx_entry_for(m);
    atomic_store(&e->owner, NULL);
    int r = pthread_cond_wait(cnd(c), e->m);
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
    if (no_rebase < 0) no_rebase = getenv("KL_NO_REBASE") != NULL;
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
        const char *e = getenv("KL_CONDWAIT_CAP_MS");
        cap_ms = e ? atoi(e) : 0;
    }
    if (trace < 0) trace = getenv("KL_TRACE_CONDWAIT") ? 1 : 0;
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
    int r = pthread_cond_timedwait(cnd(c), e->m, ts);
    atomic_store(&e->locksite, __builtin_return_address(0));
    atomic_store(&e->owner, (void *)pthread_self());
    return r;
}
int klb_pthread_condattr_init(long *a)                 { *a = 0; return 0; }
int klb_pthread_condattr_destroy(long *a)              { (void)a; return 0; }
int klb_pthread_condattr_setclock(long *a, int clk)    { (void)a; (void)clk; return 0; }

// ---------- rwlock ----------
int klb_pthread_rwlock_init(void *l, const void *a) { (void)a; g_rwl_recycle(l); rwl(l); return 0; }
int klb_pthread_rwlock_destroy(void *p) { g_rwl_recycle(p); return 0; }
int klb_pthread_rwlock_rdlock(void *l) { return pthread_rwlock_rdlock(rwl(l)); }
int klb_pthread_rwlock_wrlock(void *l) { return pthread_rwlock_wrlock(rwl(l)); }
int klb_pthread_rwlock_unlock(void *l) { return pthread_rwlock_unlock(rwl(l)); }

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
#define KL_MAX_KEYS 512
static pthread_key_t g_keys[KL_MAX_KEYS];
static _Atomic int   g_nkeys = 0;
int klb_pthread_key_create(int *out, void (*dtor)(void *)) {
    int i = atomic_fetch_add(&g_nkeys, 1);
    if (i >= KL_MAX_KEYS || pthread_key_create(&g_keys[i], dtor) != 0) return EAGAIN;
    *out = i; return 0;
}
int klb_pthread_key_delete(int k) {
    if (k < 0 || k >= atomic_load(&g_nkeys)) return EINVAL;
    return pthread_key_delete(g_keys[k]);
}
void *klb_pthread_getspecific(int k) {
    return (k >= 0 && k < atomic_load(&g_nkeys)) ? pthread_getspecific(g_keys[k]) : NULL;
}
int klb_pthread_setspecific(int k, const void *v) {
    return (k >= 0 && k < atomic_load(&g_nkeys)) ? pthread_setspecific(g_keys[k], v) : EINVAL;
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
// Every guest thread needs the bionic stack-guard canary in Darwin TSD slot 5 (S0.1).
//
// Liveness registry for the mutex-owner dump: a holder whose tid is dead is
// the signature of a thread that exited holding a mutex (which reads exactly
// like a deadlock). Registered in the tramp, unregistered on return/exit.
#define KL_MAX_THREADS_LIVE 256
static _Atomic(void *) g_live_threads[KL_MAX_THREADS_LIVE];
static char            g_live_names[KL_MAX_THREADS_LIVE][24];
static void thread_register(void *self) {
    for (int i = 0; i < KL_MAX_THREADS_LIVE; i++) {
        void *expect = NULL;
        if (atomic_compare_exchange_strong(&g_live_threads[i], &expect, self)) {
            pthread_getname_np(pthread_self(), g_live_names[i],
                               sizeof g_live_names[i]);
            return;
        }
    }
}
static void thread_unregister(void *self) {
    for (int i = 0; i < KL_MAX_THREADS_LIVE; i++) {
        void *expect = self;
        if (atomic_compare_exchange_strong(&g_live_threads[i], &expect, NULL))
            return;
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
typedef struct { void *(*fn)(void *); void *arg; } tramp;
static void *thread_tramp(void *p) {
    tramp t = *(tramp *)p; free(p);
    kl_thread_init();
    thread_register(pthread_self());
    void *r = t.fn(t.arg);
    thread_unregister(pthread_self());
    return r;
}
int klb_pthread_create(pthread_t *out, const bionic_attr *ga,
                       void *(*fn)(void *), void *arg) {
    pthread_attr_t da;
    pthread_attr_init(&da);
    if (ga) {
        if (ga->stack_size) pthread_attr_setstacksize(&da, ga->stack_size);
        if (ga->flags & BIONIC_ATTR_DETACHED)
            pthread_attr_setdetachstate(&da, PTHREAD_CREATE_DETACHED);
    }
    tramp *t = malloc(sizeof *t);
    t->fn = fn; t->arg = arg;
    int rc = pthread_create(out, &da, thread_tramp, t);
    pthread_attr_destroy(&da);
    if (rc) free(t);
    return rc;
}
int   klb_pthread_join(pthread_t t, void **r)  { return pthread_join(t, r); }
int   klb_pthread_detach(pthread_t t)          { return pthread_detach(t); }
void  klb_pthread_exit(void *r)                { thread_unregister(pthread_self()); pthread_exit(r); }
pthread_t klb_pthread_self(void)               { return pthread_self(); }
int   klb_pthread_equal(pthread_t a, pthread_t b) { return pthread_equal(a, b); }
int   klb_pthread_kill(pthread_t t, int sig)   {
    int r = pthread_kill(t, sig);
    if (getenv("KL_TRACE_SIG"))
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
#define KL_MAX_SEMS 1024

// The count is tracked alongside the dispatch semaphore rather than left to GCD.
// It has to be: sem_getvalue used to return a constant 0 because "GCD gives no
// way to read the count", and IL2CPP polls it. The loop at libil2cpp+0x12d57d4
// is a barrier — usleep(3000), sem_getvalue, compare against a target of
// initial+N, repeat — so a value that never changes is an infinite spin with the
// main thread apparently just asleep. Trap 6d again: a silent zero read as an
// answer rather than as "unknown".
typedef struct {
    dispatch_semaphore_t d;
    _Atomic int          count;
} kl_sem;
static kl_sem g_sems[KL_MAX_SEMS];
static _Atomic int g_nsems = 1;                 // index 0 means "uninitialised"

int klb_sem_init(int *s, int pshared, unsigned value) {
    (void)pshared;
    int i = atomic_fetch_add(&g_nsems, 1);
    if (i >= KL_MAX_SEMS) { errno = ENOSPC; return -1; }
    // Fill the slot BEFORE publishing the index. The old order bumped g_nsems
    // first, which made the range check pass while the slot was still empty — a
    // waiter on another thread then got EINVAL from a semaphore that had, as far
    // as its owner was concerned, been initialised.
    atomic_store(&g_sems[i].count, (int)value);
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

int klb_sem_destroy(int *s)  { *s = 0; return 0; }

int klb_sem_post(int *s) {
    kl_sem *k = sem_of(s);
    if (!k) { errno = EINVAL; return -1; }
    if (getenv("KL_TRACE_SIG")) {
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
    dispatch_semaphore_wait(k->d, DISPATCH_TIME_FOREVER);
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
