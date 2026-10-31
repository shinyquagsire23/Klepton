// CASP through x18:x19, EXECUTED. `make casp`.
//
// tools/check_x18.py can say the decoder found the site and that the rewritten
// word still disassembles to a CASP. It cannot say the veneer is RIGHT, and this
// one moves two registers and a TSD slot around an atomic instruction — so it is
// run, against a real 128-bit cell, in both positions x18 can take:
//
//   Rs = x18:x19   the expected value, and afterwards what memory held
//   Rt = x18:x19   the new value
//
// each for a compare that succeeds and one that fails. Valve's Source 2 build
// has eleven of these; no Android guest so far has had one.
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

#define CASPAL(rs, rt, rn) (0x4860FC00u | ((uint32_t)(rs) << 16) | ((uint32_t)(rn) << 5) | (rt))
#define PUSH_X19_X30  0xA9BF7BF3u
#define POP_X19_X30   0xA8C17BF3u
#define MOV(d, s)     (0xAA0003E0u | ((uint32_t)(s) << 16) | (d))
#define RET           0xD65F03C0u

// fn(cell, x19 on entry, pair low, pair high) -> whatever the body leaves in x0.
typedef uint64_t (*fn4)(uint64_t *, uint64_t, uint64_t, uint64_t);

static fn4 build(const uint32_t *body, unsigned n, kl_x18_stats *st) {
    size_t cap = 0x4000;
    uint8_t *code = mmap(NULL, cap * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); exit(2); }
    memcpy(code, body, n * 4);
    size_t used = 0;
    if (kl_x18_emit(code, n * 4, (uint64_t)(uintptr_t)code, code + cap, cap,
                    (uint64_t)(uintptr_t)(code + cap), st, &used) != 0) exit(2);
    if (mprotect(code, cap * 2, PROT_READ | PROT_EXEC) != 0) { perror("mprotect"); exit(2); }
    sys_icache_invalidate(code, cap * 2);
    return (fn4)(void *)code;
}

static uint64_t shadow(void) { return (uint64_t)(uintptr_t)pthread_getspecific(KLX_TSD_SLOT); }
static void set_shadow(uint64_t v) { pthread_setspecific(KLX_TSD_SLOT, (void *)(uintptr_t)v); }

int main(void) {
    printf("=== CASP through x18:x19 ===\n");
    if (kl_x18_init() != 0) { ck(0, "the x18 slot is claimed"); return 1; }

    klx_info in;
    klx_decode(CASPAL(18, 2, 0), &in);
    ck(in.ok && in.hazard == KLX_HZ_PAIR && in.nfields == 1 && in.roles == (KLX_R | KLX_W) &&
       (in.gp_used & (1u << 19)) && (in.gp_used & (1u << 3)),
       "the decoder sees Rs = x18 as a read-write pair and counts x19 and x3 as used");
    klx_decode(CASPAL(2, 18, 0), &in);
    ck(in.ok && in.hazard == KLX_HZ_PAIR && in.nfields == 1 && in.roles == KLX_R,
       "...and Rt = x18 as a read-only one");

    kl_x18_stats st;
    _Alignas(16) uint64_t cell[2];

    // ---- Rs = x18:x19. x0 = cell, x1 -> x19, x2:x3 = the new value. ----
    uint32_t rs_body[] = { PUSH_X19_X30, MOV(19, 1), CASPAL(18, 2, 0), MOV(0, 19),
                           POP_X19_X30, RET };
    fn4 rs = build(rs_body, 6, &st);
    ck(st.sites == 1 && st.patched == 1 && !st.refused, "Rs = x18: one site, veneered");

    cell[0] = 0x1111; cell[1] = 0x2222;
    set_shadow(0x1111);
    uint64_t hi = rs(cell, 0x2222, 0xAAAA, 0xBBBB);
    ck(cell[0] == 0xAAAA && cell[1] == 0xBBBB, "a matching compare stores the new pair");
    ck(shadow() == 0x1111 && hi == 0x2222, "...and x18:x19 read back what memory held");

    cell[0] = 0x1111; cell[1] = 0x2222;
    set_shadow(0x9999);                                 // expected low is wrong
    hi = rs(cell, 0x2222, 0xAAAA, 0xBBBB);
    ck(cell[0] == 0x1111 && cell[1] == 0x2222, "a failing compare stores nothing");
    ck(shadow() == 0x1111 && hi == 0x2222, "...and x18:x19 receive memory's value");

    // ---- Rt = x18:x19. x0 = cell, x1 -> x19, x2:x3 = the expected value. ----
    uint32_t rt_body[] = { PUSH_X19_X30, MOV(19, 1), CASPAL(2, 18, 0), MOV(0, 2),
                           POP_X19_X30, RET };
    fn4 rt = build(rt_body, 6, &st);
    ck(st.sites == 1 && st.patched == 1 && !st.refused, "Rt = x18: one site, veneered");

    cell[0] = 0x1111; cell[1] = 0x2222;
    set_shadow(0xCCCC);
    uint64_t lo = rt(cell, 0xDDDD, 0x1111, 0x2222);
    ck(cell[0] == 0xCCCC && cell[1] == 0xDDDD && lo == 0x1111,
       "a matching compare stores the shadow slot and x19");
    ck(shadow() == 0xCCCC, "...and leaves the shadow slot alone");

    cell[0] = 0x1111; cell[1] = 0x2222;
    lo = rt(cell, 0xDDDD, 0x7777, 0x2222);
    ck(cell[0] == 0x1111 && cell[1] == 0x2222 && lo == 0x1111, "a failing compare stores nothing");

    // ---- blr x18 and adr x18: the other two shapes Source 2 needed ----
    // `blr x18` must call what the shadow slot holds and come back to the
    // instruction after the site, so the callee's x0 + 1 is what returns.
    {
        static const uint32_t callee_code[] = { 0x91001C00u /* add x0, x0, #7 */, RET };
        uint8_t *cal = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        memcpy(cal, callee_code, sizeof callee_code);
        mprotect(cal, 0x4000, PROT_READ | PROT_EXEC);
        sys_icache_invalidate(cal, 0x4000);
        uint32_t body[] = { PUSH_X19_X30, MOV(0, 1), 0xD63F0240u /* blr x18 */,
                            0x91000400u /* add x0, x0, #1 */, POP_X19_X30, RET };
        fn4 f = build(body, 6, &st);
        set_shadow((uint64_t)(uintptr_t)cal);
        ck(st.sites == 1 && st.patched == 1 && !st.refused, "blr x18: one site, veneered");
        ck(f(cell, 100, 0, 0) == 108, "...calls the shadow slot's target and returns after the site");
    }
    {
        // adr x18, .+8 ; ret — the shadow slot must hold the address two
        // instructions past the start of the body.
        uint32_t body[] = { 0x10000052u /* adr x18, .+8 */, RET, RET };
        fn4 f = build(body, 3, &st);
        set_shadow(0);
        f(cell, 0, 0, 0);
        ck(st.patched == 1 && !st.refused && shadow() == (uint64_t)(uintptr_t)(void *)f + 8,
           "adr x18, label puts the label's address in the shadow slot");
    }

    set_shadow(0);
    printf(g_fail ? "FAIL: %d check(s)\n" : "PASS: CASP through x18:x19\n", g_fail);
    return g_fail != 0;
}
