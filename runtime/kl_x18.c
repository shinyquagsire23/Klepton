// S0.5 — A64 decoder for x18 substitution. See kl_x18.h for why this exists.
//
// The job is narrow: given one instruction word, say which bit-fields are
// general-purpose register operands, which of those name x18, and whether each
// is read, written or both. It deliberately does not model semantics — a veneer
// re-emits the same instruction word with a different register number in those
// fields, so the only thing that has to be right is the field map.
//
// Two rules shape the whole file:
//
//   1. Positive recognition only. An encoding this decoder has not been taught
//      returns ok = 0 and the loader refuses to patch that site and says so.
//      The alternative — assume bits[4:0] is always a register — is wrong in
//      four ways that all occur in the real corpus: `ld4 {v16..v19}, [x18]`
//      (vector register), `prfm pldl1keep, [x18]` (prefetch op), `ccmp w18, #0,
//      #0, ne` (nzcv), and `b` (branch immediate). Guessing there would corrupt
//      the instruction.
//   2. Register 31 is not a register. In almost every encoding it means XZR or
//      SP, and it is never x18, so it is skipped — including for gp_used, since
//      a veneer can never collide with it.
//
// tests/t_x18.c checks every answer against objdump over all 17,619 real sites,
// in both directions: no site objdump calls x18 may be missed, and no field the
// decoder calls x18 may disagree with the disassembly.
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "kl_x18.h"
#include "kl_env.h"

static void addfld(klx_info *o, uint32_t w, uint8_t shift, uint8_t role) {
    unsigned r = (w >> shift) & 31;
    if (r == 31) return;                      // XZR / SP
    o->gp_used |= 1u << r;
    if (r != 18) return;
    if (o->nfields < 4) {
        o->x18[o->nfields].shift = shift;
        o->x18[o->nfields].role  = role;
        o->nfields++;
    }
    o->roles |= role;
}

// ---------- Data processing -- immediate (op0 = 100x) ----------
static int dp_imm(uint32_t w, klx_info *o) {
    if (((w >> 24) & 0x1f) == 0x10) {         // adr / adrp: immhi occupies [23:5]
        o->pcrel = ((w >> 31) & 1) ? KLX_PC_ADRP : KLX_PC_ADR;
        addfld(o, w, KLX_RD, KLX_W);
        return 1;
    }
    unsigned opc = (w >> 29) & 3;
    switch ((w >> 23) & 0x3f) {
    case 0x22:                                 // add/sub (immediate)
    case 0x23:                                 // add/sub (immediate, with tags)
        // Rd == 31 means SP here — but only while S is clear. adds/subs, and so
        // the cmp/cmn aliases, discard into XZR instead, which is why this must
        // test the S bit rather than Rd alone.
        if (!((w >> 29) & 1) && (w & 31) == 31) o->hazard = KLX_HZ_SPWRITE;
        addfld(o, w, KLX_RD, KLX_W);
        addfld(o, w, KLX_RN, KLX_R);
        return 1;
    case 0x24:                                 // logical (immediate)
        // N is the top bit of the bitmask-immediate encoding and only exists in
        // the 64-bit form: sf == 0 with N == 1 is unallocated. Words like
        // 52412072 in the middle of Steam Link's .text hit exactly this, and
        // they are not instructions at all — they are constant-pool bytes with
        // no symbol to exclude them. Refusing keeps the veneer off data.
        if (!((w >> 31) & 1) && ((w >> 22) & 1)) return 0;
        addfld(o, w, KLX_RD, KLX_W);
        addfld(o, w, KLX_RN, KLX_R);
        return 1;
    case 0x25:                                 // move wide: imm16 covers [20:5]
        if (opc == 1) return 0;                            // unallocated
        if (!((w >> 31) & 1) && ((w >> 22) & 1)) return 0;  // 32-bit form: hw >= 2
        // movk merges into the existing value; movz/movn overwrite it.
        addfld(o, w, KLX_RD, opc == 3 ? (KLX_R | KLX_W) : KLX_W);
        return 1;
    case 0x26:                                 // bitfield
        if (opc == 3) return 0;                // unallocated
        if (((w >> 31) & 1) != ((w >> 22) & 1)) return 0;   // N must track sf
        // bfm (and so bfi/bfxil) leaves the untouched bits of Rd in place, which
        // makes Rd a read as well. sbfm/ubfm overwrite the whole register.
        addfld(o, w, KLX_RD, opc == 1 ? (KLX_R | KLX_W) : KLX_W);
        addfld(o, w, KLX_RN, KLX_R);
        return 1;
    case 0x27:                                 // extract (extr, and so ror #imm)
        if (opc != 0 || ((w >> 21) & 1)) return 0;          // op21/o0 unallocated
        if (((w >> 31) & 1) != ((w >> 22) & 1)) return 0;   // N must track sf
        // ...and in the 32-bit form imms only has five meaningful bits, so a set
        // bit 5 is unallocated too. Same story as the logical case above: the
        // words that reach here are data, not a `ror` we are mis-reading.
        if (!((w >> 31) & 1) && ((w >> 15) & 1)) return 0;
        addfld(o, w, KLX_RD, KLX_W);
        addfld(o, w, KLX_RN, KLX_R);
        addfld(o, w, KLX_RM, KLX_R);
        return 1;
    }
    return 0;
}

