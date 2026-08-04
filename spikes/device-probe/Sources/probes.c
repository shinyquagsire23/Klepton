// Klepton on-device probe battery (visionOS).
// Batches every device-only unknown into one run. See PLANNING.md §5.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <signal.h>
#include <setjmp.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <mach/mach.h>
#include "probes.h"

// private, but the definitive answer on whether code-signing enforcement is relaxed
extern int csops(pid_t, unsigned int, void *, size_t);
#define CS_OPS_STATUS 0
#define CS_VALID 0x0000001
#define CS_GET_TASK_ALLOW 0x0000004
#define CS_HARD 0x0000100
#define CS_KILL 0x0000200
#define CS_ENFORCEMENT 0x0001000
#define CS_DEBUGGED 0x10000000

// ---------- tiny string builder ----------
typedef struct { char *p; size_t len, cap; } SB;
static void sb_init(SB *s){ s->cap=8192; s->len=0; s->p=malloc(s->cap); s->p[0]=0; }
static void sb(SB *s, const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    char tmp[2048]; int n = vsnprintf(tmp, sizeof tmp, fmt, a); va_end(a);
    if (n < 0) return;
    if (s->len + (size_t)n + 2 > s->cap) { s->cap = (s->len + n + 2) * 2; s->p = realloc(s->p, s->cap); }
    memcpy(s->p + s->len, tmp, n); s->len += n; s->p[s->len++] = '\n'; s->p[s->len] = 0;
}

// ---------- system register access ----------
static inline uint64_t rd_tpidrro(void){ uint64_t v; __asm__ volatile("mrs %0, tpidrro_el0":"=r"(v)); return v; }
static inline uint64_t rd_tpidr(void)  { uint64_t v; __asm__ volatile("mrs %0, tpidr_el0"  :"=r"(v)); return v; }
static inline void     wr_tpidr(uint64_t v){ __asm__ volatile("msr tpidr_el0, %0" :: "r"(v)); }
// exactly what a klepton-ld-rewritten guest stack-protector prologue executes
static inline uint64_t guest_canary(void){
    uint64_t v; __asm__ volatile("mrs %0, tpidrro_el0\n\tldr %0, [%0, #40]":"=r"(v)); return v;
}

unsigned long long klepton_tsd_slot(int i){ return ((uint64_t*)rd_tpidrro())[i]; }

// ---------- SIGILL guard, to detect trapped MRS ----------
static sigjmp_buf g_jb; static volatile sig_atomic_t g_trapped;
static void on_sigill(int s){ (void)s; g_trapped = 1; siglongjmp(g_jb, 1); }

