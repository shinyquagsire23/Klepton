// Threads, futexes and the bookkeeping half of signals.
//
// A guest thread is a Darwin pthread that happens to run on a stack the guest
// allocated, with a thread pointer the guest chose. clone() is where both are
// handed over: glibc has already built the new thread's stack, TCB and TLS, and
// asks only for something to start executing at the instruction after its `svc`
// with x0 = 0. So the child is given a copy of the parent's register frame and
// resumes exactly there.
//
// Leaving is the delicate half. The kernel's last act for an exiting thread is
// to zero its tid word and wake whoever sleeps on it — and the joiner's first
// act on waking is to unmap the dead thread's stack. So that write cannot be
// made from the guest stack: the thread goes back to its host stack first
// (longjmp into the frame that entered the guest) and only then says it is gone.
#include <errno.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "kl_lx.h"
#include "../klepton.h"
#include "../kl_x18.h"

#define LX_ENOSYS    38
#define LX_ETIMEDOUT 110

#define CLONE_VM             0x00000100
#define CLONE_VFORK          0x00004000
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

typedef struct lx_thread {
    int         tid;
    uint32_t   *clear_tid;      // set_tid_address / CLONE_CHILD_CLEARTID
    uint32_t   *set_tid;        // CLONE_CHILD_SETTID
    uint64_t    sigmask;
    uint64_t    sp, tls;
    jmp_buf     host;           // the host-stack frame that entered the guest
    kl_lx_frame frame;          // the parent's registers at clone
    // A vfork in progress on this thread (see kl_lx_clone): the parent's frame
    // and stack pointer to come back to, and the pid the child was given.
    int         vfork_pid, vfork_status;
    uint64_t    vfork_sp;
    kl_lx_frame vfork_frame;
    void       *x18;            // the parent's shadow x18 at clone: a register like any other
} lx_thread;

static _Thread_local lx_thread *t_self;
static atomic_int g_next_tid;

static int64_t lx_errno(int e) { return -(int64_t)kl_errno_to_linux(e); }

static lx_thread *self(void) {
    if (!t_self) {                                  // the main thread: its tid is the pid
        t_self = calloc(1, sizeof *t_self);
        t_self->tid = getpid();
    }
    return t_self;
}

int kl_lx_gettid(void) { return self()->tid; }

int64_t kl_lx_set_tid_address(uint32_t *p) {
    self()->clear_tid = p;
    return self()->tid;
}

// ---- futex ----
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE 128
#define FUTEX_CLOCK_REALTIME 256

static int64_t futex_wake(uint32_t *uaddr, int n, int flags) {
    int r = n == 1 ? os_sync_wake_by_address_any(uaddr, 4, flags)
                   : os_sync_wake_by_address_all(uaddr, 4, flags);
    // The count is only ever compared against zero. Waking more sleepers than
    // asked is within the contract: every futex wait may return spuriously.
    return r == 0 ? 1 : 0;
}

int64_t kl_lx_futex(uint32_t *uaddr, int op, uint32_t val, const struct timespec *ts,
                    uint32_t *uaddr2, uint32_t val3) {
    int cmd = op & 0x7f;
    int flags = (op & FUTEX_PRIVATE) ? OS_SYNC_WAIT_ON_ADDRESS_NONE : OS_SYNC_WAIT_ON_ADDRESS_SHARED;
    int wflags = (op & FUTEX_PRIVATE) ? OS_SYNC_WAKE_BY_ADDRESS_NONE : OS_SYNC_WAKE_BY_ADDRESS_SHARED;

    switch (cmd) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        // Checked here as well as inside the wait: Linux promises EAGAIN for a
        // word that already moved, and callers tell that apart from a wake.
        if (atomic_load((_Atomic uint32_t *)uaddr) != val) return lx_errno(EAGAIN);
        int r;
        if (!ts) {
            r = os_sync_wait_on_address(uaddr, val, 4, flags);
        } else {
            // FUTEX_WAIT's timeout is relative; WAIT_BITSET's is an absolute
            // time on the monotonic clock, or the realtime one if flagged.
            int64_t ns = (int64_t)ts->tv_sec * 1000000000ll + ts->tv_nsec;
            if (cmd == FUTEX_WAIT_BITSET) {
                struct timespec now;
                clock_gettime((op & FUTEX_CLOCK_REALTIME) ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
                ns -= (int64_t)now.tv_sec * 1000000000ll + now.tv_nsec;
            }
            if (ns <= 0) return -(int64_t)LX_ETIMEDOUT;
            r = os_sync_wait_on_address_with_timeout(uaddr, val, 4, flags,
                                                     OS_CLOCK_MACH_ABSOLUTE_TIME, (uint64_t)ns);
        }
        if (r >= 0) return 0;
        if (errno == ETIMEDOUT) return -(int64_t)LX_ETIMEDOUT;
        return lx_errno(errno == EFAULT ? EFAULT : EINTR);
    }
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
        return futex_wake(uaddr, (int)val, wflags);
    case FUTEX_CMP_REQUEUE:
        if (atomic_load((_Atomic uint32_t *)uaddr) != val3) return lx_errno(EAGAIN);
        // fallthrough
    case FUTEX_REQUEUE:
        // Moving sleepers to another queue is an optimisation of "wake them and
        // let them queue there themselves", so that is what they get.
        (void)uaddr2;
        return futex_wake(uaddr, 2, wflags);
    default:
        return -(int64_t)LX_ENOSYS;                 // PI and robust futexes
    }
}