// ---------- Branches, exception generating, system (op0 = 101x) ----------
static int branch_sys(uint32_t w, klx_info *o) {
    if (((w >> 26) & 0x1f) == 0x05) return 1;  // b / bl — [4:0] is part of imm26
    if (((w >> 25) & 0x7f) == 0x2a) return 1;  // b.cond
    if (((w >> 24) & 0xff) == 0xd4) return 1;  // svc / brk / hlt
    if (((w >> 22) & 0x3ff) == 0x354) {        // system: mrs / msr / hints / barriers
        // Trap 26. `mrs Xt, CTR_EL0` is S3_3_C0_C0_1, i.e. 0xD53B0020 | Rt, and
        // reading it from EL0 is an ILLEGAL INSTRUCTION on Darwin — measured on
        // macOS as well as visionOS. It is flagged here rather than refused
        // because the veneer answers it; note the flag is orthogonal to the x18
        // fields, so `mrs x18, CTR_EL0` is both and the emitter serves both.
        if ((w & 0xFFFFFFE0u) == 0xD53B0020u) o->sysreg = KLX_SYS_CTR_EL0;
        // Hints and barriers encode Rt as 31, so they fall out via addfld.
        addfld(o, w, KLX_RD, ((w >> 21) & 1) ? KLX_W : KLX_R);
        return 1;
    }
    if (((w >> 25) & 0x7f) == 0x6b) {          // br / blr / ret
        // An indirect branch consumes a register that must still hold the target
        // at the moment it branches, and nothing can restore that register
        // afterwards — there is no memory-indirect branch on A64. blr would in
        // fact fall through (it returns to just after itself), but that leaves
        // x30 pointing into the veneer, which an unwinder would not recognise.
        //
        // `br` alone is separable, and the emitter serves it: because control
        // never comes back, the register it branches through does not have to be
        // restored — it only has to be one the destination does not need. That
        // is exactly x18 and nothing else, which is why this is a special case
        // rather than the general shape. See the KLX_HZ_TERMBR arm of the
        // emitter for the argument and for the window it cannot close.
        o->hazard = ((w & 0xFFFFFC1Fu) == 0xD61F0000u) ? KLX_HZ_TERMBR
                                                       : KLX_HZ_NOFALL;
        addfld(o, w, KLX_RN, KLX_R);
        return 1;
    }
    unsigned c = (w >> 25) & 0x3f;
    if (c == 0x1a || c == 0x1b) {              // cbz/cbnz, tbz/tbnz
        o->pcrel = (c == 0x1a) ? KLX_PC_CBZ : KLX_PC_TBZ;
        addfld(o, w, KLX_RD, KLX_R);
        return 1;
    }
    return 0;
}

// ---------- Loads and stores (op0 = x1x0) ----------
static int ldst(uint32_t w, klx_info *o) {
    // Advanced SIMD load/store structures — ld1/st1/ld2/ld4/ld1r and friends.
    // Rt here is a *vector* register, so bits[4:0] == 18 means v18: the whole
    // point of recognising this class is to leave those alone.
    if ((w & 0xBE000000) == 0x0C000000) {
        unsigned post = (w >> 23) & 1;
        if (post && ((w >> 5) & 31) == 31) o->hazard = KLX_HZ_SPWRITE;
        addfld(o, w, KLX_RN, post ? (KLX_R | KLX_W) : KLX_R);
        if (post) addfld(o, w, KLX_RM, KLX_R);   // Rm == 31 selects the immediate form
        return 1;
    }
    // Load/store exclusive
    if ((w & 0x3F000000) == 0x08000000) {
        unsigned L = (w >> 22) & 1, o2 = (w >> 23) & 1;
        addfld(o, w, KLX_RN, KLX_R);
        addfld(o, w, KLX_RD, L ? KLX_W : KLX_R);
        addfld(o, w, KLX_RA, L ? KLX_W : KLX_R);            // Rt2, pair forms
        if (!L && !o2) addfld(o, w, KLX_RM, KLX_W);         // Rs receives the status
        return 1;
    }
    // Load register (literal) — [9:5] is part of imm19, not Rn
    if ((w & 0x3B000000) == 0x18000000) {
        if (!((w >> 26) & 1)) {
            o->pcrel = KLX_PC_LITERAL;
            addfld(o, w, KLX_RD, KLX_W);
        }
        return 1;
    }
    // Load/store pair
    if ((w & 0x3A000000) == 0x28000000) {
        unsigned V = (w >> 26) & 1, L = (w >> 22) & 1, mode = (w >> 23) & 3;
        if ((mode == 1 || mode == 3) && ((w >> 5) & 31) == 31) o->hazard = KLX_HZ_SPWRITE;
        addfld(o, w, KLX_RN, (mode == 1 || mode == 3) ? (KLX_R | KLX_W) : KLX_R);
        if (!V) {
            addfld(o, w, KLX_RD, L ? KLX_W : KLX_R);
            addfld(o, w, KLX_RA, L ? KLX_W : KLX_R);
        }
        return 1;
    }
    // Load/store register: unscaled, pre/post-indexed, register offset, unsigned
    // offset, and the atomic memory operations.
    if ((w & 0x3B000000) == 0x38000000 || (w & 0x3B000000) == 0x39000000) {
        unsigned size = (w >> 30) & 3, V = (w >> 26) & 1, opc = (w >> 22) & 3;
        // size == 11, V == 0, opc == 10 is PRFM/PRFUM, where bits[4:0] name a
        // prefetch operation (pldl1keep and friends) rather than a register.
        int prefetch = (size == 3 && !V && opc == 2);
        int is_load  = (opc != 0);
        int rt_gp    = (!V && !prefetch);

        if ((w >> 24) & 1) {                                // unsigned offset
            addfld(o, w, KLX_RN, KLX_R);
            if (rt_gp) addfld(o, w, KLX_RD, is_load ? KLX_W : KLX_R);
            return 1;
        }
        unsigned op2 = (w >> 10) & 3;
        if ((w >> 21) & 1) {
            if (op2 == 2) {                                 // register offset
                addfld(o, w, KLX_RN, KLX_R);
                addfld(o, w, KLX_RM, KLX_R);
                if (rt_gp) addfld(o, w, KLX_RD, is_load ? KLX_W : KLX_R);
                return 1;
            }
            if (op2 == 0) {                                 // atomics / swp / cas
                addfld(o, w, KLX_RN, KLX_R);
                addfld(o, w, KLX_RM, KLX_R);                // Rs
                if (rt_gp) addfld(o, w, KLX_RD, KLX_W);
                return 1;
            }
            return 0;                                       // LDAPR etc — refuse
        }
        if (op2 == 2) return 0;                             // ldtr/sttr unprivileged
        unsigned wb = (op2 == 1 || op2 == 3);               // post- / pre-indexed
        if (wb && ((w >> 5) & 31) == 31) o->hazard = KLX_HZ_SPWRITE;
        addfld(o, w, KLX_RN, wb ? (KLX_R | KLX_W) : KLX_R);
        if (rt_gp) addfld(o, w, KLX_RD, is_load ? KLX_W : KLX_R);
        return 1;
    }
    return 0;
}

