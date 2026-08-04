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
#include <stdatomic.h>
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
#define SYNC_TABLE(kind, tab, count, init_expr)                                  \
    static kind *tab[KL_MAX_SYNC];                                               \
    static _Atomic uint32_t count = 1;              /* 0 means uninitialised */  \
    static kind *tab##_get(void *g) {                                            \
        _Atomic uint32_t *slot = (_Atomic uint32_t *)(g);                        \
        uint32_t idx = atomic_load(slot);                                        \
        if (idx) return tab[idx];                                                \
        kind *fresh = malloc(sizeof *fresh);                                     \
        init_expr;                                                               \
        uint32_t mine = atomic_fetch_add(&count, 1);                             \
        if (mine >= KL_MAX_SYNC) abort();                                        \
        tab[mine] = fresh;                                                       \
        uint32_t expect = 0;                                                     \
        if (!atomic_compare_exchange_strong(slot, &expect, mine)) {              \
            /* lost the race; leak this slot rather than free a live object */   \
            return tab[expect];                                                  \
        }                                                                        \
        return fresh;                                                            \
    }

SYNC_TABLE(pthread_mutex_t, g_mtx, g_mtx_n, ({
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(fresh, &a); pthread_mutexattr_destroy(&a);
}))
SYNC_TABLE(pthread_cond_t,   g_cnd, g_cnd_n, pthread_cond_init(fresh, NULL))
SYNC_TABLE(pthread_rwlock_t, g_rwl, g_rwl_n, pthread_rwlock_init(fresh, NULL))

static pthread_mutex_t  *mtx(void *g) { return g_mtx_get(g); }
static pthread_cond_t   *cnd(void *g) { return g_cnd_get(g); }
static pthread_rwlock_t *rwl(void *g) { return g_rwl_get(g); }

// _destroy just releases the guest's claim; the object itself is left in the table
// because another thread may still hold a pointer to it.
static void sync_release(void *g) { atomic_store((_Atomic uint32_t *)g, 0); }

// ---------- mutex ----------
int klb_pthread_mutex_init(void *m, const void *a) {
    (void)a; sync_release(m); mtx(m); return 0;
}
int klb_pthread_mutex_lock(void *m)    { return pthread_mutex_lock(mtx(m)); }
int klb_pthread_mutex_unlock(void *m)  { return pthread_mutex_unlock(mtx(m)); }
int klb_pthread_mutex_trylock(void *m) { return pthread_mutex_trylock(mtx(m)); }
int klb_pthread_mutex_destroy(void *p) { sync_release(p); return 0; }
// bionic pthread_mutexattr_t is a plain int holding the type.
int klb_pthread_mutexattr_init(int *a)            { *a = 0; return 0; }
int klb_pthread_mutexattr_destroy(int *a)         { (void)a; return 0; }
int klb_pthread_mutexattr_settype(int *a, int t)  { *a = t; return 0; }

// ---------- condition variable ----------
int klb_pthread_cond_init(void *c, const void *a)  { (void)a; sync_release(c); cnd(c); return 0; }
int klb_pthread_cond_destroy(void *p) { sync_release(p); return 0; }
int klb_pthread_cond_signal(void *c)    { return pthread_cond_signal(cnd(c)); }
int klb_pthread_cond_broadcast(void *c) { return pthread_cond_broadcast(cnd(c)); }
int klb_pthread_cond_wait(void *c, void *m) { return pthread_cond_wait(cnd(c), mtx(m)); }
int klb_pthread_cond_timedwait(void *c, void *m, const struct timespec *ts) {
    return pthread_cond_timedwait(cnd(c), mtx(m), ts);
}
int klb_pthread_condattr_init(long *a)                 { *a = 0; return 0; }
int klb_pthread_condattr_destroy(long *a)              { (void)a; return 0; }
int klb_pthread_condattr_setclock(long *a, int clk)    { (void)a; (void)clk; return 0; }

// ---------- rwlock ----------
int klb_pthread_rwlock_init(void *l, const void *a) { (void)a; sync_release(l); rwl(l); return 0; }
int klb_pthread_rwlock_destroy(void *p) { sync_release(p); return 0; }
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
typedef struct { void *(*fn)(void *); void *arg; } tramp;
static void *thread_tramp(void *p) {
    tramp t = *(tramp *)p; free(p);
    kl_thread_init();
    return t.fn(t.arg);
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
void  klb_pthread_exit(void *r)                { pthread_exit(r); }
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