// ---- clone and exit ----
void kl_lx_thread_resume(const kl_lx_frame *f, uint64_t sp) __attribute__((noreturn));

// The threads that exist, by tid. Only so that signal 0 can be answered: on
// Linux kill/tkill/tgkill with signal 0 deliver nothing and report whether the
// target is there, and code uses it as "is that thread still alive?". Valve's
// video library prunes its per-thread profiler records that way — and when the
// answer for every OTHER thread was "no such call", it freed the records of
// threads that were running and they went on using them (libvideo.so+0x1180b0,
// the first level's Combine screen video).
static pthread_mutex_t g_live_mu = PTHREAD_MUTEX_INITIALIZER;
static int *g_live;
static int  g_nlive, g_live_cap;

static void live_add(int tid) {
    pthread_mutex_lock(&g_live_mu);
    if (g_nlive == g_live_cap) {
        g_live_cap = g_live_cap ? g_live_cap * 2 : 64;
        g_live = realloc(g_live, (size_t)g_live_cap * sizeof *g_live);
    }
    g_live[g_nlive++] = tid;
    pthread_mutex_unlock(&g_live_mu);
}

static void live_remove(int tid) {
    pthread_mutex_lock(&g_live_mu);
    for (int i = 0; i < g_nlive; i++)
        if (g_live[i] == tid) { g_live[i] = g_live[--g_nlive]; break; }
    pthread_mutex_unlock(&g_live_mu);
}

static int live_has(int tid) {
    if (tid == getpid()) return 1;                  // the initial thread
    int found = 0;
    pthread_mutex_lock(&g_live_mu);
    for (int i = 0; i < g_nlive && !found; i++) found = g_live[i] == tid;
    pthread_mutex_unlock(&g_live_mu);
    return found;
}

// tkill / tgkill. Signal 0 is the existence test; a real signal can only be
// raised on the calling thread, which is all the delivery there is here.
int64_t kl_lx_tkill(int tid, int sig) {
    if (!live_has(tid)) return lx_errno(ESRCH);
    if (sig == 0) return 0;
    if (tid == kl_lx_gettid()) return kl_lx_raise(sig);
    static int said;
    if (!said++)
        fprintf(stderr, "  [klepton] linux: signal %d sent to another thread (tid %d) — "
                        "signals are not delivered across threads yet, answering ENOSYS\n",
                sig, tid);
    return -(int64_t)38;                            // ENOSYS, in Linux's numbering
}

