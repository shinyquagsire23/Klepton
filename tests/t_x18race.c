// The `br x18` veneer's race windows, MANUFACTURED and executed — `make x18race`,
// in `make check`.
//
// The kernel zeroes x18 on every exception return, so an interrupt that lands
// inside a veneer which holds a value in x18 loses it. Where that happens at
// random, in a guest thread, on a headset, a fix for it cannot be verified
// where it matters. So the race is manufactured here instead: entering the
// veneer's tail with x18 already zero produces a state indistinguishable from
// the real thing, and the code under test either survives it or this process
// is gone.
//
// No guest, no headset; milliseconds.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include "../runtime/klepton.h"     // kl_thread_init — seeds the TSD slot
#include "../runtime/kl_x18.h"

static int g_fail;
static void ck(int ok, const char *what) {
    if (!ok) g_fail++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// The `ldr`/`br` window and the `cbz` guard in front of it.
//
// An interrupt landing on the veneer's `br x18` branches to address 0, which on
// visionOS is an unblockable SIGKILL: no signal is delivered and no handler can
// see it. So the veneer carries a `cbz` between the load and the branch, and
// this probe enters where the kernel leaves execution — at the `cbz`, with
// x18 already zeroed — and must come back with the slot's value rather than
// branching to 0.
//
//   b   9f                    <- jump to the manufactured entry
// 0:mrs x18, tpidrro_el0
//   ldr x18, [x18, #4000]
// 2:cbz x18, 0b               <- the guard
//   mov x0, x18
//   ret
// 9:mov x18, xzr              <- stands in for the exception return
//   b   2b
//
// Written as assembly rather than hand-encoded words because the point is that
// the shape matches what kl_x18.c emits. 4000 is KLX_TSD_SLOT * 8 and is
// checked against it below. Without the guard the instruction after the `cbz`
// slot is `br x18` with x18 zero, and this process is gone with nothing
// written down.
__attribute__((naked, noinline)) static uint64_t x18_br_guard_probe(void) {
    __asm__ volatile(
        "b   9f\n"
        "0:\n"
        "mrs x18, tpidrro_el0\n"
        "ldr x18, [x18, #4000]\n"
        "2:\n"
        "cbz x18, 0b\n"
        "mov x0, x18\n"
        "ret\n"
        "9:\n"
        "mov x18, xzr\n"
        "b   2b\n");
}

// ---------------- the jump-table rewrite, built and then EXECUTED ------------
//
// The rewrite stops the `br x18` dispatch from branching through x18 at all: the
// table's own 32-bit offsets are repointed at thunks, the dispatch goes through
// x16, and each thunk restores x16/x17 before a direct `b` to the real target.
// Checking the emitter's counters would not prove any of that — the only
// convincing test is to assemble the idiom the real guests use, veneer it, and
// RUN it, once per arm, with x16 and x17 carrying values that must survive.
//
// The idiom is libfmod+0x85628's, including the detail that rules out the cheap
// fix: `mov x14, x12` sits between the `add x18, x14, x11` and the `br`, so the
// dispatch's own temporary is live at the targets and there is no dead register
// anywhere in the block to borrow.
#define JT_N 4
static uint64_t jt_table_va;
static void *jt_data_at(void *ctx, uint64_t va, size_t len) {
    (void)ctx;
    if (va != jt_table_va || len != JT_N * 4) return NULL;
    return (void *)(uintptr_t)va;
}
static uint32_t mov_reg(unsigned d, unsigned s) { return 0xAA0003E0u | (s << 16) | d; }

static void jt_build_and_run(void) {
    // One mapping, so the pool is trivially inside `b`'s reach of the code and
    // the table can be handed out as an ordinary address.
    const size_t MAP = 1u << 20;
    uint8_t *m = mmap(NULL, MAP, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { ck(0, "jump-table probe: mmap"); return; }
    uint32_t *code = (uint32_t *)m;
    uint32_t *tbl  = (uint32_t *)(m + 0x8000);
    uint8_t  *pool = m + 0x10000;
    jt_table_va = (uint64_t)(uintptr_t)tbl;

    // i0  mov  x10, x0                 the switch selector
    // i1  adrp x11, <table page>
    // i2  add  x11, x11, #<lo12>
    // i3  cmp  w10, #JT_N-1
    // i4  b.hi -> i17, the out-of-range arm
    // i5  ldrsw x14, [x11, x10, lsl #2]
    // i6  add  x18, x14, x11
    // i7  mov  x14, x12                <- the temporary is REUSED here
    // i8  br   x18
    // i9..i16  four targets, each `movz x0, #0xA0+k / ret`
    // i17..i18 out of range: movz x0, #0xFF / ret
    uint64_t cva = (uint64_t)(uintptr_t)code;
    int64_t  pd  = ((int64_t)(jt_table_va & ~0xFFFULL)
                  - (int64_t)((cva + 4) & ~0xFFFULL)) >> 12;
    code[0] = mov_reg(10, 0);
    code[1] = 0x90000000u | ((uint32_t)(pd & 3) << 29) |
              ((uint32_t)((pd >> 2) & 0x7FFFF) << 5) | 11u;
    code[2] = 0x91000000u | ((uint32_t)(jt_table_va & 0xFFF) << 10) | (11u << 5) | 11u;
    code[3] = 0x7100001Fu | ((uint32_t)(JT_N - 1) << 10) | (10u << 5);
    code[4] = 0x54000008u | ((uint32_t)(17 - 4) << 5);
    code[5] = 0xB8A07800u | (10u << 16) | (11u << 5) | 14u;
    code[6] = 0x8B000000u | (11u << 16) | (14u << 5) | 18u;
    code[7] = mov_reg(14, 12);
    code[8] = 0xD61F0240u;                                    // br x18
    for (unsigned k = 0; k < JT_N; k++) {
        code[9 + 2 * k]     = 0xD2800000u | ((0xA0u + k) << 5);
        code[9 + 2 * k + 1] = 0xD65F03C0u;                    // ret
        tbl[k] = (uint32_t)(int32_t)((int64_t)(cva + (9 + 2 * k) * 4)
                                   - (int64_t)jt_table_va);
    }
    code[17] = 0xD2800000u | (0xFFu << 5);
    code[18] = 0xD65F03C0u;

    kl_x18_image img = { jt_data_at, NULL };
    kl_x18_stats st;
    size_t used = 0;
    int rc = kl_x18_emit_img(code, 19 * 4, cva, pool, MAP - 0x10000,
                             (uint64_t)(uintptr_t)pool, &img, &st, &used);
    ck(rc == 0, "jump-table probe: emitted");
    ck(st.jt_sites == 1, "the `br x18` dispatch was recognised and converted");
    ck(st.jt_thunks == JT_N, "...with one thunk per table target");
    ck(st.jt_fallback == 0, "...and nothing left on the guarded veneer");
    ck(code[8] != 0xD61F0240u, "the `br x18` itself is gone from the guest text");

    if (mprotect(m, MAP, PROT_READ | PROT_EXEC) != 0) {
        ck(0, "jump-table probe: mprotect"); munmap(m, MAP); return;
    }
    sys_icache_invalidate(m, MAP);

    // Run every arm with x16/x17 poisoned. They are live at a real jump-table
    // target — which is precisely why the branch cannot just use one of them —
    // so the thunk restoring them is the whole correctness claim, and it is
    // read back rather than trusted.
    int ok_val = 1, ok_regs = 1;
    for (unsigned k = 0; k < JT_N + 1; k++) {
        uint64_t r0 = 0, r16 = 0, r17 = 0;
        uint64_t sel = k;
        __asm__ volatile(
            "mov x0, %3\n"
            "mov x16, #0x1616\n"
            "mov x17, #0x1717\n"
            "blr %4\n"
            "mov %0, x0\n"
            "mov %1, x16\n"
            "mov %2, x17\n"
            : "=&r"(r0), "=&r"(r16), "=&r"(r17)
            : "r"(sel), "r"(code)
            : "x0","x1","x9","x10","x11","x12","x13","x14","x15","x16","x17",
              "x18","x30","cc","memory");
        uint64_t want = k < JT_N ? 0xA0 + k : 0xFF;
        if (r0 != want) {
            ok_val = 0;
            printf("       arm %u returned 0x%llx, wanted 0x%llx\n",
                   k, (unsigned long long)r0, (unsigned long long)want);
        }
        if (r16 != 0x1616 || r17 != 0x1717) ok_regs = 0;
    }
    ck(ok_val, "every arm of the table reached its own target through the thunks");
    ck(ok_regs, "...with x16 and x17 intact at the target, which is the whole fix");
    munmap(m, MAP);
}

int main(void) {
    printf("=== x18 veneer race windows ===\n");
    ck(KLX_TSD_SLOT * 8 == 4000,
       "the probe's #4000 is KLX_TSD_SLOT*8 (the shadow slot's offset)");

    // The veneers load the guest's shadow x18 out of the TSD slot, so the slot
    // has to exist before anything can be reloaded from it.
    kl_thread_init();

    // The slot has to hold a real target first. kl_thread_init leaves this
    // thread's shadow x18 at zero — no guest code has run — and a zero slot is
    // the one case the guard deliberately SPINS on rather than branching to 0
    // (a guest asking to branch to address 0 is a guest bug; a live process
    // with a stack beats an unblockable SIGKILL). Seeding it is therefore part
    // of manufacturing the race, not a workaround: without it this test hangs,
    // which is itself the documented behaviour.
    uint64_t tsd_base;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tsd_base));
    uint64_t *slot = &((uint64_t *)tsd_base)[KLX_TSD_SLOT];
    uint64_t saved = *slot;
    *slot = 0xdeadbeef00;
    uint64_t g = x18_br_guard_probe();
    *slot = saved;
    ck(g == 0xdeadbeef00, "the guard reloaded the slot's value after the zeroing");
    ck(g != 0, "...instead of letting `br x18` reach address 0");
    printf("       shadow x18 after the guard: 0x%llx\n", (unsigned long long)g);

    // ...and the jump-table rewrite, which removes the `br x18` window rather
    // than narrowing it.
    printf("=== br x18 jump-table rewrite ===\n");
    jt_build_and_run();

    printf(g_fail ? "=== FAILED (%d) ===\n" : "=== x18 veneer race windows OK ===\n", g_fail);
    return g_fail ? 1 : 0;
}