// ---------- Data processing -- register (op0 = x101) ----------
static int dp_reg(uint32_t w, klx_info *o) {
    unsigned c = (w >> 24) & 0x1f;
    if (c == 0x0a || c == 0x0b) {              // logical / add-sub, shifted or extended
        // In the add/sub extended-register form alone, Rd == 31 is SP — and,
        // as above, only when S is clear.
        if (c == 0x0b && ((w >> 21) & 1) && !((w >> 29) & 1) && (w & 31) == 31)
            o->hazard = KLX_HZ_SPWRITE;
        addfld(o, w, KLX_RD, KLX_W);
        addfld(o, w, KLX_RN, KLX_R);
        addfld(o, w, KLX_RM, KLX_R);
        return 1;
    }
    if (c == 0x1b) {                           // 3-source: madd/msub/smaddl/umulh/...
        // op54 is unallocated for anything but 00, and the 32-bit form has only
        // madd/msub (op31 == 000). 5b9cca4f — data in Steam Link's .text with no
        // symbol over it — decodes here with op54 = 10 and would otherwise be
        // patched as if it were a real multiply.
        if ((w >> 29) & 3) return 0;
        if (!((w >> 31) & 1) && ((w >> 21) & 7)) return 0;
        addfld(o, w, KLX_RD, KLX_W);
        addfld(o, w, KLX_RN, KLX_R);
        addfld(o, w, KLX_RM, KLX_R);
        addfld(o, w, KLX_RA, KLX_R);
        return 1;
    }
    if (c != 0x1a) return 0;
    switch ((w >> 21) & 7) {
    case 0:                                    // add/sub with carry
    case 4:                                    // conditional select
        addfld(o, w, KLX_RD, KLX_W);
        addfld(o, w, KLX_RN, KLX_R);
        addfld(o, w, KLX_RM, KLX_R);
        return 1;
    case 2:                                    // conditional compare
        // [4:0] is nzcv and [14:10] is the condition — neither is a register.
        // The immediate form additionally puts imm5 where Rm would be.
        addfld(o, w, KLX_RN, KLX_R);
        if (!((w >> 11) & 1)) addfld(o, w, KLX_RM, KLX_R);
        return 1;
    case 6:                                    // 1-source (bit30 = 1) or 2-source
        addfld(o, w, KLX_RD, KLX_W);
        addfld(o, w, KLX_RN, KLX_R);
        // 1-source (rbit/rev/clz) puts opcode2 in [20:16], not Rm.
        if (!((w >> 30) & 1)) addfld(o, w, KLX_RM, KLX_R);
        return 1;
    }
    return 0;
}

// ---------- Scalar FP and Advanced SIMD (op0 = x111) ----------
// Only two families here name a general-purpose register at all. Everything
// else in the group is vector-only, so a field holding 18 means v18/d18/s18 and
// must be left alone — t_x18.c verifies that claim against the disassembly
// rather than taking it on trust.
static int simd_fp(uint32_t w, klx_info *o) {
    // Scalar floating-point. Only the two conversion families touch a GP
    // register; the rest of the group — fmul/fadd/fcsel/fneg, the fmov
    // immediate form — is entirely vector, and s18/d18 there is not our x18.
    // Both conversion families sit under bits[28:24] == 11110: bit21 == 0 is
    // the fixed-point form, and bit21 == 1 needs bits[15:10] == 0, which is
    // what separates it from FP data-processing, compare and conditional
    // select.
    if (!((w >> 30) & 1) && ((w >> 24) & 0x1f) == 0x1e &&
        (!((w >> 21) & 1) || !((w >> 10) & 0x3f))) {
        unsigned opcode = (w >> 16) & 7;
        // opcode 010/011 are SCVTF/UCVTF and 111 is FMOV Vd,Rn — those read a GP
        // register into the FP side. The rest (FCVT*, FMOV Rd,Vn) write one.
        if (opcode == 2 || opcode == 3 || opcode == 7) addfld(o, w, KLX_RN, KLX_R);
        else                                           addfld(o, w, KLX_RD, KLX_W);
        return 1;
    }
    // Advanced SIMD copy: dup / ins / umov / smov.
    if ((w & 0x9FE08400) == 0x0E000400) {
        if ((w >> 29) & 1) return 1;                   // ins (element): vector only
        switch ((w >> 11) & 0xf) {
        case 0x0: return 1;                            // dup (element): vector only
        case 0x1:                                      // dup (general)
        case 0x3: addfld(o, w, KLX_RN, KLX_R); return 1;   // ins (general)
        case 0x5:                                      // smov
        case 0x7: addfld(o, w, KLX_RD, KLX_W); return 1;   // umov
        }
        return 0;
    }
    return 1;                                          // vector-only: no GP fields
}

int klx_decode(uint32_t w, klx_info *o) {
    memset(o, 0, sizeof *o);
    unsigned op0 = (w >> 25) & 0xf;
    if      ((op0 & 0xe) == 0x8) o->ok = dp_imm(w, o);      // 100x
    else if ((op0 & 0xe) == 0xa) o->ok = branch_sys(w, o);  // 101x
    else if ((op0 & 0x5) == 0x4) o->ok = ldst(w, o);        // x1x0
    else if ((op0 & 0x7) == 0x5) o->ok = dp_reg(w, o);      // x101
    else if ((op0 & 0x7) == 0x7) o->ok = simd_fp(w, o);     // x111
    else                         o->ok = 0;                 // reserved / SVE / SME
    if (!o->ok) { o->nfields = 0; o->roles = 0; o->gp_used = 0; o->sysreg = KLX_SYS_NONE; }
    return o->ok;
}

uint32_t klx_substitute(uint32_t w, const klx_info *info, unsigned reg) {
    for (unsigned i = 0; i < info->nfields; i++) {
        unsigned sh = info->x18[i].shift;
        w = (w & ~(31u << sh)) | ((reg & 31u) << sh);
    }
    return w;
}

// ============================ veneer emission ============================
//
// Each x18 site is replaced by a single `b` to a veneer, so the instruction
// count never changes and every pc-relative offset elsewhere in the image stays
// valid — which is what makes this possible in place, without the offline
// re-layout that PLANNING S0.5 assumed any fix would need.
//
// The veneer keeps x18's value in a per-thread TSD slot and never lets it live
// in the architectural register at all:
//
//      stp  a, b, [sp, #-16]        spill two scratch regs into the red zone
//      mrs  a, tpidrro_el0          (sp must NOT move: 317 sites have
//      ldr  b, [a, #slot*8]          sp-relative operands, and s06_redzone.c
//      <the original insn, x18->b>   shows the red zone survives both
//      str  b, [a, #slot*8]          preemption and signal delivery)
//      ldp  a, b, [sp, #-16]
//      b    <site+4>
//
// The ldr is emitted only if the site reads x18 and the str only if it writes
// it, so the common read-only case is six instructions. The load and store are
// dropped for scratch registers rather than x18 itself, and a, b are ordinary
// registers that survive preemption normally — which is the whole reason this
// is sound where the architectural x18 is not.
#define VEN_MAX_INSN KLX_VEN_MAX_INSN
#define REFUSE_KINDS 24