#define GUARDED(body, okfmt, ...) do {                                   \
    struct sigaction na, oa; memset(&na,0,sizeof na);                    \
    na.sa_handler = on_sigill; sigemptyset(&na.sa_mask);                 \
    sigaction(SIGILL, &na, &oa); g_trapped = 0;                          \
    if (sigsetjmp(g_jb, 1) == 0) { body; sb(s, okfmt, ##__VA_ARGS__); }  \
    else sb(s, "    !! TRAPPED (SIGILL)");                               \
    sigaction(SIGILL, &oa, NULL);                                        \
} while (0)

// ---------- P5: canary mechanism under preemption ----------
typedef struct { uint64_t want; long mismatches; int rounds; } CTX;
static void *canary_worker(void *arg) {
    CTX *c = arg;
    ((uint64_t*)rd_tpidrro())[5] = c->want;      // klepton_thread_init()
    volatile long sink = 0;
    for (int r = 0; r < c->rounds; r++) {
        for (volatile long i = 0; i < 2000000L; i++) sink += i;  // force preemption
        usleep(400);                                              // force ctx switch
        if (guest_canary() != c->want) c->mismatches++;
    }
    return NULL;
}

// ---------- P6: dlopen an embedded framework ----------
static void probe_dlopen(SB *s, const char *bundle, const char *fw, const char *note) {
    char path[1024];
    snprintf(path, sizeof path, "%s/Frameworks/%s.framework/%s", bundle, fw, fw);
    sb(s, "  %s  (%s)", fw, note);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { sb(s, "    dlopen FAILED: %s", dlerror()); return; }
    sb(s, "    dlopen OK  handle=%p", h);
    int (*fn)(void) = (int(*)(void))dlsym(h, "klepton_probe_value");
    if (!fn) { sb(s, "    dlsym FAILED: %s", dlerror()); dlclose(h); return; }
    int v = fn();
    sb(s, "    dlsym+call OK -> %d %s", v, v == 0x4B4C ? "[CORRECT]" : "[WRONG VALUE]");
    dlclose(h);
}

char *klepton_run_probes(const char *bundle_path) {
    SB S; SB *s = &S; sb_init(s);
    sb(s, "===== KLEPTON DEVICE PROBE =====");

    // ---- P1 environment ----
    sb(s, "\n[P1] environment");
    sb(s, "    getpagesize()   = %d", getpagesize());
    sb(s, "    vm_page_size    = %lu", (unsigned long)vm_page_size);
    sb(s, "    sizeof(void*)   = %zu", sizeof(void*));
    sb(s, "    pthread_mutex_t = %zu bytes  (bionic = 4)", sizeof(pthread_mutex_t));
    sb(s, "    sem_t           = %zu bytes", sizeof(sem_t));

    // ---- P2 system registers readable? ----
    sb(s, "\n[P2] system register access from EL0");
    { uint64_t v = 0; GUARDED(v = rd_tpidrro(), "    mrs tpidrro_el0 = %016llx  [OK]", (unsigned long long)v); }
    { uint64_t v = 0; GUARDED(v = rd_tpidr(),   "    mrs tpidr_el0   = %016llx  [OK]", (unsigned long long)v); }

    // ---- P3 Darwin TSD layout ----
    sb(s, "\n[P3] Darwin TSD via TPIDRRO_EL0  (bionic slot 5 = STACK_GUARD)");
    uint64_t tp = rd_tpidrro();
    sb(s, "    pthread_self = %016llx   TPIDRRO = %016llx  (delta 0x%llx)",
       (unsigned long long)(uintptr_t)pthread_self(), (unsigned long long)tp,
       (unsigned long long)(tp - (uintptr_t)pthread_self()));
    for (int i = 0; i < 9; i++)
        sb(s, "      slot %d (+%2d) = %016llx%s", i, i*8,
           (unsigned long long)((uint64_t*)tp)[i], i == 5 ? "   <== must be FREE" : "");
    sb(s, "    VERDICT: slot 5 %s", ((uint64_t*)tp)[5] == 0 ? "is ZERO [FREE - good]" : "is IN USE [PROBLEM]");

    // ---- P4 TPIDR_EL0 clobber behaviour (does the macOS finding hold?) ----
    sb(s, "\n[P4] TPIDR_EL0 volatility");
    { const uint64_t SENT = 0xDEADBEEF12340000ULL;
      uint64_t o = rd_tpidr(); wr_tpidr(SENT); uint64_t a = rd_tpidr(); wr_tpidr(o);
      sb(s, "    write/readback   : %s", a == SENT ? "survived [WRITABLE]" : "clobbered");
      wr_tpidr(SENT); usleep(2000); uint64_t b = rd_tpidr(); wr_tpidr(o);
      sb(s, "    across usleep    : %s", b == SENT ? "survived" : "CLOBBERED (expected)");
      sb(s, "    -> TPIDR_EL0 %s", b == SENT ? "may be usable (differs from macOS!)"
                                             : "unusable, as on macOS. Rewrite to TPIDRRO stands."); }

    // ---- P5 the actual klepton-ld TLS fix, under preemption ----
    sb(s, "\n[P5] TPIDRRO_EL0 + slot 5 canary under preemption (the klepton-ld fix)");
    { enum { NT = 4 }; pthread_t t[NT]; CTX c[NT]; long bad = 0;
      for (int i = 0; i < NT; i++) { c[i].want = 0xC0FFEE0000ULL + i; c[i].mismatches = 0; c[i].rounds = 12;
                                     pthread_create(&t[i], NULL, canary_worker, &c[i]); }
      for (int i = 0; i < NT; i++) { pthread_join(t[i], NULL); bad += c[i].mismatches; }
      sb(s, "    4 threads x 12 rounds, forced preemption + usleep");
      sb(s, "    VERDICT: %ld mismatches %s", bad, bad == 0 ? "[MECHANISM WORKS]" : "[BROKEN]"); }

    // ---- P6 W^X: confirms the AOT premise ----
    sb(s, "\n[P6] W^X / executable memory (confirms AOT premise)");
    { size_t L = 65536;
      void *rw = mmap(NULL, L, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
      sb(s, "    mmap RW            : %s", rw != MAP_FAILED ? "OK" : strerror(errno));
      if (rw != MAP_FAILED) {
        int r = mprotect(rw, L, PROT_READ|PROT_EXEC);
        sb(s, "    mprotect R+X       : %s", r == 0 ? "ALLOWED (!! unexpected)" : strerror(errno));
        munmap(rw, L);
      }
      void *wx = mmap(NULL, L, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON, -1, 0);
      sb(s, "    mmap RWX           : %s", wx != MAP_FAILED ? "ALLOWED (!! unexpected)" : strerror(errno));
      if (wx != MAP_FAILED) munmap(wx, L);
      void *jit = mmap(NULL, L, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON|MAP_JIT, -1, 0);
      sb(s, "    mmap RWX+MAP_JIT   : %s", jit != MAP_FAILED ? "ALLOWED" : strerror(errno));
      if (jit != MAP_FAILED) munmap(jit, L); }

    // ---- P7 dlopen embedded frameworks (S0.2 core) ----
    sb(s, "\n[P7] dlopen of embedded frameworks  (S0.2)");
    sb(s, "    bundle: %s", bundle_path ? bundle_path : "(null)");
    if (bundle_path) {
        probe_dlopen(s, bundle_path, "KleptonProbeA", "default segment alignment");
        probe_dlopen(s, bundle_path, "KleptonProbeB", "64KB segalign, as translated guest libs");
    }

    // ---- P11 does RWX memory actually EXECUTE? (P6 only tested mapping creation) ----
    sb(s, "\n[P11] can we actually EXECUTE from anonymous memory?");
    { unsigned int st = 0;
      if (csops(getpid(), CS_OPS_STATUS, &st, sizeof st) == 0) {
        sb(s, "    csops status = 0x%08x", st);
        sb(s, "      CS_VALID=%d CS_HARD=%d CS_KILL=%d CS_ENFORCEMENT=%d",
           !!(st&CS_VALID), !!(st&CS_HARD), !!(st&CS_KILL), !!(st&CS_ENFORCEMENT));
        sb(s, "      CS_GET_TASK_ALLOW=%d  CS_DEBUGGED=%d  <== if DEBUGGED=1, results below are NOT representative",
           !!(st&CS_GET_TASK_ALLOW), !!(st&CS_DEBUGGED));
      } else sb(s, "    csops failed: %s", strerror(errno));

      struct kinfo_proc kp; size_t kl = sizeof kp;
      int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
      if (sysctl(mib, 4, &kp, &kl, NULL, 0) == 0)
        sb(s, "    debugger attached (P_TRACED) = %d", !!(kp.kp_proc.p_flag & P_TRACED));
    }
    // A codesigning violation is SIGKILL and cannot be caught -> must fork.
    { static const uint32_t code[2] = { 0x52896980u /* mov w0,#0x4B4C */, 0xd65f03c0u /* ret */ };
      const char *modes[2] = { "mmap RW then mprotect RX", "mmap RWX directly" };
      for (int mode = 0; mode < 2; mode++) {
        pid_t pid = fork();
        if (pid == 0) {
            size_t L = 16384; void *m;
            if (mode == 0) {
                m = mmap(NULL, L, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
                if (m == MAP_FAILED) _exit(10);
                memcpy(m, code, sizeof code);
                if (mprotect(m, L, PROT_READ|PROT_EXEC) != 0) _exit(11);
            } else {
                m = mmap(NULL, L, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON, -1, 0);
                if (m == MAP_FAILED) _exit(10);
                memcpy(m, code, sizeof code);
            }
            __builtin___clear_cache((char*)m, (char*)m + sizeof code);
            int (*fn)(void) = (int(*)(void))m;
            int v = fn();                       // <-- the actual test
            _exit(v == 0x4B4C ? 0 : 12);
        }
        int st2 = 0; waitpid(pid, &st2, 0);
        if (WIFSIGNALED(st2)) {
            int sg = WTERMSIG(st2);
            sb(s, "    %-26s : KILLED by signal %d (%s) -> EXECUTION BLOCKED",
               modes[mode], sg, sg == 9 ? "SIGKILL/codesigning" : strsignal(sg));
        } else {
            int rc = WEXITSTATUS(st2);
            sb(s, "    %-26s : %s", modes[mode],
               rc == 0  ? "EXECUTED SUCCESSFULLY  <<< W^X IS NOT ENFORCED"
             : rc == 10 ? "mmap failed"
             : rc == 11 ? "mprotect failed -> blocked at mprotect"
             : rc == 12 ? "ran but returned wrong value" : "unknown");
        }
      }
    }

    sb(s, "\n===== END =====");
    return S.p;
}
