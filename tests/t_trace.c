// Does the GL tracing trampoline actually forward its arguments untouched?
//
// runtime/gfx/kl_gl_trace.S sits between the guest and a real GL entry point, logs the
// name, and tail-branches on. If it corrupts one register the failure is not a
// crash — it is a *wrong picture*, or a texture uploaded from the wrong pointer,
// and the trace itself would be the thing lying to us. An instrument that has to
// be trusted has to be tested, so this checks every class the ABI can carry:
//
//   x0-x7   integer and pointer arguments
//   v0-v7   floating-point arguments — glClearColor and every other float entry
//           point lives here, and a trampoline that saved only the integer half
//           would pass a test built from ints alone
//   stack   arguments 9 and beyond. glTexSubImage3D takes 11, three of them on the
//           stack, so this is the case that matters most and the one a naive
//           trampoline breaks: if its frame is still open at the tail branch, the
//           callee reads the saved registers as its own last parameters
//   x30     the return address, so the callee returns to us and not into the
//           trampoline
//
// Deliberately stressing rather than happy-path: the variadic ABI work found
// two bugs that passed a 4-argument test and failed at 9.
#include <stdio.h>
#include <string.h>
#include "../runtime/klepton.h"

typedef struct { const char *name; void *real; } trace_desc;
extern void kl_gl_trace_tramp(void);

static int g_calls;

// The probe. Eight integers fill x0-x7, eight doubles fill v0-v7, and the last
// three integers are forced onto the stack — exactly glTexSubImage3D's shape.
static long probe(long a1, long a2, long a3, long a4,
                  long a5, long a6, long a7, long a8,
                  double d1, double d2, double d3, double d4,
                  double d5, double d6, double d7, double d8,
                  long s9, long s10, long s11) {
    g_calls++;
    long isum = a1 + a2 * 2 + a3 * 3 + a4 * 4 + a5 * 5 + a6 * 6 + a7 * 7 + a8 * 8;
    long dsum = (long)(d1 * 1 + d2 * 2 + d3 * 3 + d4 * 4 +
                       d5 * 5 + d6 * 6 + d7 * 7 + d8 * 8);
    long ssum = s9 * 9 + s10 * 10 + s11 * 11;
    return isum + dsum * 1000 + ssum * 1000000;
}

typedef long (*probe_fn)(long, long, long, long, long, long, long, long,
                         double, double, double, double,
                         double, double, double, double,
                         long, long, long);

#define ARGS 11, 22, 33, 44, 55, 66, 77, 88,                  \
             1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5,          \
             999, 1234, 5678

int main(void) {
    printf("=== GL trace trampoline ABI ===\n");

    long direct = probe(ARGS);
    printf("  direct call                  %ld\n", direct);

    trace_desc desc = { "probe", (void *)probe };
    probe_fn traced = (probe_fn)kl_trace_stub("probe", &desc, (void *)kl_gl_trace_tramp);
    if (!traced) {
        printf("FAIL: kl_trace_stub returned nothing\n");
        return 1;
    }

    int before = g_calls;
    long through = traced(ARGS);
    printf("  through the trampoline       %ld\n", through);

    int ok = 1;
    if (through != direct) {
        printf("FAIL: arguments were corrupted in transit (%ld != %ld)\n", through, direct);
        // Narrow it for whoever reads this: the three sums occupy disjoint decimal
        // ranges, so the digits say which register class was lost.
        long d = through - direct;
        printf("      delta %ld — %s\n", d,
               d % 1000000 == 0 ? "stack arguments" :
               d % 1000 == 0    ? "floating-point arguments" : "integer arguments");
        ok = 0;
    }
    if (g_calls != before + 1) {
        printf("FAIL: the real function ran %d times, expected once\n", g_calls - before);
        ok = 0;
    }

    // Same name, same trampoline: the pool must hand back the same stub rather
    // than burning a cell per lookup. The guest resolves entry points repeatedly.
    void *again = kl_trace_stub("probe", &desc, (void *)kl_gl_trace_tramp);
    if (again != (void *)traced) {
        printf("FAIL: kl_trace_stub did not dedup (%p vs %p)\n", again, (void *)traced);
        ok = 0;
    }

    // And it must survive being called repeatedly — the trampoline touches a
    // counter, and a GL trace makes tens of thousands of these.
    for (int i = 0; i < 1000; i++)
        if (traced(ARGS) != direct) { printf("FAIL: call %d differed\n", i); ok = 0; break; }

    printf(ok ? "=== trampoline forwards x0-x7, v0-v7 and stack arguments intact ===\n"
              : "=== FAILED ===\n");
    return ok ? 0 : 1;
}