// The slot is a compile-time constant now (see kl_x18.h for why, and for the
// measurement behind the number). g_slot only records that it has been checked.
static int g_slot = -1;
static struct { uint32_t word; unsigned n; } g_refused[REFUSE_KINDS];
static unsigned g_nrefused_kinds, g_refused_total;

// Claim the slot before anything in this process has had a chance to take it.
//
// The key walk can only succeed while 300 is still unissued, and Darwin hands
// external keys out UPWARD and never reissues a held one — so this is a race
// against every other library in the process, and in an app bundle we do not get
// to run first. On the visionOS 27 simulator the app's boot thread is offered
// **330 as its very first key**: SwiftUI, UIKit and their dependencies have
// burned past 300 before a single line of ours executes, and the guest then
// refuses to load with "TSD slot 300 is unavailable", which reads as a platform
// limit rather than as a starting-gun problem.
//
// A constructor is early enough because dyld runs image initializers before
// main(), and the frameworks' key consumption happens during app launch rather
// than at load. It costs one pthread key in every process that links the
// runtime, which is what the key was going to cost anyway — `build/m_boot` and
// `build/m_slink` already claim it in their first statement, and this simply
// makes them not have to.
__attribute__((constructor))
static void klx_claim_slot_early(void) {
    // Quietly: a process that links the runtime and never loads a guest (a unit
    // test, a tool) has no use for the slot and no business printing about it.
    // Everything that DOES load a guest calls kl_x18_init() again on its load
    // path, and that call reports.
    (void)kl_x18_init();
}

int kl_x18_init(void) {
    if (g_slot >= 0) return 0;

    uint64_t tp;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    // Darwin's own os/tsd.h masks the low three bits off as a CPU number. They
    // measure as always zero on Apple silicon, and kl_thread_init has relied on
    // that since S0.1 — but a veneer indexes the TSD array on every guest x18
    // access, and a misaligned base there is a silently wrong read rather than
    // a fault. Cheap to check once, so check rather than assume.
    if (tp & 7) return -1;
    if (KLX_TSD_SLOT >= 4096) return -1;          // ldr's scaled imm12 caps the slot

    // Claim KLX_TSD_SLOT as a real pthread key rather than squatting it (see
    // kl_x18.h for what squatting cost). Darwin hands out external keys upward
    // and never reissues one that is currently held, so creating keys until it
    // yields ours walks up to it; the ones collected on the way are released
    // afterwards, so the process keeps its key budget.
    #define KLX_KEY_WALK 512
    static pthread_key_t spare[KLX_KEY_WALK];
    unsigned nspare = 0;
    int got = 0;
    unsigned first = 0, last = 0;
    for (unsigned i = 0; i < KLX_KEY_WALK; i++) {
        pthread_key_t k;
        if (pthread_key_create(&k, NULL) != 0) break;
        if (!i) first = (unsigned)k;
        last = (unsigned)k;
        if ((unsigned)k == KLX_TSD_SLOT) { got = 1; break; }
        if ((unsigned)k > KLX_TSD_SLOT) { spare[nspare++] = k; break; }
        spare[nspare++] = k;
    }
    for (unsigned i = 0; i < nspare; i++) pthread_key_delete(spare[i]);
    if (!got) {
        // WHICH keys it was offered, because "something else holds it" has two
        // very different causes and the fix differs: a process that is already
        // PAST 300 when we ask (first > 300 — too much ran before us, and the
        // answer is to ask earlier) versus one that runs out of keys on the way
        // (last < 300 — the walk was refused, and the answer is elsewhere).
        // Without the numbers those read identically.
        fprintf(stderr, "  [klepton] x18: could not claim TSD key %d — this "
                        "process offered keys %u..%u instead. Veneers are baked "
                        "against %d; see runtime/kl_x18.h.\n",
                KLX_TSD_SLOT, first, last, KLX_TSD_SLOT);
        return -1;
    }

    // The veneer reads the key inline as [tsd + k*8] rather than through
    // pthread_getspecific, so prove that the direct access agrees with the API
    // on this key before emitting seventeen thousand copies of the assumption.
    pthread_setspecific(KLX_TSD_SLOT, (void *)(uintptr_t)0x5A5A5A5AU);
    if (((void **)(uintptr_t)tp)[KLX_TSD_SLOT] != (void *)(uintptr_t)0x5A5A5A5AU) {
        fprintf(stderr, "  [klepton] x18: TSD key %d is not at tsd[%d] on this OS\n",
                KLX_TSD_SLOT, KLX_TSD_SLOT);
        return -1;
    }
    pthread_setspecific(KLX_TSD_SLOT, NULL);

    g_slot = KLX_TSD_SLOT;
    return 0;
}

// ---------- trap 0b, second detector: is this word's NEIGHBOURHOOD code? ----
//
// kl_x18_data_ranges finds data inside executable sections from sized
// STT_OBJECT symbols, and symbol coverage is the ceiling on what that can see.
// Steam Link's libshell_arm64-v8a.so is where the ceiling showed: it carries
// CRYPTOGAMS assembly whose constant tables — AES, GHASH, the ChaCha20
// "expand 32-byte k" block, several banner strings — sit in .text as LOCAL
// labels with no symbol at all. Twelve of that library's fourteen apparent x18
// sites were words of those tables, and patching them replaced crypto
// constants with branches. The symptom was a TLS handshake that got as far as
// the server's first encrypted record and then answered `bad_record_mac`,
// 25 MB and one subsystem away from here.
//
// The discriminator is the A64 top-level encoding group, bits [28:25]: groups
// 0b0000, 0b0001 and 0b0011 are unallocated (0b0010 is SVE, which nothing here
// targets, so it is counted too). Real code contains none; random table data
// hits one about three words in sixteen.
//
// It is a window test, not a per-word test, because a single word must stay
// patchable wherever it legitimately appears. Measured over every real site in
// both Beat Saber targets — 17,619 of them, in-FDE and out — the count in a
// +/-16-word window is ZERO, every time. Over libshell's twelve false
// positives it is 3 to 12. The threshold is 2 rather than 1 purely for margin
// against a literal pool landing inside the window; nothing observed needs it.
//
// A missed real site is the other half of trap 0 and must not be silent, so
// every refusal is counted and `make check`'s veneer totals are the gate: they
// are exact numbers, and this rule must not move them.
//
// A WORD OF ZERO IS NOT EVIDENCE. It is what a linker leaves in the GAP between
// functions, and a gap is adjacent to code by definition — so counting it makes
// the window vote on the wrong side of a boundary it cannot see. It cost the
// arc that found it: libunity.so+0x3f2118 is a `-fstack-protector` prologue
// three words past thirteen zero words of padding, the window refused it, and a
// REFUSED TLS SITE IS TRAP 1 BACK (see rewrite_tls in kl_image.c). It presented
// as a SIGSEGV in the guest 40 minutes of bisect away, on the lifecycle path
// only, with `make check` green throughout.
//
// The margin for dropping it is measured, not assumed: across every trap-0d
// refusal in both guests' entire library sets, the NON-ZERO evidence alone is 3
// to 12 words against a threshold of 2 — the crypto tables this exists to
// protect are dense pseudorandom data and do not need zeros to be recognised.
// Zero-only windows, meanwhile, are exactly one site in either guest, and it is
// the one above.
#define KLX_WIN_HALF   16
#define KLX_WIN_LIMIT  2

