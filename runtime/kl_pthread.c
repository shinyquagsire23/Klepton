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
int   klb_pthread_kill(pthread_t t, int sig)   { return pthread_kill(t, sig); }
int   klb_pthread_sigmask(int how, const sigset_t *s, sigset_t *o) {
    return pthread_sigmask(how, s, o);
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
static dispatch_semaphore_t g_sems[KL_MAX_SEMS];
static _Atomic int g_nsems = 1;                 // index 0 means "uninitialised"

int klb_sem_init(int *s, int pshared, unsigned value) {
    (void)pshared;
    int i = atomic_fetch_add(&g_nsems, 1);
    if (i >= KL_MAX_SEMS) { errno = ENOSPC; return -1; }
    // Fill the slot BEFORE publishing the index. The old order bumped g_nsems
    // first, which made sem_of's range check pass while g_sems[i] was still
    // NULL — a waiter on another thread then got EINVAL from a semaphore that
    // had, as far as its owner was concerned, been initialised.
    g_sems[i] = dispatch_semaphore_create(value);
    atomic_thread_fence(memory_order_release);
    *s = i;
    return 0;
}
static dispatch_semaphore_t sem_of(int *s) {
    int i = *s;
    if (i > 0 && i < KL_MAX_SEMS) {
        dispatch_semaphore_t d = g_sems[i];
        if (d) return d;
    }
    static _Atomic int logged;
    if (atomic_fetch_add(&logged, 1) < 8)
        fprintf(stderr, "  [klepton] sem: no semaphore for slot %d (*sem=%d) — "
                        "the guest is waiting on one it never initialised here\n", i, *s);
    return NULL;
}
int klb_sem_destroy(int *s)  { *s = 0; return 0; }
int klb_sem_post(int *s)     { dispatch_semaphore_t d = sem_of(s);
                               if (!d) { errno = EINVAL; return -1; }
                               dispatch_semaphore_signal(d); return 0; }
int klb_sem_wait(int *s)     { dispatch_semaphore_t d = sem_of(s);
                               if (!d) { errno = EINVAL; return -1; }
                               dispatch_semaphore_wait(d, DISPATCH_TIME_FOREVER); return 0; }
int klb_sem_timedwait(int *s, const struct timespec *ts) {
    dispatch_semaphore_t d = sem_of(s);
    if (!d) { errno = EINVAL; return -1; }
    static _Atomic int logged;
    if (ts && atomic_fetch_add(&logged, 1) < 4) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        fprintf(stderr, "  [klepton] sem_timedwait deadline %lld.%09ld, realtime now "
                        "%lld.%09ld (delta %lld s)\n",
                (long long)ts->tv_sec, ts->tv_nsec,
                (long long)now.tv_sec, now.tv_nsec,
                (long long)(ts->tv_sec - now.tv_sec));
    }
    dispatch_time_t when = dispatch_walltime(ts, 0);
    if (dispatch_semaphore_wait(d, when) != 0) { errno = ETIMEDOUT; return -1; }
    return 0;
}
int klb_sem_getvalue(int *s, int *out) {
    (void)s; *out = 0;                 // GCD gives no way to read the count
    return 0;
}
