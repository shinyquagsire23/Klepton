// S0.5b — can the x18 veneer spill scratch registers below sp?
//
// The veneer needs two scratch GP registers, and the only place to save them
// without already having a scratch register is the stack: arm64 has no absolute
// addressing, so the first spill must be sp-relative. But it cannot *move* sp —
// 317 of libunity's x18 sites have sp-relative operands whose immediates would
// then be wrong, e.g. `ldr w18, [sp, #0x18]`. So the spill has to go below sp,
// into the red zone, leaving sp untouched.
//
// That is only safe if nothing else writes there. Two things could:
//
//   1. Preemption. Safe by construction — the kernel saves thread state in
//      kernel memory, not on the user stack. Measured here anyway.
//   2. Signal delivery. This one is a real question: _sigtramp builds a frame
//      on the user stack, and if it starts at sp rather than sp-128 it lands
//      exactly on our spill.
//
// Note the guest cannot be competing for the same 128 bytes: AAPCS64 as Android
// uses it has no red zone at all, so guest leaf functions never store below sp.
// The only claimant is Darwin itself.
//
// The window has to be a leaf sequence — any call would move sp and write over
// the red zone legitimately — so the signal must arrive asynchronously, which
// is why this spins on a timer rather than calling raise().
//
// Build: cc -O1 -o s06_redzone s06_redzone.c
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define MAGIC 0x0000BEEF12345678ULL

static volatile sig_atomic_t g_sigs = 0;

// A handler that actually uses stack, as a real one would. If Darwin builds the
// signal frame at sp instead of sp-128, this is what lands on the spill.
static void onalrm(int s) {
    (void)s;
    volatile char pad[4096];
    memset((void *)pad, 0xA5, sizeof pad);
    g_sigs++;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onalrm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    struct itimerval it = {{0, 1000}, {0, 1000}};      // 1 kHz
    setitimer(ITIMER_REAL, &it, NULL);

    unsigned long iters = 0, lost = 0, first = 0;
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        uint64_t a, b, c;
        // Probe the near edge, the middle and the far edge of the 128 bytes.
        // No calls anywhere in here: sp must stay put for the window to mean
        // anything, and `stur` is the unscaled form that takes a negative
        // immediate.
        __asm__ volatile(
            "mov  x9, %3            \n"
            "stur x9, [sp, #-8]     \n"
            "stur x9, [sp, #-64]    \n"
            "stur x9, [sp, #-128]   \n"
            "mov  x10, #4000        \n"
            "1:  subs x10, x10, #1  \n"
            "    b.ne 1b            \n"
            "ldur %0, [sp, #-8]     \n"
            "ldur %1, [sp, #-64]    \n"
            "ldur %2, [sp, #-128]   \n"
            : "=&r"(a), "=&r"(b), "=&r"(c)
            : "r"((uint64_t)MAGIC)
            : "x9", "x10", "memory", "cc");

        iters++;
        if (a != MAGIC || b != MAGIC || c != MAGIC) {
            if (!lost) first = iters;
            lost++;
        }
        if ((iters & 0x3ff) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - t0.tv_sec >= 3) break;
        }
    }

    printf("signals delivered  : %d\n", (int)g_sigs);
    printf("red zone corrupted : %lu / %lu%s\n", lost, iters,
           lost ? "" : "  (survives both preemption and signal delivery)");
    if (lost) printf("first loss at      : %lu\n", first);
    return lost != 0;
}