static int klx_unallocated(uint32_t w) {
    if (!w) return 0;                      // padding, not a constant table
    unsigned op0 = (w >> 25) & 0xf;
    return op0 <= 3;                       // 0/1/3 unallocated, 2 = SVE
}
// `i` is a word index into `w`; the window is clamped to the chunk, which is a
// whole executable section minus its known data ranges, so clamping only ever
// bites at a section edge.
static int klx_looks_like_data(const uint32_t *w, size_t n, size_t i) {
    size_t lo = i > KLX_WIN_HALF ? i - KLX_WIN_HALF : 0;
    size_t hi = i + KLX_WIN_HALF + 1 < n ? i + KLX_WIN_HALF + 1 : n;
    unsigned bad = 0;
    for (size_t k = lo; k < hi; k++)
        if (klx_unallocated(w[k]) && ++bad >= KLX_WIN_LIMIT) return 1;
    return 0;
}

// Exposed so the offline tool reports the same verdict the loader acts on —
// two answers to "is this word data" is how the pair drifts.
int kl_x18_is_data(const void *code, size_t size, size_t index) {
    return klx_looks_like_data((const uint32_t *)code, size / 4, index);
}

// One survey feeding both the count and the emitter, so a chunk with no real
// sites still reports its data words. The emitter returns early in that case —
// correctly, there is nothing to veneer — and counting there alone silently
// undercounted, which is the one thing this number must not do.
//
// `ctr` is trap 26's population and is counted APART: the x18 totals are exact
// numbers `make check` gates on, and a second population sharing them would
// move a number that is meant to be stable. The data-word test applies to both,
// for the same reason — 0xd53b0029 is a perfectly ordinary word to find in a
// constant table.
static unsigned klx_survey(const void *code, size_t size, unsigned *data,
                           unsigned *ctr) {
    const uint32_t *w = (const uint32_t *)code;
    size_t n = size / 4;
    unsigned c = 0;
    if (data) *data = 0;
    if (ctr) *ctr = 0;
    for (size_t i = 0; i < n; i++) {
        klx_info in;
        klx_decode(w[i], &in);
        if (!in.nfields && !in.sysreg) continue;
        if (klx_looks_like_data(w, n, i)) { if (data && in.nfields) (*data)++; continue; }
        if (in.nfields) c++;
        if (in.sysreg && ctr) (*ctr)++;
    }
    return c;
}

unsigned kl_x18_count(const void *code, size_t size) {
    unsigned ctr = 0;
    return klx_survey(code, size, NULL, &ctr) + ctr;
}

// ---------- instruction encoders ----------
static uint32_t enc_stp(unsigned t, unsigned t2, int off) {   // stp xT, xT2, [sp, #off]
    return 0xA9000000u | ((uint32_t)((off / 8) & 0x7f) << 15) | (t2 << 10) | (31u << 5) | t;
}
static uint32_t enc_ldp(unsigned t, unsigned t2, int off) {
    return 0xA9400000u | ((uint32_t)((off / 8) & 0x7f) << 15) | (t2 << 10) | (31u << 5) | t;
}
static uint32_t enc_ldr(unsigned t, unsigned n, unsigned off) {   // ldr xT, [xN, #off]
    return 0xF9400000u | ((off / 8) << 10) | (n << 5) | t;
}
static uint32_t enc_str(unsigned t, unsigned n, unsigned off) {
    return 0xF9000000u | ((off / 8) << 10) | (n << 5) | t;
}
static uint32_t enc_mrs_tpidrro(unsigned t) { return 0xD53BD060u | t; }
static uint32_t enc_movz(unsigned t, uint16_t imm, unsigned shift16) {  // movz xT, #imm, lsl #(16*s)
    return 0xD2800000u | ((uint32_t)shift16 << 21) | ((uint32_t)imm << 5) | t;
}
static uint32_t enc_movk(unsigned t, uint16_t imm, unsigned shift16) {
    return 0xF2800000u | ((uint32_t)shift16 << 21) | ((uint32_t)imm << 5) | t;
}

// Materialise a 64-bit constant into xT with as few movz/movk as it needs.
// Always at least one instruction, so a zero constant is `movz xT, #0`.
static unsigned enc_mov_imm64(unsigned t, uint64_t v, uint32_t *out) {
    unsigned k = 0;
    for (unsigned s = 0; s < 4; s++) {
        uint16_t part = (uint16_t)(v >> (16 * s));
        if (!part) continue;
        uint32_t insn = k ? enc_movk(t, part, s) : enc_movz(t, part, s);
        out[k++] = insn;
    }
    if (!k) out[k++] = enc_movz(t, 0, 0);
    return k;
}

static int enc_b(uint64_t from, uint64_t to, uint32_t *out) {
    int64_t d = (int64_t)to - (int64_t)from;
    if ((d & 3) || d < -(1LL << 27) || d >= (1LL << 27)) return 0;   // +/-128 MB
    *out = 0x14000000u | (uint32_t)((d >> 2) & 0x03ffffffu);
    return 1;
}

// Rebuild `adrp` against the veneer's own pc so it still names the same page.
// The pool sits within 128 MB of the code, and adrp reaches +/-4 GB, so this
// never fails in practice — but it is checked, because a wrong page here would
// be a plausible-looking pointer rather than a crash.
static uint32_t reenc_adrp(uint32_t w, uint64_t oldpc, uint64_t newpc, int *ok) {
    int64_t imm = (int64_t)((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3));
    imm = (imm << 43) >> 43;                                  // sign-extend 21 bits
    int64_t target = (int64_t)(oldpc & ~0xFFFULL) + (imm << 12);
    int64_t d = (target - (int64_t)(newpc & ~0xFFFULL)) >> 12;
    if (d < -(1LL << 20) || d >= (1LL << 20)) { *ok = 0; return w; }
    *ok = 1;
    uint32_t v = (uint32_t)((uint64_t)d & 0x1fffff);
    return (w & ~((0x7ffffu << 5) | (3u << 29))) | ((v >> 2) << 5) | ((v & 3) << 29);
}