static void thread_gone(lx_thread *t) {
    live_remove(t->tid);
    if (t->clear_tid) {
        atomic_store((_Atomic uint32_t *)t->clear_tid, 0);
        os_sync_wake_by_address_all(t->clear_tid, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
        os_sync_wake_by_address_all(t->clear_tid, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    }
}

static void *thread_main(void *arg) {
    lx_thread *t = arg;
    t_self = t;
    pthread_setspecific(KLX_TP_SLOT, (void *)(uintptr_t)t->tls);
    pthread_setspecific(KLX_SVC_SLOT, (void *)kl_lx_svc_entry);
    pthread_setspecific(KLX_TSD_SLOT, t->x18);
    if (t->set_tid) *t->set_tid = (uint32_t)t->tid;
    if (setjmp(t->host) == 0)
        kl_lx_thread_resume(&t->frame, t->sp);
    // Back on the host stack, from kl_lx_thread_exit.
    thread_gone(t);
    free(t);
    return NULL;
}

int64_t kl_lx_clone(kl_lx_frame *f) {
    uint64_t flags = f->x[0], sp = f->x[1], tls = f->x[3];
    uint32_t *ptid = (uint32_t *)f->x[2], *ctid = (uint32_t *)f->x[4];
    // vfork, which is how glibc's posix_spawn, popen and system start a child:
    // CLONE_VM | CLONE_VFORK on a throwaway stack, with the parent suspended
    // until the child execs or exits. There is no second process to give it — a
    // headset app cannot have one — but the contract can still be kept exactly:
    // the "child" runs on this very thread, on the stack it was given, until it
    // reaches execve (refused, and named — what the guest wanted to run is the
    // useful part) and then _exit, at which point the parent's frame is resumed
    // with the child's pid. The parent then learns of the failed exec through
    // glibc's own channel instead of through a clone that never happened.
    //
    // While it runs, the child may not touch what a real child would own a copy
    // of: kl_lx_in_vfork() makes the descriptor and signal calls report success
    // without doing anything, because here they would hit the parent's.
    if ((flags & CLONE_VM) && (flags & CLONE_VFORK) && sp) {
        lx_thread *t = self();
        if (!g_next_tid) atomic_compare_exchange_strong(&g_next_tid, &(int){0}, getpid() + 1);
        t->vfork_pid = atomic_fetch_add(&g_next_tid, 1);
        t->vfork_frame = *f;
        t->vfork_sp = (uint64_t)(uintptr_t)f + sizeof *f;      // the parent's sp at its svc
        kl_lx_frame child = *f;
        child.x[0] = 0;
        // `child` lives on this stack frame, which is the parent's guest stack
        // and stays untouched while the child runs on its own.
        kl_lx_thread_resume(&child, sp);
    }
    // Otherwise only threads. A fork proper (no CLONE_VM) needs a second address
    // space, and there is one.
    if (!(flags & CLONE_VM) || !(flags & CLONE_THREAD) || !sp) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [klepton] linux: the guest tried to fork (clone flags "
                            "%#llx) — refused, there is only this process\n",
                    (unsigned long long)flags);
        return lx_errno(EAGAIN);
    }
    lx_thread *t = calloc(1, sizeof *t);
    if (!g_next_tid) atomic_compare_exchange_strong(&g_next_tid, &(int){0}, getpid() + 1);
    t->tid = atomic_fetch_add(&g_next_tid, 1);
    live_add(t->tid);
    t->sp = sp;
    t->tls = (flags & CLONE_SETTLS) ? tls : (uint64_t)(uintptr_t)pthread_getspecific(KLX_TP_SLOT);
    t->frame = *f;
    t->frame.x[0] = 0;                              // clone's answer in the child
    t->sigmask = self()->sigmask;
    t->x18 = pthread_getspecific(KLX_TSD_SLOT);
    // KL_LX_TCBTRACE=<offset below tp, hex>: what a new thread's descriptor
    // already holds at that offset when the thread is made — the question being
    // whether glibc handed over a descriptor that still carries the last
    // thread's data.
    {
        static long off = -2;
        if (off == -2) { const char *e = getenv("KL_LX_TCBTRACE"); off = e && *e ? strtol(e, NULL, 16) : -1; }
        if (off > 0 && (flags & CLONE_SETTLS) && tls > (uint64_t)off)
            fprintf(stderr, "  [klepton] linux: clone tls %#llx stack %#llx: [tp-%#lx] = %#llx %#llx\n",
                    (unsigned long long)tls, (unsigned long long)sp, off,
                    (unsigned long long)((const uint64_t *)(uintptr_t)(tls - (uint64_t)off))[-1],
                    (unsigned long long)*(const uint64_t *)(uintptr_t)(tls - (uint64_t)off));
    }
    if (flags & CLONE_CHILD_CLEARTID) t->clear_tid = ctid;
    if (flags & CLONE_CHILD_SETTID)   t->set_tid = ctid;
    if (flags & CLONE_PARENT_SETTID)  *ptid = (uint32_t)t->tid;

    // The host stack only carries thread_main and the way out; guest code runs
    // on the stack the guest supplied.
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 512 * 1024);
    pthread_t th;
    int tid = t->tid, e = pthread_create(&th, &at, thread_main, t);
    pthread_attr_destroy(&at);
    if (e) { free(t); return lx_errno(e); }
    return tid;
}

