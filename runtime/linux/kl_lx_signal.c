// When the guest faults: say where, in the guest's terms.
//
// A crash inside translated code is reported by the system against a dylib
// offset, which is the ELF's address plus an image shift that differs per
// library — useless next to a disassembly of the .so. So the fatal signals are
// caught here and described the way the guest's own tools would: library and
// ELF offset for pc, lr and each frame, the fault address, and whether it falls
// in an image at all.
//
// This is a reporter, not delivery: the guest's own handlers are not called from
// here yet. It runs on an alternate stack because the commonest fault of all is
// a guest stack that ran out.
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <pthread.h>
#include "kl_lx.h"
#include "../kl_x18.h"
#include "../kl_env.h"

static void where(char *out, size_t cap, uint64_t addr) {
    uint64_t lo, data, hi;
    const char *name;
    for (int i = 0; kl_lx_image_info(i, &lo, &data, &hi, &name); i++)
        if (addr >= lo && addr < hi) {
            const char *b = strrchr(name, '/');
            snprintf(out, cap, "%s+%#llx", b ? b + 1 : name, (unsigned long long)(addr - lo));
            return;
        }
    // Not guest code. Host code is on these stacks too — the guest calls
    // straight into the Vulkan and OpenXR implementations — and a fault there is
    // usually a handle of the guest's that the host side did not expect.
    Dl_info di;
    if (dladdr((void *)(uintptr_t)addr, &di) && di.dli_fname) {
        const char *b = strrchr(di.dli_fname, '/');
        if (di.dli_sname)
            snprintf(out, cap, "[host] %s`%s+%#llx", b ? b + 1 : di.dli_fname, di.dli_sname,
                     (unsigned long long)(addr - (uint64_t)(uintptr_t)di.dli_saddr));
        else
            snprintf(out, cap, "[host] %s+%#llx", b ? b + 1 : di.dli_fname,
                     (unsigned long long)(addr - (uint64_t)(uintptr_t)di.dli_fbase));
        return;
    }
    snprintf(out, cap, "%#llx", (unsigned long long)addr);
}

// KL_LX_FAULT_STRINGS=1: text found on a dying thread's stack. A guest that
// gives up on purpose has usually just formatted the reason into a local buffer
// and failed to show it (its message box is an external program it cannot
// start), and this is where that buffer still is.
void kl_lx_stack_strings(uint64_t sp) {
    if (!kl_env_on("KL_LX_FAULT_STRINGS", 0)) return;
    fprintf(stderr, "    strings on the stack:\n");
    const unsigned char *p = (const unsigned char *)(uintptr_t)sp, *end = p + 0x8000;
    for (const unsigned char *q = p; q < end; ) {
        const unsigned char *e = q;
        while (e < end && *e >= 0x20 && *e < 0x7f) e++;
        if (e - q >= 16) fprintf(stderr, "      %.*s\n", (int)(e - q > 400 ? 400 : e - q), q);
        q = e + 1;
    }
}