static uint64_t br_target(uint32_t w, uint64_t pc, unsigned kind) {
    int64_t imm = (kind == KLX_PC_CBZ) ? (int64_t)((w >> 5) & 0x7ffff)
                                       : (int64_t)((w >> 5) & 0x3fff);
    imm = (kind == KLX_PC_CBZ) ? ((imm << 45) >> 45) : ((imm << 50) >> 50);
    return pc + (uint64_t)(imm << 2);
}
static uint32_t set_br_imm(uint32_t w, unsigned kind, int32_t insns) {
    if (kind == KLX_PC_CBZ) return (w & ~(0x7ffffu << 5)) | (((uint32_t)insns & 0x7ffff) << 5);
    return (w & ~(0x3fffu << 5)) | (((uint32_t)insns & 0x3fff) << 5);
}

// Two registers the instruction does not itself name. It has at most four
// operands, so a pool of six candidates always yields two.
static void pick_scratch(uint32_t used, unsigned *a, unsigned *b) {
    static const unsigned cand[] = {16, 17, 9, 10, 11, 12, 13, 14, 15, 8};
    unsigned got[2], n = 0;
    for (unsigned i = 0; i < sizeof cand / sizeof cand[0] && n < 2; i++)
        if (!(used & (1u << cand[i]))) got[n++] = cand[i];
    *a = got[0];
    *b = got[1];
}

static void note_refusal(uint32_t word) {
    g_refused_total++;
    for (unsigned i = 0; i < g_nrefused_kinds; i++)
        if (g_refused[i].word == word) { g_refused[i].n++; return; }
    if (g_nrefused_kinds < REFUSE_KINDS)
        g_refused[g_nrefused_kinds++] = (typeof(g_refused[0])){word, 1};
}

// KL_X18=0 disables the rewrite but NOT the survey. Counting has to stay
// unconditional so that "how many x18 sites does this image have" means the same
// thing in both arms of an A/B — otherwise the control run of tests/t_guest.c
// reports zero sites and refuses to compare anything, which looks like a pass.
static int veneer_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = kl_env_on("KL_X18", 1);   // ON unless explicitly KL_X18=0
    }
    return cached;
}

// Trap 26's own knob, deliberately independent of KL_X18: the two rewrites fix
// different registers and an A/B on one must not silently move the other.
// KL_CTR=0 leaves `mrs Xt, CTR_EL0` alone, which is a SIGILL wherever it runs.
static int ctr_enabled(void) {
    static int cached = -1;
    if (cached < 0) cached = kl_env_on("KL_CTR", 1);
    return cached;
}

// ...and what the veneer answers. See KLX_CTR_EL0_VALUE in kl_x18.h for why
// this value and not the IDC=1/DIC=1 one trap 26 designed.
uint64_t kl_x18_ctr_value(void) {
    static uint64_t cached;
    static int init;
    if (!init) {
        init = 1;
        cached = KLX_CTR_EL0_VALUE;
        const char *s = kl_env_str("KL_CTR_EL0", NULL);
        if (s && *s) cached = strtoull(s, NULL, 0);
    }
    return cached;
}
#define ctr_value() kl_x18_ctr_value()

