// x18 veneer verification with a guest we control.
//
// Beat Saber can only ever tell us that nothing crashed. That is a weak claim
// about a register substitution: get a bit-field wrong and the instruction still
// executes, it just computes something else — and Unity would happily render a
// slightly wrong frame for hours. This test closes that gap by building the same
// source twice, once for ARM64 Linux (where the allocator uses x18 freely, and
// does: `mov x18, sp` then `ldrb w4, [x18, x4]`, a stack array addressed through
// it) and once for the host (where x18 is reserved and never allocated). The two
// must return identical numbers.
//
// Three things are checked, in increasing order of what they would catch:
//
//   1. the guest .so actually contains x18 instructions — otherwise everything
//      below is vacuous and passing means nothing,
//   2. guest results equal host results, single-threaded — catches a decoder
//      that substituted the wrong field,
//   3. the same under concurrency, plus the direct probe from spikes/s05_x18.c
//      run as *guest* code — catches a veneer that is correct but not actually
//      preemption-proof, which is the whole point of the exercise.
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/klepton.h"
#include "../guest/torture.h"

typedef uint64_t (*mix_fn)(uint64_t, int);
typedef kt_pair  (*walk_fn)(uint64_t, int);
typedef uint64_t (*x18_fn)(int, uint64_t);

static mix_fn  g_mix;
static walk_fn g_walk;

#define MAGIC 0x0000BEEF12345678ULL
#define NTHREADS 8
#define PER_THREAD 400

static int fail(const char *msg) { fprintf(stderr, "FAIL: %s\n", msg); return 1; }

// Every thread runs the guest mixer against the host's answer. Concurrency is
// the point: it is what produces the preemption that loses x18 in the first
// place, and 0/500 Beat Saber runs only ever exercised one code path.
static void *worker(void *arg) {
    unsigned long base = (unsigned long)(uintptr_t)arg;
    for (int i = 0; i < PER_THREAD; i++) {
        uint64_t seed = 0x9E3779B97F4A7C15ULL * (base + (unsigned)i) + 12345;
        uint64_t want = kt_mix(seed, 512);
        uint64_t got  = g_mix(seed, 512);
        if (want != got) {
            fprintf(stderr, "  MISMATCH seed=%016" PRIx64 " host=%016" PRIx64
                            " guest=%016" PRIx64 "\n", seed, want, got);
            return (void *)1;
        }
        kt_pair hw = kt_walk(seed, 9), gw = g_walk(seed, 9);
        if (hw.lo != gw.lo || hw.hi != gw.hi) {
            fprintf(stderr, "  MISMATCH walk seed=%016" PRIx64 "\n", seed);
            return (void *)1;
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/guest_torture.so";

    printf("=== loading guest payload: %s ===\n", path);
    kl_image *img = kl_load(path);
    if (!img) return fail(kl_error());
    kl_register_image("guest_torture.so", img);
    kl_run_init(img);

    const kl_stats *st = kl_get_stats(img);
    printf("  x18 sites: %u   veneered: %u   refused: %u\n",
           st->x18_sites, st->x18_patched, st->x18_refused);
    // Without this the rest of the test proves nothing at all.
    if (st->x18_sites == 0)
        return fail("guest payload contains no x18 instructions — the whole "
                    "comparison below would be vacuous. Check the build flags "
                    "in the Makefile (an Android triple reserves x18).");

    g_mix  = (mix_fn)kl_sym(img, "kt_mix");
    g_walk = (walk_fn)kl_sym(img, "kt_walk");
    x18_fn direct = (x18_fn)kl_sym(img, "kt_x18_direct");
    if (!g_mix || !g_walk || !direct) return fail("guest payload is missing exports");

    // ---- 1. single-threaded differential ----
    printf("\n=== host vs guest, single-threaded ===\n");
    for (uint64_t seed = 0; seed < 64; seed++) {
        uint64_t want = kt_mix(seed, 1024), got = g_mix(seed, 1024);
        if (want != got) {
            fprintf(stderr, "  seed %" PRIu64 ": host %016" PRIx64 " != guest %016"
                            PRIx64 "\n", seed, want, got);
            return fail("guest and host disagree — a veneered instruction is "
                        "computing the wrong thing");
        }
    }
    printf("  64 seeds x 1024 rounds: identical\n");
    printf("  kt_mix(1,1024) = %016" PRIx64 "\n", g_mix(1, 1024));

    // ---- 2. the direct probe, as guest code ----
    // On bare Darwin this is what loses 14,952 values in 1.6M iterations
    // (spikes/s05_x18.c). Under veneering both the write and the read go to the
    // per-thread shadow, so the kernel zeroing the architectural register is
    // simply not observable.
    printf("\n=== x18 survives preemption inside guest code ===\n");
    uint64_t lost = direct(200000, MAGIC);
    printf("  lost %" PRIu64 " / 200000\n", lost);
    if (lost) return fail("x18 did not survive preemption in guest code");

    // ---- 3. concurrent differential ----
    printf("\n=== host vs guest, %d threads x %d iterations ===\n", NTHREADS, PER_THREAD);
    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++)
        pthread_create(&th[i], NULL, worker, (void *)(uintptr_t)(i * 100000u + 1));
    int bad = 0;
    for (int i = 0; i < NTHREADS; i++) {
        void *r = NULL;
        pthread_join(th[i], &r);
        bad |= (r != NULL);
    }
    if (bad) return fail("guest and host disagreed under concurrency");
    printf("  %d results, all identical to the host\n", NTHREADS * PER_THREAD * 2);

    printf("\n=== VERIFIED: veneered guest code computes the same answers "
           "as unveneered host code, under preemption ===\n");
    return 0;
}
