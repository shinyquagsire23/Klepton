// S0.5 — is x18 usable by guest code on Darwin arm64?
//
// AAPCS64 calls x18 "the platform register" and leaves its meaning to the OS.
// Android leaves it as a general-purpose temporary unless the build opts into
// ShadowCallStack, so the Quest toolchain allocates values into it freely:
// 14,536 instructions in libunity.so mention x18 and 2,043 of those write it.
// Apple instead reserves it — "The platform reserves register x18. Don't use
// this register." — and clang will warn if you name it in a clobber list.
//
// Reserved is not the same as clobbered, so this measures which one it is, and
// in particular whether a value in x18 survives:
//
//   1. straight-line code with no kernel entry at all
//   2. a syscall
//   3. nothing but the passage of time — i.e. a timer interrupt
//
// (3) is the question that decides how bad this is. If only syscalls clobber
// x18, guest code is merely fragile around calls. If preemption does, then no
// window is safe and guest .text using x18 is unsound on Darwin however it is
// loaded.
//
// Build: cc -O1 -Wno-inline-asm -o s05_x18 s05_x18.c
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define MAGIC 0xDEADBEEFCAFEULL

static uint64_t probe(int sleep_us) {
    uint64_t out;
    asm volatile("mov x18, %0" :: "r"((uint64_t)MAGIC) : "x18");
    if (sleep_us) usleep(sleep_us);
    asm volatile("mov %0, x18" : "=r"(out));
    return out;
}

int main(void) {
    printf("single read, no delay : 0x%llx\n", (unsigned long long)probe(0));
    printf("single read, usleep   : 0x%llx\n", (unsigned long long)probe(1000));

    unsigned long lost = 0, n = 500000;
    for (unsigned long i = 0; i < n; i++) if (probe(0) != MAGIC) lost++;
    printf("straight-line         : lost %lu / %lu\n", lost, n);

    lost = 0; n = 300;
    for (unsigned long i = 0; i < n; i++) if (probe(200) != MAGIC) lost++;
    printf("across a syscall      : lost %lu / %lu\n", lost, n);

    // The decisive case: burn time in userspace, issue no syscalls, and see
    // whether x18 survives anyway. Any loss here is a timer interrupt.
    uint64_t v;
    unsigned long iters = 0, first = 0;
    struct timespec t0, now;
    lost = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        asm volatile("mov x18, %0" :: "r"((uint64_t)MAGIC) : "x18");
        for (volatile int k = 0; k < 2000; k++) { }     // ~microseconds, no syscall
        asm volatile("mov %0, x18" : "=r"(v));
        iters++;
        if (v != MAGIC) { if (!lost) first = iters; lost++; }
        if ((iters & 0x3ff) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - t0.tv_sec >= 3) break;
        }
    }
    printf("preemption only       : lost %lu / %lu (first at %lu)\n", lost, iters, first);
    return 0;
}
