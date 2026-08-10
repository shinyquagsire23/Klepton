// See kl_fault.h. This was tests/t_boot.c's report_fault() until the visionOS
// port needed the same reporter inside an app bundle (PLANNING §12.7); it is
// the same code, moved rather than copied, because a signal handler that
// exists twice gets fixed once.
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ucontext.h>
#include "klepton.h"
#include "kl_fault.h"
#include "kl_egl.h"
#include "kl_opensl.h"
#include "kl_ovrp.h"
#include "kl_ovrplat.h"
#include "kl_openxr.h"
#include "kl_mediandk.h"
#include "kl_aaudio.h"
#include "kl_il2cpp.h"

static void (*g_extra[KL_FAULT_MAX_REPORTERS])(FILE *);
static unsigned g_extra_n;

void kl_fault_add_reporter(void (*fn)(FILE *)) {
    if (fn && g_extra_n < KL_FAULT_MAX_REPORTERS) g_extra[g_extra_n++] = fn;
}

// This has to survive being called in a broken process, so it uses write(2)
// rather than stdio and does not attempt a symbolised backtrace.
static void report_fault(int sig, siginfo_t *si, void *uctx) {
    // Our own handler, so this is a Darwin ucontext_t and reading it is safe —
    // the mismatch in trap 5 is about the *guest's* handlers seeing one.
    ucontext_t *uc = uctx;
    void *pc = uc ? (void *)uc->uc_mcontext->__ss.__pc : NULL;
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);

    size_t      off = 0;
    const char *img = kl_addr_image(pc, &off);
    // Not a guest image, so it is ours or the system's — dladdr names it, which
    // turns "<host>+0x0" into something actionable. Only safe-ish because we are
    // already dying; it is not async-signal-safe.
    Dl_info di;
    if (!img && pc && dladdr(pc, &di) && di.dli_sname) {
        img = di.dli_sname;
        off = (size_t)((const char *)pc - (const char *)di.dli_saddr);
    }

    char buf[256];
    int  n = snprintf(buf, sizeof buf,
                      "\n[klepton] fault: signal %d at %p, pc %s+0x%zx (%p), thread %llu%s\n",
                      sig, si ? si->si_addr : NULL, img ? img : "<host>", off, pc,
                      (unsigned long long)tid,
                      pthread_main_np() ? " (main)" : " (a guest worker thread)");
    if (n > 0) { ssize_t w = write(2, buf, (size_t)n); (void)w; }

    // Walk the frame chain. The faulting pc is usually in libsystem — memmove
    // with a null pointer says nothing about who passed it — so what matters is
    // the first guest frame above it. AAPCS64 keeps x29 as the frame pointer and
    // stores {fp, lr} at [fp], which both the guest and our own code honour.
    void **fp = uc ? (void **)uc->uc_mcontext->__ss.__fp : NULL;
    for (int depth = 0; fp && depth < 12; depth++) {
        void *ret = fp[1];
        if (!ret) break;
        size_t roff = 0;
        const char *rimg = kl_addr_image(ret, &roff);
        const char *mm = kl_il2cpp_method_at(ret);
        Dl_info rdi;
        if (mm)
            n = snprintf(buf, sizeof buf, "    #%-2d %s  [%s+0x%zx]\n", depth, mm,
                         rimg ? rimg : "?", roff);
        else if (rimg)
            n = snprintf(buf, sizeof buf, "    #%-2d %s+0x%zx\n", depth, rimg, roff);
        else if (dladdr(ret, &rdi) && rdi.dli_sname)
            n = snprintf(buf, sizeof buf, "    #%-2d %s+0x%tx\n", depth, rdi.dli_sname,
                         (const char *)ret - (const char *)rdi.dli_saddr);
        else
            n = snprintf(buf, sizeof buf, "    #%-2d %p\n", depth, ret);
        if (n > 0) { ssize_t w2 = write(2, buf, (size_t)n); (void)w2; }
        void **next = (void **)fp[0];
        if (next <= fp) break;                  // stacks grow down; anything else is junk
        fp = next;
    }

    // SIGABRT here is usually the *guest* dying, not us — Unity's audio thread
    // aborts outright when FMOD cannot open an output device. Those paths never
    // touch kl_fatal_prepare(), so this is the only place the graphics surface
    // report can still be emitted. Not async-signal-safe, but nothing after this
    // point is going to run anyway.
    // SIGALRM means the guest is still alive and blocked, which is a different
    // question from a crash — the surface reports say how far it got and what it
    // was waiting on, so emit them here too.
    // Print on every fatal signal, not just the orderly ones. A SIGSEGV loses
    // the surface reports otherwise, and under KL_PERMISSIVE a crash is the
    // *expected* end of a scouting run — the guest carries on with answers we
    // invented and eventually walks into one.
    if (sig == SIGABRT || sig == SIGALRM || sig == SIGSEGV || sig == SIGBUS ||
        sig == SIGTRAP) {
        for (unsigned i = 0; i < g_extra_n; i++) g_extra[i](stderr);
        kl_pthread_report(stderr);
        kl_egl_report(stderr);
        kl_opensl_report(stderr);
        kl_ovrp_report(stderr);
        kl_ovrplat_report(stderr);
        kl_openxr_report(stderr);
        kl_mediandk_report(stderr);
        kl_aaudio_report(stderr);
    }

    // KL_FAULT_WAIT=1: park here instead of dying so a debugger can attach and
    // inspect every thread at the moment of the fault. This exists because the
    // AGX abort is masked when the guest runs *under* lldb from the start (the
    // debugger's signal traffic serializes the run enough to avoid the race),
    // so the only way to get a symbolicated look at it is post-mortem attach.
    if (kl_env_on("KL_FAULT_WAIT", 0)) {
        n = snprintf(buf, sizeof buf,
                     "[klepton] KL_FAULT_WAIT: pid %d parked on signal %d — "
                     "attach with `lldb -p %d`, then `bt all`\n", getpid(), sig,
                     getpid());
        if (n > 0) { ssize_t w3 = write(2, buf, (size_t)n); (void)w3; }
        for (;;) pause();
    }

    // Die of the original signal, so whoever is watching still reports it as one.
    signal(sig, SIG_DFL);
    raise(sig);
}

void kl_fault_install(void) {
    static int done;
    if (done) return;
    done = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = report_fault;
    sa.sa_flags     = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);   // guest __builtin_trap / brk assertions
}