// The emitter proper. `code_va`/`pool_va` are the addresses the two buffers will
// have when the code executes, which is what every `b` and `adrp` is computed
// against; the buffers themselves may live anywhere (a file image being laid out
// offline, or the mapping itself at load time).
int kl_x18_emit(void *code, size_t size, uint64_t code_va,
                void *pool, size_t poolcap, uint64_t pool_va,
                kl_x18_stats *st, size_t *pool_used) {
    memset(st, 0, sizeof *st);

    uint32_t *w = (uint32_t *)code;
    size_t    n = size / 4;

    unsigned want = klx_survey(code, size, &st->data_words, &st->ctr_sites);
    st->sites = want;
    if (pool_used) *pool_used = 0;
    int do_x18 = want && veneer_enabled();
    int do_ctr = st->ctr_sites && ctr_enabled();
    if (!do_x18 && !do_ctr) return 0;

    uint32_t *out = (uint32_t *)pool, *pool_end = (uint32_t *)((uint8_t *)pool + poolcap);

    for (size_t i = 0; i < n; i++) {
        klx_info in;
        klx_decode(w[i], &in);
        if (!in.nfields && !in.sysreg) continue;
        // Not a refusal: a refusal is a site we could not veneer, and this is a
        // word that was never an instruction. Already tallied by the survey
        // above, so only skipped here.
        if (klx_looks_like_data(w, n, i)) continue;

        // Which rewrite this word wants. A word can want both — `mrs x18,
        // CTR_EL0` — and then the CTR veneer is the one that serves it, because
        // it can put the constant into the shadow slot but the x18 veneer
        // cannot avoid executing the `mrs`.
        int ctr_site = in.sysreg == KLX_SYS_CTR_EL0 && do_ctr;
        int x18_site = in.nfields && do_x18 && !ctr_site;
        if (!ctr_site && !x18_site) continue;

        uint64_t spc = code_va + (uint64_t)i * 4;
        uint64_t vpc = pool_va + (uint64_t)((uint8_t *)out - (uint8_t *)pool);
        uint32_t word = w[i];

        // Trap 26: `mrs Xt, CTR_EL0`. No spill, no TSD and no scratch — the
        // veneer materialises a constant into the destination the instruction
        // already names and branches back. The only case that needs the x18
        // machinery is `mrs x18, CTR_EL0`, where the destination is the shadow
        // slot rather than a register; it does not occur in either guest, and
        // it is served rather than refused because a refusal here is a SIGILL.
        if (ctr_site) {
            uint32_t body[VEN_MAX_INSN];
            unsigned k = 0;
            int ok = 1;
            unsigned rt = word & 31u;
            if (out + VEN_MAX_INSN > pool_end) {
                note_refusal(word); st->ctr_refused++; continue;
            }
            if (in.nfields) {                       // destination is x18 itself
                unsigned a, b;
                pick_scratch(in.gp_used, &a, &b);
                body[k++] = enc_stp(a, b, -16);
                body[k++] = enc_mrs_tpidrro(a);
                k += enc_mov_imm64(b, ctr_value(), body + k);
                body[k++] = enc_str(b, a, (unsigned)KLX_TSD_SLOT * 8);
                body[k++] = enc_ldp(a, b, -16);
            } else {
                k += enc_mov_imm64(rt, ctr_value(), body + k);
            }
            ok &= (k < VEN_MAX_INSN);
            if (ok) { ok &= enc_b(vpc + k * 4, spc + 4, &body[k]); k++; }
            uint32_t patch;
            if (ok) ok &= enc_b(spc, vpc, &patch);
            if (!ok) { note_refusal(word); st->ctr_refused++; continue; }
            memcpy(out, body, k * 4);
            out += k;
            w[i] = patch;
            st->ctr_patched++;
            continue;
        }

        // `br x18` — trap 0's one terminal case, and the only site shape served
        // without a spill or a scratch register:
        //
        //      mrs  x18, tpidrro_el0
        //      ldr  x18, [x18, #slot*8]
        //      br   x18
        //
        // Why x18 and only x18. An indirect branch needs a register still holding
        // the target when it branches, and control never returns, so that
        // register is never restored — the destination gets it clobbered. Every
        // ordinary register is therefore unusable unless it is provably dead at
        // the destination, and at a jump-table dispatch nothing is: all six sites
        // in libunity are `ldrsw x18,[table,idx]` / `add x18,x18,base` / `br x18`
        // with x16 AND x17 live across them (libunity+0xab1d70, itself a table
        // target, reads the x17 set before the branch). x18 is the one register
        // guest code cannot observe — every guest access to it is veneered to the
        // TSD slot, so the architectural register carries nothing, which is the
        // whole premise of this file. Clobbering it costs nothing.
        //
        // What this does NOT close: the kernel zeroes x18 on any exception
        // return, so a timer interrupt landing between the `ldr` and the `br`
        // branches to 0. That window is one instruction and cannot be made
        // smaller — a64 has no memory-indirect branch, so the target must sit in
        // a register across at least one boundary. It is a real race, not a
        // theoretical one (trap 0), and it is left standing deliberately: the
        // alternative is refusing the site, which is not a smaller risk but a
        // CERTAIN branch through a zeroed x18 every time it executes. Beat Saber
        // 1.6.0 reaches one of these during scene load and dies on it every run.
        //
        // It is also self-identifying if it ever fires: a fault at address 0 with
        // pc 0 and a guest frame above it is this and essentially nothing else,
        // because a jump-table target is never address 0.
        if (in.hazard == KLX_HZ_TERMBR) {
            if (out + VEN_MAX_INSN > pool_end) {
                note_refusal(word); st->refused++; continue;
            }
            uint32_t body[VEN_MAX_INSN], patch;
            unsigned k = 0;
            body[k++] = enc_mrs_tpidrro(18);
            body[k++] = enc_ldr(18, 18, (unsigned)KLX_TSD_SLOT * 8);
            body[k++] = word;                  // br x18, against the loaded value
            if (!enc_b(spc, vpc, &patch)) { note_refusal(word); st->refused++; continue; }
            memcpy(out, body, k * 4);
            out += k;
            w[i] = patch;
            st->patched++;
            continue;
        }

        if (in.hazard || in.pcrel == KLX_PC_ADR || in.pcrel == KLX_PC_LITERAL ||
            out + VEN_MAX_INSN > pool_end) { note_refusal(word); st->refused++; continue; }

        unsigned a, b;
        pick_scratch(in.gp_used, &a, &b);

        uint32_t body[VEN_MAX_INSN];
        unsigned k = 0;
        int      ok = 1;

        body[k++] = enc_stp(a, b, -16);
        body[k++] = enc_mrs_tpidrro(a);

        if (in.pcrel == KLX_PC_CBZ || in.pcrel == KLX_PC_TBZ) {
            uint64_t target = br_target(word, spc, in.pcrel);
            body[k++] = enc_ldr(b, a, (unsigned)KLX_TSD_SLOT * 8);
            // Inverted (bit 24 flips cbz<->cbnz and tbz<->tbnz) so the taken
            // path is a short forward skip and the real target is reached by an
            // unrestricted `b` — cbz only spans 1 MB, which the pool may exceed.
            body[k] = set_br_imm(klx_substitute(word, &in, b) ^ (1u << 24), in.pcrel, 3);
            k++;
            body[k++] = enc_ldp(a, b, -16);
            ok &= enc_b(vpc + k * 4, target, &body[k]); k++;
            body[k++] = enc_ldp(a, b, -16);
            ok &= enc_b(vpc + k * 4, spc + 4, &body[k]); k++;
        } else {
            uint32_t sub = klx_substitute(word, &in, b);
            if (in.roles & KLX_R) body[k++] = enc_ldr(b, a, (unsigned)KLX_TSD_SLOT * 8);
            if (in.pcrel == KLX_PC_ADRP) {
                int fits;
                sub = reenc_adrp(sub, spc, vpc + k * 4, &fits);
                ok &= fits;
            }
            body[k++] = sub;
            if (in.roles & KLX_W) body[k++] = enc_str(b, a, (unsigned)KLX_TSD_SLOT * 8);
            body[k++] = enc_ldp(a, b, -16);
            ok &= enc_b(vpc + k * 4, spc + 4, &body[k]); k++;
        }

        uint32_t patch;
        ok &= enc_b(spc, vpc, &patch);
        if (!ok) { note_refusal(word); st->refused++; continue; }

        // KL_X18_MAP=<file>: "pool_addr site_addr" per site, so a guest return
        // address caught in a shim (always a veneer address) can be mapped back
        // to the call site in the image.
        {
            static FILE *mapf;
            static int map_init;
            if (!map_init) { mapf = kl_env_str("KL_X18_MAP", NULL) ? fopen(kl_env_str("KL_X18_MAP", NULL), "a") : NULL; map_init = 1; }
            if (mapf) { fprintf(mapf, "%llx %llx\n", (unsigned long long)vpc, (unsigned long long)spc); fflush(mapf); }
        }

        memcpy(out, body, k * 4);
        out += k;
        w[i] = patch;
        st->patched++;
    }

    if (pool_used) *pool_used = (size_t)((uint8_t *)out - (uint8_t *)pool);
    return 0;
}

