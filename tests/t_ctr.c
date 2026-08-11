// Trap 26 — the CTR_EL0 veneer, EXECUTED. `make ctr`, in `make check`.
//
// This gate exists because the failure it guards is a process kill with no
// diagnostic of its own: a guest `mrs Xt, CTR_EL0` is an illegal instruction
// from EL0 on Darwin, and the Steam Link shell dies on a physical Vision Pro
// the first time Qt's `__clear_cache` runs. The device is an expensive place to
// find that out — and the expensive part is not the crash, it is that the
// visionOS *simulator* never reaches that code, so the whole rung of the ladder
// below the device had nothing to say about it.
//
// **The host has everything to say about it.** `mrs Xt, CTR_EL0` traps on macOS
// too (measured; trap 26's record says otherwise and is wrong — the simulator's
// silence is Qt's code path not being taken there, not the kernel permitting
// the read). So this runs the real instruction, in a real executable mapping,
// through the real emitter:
//
//   1. the control — the raw instruction, in a child, must die on SIGILL.
//      Without it, a run where the veneer did nothing at all would pass.
//   2. the decoder finds the site for every Xt, keeps x18 visible as a
//      destination, and does not flag the neighbouring system register.
//   3. the emitter's rewrite, executed: the veneered code RETURNS, and returns
//      the fabricated CTR_EL0.
//   4. ...leaving every register but Xt alone.
//   5. `mrs x18, CTR_EL0` — both kinds of site at once — lands in the x18
//      shadow slot rather than in the architectural register.
//   6. the shape the guest actually has: __clear_cache's own two `tbnz`es,
//      reading our answer, take the branch the fabricated IDC/DIC imply.
//
// No guest, no headset, no Steam host; a few milliseconds.
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../runtime/kl_x18.h"

