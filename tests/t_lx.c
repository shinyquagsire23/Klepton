// The Linux ABI's veneers, EXECUTED. `make lx`.
//
// Same reasoning as tests/t_ctr.c: what these veneers replace has no error
// surface of its own. A thread-pointer read left alone returns a register the
// kernel clobbers, and an `svc #0` left alone is a Darwin syscall made with a
// Linux number — both come back with *an* answer. So the emitter's output is
// run, in a real executable mapping, and checked against the slots:
//
//   1. the decoder flags all three shapes under KLX_ABI_LINUX and none of them
//      under KLX_ABI_ANDROID, where the counts `make check` gates on live.
//   2. `mrs Xt, TPIDR_EL0` answers tsd[KLX_TP_SLOT], for every Xt it can.
//   3. `msr TPIDR_EL0, Xt` stores there, and clobbers nothing.
//   4. both directions through x18 use the shadow slot, not the register.
//   5. `svc #0` calls the entry in tsd[KLX_SVC_SLOT] with the guest's
//      registers, returns its x0, and puts x16, x30 and sp back.
//
// No guest and no headset; a few milliseconds.
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../runtime/kl_x18.h"

static int g_fail;
static void ck(int ok, const char *what) {
    if (!ok) g_fail++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// A page pair: code, and the veneer pool right after it, both made executable.
typedef struct { uint8_t *code; uint8_t *pool; size_t cap; } arena;

static uint64_t (*build(arena *a, const uint32_t *body, unsigned n,
                        kl_x18_stats *st))(uint64_t) {
    a->cap = 0x4000;
    a->code = mmap(NULL, a->cap * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (a->code == MAP_FAILED) { perror("mmap"); exit(2); }
    a->pool = a->code + a->cap;
    memcpy(a->code, body, n * 4);
    size_t used = 0;
    if (kl_x18_emit(a->code, n * 4, (uint64_t)(uintptr_t)a->code,
                    a->pool, a->cap, (uint64_t)(uintptr_t)a->pool, st, &used) != 0) {
        fprintf(stderr, "kl_x18_emit failed\n");
        exit(2);
    }
    if (mprotect(a->code, a->cap * 2, PROT_READ | PROT_EXEC) != 0) { perror("mprotect"); exit(2); }
    sys_icache_invalidate(a->code, a->cap * 2);
    return (uint64_t (*)(uint64_t))(void *)a->code;
}
static void arena_free(arena *a) { munmap(a->code, a->cap * 2); }

#define MRS_TP(rt)    (0xD53BD040u | (rt))
#define MSR_TP(rt)    (0xD51BD040u | (rt))
#define SVC0          0xD4000001u
#define SVC1          0xD4000021u
#define RET           0xD65F03C0u
#define MOV(d, s)     (0xAA0003E0u | ((uint32_t)(s) << 16) | (d))
#define MOVZ(rt, imm) (0xD2800000u | ((uint32_t)(imm) << 5) | (rt))
#define ADD(d, n, m)  (0x8B000000u | ((uint32_t)(m) << 16) | ((uint32_t)(n) << 5) | (d))
#define MOV_FROM_SP(d) (0x910003E0u | (d))
#define SUB(d, n, m)  (0xCB000000u | ((uint32_t)(m) << 16) | ((uint32_t)(n) << 5) | (d))

// What a veneered svc reaches. It sees the guest's registers as they were at the
// `svc`, so a syscall-shaped answer is x8 (the number) combined with x0 and x1
// (the first two arguments). It deliberately trashes x16 and x17 on the way: the
// veneer owes the guest its x16 back, and x17 is the real entry's job, not the
// veneer's, so only x16 is checked below.
__attribute__((naked)) static void svc_entry(void) {
    __asm__("add x0, x0, x1\n"
            "add x0, x0, x8, lsl #16\n"
            "mov x16, #0xdead\n"
            "ret\n");
}

int main(void) {
    printf("=== the Linux ABI veneers ===\n");

    // ---- 1. the decoder, in both ABIs ----
    klx_info in;
    int quiet = 1;
    for (unsigned rt = 0; rt < 31; rt++) {
        klx_decode(MRS_TP(rt), &in); quiet &= in.ok && in.sysreg == KLX_SYS_NONE;
        klx_decode(MSR_TP(rt), &in); quiet &= in.ok && in.sysreg == KLX_SYS_NONE;
    }
    klx_decode(SVC0, &in); quiet &= in.ok && in.sysreg == KLX_SYS_NONE;
    ck(quiet, "under the Android ABI none of the three shapes is a site");

    kl_x18_set_abi(KLX_ABI_LINUX);
    int found = 1;
    for (unsigned rt = 0; rt < 31; rt++) {
        klx_decode(MRS_TP(rt), &in);
        found &= in.sysreg == KLX_SYS_TP_READ && (in.nfields == 1) == (rt == 18);
        klx_decode(MSR_TP(rt), &in);
        found &= in.sysreg == KLX_SYS_TP_WRITE && (in.nfields == 1) == (rt == 18);
    }
    klx_decode(SVC0, &in); found &= in.sysreg == KLX_SYS_SVC;
    klx_decode(SVC1, &in); found &= in.sysreg == KLX_SYS_NONE;
    klx_decode(0xD53BD060u, &in); found &= in.sysreg == KLX_SYS_NONE;   // tpidrro_el0
    ck(found, "under the Linux ABI all three are, x18 stays visible, and svc #1 "
              "and TPIDRRO_EL0 are not");

    if (kl_x18_init_linux() != 0) {
        ck(0, "KLX_TP_SLOT and KLX_SVC_SLOT are claimed in this process");
        return 1;
    }
    ck(1, "KLX_TP_SLOT and KLX_SVC_SLOT are claimed in this process");

    const uint64_t TP = 0x0000123456789ab0ull;
    kl_x18_stats st;
    arena a;

    // ---- 2. the read, for every destination that is not x18 ----
    pthread_setspecific(KLX_TP_SLOT, (void *)(uintptr_t)TP);
    int reads = 1;
    for (unsigned rt = 0; rt < 18; rt++) {            // caller-saved destinations
        uint32_t body[] = { MRS_TP(rt), MOV(0, rt), RET };
        uint64_t got = build(&a, body, 3, &st)(0);
        reads &= got == TP && st.tp_sites == 1 && st.tp_patched == 1 && !st.lx_refused;
        arena_free(&a);
    }
    ck(reads, "mrs Xt, TPIDR_EL0 answers tsd[KLX_TP_SLOT] for x0..x17");

    // ---- 3. the write ----
    {
        uint32_t body[] = { MOVZ(9, 0x111), MOVZ(10, 0x222), MOV(11, 0), MSR_TP(11),
                            ADD(0, 9, 10), RET };
        uint64_t got = build(&a, body, 6, &st)(0xfeed0000ull);
        ck(st.tp_patched == 1 && (uint64_t)(uintptr_t)pthread_getspecific(KLX_TP_SLOT) == 0xfeed0000ull,
           "msr TPIDR_EL0, Xt stores into tsd[KLX_TP_SLOT]");
        ck(got == 0x333, "...and leaves the registers around it alone");
        arena_free(&a);
    }

    // ---- 4. through x18: the shadow slot, both ways ----
    {
        pthread_setspecific(KLX_TP_SLOT, (void *)(uintptr_t)TP);
        pthread_setspecific(KLX_TSD_SLOT, NULL);
        uint32_t body[] = { MRS_TP(18), RET };
        build(&a, body, 2, &st)(0);
        ck((uint64_t)(uintptr_t)pthread_getspecific(KLX_TSD_SLOT) == TP,
           "mrs x18, TPIDR_EL0 lands in the x18 shadow slot");
        arena_free(&a);

        pthread_setspecific(KLX_TSD_SLOT, (void *)(uintptr_t)0xabc000ull);
        uint32_t body2[] = { MSR_TP(18), RET };
        build(&a, body2, 2, &st)(0);
        ck((uint64_t)(uintptr_t)pthread_getspecific(KLX_TP_SLOT) == 0xabc000ull,
           "msr TPIDR_EL0, x18 stores what the shadow slot holds");
        arena_free(&a);
        pthread_setspecific(KLX_TSD_SLOT, NULL);
    }

    // ---- 5. svc ----
    {
        pthread_setspecific(KLX_SVC_SLOT, (void *)svc_entry);
        // x0 = arg, x1 = 5, x8 = 93; x16 and sp are recorded before and compared
        // after, and x30 is proven by the fact that `ret` comes back at all.
        uint32_t body[] = {
            MOVZ(1, 5), MOVZ(8, 93), MOVZ(16, 0x77), MOV_FROM_SP(9),
            SVC0,
            MOV_FROM_SP(10), SUB(9, 9, 10),          // x9 = sp before - sp after
            MOVZ(11, 0x77), SUB(11, 11, 16),         // x11 = 0x77 - x16
            ADD(9, 9, 11), ADD(0, 0, 9),             // any drift lands in the result
            RET };
        uint64_t got = build(&a, body, sizeof body / 4, &st)(1000);
        ck(st.svc_sites == 1 && st.svc_patched == 1, "svc #0 is veneered");
        ck(got == 1000 + 5 + (93ull << 16),
           "...calls tsd[KLX_SVC_SLOT] with the guest's x0, x1 and x8, and "
           "returns with x16, x30 and sp restored");
        arena_free(&a);
    }

    printf(g_fail ? "FAIL: %d check(s)\n" : "PASS: the Linux ABI veneers\n", g_fail);
    return g_fail != 0;
}