// The load-time path: allocate a pool next to the code and emit into it. Here
// the executing addresses *are* the buffer addresses, which is the degenerate
// case of kl_x18_emit.
int kl_x18_patch(void *code, size_t size, kl_x18_stats *st) {
    memset(st, 0, sizeof *st);

    // Same reason as in kl_x18_emit: the data-word tally has to survive the
    // "nothing to veneer here" path, or a chunk of pure data reports nothing.
    unsigned want = klx_survey(code, size, &st->data_words, &st->ctr_sites);
    st->sites = want;
    unsigned veneers = (veneer_enabled() ? want : 0) +
                       (ctr_enabled() ? st->ctr_sites : 0);
    if (!veneers) return 0;
    // The TSD slot is only needed by the x18 half; a chunk whose only veneer is
    // trap 26's does not touch it. Checking it anyway would refuse to fix a
    // SIGILL because of a register the code in question never names.
    if (want && veneer_enabled() && g_slot < 0 && kl_x18_init() != 0) return -1;

    // The pool goes immediately after the code so that `b` reaches it from
    // anywhere in the range. mmap treats the address as a hint, so the result
    // is range-checked per site rather than assumed.
    size_t   cap  = ((size_t)veneers * VEN_MAX_INSN * 4 + 0xFFFF) & ~(size_t)0xFFFF;
    uint8_t *hint = (uint8_t *)(((uintptr_t)code + size + 0xFFFF) & ~(uintptr_t)0xFFFF);
    uint8_t *base = mmap(hint, cap, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) return -1;

    int rc = kl_x18_emit(code, size, (uint64_t)(uintptr_t)code,
                         base, cap, (uint64_t)(uintptr_t)base, st, NULL);

    mprotect(base, cap, PROT_READ | PROT_EXEC);
    sys_icache_invalidate(base, cap);
    return rc;
}

// ---------------------------------------------------- data inside code
//
// See kl_x18.h for what this is for and what it cannot see. The ELF64 subset
// here is local on purpose: this file is deliberately not linked against the
// runtime (tests/t_x18.c links kl_x18.c alone, so a decoder bug cannot hide
// behind a working loader), so it cannot borrow kl_image.c's declarations.
typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } klx_ehdr;
typedef struct { uint32_t sh_name, sh_type; uint64_t sh_flags, sh_addr, sh_offset,
    sh_size; uint32_t sh_link, sh_info; uint64_t sh_addralign, sh_entsize; } klx_shdr;
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx;
    uint64_t st_value, st_size; } klx_sym;
#define KLX_SHT_SYMTAB   2
#define KLX_SHT_DYNSYM   11
#define KLX_SHT_PROGBITS 1
#define KLX_SHF_EXECINSTR 0x4
#define KLX_STT_OBJECT   1

static int range_cmp(const void *a, const void *b) {
    const klx_range *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start ? 1 : 0;
}

unsigned kl_x18_data_ranges(const void *elf, size_t size, klx_range *out, unsigned max) {
    if (!elf || size < sizeof(klx_ehdr) || !out || !max) return 0;
    const uint8_t *f = elf;
    const klx_ehdr *eh = (const klx_ehdr *)f;
    if (memcmp(eh->e_ident, "\177ELF", 4) != 0) return 0;
    if (!eh->e_shoff || !eh->e_shnum) return 0;
    if (eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(klx_shdr) > size) return 0;
    const klx_shdr *sh = (const klx_shdr *)(f + eh->e_shoff);

    unsigned n = 0, dropped = 0;
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != KLX_SHT_SYMTAB && sh[i].sh_type != KLX_SHT_DYNSYM) continue;
        if (!sh[i].sh_entsize || sh[i].sh_offset + sh[i].sh_size > size) continue;
        uint64_t count = sh[i].sh_size / sh[i].sh_entsize;
        for (uint64_t k = 0; k < count; k++) {
            const klx_sym *sym = (const klx_sym *)(f + sh[i].sh_offset + k * sh[i].sh_entsize);
            // A zero-size object tells us nothing about where it ends, and
            // SHN_UNDEF has no address at all.
            if ((sym->st_info & 0xf) != KLX_STT_OBJECT || !sym->st_size || !sym->st_shndx)
                continue;
            if (sym->st_shndx >= eh->e_shnum) continue;
            const klx_shdr *owner = &sh[sym->st_shndx];
            if (owner->sh_type != KLX_SHT_PROGBITS ||
                !(owner->sh_flags & KLX_SHF_EXECINSTR)) continue;   // ordinary data, already skipped
            if (n >= max) { dropped++; continue; }
            out[n].start = sym->st_value & ~(uint64_t)3;
            out[n].end   = (sym->st_value + sym->st_size + 3) & ~(uint64_t)3;
            n++;
        }
    }
    if (dropped)
        fprintf(stderr, "  [klepton] x18: %u data-in-code ranges beyond the %u-entry "
                        "limit were DROPPED — those words will be treated as code\n",
                dropped, max);
    if (!n) return 0;

    qsort(out, n, sizeof *out, range_cmp);
    unsigned m = 0;                                    // merge overlaps/adjacency
    for (unsigned i = 1; i < n; i++) {
        if (out[i].start <= out[m].end) {
            if (out[i].end > out[m].end) out[m].end = out[i].end;
        } else {
            out[++m] = out[i];
        }
    }
    return m + 1;
}

int kl_x18_next_code(uint64_t sec_va, size_t sec_size,
                     const klx_range *skip, unsigned nskip,
                     uint64_t *cursor, uint64_t *out_va, size_t *out_size) {
    uint64_t end = sec_va + sec_size;
    uint64_t p = *cursor;
    if (p < sec_va) p = sec_va;
    while (p < end) {
        const klx_range *hit = NULL;                   // first range still ahead of p
        for (unsigned i = 0; i < nskip; i++) {         // sorted and merged, so the
            if (skip[i].end <= p) continue;            // first overlap is the nearest
            if (skip[i].start >= end) break;
            hit = &skip[i];
            break;
        }
        if (!hit) { *out_va = p; *out_size = (size_t)(end - p); *cursor = end; return 1; }
        if (hit->start > p) {
            *out_va = p;
            *out_size = (size_t)(hit->start - p);
            *cursor = hit->end < end ? hit->end : end;
            return 1;
        }
        p = hit->end;                                  // p is inside the range: step over it
    }
    *cursor = end;
    return 0;
}

void kl_x18_report(FILE *f) {
    if (!g_refused_total) return;
    fprintf(f, "  [klepton] x18: %u sites refused (still exposed to trap 0):\n",
            g_refused_total);
    for (unsigned i = 0; i < g_nrefused_kinds; i++)
        fprintf(f, "    %08x  x%u\n", g_refused[i].word, g_refused[i].n);
    if (g_refused_total > g_nrefused_kinds)
        fprintf(f, "    (encodings beyond the first %u kinds are counted, not listed)\n",
                REFUSE_KINDS);
}