static int g_fail;
static void ck(int ok, const char *what) {
    if (!ok) g_fail++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// A page pair: code, and the veneer pool right after it, both made executable.
typedef struct { uint8_t *code; uint8_t *pool; size_t cap; } arena;

static arena arena_new(void) {
    arena a = {0};
    a.cap = 0x4000;
    uint8_t *p = mmap(NULL, a.cap * 2, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    a.code = p;
    a.pool = p + a.cap;
    return a;
}
static void arena_arm(arena *a) {
    if (mprotect(a->code, a->cap * 2, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect"); exit(2);
    }
    sys_icache_invalidate(a->code, a->cap * 2);
}
static void arena_free(arena *a) { munmap(a->code, a->cap * 2); }

// Emit `body` into the arena, run the emitter over it, and hand back a callable.
// The emitter takes the addresses the buffers will have when executed, which
// here are simply the buffers themselves — the degenerate case kl_x18_patch
// uses, so this exercises the same path the ELF loader does.
static uint64_t (*build(arena *a, const uint32_t *body, unsigned n,
                        kl_x18_stats *st))(uint64_t) {
    memcpy(a->code, body, n * 4);
    size_t used = 0;
    if (kl_x18_emit(a->code, n * 4, (uint64_t)(uintptr_t)a->code,
                    a->pool, a->cap, (uint64_t)(uintptr_t)a->pool, st, &used) != 0) {
        fprintf(stderr, "kl_x18_emit failed\n");
        exit(2);
    }
    arena_arm(a);
    return (uint64_t (*)(uint64_t))(void *)a->code;
}

#define MRS_CTR(rt)   (0xD53B0020u | (rt))
#define MRS_DCZID(rt) (0xD53B00E0u | (rt))     // S3_3_C0_C0_7 — legal from EL0
#define RET           0xD65F03C0u
#define MOV(d, s)     (0xAA0003E0u | ((uint32_t)(s) << 16) | (d))
#define MOVZ(rt, imm) (0xD2800000u | ((uint32_t)(imm) << 5) | (rt))
#define SUB_SP_16     0xD10043FFu
#define ADD_SP_16     0x910043FFu
#define STR_SP(rt)    (0xF90003E0u | (rt))
#define LDR_SP(rt)    (0xF94003E0u | (rt))

// Rung 1. The raw instruction, in a child, so its death is data rather than
// ours. A veneer that silently did nothing would otherwise pass every rung
// below by executing the real `mrs` and returning the real value.
static int raw_mrs_dies(void) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(2); }
    if (pid == 0) {
        arena a = arena_new();
        uint32_t body[] = { MRS_CTR(0), RET };
        memcpy(a.code, body, sizeof body);
        arena_arm(&a);
        uint64_t (*fn)(uint64_t) = (uint64_t (*)(uint64_t))(void *)a.code;
        uint64_t v = fn(0);
        // If it comes back, say so in the exit code rather than by printing:
        // stdout is shared with the parent and this child is expected to die.
        _exit(v ? 41 : 40);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        printf("       (control: the raw `mrs x0, CTR_EL0` died on signal %d)\n",
               WTERMSIG(status));
        return WTERMSIG(status) == 4 /* SIGILL */;
    }
    printf("       (control: the raw `mrs x0, CTR_EL0` RETURNED — this kernel "
           "permits the read, so nothing below proves the veneer ran)\n");
    return 0;
}

int main(void) {
    printf("=== trap 26: the CTR_EL0 veneer ===\n");
    const uint64_t want = kl_x18_ctr_value();
    printf("  the veneer answers %#llx (IDC=%llu DIC=%llu DminLine=%llu "
           "IminLine=%llu)\n", (unsigned long long)want,
           (unsigned long long)((want >> 28) & 1), (unsigned long long)((want >> 29) & 1),
           (unsigned long long)((want >> 16) & 0xf), (unsigned long long)(want & 0xf));

    ck(raw_mrs_dies(), "the un-veneered instruction is illegal on this kernel");

    // ---- 2. the decoder ----
    int found = 1, roled = 1, clean = 1;
    for (unsigned rt = 0; rt < 32; rt++) {
        klx_info in;
        klx_decode(MRS_CTR(rt), &in);
        if (!in.ok || in.sysreg != KLX_SYS_CTR_EL0) found = 0;
        // Rt is also a plain destination register, and for x18 that has to stay
        // visible — `mrs x18, CTR_EL0` is both kinds of site at once.
        if (rt == 18 && in.nfields != 1) roled = 0;
        // DCZID_EL0 is one CRm field away and IS legal from EL0, so flagging it
        // would be a wrong answer with no symptom: the veneer would answer a
        // block size the guest then divides by.
        klx_decode(MRS_DCZID(rt), &in);
        if (!in.ok || in.sysreg != KLX_SYS_NONE) clean = 0;
    }
    ck(found, "the decoder calls `mrs Xt, CTR_EL0` a CTR_EL0 read, all 32 Xt");
    ck(roled, "...and still sees x18 as a destination when Xt is x18");
    ck(clean, "...and does not flag `mrs Xt, DCZID_EL0`, which EL0 may read");

    // ---- 3. the rewrite, executed, for every destination ----
    //
    // x0..x29 except x18, which is rung 5. The callee-saved half (x19..x29) is
    // saved and restored around the site by the generated code, and the value
    // is carried out in a caller-saved temporary — otherwise this function
    // returns to a caller whose frame pointer we scribbled on, which is a crash
    // a long way from the thing being measured. x30 is the return address and
    // x31 is not a register.
    unsigned ran = 0, good = 0;
    for (unsigned rt = 0; rt <= 29; rt++) {
        if (rt == 18) continue;
        unsigned tmp = (rt == 8) ? 9 : 8;
        arena a = arena_new();
        kl_x18_stats st;
        uint32_t body[] = {
            SUB_SP_16, STR_SP(rt),
            MRS_CTR(rt),
            MOV(tmp, rt),
            LDR_SP(rt), ADD_SP_16,
            MOV(0, tmp), RET,
        };
        uint64_t (*fn)(uint64_t) = build(&a, body, 8, &st);
        if (st.ctr_sites != 1 || st.ctr_patched != 1 || st.ctr_refused) {
            char msg[128];
            snprintf(msg, sizeof msg, "x%u: emitter reported %u site(s) / %u "
                     "patched / %u refused", rt, st.ctr_sites, st.ctr_patched,
                     st.ctr_refused);
            ck(0, msg);
        } else {
            ran++;
            if (fn(0) == want) good++;
        }
        arena_free(&a);
    }
    {
        char msg[128];
        snprintf(msg, sizeof msg, "veneered `mrs Xt, CTR_EL0` returns %#llx for "
                 "%u of %u destinations", (unsigned long long)want, good, ran);
        ck(ran == 29 && good == ran, msg);
    }

    // ---- 4. ...and touches nothing else ----
    {
        arena a = arena_new();
        kl_x18_stats st;
        uint32_t body[] = { MOVZ(1, 0x5A5A), MRS_CTR(9), MOV(0, 1), RET };
        uint64_t (*fn)(uint64_t) = build(&a, body, 4, &st);
        ck(fn(0) == 0x5A5A, "the veneer leaves every register but Xt alone");
        arena_free(&a);
    }

    // ---- 5. `mrs x18, CTR_EL0` — both kinds of site at once ----
    //
    // The destination is the one register that must never hold guest state on
    // Darwin (trap 0), so the veneer puts the constant in the shadow slot
    // instead. Read back through pthread_getspecific, which is the API side of
    // the same slot the veneers index directly.
    if (kl_x18_init() != 0) {
        printf("       (skipped: TSD slot %d could not be claimed in this "
               "process, so `mrs x18, CTR_EL0` cannot be checked)\n", KLX_TSD_SLOT);
    } else {
        arena a = arena_new();
        kl_x18_stats st;
        // No save/restore scaffolding: x18 is reserved on Darwin, nothing here
        // holds anything in it, and any `str x18` would itself be an x18 site.
        uint32_t body[] = { MRS_CTR(18), RET };
        uint64_t (*fn)(uint64_t) = build(&a, body, 2, &st);
        pthread_setspecific(KLX_TSD_SLOT, (void *)(uintptr_t)0xDEAD);
        fn(0);
        uint64_t slot = (uint64_t)(uintptr_t)pthread_getspecific(KLX_TSD_SLOT);
        char msg[160];
        snprintf(msg, sizeof msg, "`mrs x18, CTR_EL0` writes %#llx into the x18 "
                 "shadow slot (%u CTR site, %u x18 site)",
                 (unsigned long long)slot, st.ctr_patched, st.patched);
        ck(slot == want && st.ctr_patched == 1 && st.patched == 0, msg);
        pthread_setspecific(KLX_TSD_SLOT, NULL);
        arena_free(&a);
    }

    // ---- 6. the shape the guest actually has ----
    //
    // libQt6Core_arm64-v8a.so +0x588fac, reduced to the half that decides:
    // `mrs x9, CTR_EL0` then the two `tbnz`es that skip the `dc cvau` and
    // `ic ivau` loops. The loops themselves are left out because they are legal
    // here — only the `mrs` is trapped — so running them would measure nothing.
    // Returns 1 for the IDC exit, 2 for the DIC exit, 0 for neither.
    {
        arena a = arena_new();
        kl_x18_stats st;
        uint32_t body[] = {
            MRS_CTR(9),                      //   mrs  x9, CTR_EL0
            0x37E00089u,                     //   tbnz w9, #28, +16   (IDC)
            0x37E800A9u,                     //   tbnz w9, #29, +20   (DIC)
            MOVZ(0, 0), RET,
            MOVZ(0, 1), RET,                 //   IDC exit
            MOVZ(0, 2), RET,                 //   DIC exit
        };
        uint64_t (*fn)(uint64_t) = build(&a, body, 9, &st);
        uint64_t taken = fn(0);
        int idc = (want >> 28) & 1, dic = (want >> 29) & 1;
        uint64_t expect = idc ? 1 : (dic ? 2 : 0);
        char msg[192];
        snprintf(msg, sizeof msg,
                 "__clear_cache's own tests read our answer as %s, so %s",
                 taken == 1 ? "IDC set" : taken == 2 ? "DIC set" : "neither set",
                 expect == 0 ? "both maintenance loops run"
                             : "the maintenance loops are skipped");
        ck(taken == expect, msg);
        arena_free(&a);
    }

    printf("       (KL_CTR=0 is the A/B and KL_CTR_EL0=<hex> changes the answer; "
           "both are read once, before the first library loads)\n");
    printf("%s: trap 26\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