// KL_LX_FAULT_PEEK="libvideo.so+0x650120,libfoo.so+0x10": words of the guest's
// own data, printed with a fault. A register dump says what a pointer was; this
// says what the global it was loaded through holds, which is usually the next
// question and otherwise needs a debugger the device does not have.
static void fault_peek(const uint64_t *x) {
    const char *spec = getenv("KL_LX_FAULT_PEEK");
    if (!spec || !*spec) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    char *save = NULL;
    for (char *item = strtok_r(buf, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        // "tcb:x26": where in this thread's glibc descriptor (the 4 KiB below
        // the thread pointer) the value of that register is stored.
        if (!strncmp(item, "tcb:x", 5)) {
            int r = atoi(item + 5);
            uint64_t want = r >= 0 && r < 29 ? x[r] : 0;
            uint64_t tp = (uint64_t)(uintptr_t)pthread_getspecific(KLX_TP_SLOT);
            fprintf(stderr, "    peek tcb: tp %#llx, looking for %#llx\n",
                    (unsigned long long)tp, (unsigned long long)want);
            if (tp > 0x1000 && !(tp & 7))
                for (uint64_t a = tp - 0x1000; a < tp + 0x100; a += 8)
                    if (*(const uint64_t *)(uintptr_t)a == want)
                        fprintf(stderr, "    peek tcb: found at tp%+lld (word before: %#llx)\n",
                                (long long)(a - tp),
                                (unsigned long long)((const uint64_t *)(uintptr_t)a)[-1]);
            continue;
        }
        // "x26": the memory that register points at, 32 words of it.
        if (item[0] == 'x' && item[1] >= '0' && item[1] <= '9' && !strchr(item, '+')) {
            int r = atoi(item + 1);
            uint64_t at = r >= 0 && r < 29 ? x[r] : 0;
            if (at < (1ull << 32) || (at & 7)) continue;
            // Only if it is really there: this runs inside a fault handler.
            vm_address_t ra = (vm_address_t)at;
            vm_size_t rs = 0;
            vm_region_basic_info_data_64_t bi;
            mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t obj = MACH_PORT_NULL;
            if (vm_region_64(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                             (vm_region_info_t)&bi, &cnt, &obj) != KERN_SUCCESS ||
                (uint64_t)ra > at || !(bi.protection & VM_PROT_READ) ||
                at + 256 > (uint64_t)ra + rs)
                continue;
            const uint64_t *w = (const uint64_t *)(uintptr_t)at;
            for (int k = 0; k < 32; k += 4)
                fprintf(stderr, "    peek %s+%#05x: %016llx %016llx %016llx %016llx\n", item,
                        k * 8, (unsigned long long)w[k], (unsigned long long)w[k + 1],
                        (unsigned long long)w[k + 2], (unsigned long long)w[k + 3]);
            continue;
        }
        char *plus = strrchr(item, '+');
        if (!plus) continue;
        *plus = 0;
        uint64_t off = strtoull(plus + 1, NULL, 16), lo, data, hi;
        const char *path;
        for (int i = 0; kl_lx_image_info(i, &lo, &data, &hi, &path); i++) {
            const char *b = strrchr(path, '/');
            if (strcmp(b ? b + 1 : path, item) || lo + off + 32 > hi) continue;
            const uint64_t *w = (const uint64_t *)(uintptr_t)(lo + off);
            fprintf(stderr, "    peek %s+%#llx: %016llx %016llx %016llx %016llx\n", item,
                    (unsigned long long)off, (unsigned long long)w[0],
                    (unsigned long long)w[1], (unsigned long long)w[2],
                    (unsigned long long)w[3]);
        }
    }
}

// The frame-pointer chain, for as long as it stays above sp and aligned.
static void frames(uint64_t fp, uint64_t sp) {
    char a[512];
    fprintf(stderr, "    frames:\n");
    for (int n = 0; n < 32 && fp >= sp && !(fp & 7) && fp - sp < (64ull << 20); n++) {
        uint64_t *f = (uint64_t *)(uintptr_t)fp;
        where(a, sizeof a, f[1]);
        fprintf(stderr, "      %s\n", a);
        if (f[0] <= fp) break;
        fp = f[0];
    }
}

// Where the guest is, from inside one of its syscalls — for a guest that kills
// itself (abort, a failed stack check) rather than faulting.
void kl_lx_backtrace(const kl_lx_frame *f) {
    char a[512];
    where(a, sizeof a, f->guest_x30);
    fprintf(stderr, "    in a syscall made below %s (tid %d)\n", a, kl_lx_gettid());
    frames(f->x[29], (uint64_t)(uintptr_t)f);
}

static void on_fault(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    struct __darwin_arm_thread_state64 *ss = &uc->uc_mcontext->__ss;
    uint64_t pc = __darwin_arm_thread_state64_get_pc(*ss), lr = __darwin_arm_thread_state64_get_lr(*ss);
    uint64_t fp = __darwin_arm_thread_state64_get_fp(*ss), sp = __darwin_arm_thread_state64_get_sp(*ss);
    char a[512], b[512], c[512];
    where(a, sizeof a, pc); where(b, sizeof b, lr); where(c, sizeof c, (uint64_t)(uintptr_t)si->si_addr);
    fflush(NULL);
    fprintf(stderr, "\n  [klepton] linux: the guest took signal %d (%s) on tid %d\n"
                    "    fault address  %s\n    pc             %s\n    lr             %s\n"
                    "    sp             %#llx\n",
            sig, sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL"
               : sig == SIGTRAP ? "SIGTRAP" : "SIGFPE", kl_lx_gettid(), c, a, b,
            (unsigned long long)sp);
    if (kl_lx_wanted_exec(pc))
        fprintf(stderr, "    THE PC IS IN MEMORY THE GUEST ASKED TO BE EXECUTABLE: this is "
                        "JIT-compiled code. It cannot run here;\n    the JIT has to be "
                        "switched off in the guest (lr says who jumped).\n");
    for (int i = 0; i < 31; i += 4)
        fprintf(stderr, "    x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  %s%-2d %016llx\n",
                i, (unsigned long long)ss->__x[i], i + 1, (unsigned long long)ss->__x[i + 1],
                i + 2, (unsigned long long)ss->__x[i + 2], i + 3 < 29 ? "x" : i + 3 == 29 ? "fp" : "lr",
                i + 3 < 29 ? i + 3 : 0,
                (unsigned long long)(i + 3 < 29 ? ss->__x[i + 3] : i + 3 == 29 ? fp : lr));
    frames(fp, sp);
    kl_lx_stack_strings(sp);
    fault_peek((const uint64_t *)ss->__x);
    _exit(128 + sig);
}

void kl_lx_signal_init(void) {
    static uint8_t alt[256 * 1024];
    stack_t st = { .ss_sp = alt, .ss_size = sizeof alt };
    sigaltstack(&st, NULL);
    struct sigaction sa = { .sa_sigaction = on_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK };
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE };
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
}
