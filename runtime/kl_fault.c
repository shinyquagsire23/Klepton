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
#include <mach/mach.h>
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

// Walk an AAPCS64 frame chain and name each return address, guest images
// included. x29 is the frame pointer and {fp, lr} sit at [fp], which both the
// guest and our own code honour.
//
// This is the fault handler's own walk, lifted out so the ORDERLY abort paths
// can use it too. They could not before, and the gap is a real one: an abort by
// name says WHAT the guest asked for and nothing about WHO asked. For an
// unimplemented call that is enough, because the name is the work item — but
// for a call that is malformed rather than missing (a JNI handle that never
// came from us, an out pointer we were handed as NULL) the name is the one
// thing already known and the caller is the entire question.
//
// `fp` is NULL for "start from here", which drops this function's own frame.
void kl_fault_print_frames(FILE *f, void *fp_) {
    void **fp = fp_ ? (void **)fp_ : (void **)__builtin_frame_address(0);
    for (int depth = 0; fp && depth < 16; depth++) {
        void *ret = fp[1];
        if (!ret) break;
        size_t roff = 0;
        const char *rimg = kl_addr_image(ret, &roff);
        const char *mm = kl_il2cpp_method_at(ret);
        Dl_info rdi;
        if (mm)                      fprintf(f, "    #%-2d %s  [%s+0x%zx]\n", depth, mm,
                                             rimg ? rimg : "?", roff);
        else if (rimg)               fprintf(f, "    #%-2d %s+0x%zx\n", depth, rimg, roff);
        else if (dladdr(ret, &rdi) && rdi.dli_sname)
            fprintf(f, "    #%-2d %s+0x%tx\n", depth, rdi.dli_sname,
                    (const char *)ret - (const char *)rdi.dli_saddr);
        else                         fprintf(f, "    #%-2d %p\n", depth, ret);
        void **next = (void **)fp[0];
        if (next <= fp) break;                  // stacks grow down; anything else is junk
        fp = next;
    }
    fflush(f);
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

    // WHAT KIND OF PAGE the address is, for a memory fault.
    //
    // "signal 10 at 0x13fc5c000" and "signal 11 at 0x9c0" are different bugs
    // that the signal number alone barely separates, and the interesting middle
    // case is invisible without this: an address that IS mapped, at a
    // protection that refuses the access. A page-aligned SIGBUS one page past
    // live data is a GUARD page — a buffer walked off its end — and reads
    // identically to an unmapped address from here.
    //
    // mach_vm_region names the enclosing region and its protection, so the line
    // says which of the three it is. It is not async-signal-safe and neither is
    // anything else in this function; we are already dying.
    if ((sig == SIGSEGV || sig == SIGBUS) && si && si->si_addr) {
        // vm_region_64, not mach_vm_region: `mach/mach_vm.h` is an `#error` on
        // the xrOS SDK, and on arm64 vm_address_t is 64 bits anyway, so the
        // portable spelling loses nothing and builds for every platform this
        // reporter has to exist on.
        vm_address_t a = (vm_address_t)(uintptr_t)si->si_addr;
        vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        vm_address_t start = a;
        kern_return_t kr = vm_region_64(mach_task_self(), &start, &sz,
                                        VM_REGION_BASIC_INFO_64,
                                        (vm_region_info_t)&info, &cnt, &obj);
        if (kr != KERN_SUCCESS)
            n = snprintf(buf, sizeof buf,
                         "    the address is in NO mapping at all\n");
        else if (start > a)
            // The first region at or after the address starts past it, so the
            // address itself is in a hole.
            n = snprintf(buf, sizeof buf,
                         "    the address is UNMAPPED — the next region starts "
                         "%llu bytes on\n", (unsigned long long)(start - a));
        else
            n = snprintf(buf, sizeof buf,
                         "    in a %llu-byte region at %p, prot %c%c%c (max %c%c%c)"
                         "%s%s\n",
                         (unsigned long long)sz, (void *)(uintptr_t)start,
                         info.protection & VM_PROT_READ    ? 'r' : '-',
                         info.protection & VM_PROT_WRITE   ? 'w' : '-',
                         info.protection & VM_PROT_EXECUTE ? 'x' : '-',
                         info.max_protection & VM_PROT_READ    ? 'r' : '-',
                         info.max_protection & VM_PROT_WRITE   ? 'w' : '-',
                         info.max_protection & VM_PROT_EXECUTE ? 'x' : '-',
                         info.protection == VM_PROT_NONE
                             ? " — a GUARD page: something walked off the end of "
                               "the mapping before it" : "",
                         info.reserved ? " (reserved)" : "");
        if (n > 0) { ssize_t w = write(2, buf, (size_t)n); (void)w; }
    }

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
        sig == SIGTRAP || sig == SIGILL || sig == SIGFPE) {
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
    // SIGILL was missing here for the whole project, and `kl_fatal_prepare()`
    // has always known about it — its restore list is
    // {ABRT, SEGV, BUS, ILL, FPE, TRAP} — so the asymmetry was visible and
    // nothing had ever raised one. The first Steam Link shell run on a physical
    // Vision Pro did: it died with `App terminated due to signal 4` and a log
    // that simply stopped, which reads as a hang or an OS kill rather than as a
    // crash with a pc. An undefined instruction is exactly the failure a guest
    // executing DATA produces (trap 0b/0d's shape), so it is the last signal
    // that should be silent here.
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
}