int kl_lx_in_vfork(void) { return t_self && t_self->vfork_pid && t_self->vfork_sp; }

// execve from a vfork child: say what it was, and refuse.
int64_t kl_lx_execve(const char *path, char *const *argv) {
    fprintf(stderr, "  [klepton] linux: the guest tried to run a program — refused, "
                    "there is only this process:\n     %s", path ? path : "(null)");
    for (int i = 1; argv && argv[i] && i < 24; i++) fprintf(stderr, " '%s'", argv[i]);
    fprintf(stderr, "\n");
    return lx_errno(ENOENT);
}

// The child of a vfork is done: become the parent again, where it left off.
static _Noreturn void vfork_return(lx_thread *t, int code) {
    uint64_t sp = t->vfork_sp;
    t->vfork_sp = 0;
    t->vfork_status = (code & 0xff) << 8;
    t->vfork_frame.x[0] = (uint64_t)t->vfork_pid;
    // vfork_sp is the parent's stack pointer at its svc, which is what resume
    // takes: it re-plants the veneer's pair just below and lets the veneer pop.
    kl_lx_thread_resume(&t->vfork_frame, sp);
}

int64_t kl_lx_wait4(int pid, int *status) {
    lx_thread *t = self();
    if (t->vfork_pid && (pid == t->vfork_pid || pid == -1)) {
        if (status) *status = t->vfork_status;
        pid = t->vfork_pid;
        t->vfork_pid = 0;
        return pid;
    }
    return lx_errno(ECHILD);
}

_Noreturn void kl_lx_thread_exit(int code) {
    lx_thread *t = self();
    if (kl_lx_in_vfork()) vfork_return(t, code);
    if (t->tid == getpid()) {
        // The initial thread has no host frame to return to; on Linux it would
        // linger as a zombie while the others run. Wake its joiners and park.
        thread_gone(t);
        (void)code;
        pthread_exit(NULL);
    }
    longjmp(t->host, 1);
}

// ---- signals: the table, not yet the delivery ----
// arm64's kernel sigaction. The restorer is never used on arm64 but the field
// is there, between the flags and the mask.
typedef struct { uint64_t handler, flags, restorer, mask; } lx_sigaction;
static lx_sigaction g_sigact[65];

int64_t kl_lx_rt_sigaction(int sig, const void *act, void *old) {
    if (sig < 1 || sig > 64) return lx_errno(EINVAL);
    if (old) memcpy(old, &g_sigact[sig], sizeof(lx_sigaction));
    if (act) memcpy(&g_sigact[sig], act, sizeof(lx_sigaction));
    return 0;
}

int64_t kl_lx_rt_sigprocmask(int how, const uint64_t *set, uint64_t *old) {
    lx_thread *t = self();
    if (old) *old = t->sigmask;
    if (set) {
        if (how == 0) t->sigmask |= *set;           // SIG_BLOCK
        else if (how == 1) t->sigmask &= ~*set;     // SIG_UNBLOCK
        else t->sigmask = *set;                     // SIG_SETMASK
    }
    return 0;
}

// A signal the guest sends itself — in practice abort()'s SIGABRT. With a
// handler installed it is called, synchronously; without one the default action
// for everything a program raises on purpose is to die, and it does, by name.
int64_t kl_lx_raise(int sig) {
    if (sig < 1 || sig > 64) return lx_errno(EINVAL);
    uint64_t h = g_sigact[sig].handler;
    if (h == 1) return 0;                           // SIG_IGN
    if (h > 1) { ((void (*)(int, void *, void *))(uintptr_t)h)(sig, NULL, NULL); return 0; }
    if (sig == 17 || sig == 28 || sig == 23) return 0;   // CHLD, WINCH, URG: ignored by default
    fflush(NULL);
    fprintf(stderr, "  [klepton] linux: the guest raised signal %d with no handler — "
                    "terminating\n", sig);
    if (kl_lx_cur) kl_lx_backtrace(kl_lx_cur);
    _exit(128 + sig);
}
