// A64 decoder for x18 substitution. See kl_x18.h for why this exists.
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
#include <mach/mach.h>
#include "kl_x18.h"
#include "kl_env.h"

// Which guest ABI the decoder serves (kl_x18.h). A plain global: it is set once,
// before the first decode, and every pass over an image has to agree on it.
static int g_abi = KLX_ABI_ANDROID;
void kl_x18_set_abi(int abi) { g_abi = abi; }
int  kl_x18_abi(void)        { return g_abi; }

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
    if (((w >> 24) & 0xff) == 0xd4) {          // svc / brk / hlt
        // `svc #0` and nothing else: it is the only immediate a Linux toolchain
        // emits, and a different one is more likely a constant than a syscall.
        if (g_abi == KLX_ABI_LINUX && w == 0xD4000001u) o->sysreg = KLX_SYS_SVC;
        return 1;
    }
    if (((w >> 22) & 0x3ff) == 0x354) {        // system: mrs / msr / hints / barriers
        // `mrs Xt, CTR_EL0` is S3_3_C0_C0_1, i.e. 0xD53B0020 | Rt, and
        // reading it from EL0 is an ILLEGAL INSTRUCTION on Darwin — measured on
        // macOS as well as visionOS. It is flagged here rather than refused
        // because the veneer answers it; note the flag is orthogonal to the x18
        // fields, so `mrs x18, CTR_EL0` is both and the emitter serves both.
        if ((w & 0xFFFFFFE0u) == 0xD53B0020u) o->sysreg = KLX_SYS_CTR_EL0;
        // TPIDR_EL0 is S3_3_C13_C0_2. Under the Linux ABI the guest's thread
        // pointer is not a register at all (see kl_x18.h), so both directions
        // are sites. Like CTR_EL0 the flag is orthogonal to the x18 field.
        if (g_abi == KLX_ABI_LINUX) {
            if ((w & 0xFFFFFFE0u) == 0xD53BD040u) o->sysreg = KLX_SYS_TP_READ;
            if ((w & 0xFFFFFFE0u) == 0xD51BD040u) o->sysreg = KLX_SYS_TP_WRITE;
        }
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
        // Compare-and-swap lives in this group too (o1 set, Rt2 all ones) and
        // shares none of the roles below: Rs is the expected value AND receives
        // what was in memory, Rt is only read, whatever L says — there it is the
        // acquire bit. Treating it as an exclusive load made Rt write-only, so a
        // veneer never loaded the shadow slot, and missed Rs altogether.
        if (((w >> 21) & 1) && ((w >> 10) & 31) == 31) {
            addfld(o, w, KLX_RN, KLX_R);
            addfld(o, w, KLX_RM, KLX_R | KLX_W);
            addfld(o, w, KLX_RD, KLX_R);
            if (!o2) {
                // CASP: bit 31 clear, and each field is a pair (see KLX_HZ_PAIR).
                // The implied second registers are operands like any other, so a
                // veneer must not pick them as scratch.
                if (w >> 31) return 0;
                unsigned rs = (w >> 16) & 31, rt = w & 31;
                if ((rs | rt) & 1) return 0;                // unallocated
                o->gp_used |= (1u << (rs + 1)) | (1u << (rt + 1));
                o->hazard = KLX_HZ_PAIR;
            }
            return 1;
        }
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
            // opc == 11 is PRFM (literal), where bits[4:0] name a prefetch
            // operation rather than a register — the same field-is-not-Rt case
            // as PRFM/PRFUM below. prfop 18 is `pstl2keep`, so treating it as a
            // register is a phantom x18 site in ordinary code: 4 of them in
            // this corpus's libcrypto.so, each of which the loader would patch
            // a branch over.
            if (((w >> 30) & 3) != 3) addfld(o, w, KLX_RD, KLX_W);
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
// valid. That is what makes the pass possible IN PLACE, with no offline
// re-layout of the image.
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
static int g_lx_slots;      // KLX_TP_SLOT and KLX_SVC_SLOT are ours too
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
// runtime, which is what the key was going to cost anyway — `build/m_boot`
// already claims it in its first statement, and this simply makes it not have
// to.
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
    // that — but a veneer indexes the TSD array on every guest x18
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
    // The Linux ABI's two slots, while everything below them is still held: the
    // next two keys issued are 501 and 502 or they are already someone else's.
    // Not fatal here — an Android guest never looks at them — but recorded, and
    // kl_x18_init_linux() is what a Linux-ABI load asks.
    if (got) {
        pthread_key_t k1 = 0, k2 = 0;
        int r1 = pthread_key_create(&k1, NULL), r2 = pthread_key_create(&k2, NULL);
        if (!r1 && !r2 && k1 == KLX_TP_SLOT && k2 == KLX_SVC_SLOT) {
            g_lx_slots = 1;
        } else {
            if (!r1) pthread_key_delete(k1);
            if (!r2) pthread_key_delete(k2);
        }
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

int kl_x18_init_linux(void) {
    if (kl_x18_init() != 0) return -1;
    if (!g_lx_slots) {
        fprintf(stderr, "  [klepton] linux abi: TSD keys %d/%d could not be claimed "
                        "next to %d — something else holds them in this process\n",
                KLX_TP_SLOT, KLX_SVC_SLOT, KLX_TSD_SLOT);
        return -1;
    }
    // Same proof as for the x18 slot, and for the same reason: the veneers read
    // these inline.
    uint64_t tp;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    void *was = pthread_getspecific(KLX_TP_SLOT);
    pthread_setspecific(KLX_TP_SLOT, (void *)(uintptr_t)0x5A5A5A5AU);
    int ok = ((void **)(uintptr_t)tp)[KLX_TP_SLOT] == (void *)(uintptr_t)0x5A5A5A5AU;
    pthread_setspecific(KLX_TP_SLOT, was);
    if (!ok) {
        fprintf(stderr, "  [klepton] linux abi: TSD key %d is not at tsd[%d] on this OS\n",
                KLX_TP_SLOT, KLX_TP_SLOT);
        return -1;
    }
    return 0;
}

// ---------- second detector: is this word's NEIGHBOURHOOD code? ------------
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
// A missed real site leaves the guest reading Darwin's reserved x18 and must
// not be silent, so every refusal is counted and `make check`'s totals gate it:
// they
// are exact numbers, and this rule must not move them.
//
// A WORD OF ZERO IS NOT EVIDENCE. It is what a linker leaves in the GAP between
// functions, and a gap is adjacent to code by definition — so counting it makes
// the window vote on the wrong side of a boundary it cannot see.
// libunity.so+0x3f2118 is a `-fstack-protector` prologue three words past
// thirteen zero words of padding: counting zeros refuses it, and a REFUSED TLS
// SITE puts back the register the rewrite exists to fix (see rewrite_tls in
// kl_image.c). It presents as a SIGSEGV in the guest on the lifecycle path
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
//
// ...and, for ONE target, one adjacent pair is not a table either. Robo
// Recall: Unplugged (x1906871599408213, UE 4.21) keeps two-entry literal pools
// in .text right after a function's last branch — GCC, the NDK that built its
// PhysX — and two floats or two words of a byte table there read as two
// "unallocated" words to this test: 0x42c80000 0x42c80000 (100.0f twice),
// 0x05040706 0x01000302, 0x40ea0ea1 0x447fe000, all measured in its libUE4.so.
// Six real x18 sites sat within 16 words of such a pair and were left raw:
// `ldr x18, [x0]` at the top of Gu::SweepGeomGeom<CapsuleV, CapsuleV> wrote the
// REAL x18 while the two reads after it, outside the window, read the shadow
// slot — a stale value, a fault at 0x10 in a capsule sweep a minute into play
// (community fix 40, 2026-10-14; the owner confirmed the fix on a headset).
//
// It is NOT safe as a general rule, which is why it is scoped. Measured over
// the 557 distinct guest libraries on this machine, a lone adjacent pair flips
// 18 refused words to "code": 15 are real sites of the same pool shape (Tales
// from the Galaxy's Edge's PhysX, Tetris Effect, Wwise, Resonance Audio), but
// three are one word of BoringSSL's P-256 table — Beat Saber 1.40's
// libcrypto.so+0x17e800, libEOSSDK.so+0x12c3800, libUnreal.so+0x88c2800 — whose
// ±16-word window holds exactly one adjacent pair. Patching that word is the
// `bad_record_mac` failure this test exists to prevent. So the pair rule is on
// only where it was verified: when KLEPTON_TARGET names that target — AVP Play
// and visionos/run.sh export it to the build, so klepton-ld's translation sees
// it; build_run_host.sh does not, so a host run says KL_X18_PAIR_POOL=1 — or
// when KL_X18_PAIR_POOL=1 says so; KL_X18_PAIR_POOL=0 turns it off there too.
// Every other target keeps the window test exactly as it was.
static int klx_pair_pool_on(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *t = kl_env_str("KLEPTON_TARGET", NULL);
        cached = kl_env_on("KL_X18_PAIR_POOL", t && strcmp(t, "x1906871599408213") == 0);
    }
    return cached;
}

static int klx_looks_like_data(const uint32_t *w, size_t n, size_t i) {
    size_t lo = i > KLX_WIN_HALF ? i - KLX_WIN_HALF : 0;
    size_t hi = i + KLX_WIN_HALF + 1 < n ? i + KLX_WIN_HALF + 1 : n;
    unsigned bad = 0;
    if (!klx_pair_pool_on()) {
        for (size_t k = lo; k < hi; k++)
            if (klx_unallocated(w[k]) && ++bad >= KLX_WIN_LIMIT) return 1;
        return 0;
    }
    // Counted over the whole window rather than stopping at the limit, so the
    // pair can be told apart.
    size_t first = 0, last = 0;
    for (size_t k = lo; k < hi; k++)
        if (klx_unallocated(w[k])) { if (!bad) first = k; last = k; bad++; }
    if (bad < KLX_WIN_LIMIT) return 0;
    if (bad == 2 && last == first + 1) return 0;   // a two-entry literal pool
    return 1;
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
// `ctr` is the CTR_EL0 population and is counted APART: the x18 totals are exact
// numbers `make check` gates on, and a second population sharing them would
// move a number that is meant to be stable. The data-word test applies to both,
// for the same reason — 0xd53b0029 is a perfectly ordinary word to find in a
// constant table.
//
// `tp` and `svc` are the Linux ABI's populations (always zero for an Android
// image, whose decoder never flags them), apart again for the same reason.
static unsigned klx_survey(const void *code, size_t size, unsigned *data,
                           unsigned *ctr, unsigned *tp, unsigned *svc) {
    const uint32_t *w = (const uint32_t *)code;
    size_t n = size / 4;
    unsigned c = 0;
    if (data) *data = 0;
    if (ctr) *ctr = 0;
    if (tp)  *tp = 0;
    if (svc) *svc = 0;
    for (size_t i = 0; i < n; i++) {
        klx_info in;
        klx_decode(w[i], &in);
        if (!in.nfields && !in.sysreg) continue;
        if (klx_looks_like_data(w, n, i)) { if (data && in.nfields) (*data)++; continue; }
        if (in.nfields) c++;
        if (in.sysreg == KLX_SYS_CTR_EL0 && ctr) (*ctr)++;
        if ((in.sysreg == KLX_SYS_TP_READ || in.sysreg == KLX_SYS_TP_WRITE) && tp) (*tp)++;
        if (in.sysreg == KLX_SYS_SVC && svc) (*svc)++;
    }
    return c;
}

unsigned kl_x18_count(const void *code, size_t size) {
    unsigned ctr = 0, tp = 0, svc = 0;
    return klx_survey(code, size, NULL, &ctr, &tp, &svc) + ctr + tp + svc;
}

// `br x18` alone. Same data-word test as the survey, for the same reason:
// 0xD61F0240 is an ordinary word to find in a constant table, and a pool sized
// off a table entry is a pool sized off nothing.
unsigned kl_x18_count_termbr(const void *code, size_t size) {
    const uint32_t *w = (const uint32_t *)code;
    size_t n = size / 4;
    unsigned c = 0;
    for (size_t i = 0; i < n; i++)
        if (w[i] == 0xD61F0240u && !klx_looks_like_data(w, n, i)) c++;
    return c;
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
static uint32_t enc_push(unsigned t, unsigned t2) {           // stp xT, xT2, [sp, #-16]!
    return 0xA9BF0000u | (t2 << 10) | (31u << 5) | t;
}
static uint32_t enc_pop(unsigned t, unsigned t2) {            // ldp xT, xT2, [sp], #16
    return 0xA8C10000u | (t2 << 10) | (31u << 5) | t;
}
static uint32_t enc_blr(unsigned n) { return 0xD63F0000u | (n << 5); }
static uint32_t enc_mov(unsigned d, unsigned m) {             // mov xD, xM
    return 0xAA0003E0u | (m << 16) | d;
}
static int enc_bl(uint64_t from, uint64_t to, uint32_t *out) {
    int64_t d = (int64_t)to - (int64_t)from;
    if ((d & 3) || d < -(1LL << 27) || d >= (1LL << 27)) return 0;
    *out = 0x94000000u | (uint32_t)((d >> 2) & 0x03ffffffu);
    return 1;
}
// `adr xT, target`, rebuilt where it no longer sits: adrp to the page, then add
// the low twelve bits. Two instructions for one, which is why it needs a veneer
// at all — adr only reaches 1 MB and the pool may be further than that.
static int enc_addr(unsigned t, uint64_t pc, uint64_t target, uint32_t *out) {
    int64_t d = ((int64_t)(target & ~0xFFFULL) - (int64_t)(pc & ~0xFFFULL)) >> 12;
    if (d < -(1LL << 20) || d >= (1LL << 20)) return 0;
    uint32_t v = (uint32_t)((uint64_t)d & 0x1fffff);
    out[0] = 0x90000000u | ((v & 3) << 29) | ((v >> 2) << 5) | t;
    out[1] = 0x91000000u | ((uint32_t)(target & 0xFFF) << 10) | (t << 5) | t;
    return 1;
}
// cbz xT, #<insns> — a 64-bit compare-and-branch, offset in INSTRUCTIONS and
// signed. Only ever emitted with a tiny negative offset here, so the imm19
// range is not worth checking.
static uint32_t enc_cbz(unsigned t, int32_t insns) {
    return 0xB4000000u | (((uint32_t)insns & 0x7FFFFu) << 5) | t;
}
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

// ---------- pc-relative WRITES: adr / ldr-literal ----------
//
// These two were refused from the beginning because `klx_substitute` alone
// cannot move them: `adr` reaches +/-1 MB and the literal form the same, while
// the veneer pool sits up to 128 MB away. `reenc_adrp` rescues `adrp`, and
// there was no equivalent for these.
//
// **A refused WRITE is not a site left merely broken — it is a site that
// corrupts every later READ.** The unpatched instruction writes Darwin's real
// x18 while the shadow in tsd[KLX_TSD_SLOT] keeps whatever the last veneered
// write put there, and the next veneered read hands the guest that stale value.
// FNAF: Help Wanted 1 died exactly here, in `vorbis_synthesis_blockin`:
//
//     6ac82f8: cset w18, ls        veneered   -> shadow := 1
//     6ac8358: adr  x18, 0x6ac8810 REFUSED    -> real x18 := &pool, shadow still 1
//     6ac8364: ldr  q4, [x18]      veneered   -> loads from address 0x1  ** SIGSEGV **
//
// on four audio pool threads at once. Nothing in the log named it, because the
// veneer did precisely what it was told.
//
// The fix is to stop trying to relocate the instruction and instead compute the
// address it WOULD have produced. Both targets are known at emit time and both
// are reachable by `adrp` (+/-4 GB), so:
//
//     adr x18, T   ->   adrp b, page(T) ; add b, b, #off(T)  ; str b, [shadow]
//     ldr w18, T   ->   adrp b, page(T) ; ldr wb, [b,#off(T)] ; str b, [shadow]
//
// `off(T)` is `T & 0xfff`, always in range for the scaled unsigned offset
// (< 4096) and preserving the literal's natural alignment, since `adrp` clears
// exactly those low twelve bits. Seven instructions either way, inside
// KLX_VEN_MAX_INSN.
//
// This is sound only because the whole ELF image is shifted UNIFORMLY into
// __TEXT (see the image-shift line klepton-ld prints): the byte a pc-relative
// operand named in the ELF is the same byte at the same relative distance in
// the Mach-O, so `spc`/`vpc` arithmetic on the laid-out addresses lands on it.
static uint32_t enc_adrp_to(unsigned t, uint64_t from, uint64_t target, int *ok) {
    int64_t d = ((int64_t)(target & ~0xFFFULL) - (int64_t)(from & ~0xFFFULL)) >> 12;
    if (d < -(1LL << 20) || d >= (1LL << 20)) { *ok = 0; return 0; }
    *ok = 1;
    uint32_t v = (uint32_t)((uint64_t)d & 0x1fffff);
    return 0x90000000u | ((v & 3) << 29) | ((v >> 2) << 5) | t;
}
static uint32_t enc_add_imm(unsigned d, unsigned n, unsigned imm12) {   // add xD, xN, #imm
    return 0x91000000u | ((imm12 & 0xfff) << 10) | (n << 5) | d;
}
static uint32_t enc_ldr_w(unsigned t, unsigned n, unsigned off) {       // ldr wT, [xN, #off]
    return 0xB9400000u | ((off / 4) << 10) | (n << 5) | t;
}
static uint32_t enc_ldrsw(unsigned t, unsigned n, unsigned off) {       // ldrsw xT, [xN, #off]
    return 0xB9800000u | ((off / 4) << 10) | (n << 5) | t;
}

// `adr Xd, label`: imm21 is immhi[23:5]:immlo[30:29], a BYTE displacement.
static uint64_t adr_target(uint32_t w, uint64_t pc) {
    int64_t imm = (int64_t)((((uint64_t)(w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3));
    return pc + (uint64_t)((imm << 43) >> 43);
}
// `ldr Wt/Xt, label` / `ldrsw Xt, label`: imm19 at [23:5], scaled by 4.
static uint64_t lit_target(uint32_t w, uint64_t pc) {
    int64_t imm = (int64_t)((w >> 5) & 0x7ffff);
    return pc + (uint64_t)(((imm << 45) >> 45) << 2);
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

// The site-to-veneer distance, as its own question. A refusal for reach is a
// property of the POOL and so of the whole library; a refusal for encoding is a
// property of one instruction. Telling them apart is the difference between
// "move the pool" and "teach the decoder", and they were one counter.
static int out_of_reach(uint64_t site, uint64_t pool) {
    int64_t d = (int64_t)pool - (int64_t)site;
    return d < -(1LL << 27) || d >= (1LL << 27);
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

// CTR_EL0's own knob, deliberately independent of KL_X18: the two rewrites fix
// different registers and an A/B on one must not silently move the other.
// KL_CTR=0 leaves `mrs Xt, CTR_EL0` alone, which is a SIGILL wherever it runs.
static int ctr_enabled(void) {
    static int cached = -1;
    if (cached < 0) cached = kl_env_on("KL_CTR", 1);
    return cached;
}

// ...and what the veneer answers. See KLX_CTR_EL0_VALUE in kl_x18.h for why
// this value and not a fabricated IDC=1/DIC=1 one.
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

// ======================== the `br x18` jump-table rewrite =======================
//
// WHY THIS EXISTS. The guarded veneer below still branches THROUGH x18, and the
// kernel zeroes x18 on any exception return, so an interrupt taken with the `br`
// at the head of the pipeline branches to address 0. On visionOS that is an
// unblockable SIGKILL (CODESIGNING / "Invalid Page") with no signal delivered
// and nothing written. Arizona Sunshine 2 died on it in three runs of three, on
// FMOD's 32-way channel-mixer dispatch, which executes on every mix buffer.
//
// WHY NOTHING CHEAPER WORKS. The branch register has to still hold the target
// when the branch issues, and control never comes back, so it is clobbered at
// the destination — it must therefore be a register the destination does not
// need. At a jump-table target nothing is dead. Disassembled, at every real
// site: x16/x17 are set two instructions before the branch and read at the
// targets, and the dispatch's own temporaries are REUSED before the branch —
// libfmod+0x85628 ends `add x18, x14, x11 / mov x14, x12 / br x18`, so even x14,
// which the dispatch itself computes, carries a live value across it. x30 is
// dead only in non-leaf functions and leaf-ness is not decidable in a stripped
// image. So the register has to be one we RESTORE, and the only place a restore
// can run is code of our own at the destination.
//
// THE REWRITE. Recognise the dispatch, enumerate its targets out of the table,
// and give every target a thunk that restores the scratch and branches on with
// a direct `b` — no register at all in the final branch. Repointing the table's
// own 32-bit offsets at the thunks means the guest's arithmetic is untouched:
// `table + entry` still computes, and now lands on the thunk.
//
//      site:   b <dispatch>
//      dispatch: stp x16, x17, [sp, #-16]      spill into the red zone
//                mrs x16, tpidrro_el0
//                ldr x16, [x16, #slot*8]       the shadow x18 — now a thunk
//                br  x16                       x16 is NOT zeroed by the kernel
//      thunk_k:  ldp x16, x17, [sp, #-16]      restore before the guest sees it
//                b   target_k                  direct: no register live across
//
// There is no window left to lose: nothing the kernel touches is live across a
// branch. An interrupt anywhere in this sequence restores x16/x17 from the
// architectural state like any other register.
//
// WHY THE SPILL IS ON EVERY `br x18` VENEER, converted or not. A table this pass
// has repointed now yields thunk addresses to ANY dispatch that reads it, and a
// site that failed recognition still branches through the guarded veneer. Give
// every `br x18` veneer the same `stp x16, x17, [sp, #-16]` prologue and both
// cases are correct: a thunk's `ldp` restores the values that veneer just
// stored, and a raw target gets x16/x17 untouched because the fallback veneer
// only STORES them and never writes them. That one instruction is also what
// makes an alternate entry into the middle of a dispatch harmless, so no
// branch-target analysis is needed here.
//
// WHY THE RECOGNITION IS SAFE WITHOUT PROVING THE DERIVATION. The table base is
// formed by an `adrp`/`add` pair that is commonly hoisted out of the loop the
// dispatch sits in, so finding it is a backward scan and a backward scan is not
// a dataflow proof. The ANSWER is checked instead: the computed base must
// resolve to non-executable data in this image, and all K+1 entries must be
// 4-byte-aligned offsets landing inside the very chunk being veneered. A wrong
// base does not produce thirty-two of those. Anything that fails keeps the
// guarded veneer, which is exactly what it had before this pass existed.
typedef struct {
    size_t   idx;        // word index of the `br x18`
    uint64_t table_va;
    unsigned nent;
    uint64_t thunks_va;  // filled in at emit; 0 until the thunks exist
} klx_jt;

static int64_t klx_sxt(uint64_t v, unsigned bits) {
    return (int64_t)(v << (64 - bits)) >> (64 - bits);
}

// Recognise the dispatch idiom ending in the `br x18` at word `i`, scanning back
// at most KLX_JT_SCAN words for each part, and fill `r` if every check below
// holds. Returns 0 — keep the guarded veneer — on any doubt.
static int klx_jt_recognise(const uint32_t *w, size_t n, size_t i,
                            uint64_t code_va,
                            const kl_x18_image *im, klx_jt *r) {
    if (!im || !im->data_at) return 0;

    size_t lo = i > KLX_JT_SCAN ? i - KLX_JT_SCAN : 0;
    unsigned xA = 0, xB = 0, xN = 0, xBase = 0;
    size_t jadd = 0, jldr = 0, jcmp = 0, jbase = 0, jadrp = 0;
    int got;

    // add x18, xA, xB — 64-bit shifted-register form, shift 0.
    for (got = 0, jadd = i; jadd-- > lo; ) {
        uint32_t v = w[jadd];
        if ((v & 0xFFE0FC00u) == 0x8B000000u && (v & 31u) == 18u) {
            xA = (v >> 5) & 31; xB = (v >> 16) & 31; got = 1; break;
        }
        // Any earlier write of x18 means this is not the producer.
        klx_info in;
        if (klx_decode(v, &in) && (in.roles & KLX_W)) return 0;
    }
    // xA == 18 is libunity's shape (`ldrsw x18,[base,idx]` / `add x18,x18,base`)
    // and is fine — the value is in the shadow slot either way. xB naming x18
    // would put the TABLE BASE in the shadow, which no compiler emits and which
    // this recogniser has no reason to model.
    if (!got || xB == 18) return 0;

    // ldrsw xA, [xB, xN, lsl #2]
    for (got = 0, jldr = jadd; jldr-- > lo; ) {
        uint32_t v = w[jldr];
        if ((v & 0xFFE0FC00u) == 0xB8A07800u && (v & 31u) == xA &&
            ((v >> 5) & 31u) == xB) { xN = (v >> 16) & 31; got = 1; break; }
    }
    if (!got) return 0;

    // cmp wN, #K — the bound, and the only reason we know how many entries the
    // table has. Without it the index is unbounded and the rewrite would leave
    // reachable entries unrepointed.
    unsigned nent = 0;
    for (got = 0, jcmp = jldr; jcmp-- > lo; ) {
        uint32_t v = w[jcmp];
        if ((v & 0xFFC0001Fu) == 0x7100001Fu && ((v >> 5) & 31u) == xN &&
            !((v >> 22) & 1u)) { nent = ((v >> 10) & 0xFFFu) + 1; got = 1; break; }
    }
    if (!got || nent > KLX_JT_MAX_ENTRIES) return 0;

    // ...and the `b.hi` that acts on it. A `cmp` with no branch is not a bound.
    got = 0;
    for (size_t k = jcmp + 1; k <= jldr; k++)
        if ((w[k] & 0xFF00001Fu) == 0x54000008u) { got = 1; break; }
    if (!got) return 0;

    // add xB, xBase, #imm12 (shift 0), then adrp xBase, page.
    unsigned imm12 = 0;
    for (got = 0, jbase = jldr; jbase-- > lo; ) {
        uint32_t v = w[jbase];
        if ((v & 0xFF800000u) == 0x91000000u && (v & 31u) == xB &&
            !((v >> 22) & 3u)) {
            xBase = (v >> 5) & 31; imm12 = (v >> 10) & 0xFFFu; got = 1; break;
        }
    }
    if (!got) return 0;
    for (got = 0, jadrp = jbase; jadrp-- > lo; ) {
        uint32_t v = w[jadrp];
        if ((v & 0x9F000000u) == 0x90000000u && (v & 31u) == xBase) { got = 1; break; }
    }
    if (!got) return 0;

    uint32_t a = w[jadrp];
    int64_t  page = klx_sxt((((uint64_t)((a >> 5) & 0x7FFFF)) << 2) | ((a >> 29) & 3), 21);
    uint64_t apc  = code_va + (uint64_t)jadrp * 4;
    uint64_t table_va = ((apc & ~0xFFFULL) + (uint64_t)(page << 12)) + imm12;

    // The answer, checked. Non-executable data in this image, and every entry a
    // 4-byte-aligned offset landing inside the chunk being veneered.
    const int32_t *ent = (const int32_t *)im->data_at(im->ctx, table_va,
                                                      (size_t)nent * 4);
    if (!ent) return 0;
    for (unsigned e = 0; e < nent; e++) {
        uint64_t t = table_va + (uint64_t)(int64_t)ent[e];
        if (t & 3) return 0;
        if (t < code_va || t >= code_va + (uint64_t)n * 4) return 0;
    }

    r->idx = i; r->table_va = table_va; r->nent = nent; r->thunks_va = 0;
    return 1;
}

// The emitter proper. `code_va`/`pool_va` are the addresses the two buffers will
// have when the code executes, which is what every `b` and `adrp` is computed
// against; the buffers themselves may live anywhere (a file image being laid out
// offline, or the mapping itself at load time).
int kl_x18_emit(void *code, size_t size, uint64_t code_va,
                void *pool, size_t poolcap, uint64_t pool_va,
                kl_x18_stats *st, size_t *pool_used) {
    return kl_x18_emit_img(code, size, code_va, pool, poolcap, pool_va,
                           NULL, st, pool_used);
}

int kl_x18_emit_img(void *code, size_t size, uint64_t code_va,
                    void *pool, size_t poolcap, uint64_t pool_va,
                    const kl_x18_image *image,
                    kl_x18_stats *st, size_t *pool_used) {
    memset(st, 0, sizeof *st);

    uint32_t *w = (uint32_t *)code;
    size_t    n = size / 4;

    unsigned want = klx_survey(code, size, &st->data_words, &st->ctr_sites,
                               &st->tp_sites, &st->svc_sites);
    st->sites = want;
    if (pool_used) *pool_used = 0;
    int do_x18 = want && veneer_enabled();
    int do_ctr = st->ctr_sites && ctr_enabled();
    // No knob for the Linux ABI's sites: with them left alone the guest reads a
    // register the kernel clobbers and traps into Darwin's syscall table with
    // Linux numbers, so there is no control run worth having.
    int do_lx  = st->tp_sites || st->svc_sites;
    if (!do_x18 && !do_ctr && !do_lx) return 0;

    // Pass A: recognise the `br x18` jump-table dispatches BEFORE anything is
    // patched. This has to happen first and it is not an optimisation: the
    // recogniser reads the instructions above the branch — the `cmp`, the
    // `ldrsw`, the `add`, the `adrp` — and every one of those is itself an x18
    // site that the loop below overwrites with a `b` on its way past. Recognise
    // after patching and the idiom is already gone.
    klx_jt jt[KLX_JT_MAX_SITES];
    unsigned njt = 0;
    if (do_x18 && image && image->data_at) {
        for (size_t i = 0; i < n && njt < KLX_JT_MAX_SITES; i++) {
            if (w[i] != 0xD61F0240u) continue;
            if (klx_looks_like_data(w, n, i)) continue;
            if (klx_jt_recognise(w, n, i, code_va, image, &jt[njt])) njt++;
        }
    }

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
        int lx_site  = in.sysreg >= KLX_SYS_TP_READ;
        int x18_site = in.nfields && do_x18 && !ctr_site && !lx_site;
        if (!ctr_site && !x18_site && !lx_site) continue;

        uint64_t spc = code_va + (uint64_t)i * 4;
        uint64_t vpc = pool_va + (uint64_t)((uint8_t *)out - (uint8_t *)pool);
        uint32_t word = w[i];

        // The Linux ABI's three shapes (kl_x18.h). None of them executes the
        // original instruction, so there is nothing pc-relative to rebuild.
        //
        //   mrs Xt, TPIDR_EL0      mrs  Xt, tpidrro_el0
        //                          ldr  Xt, [Xt, #TP*8]
        //
        //   msr TPIDR_EL0, Xt      stp  a, b, [sp, #-16]
        //                          mrs  a, tpidrro_el0
        //                          str  Xt, [a, #TP*8]
        //                          ldp  a, b, [sp, #-16]
        //
        //   svc #0                 stp  x16, x30, [sp, #-16]!
        //                          mrs  x16, tpidrro_el0
        //                          ldr  x16, [x16, #SVC*8]
        //                          blr  x16
        //                          ldp  x16, x30, [sp], #16
        //
        // The read needs no scratch because its destination is free to hold the
        // TSD base on the way. When Xt is x18 the value goes to or comes from
        // the shadow slot instead, through `b`, as every other x18 site does.
        //
        // The svc veneer is the one shape here that MOVES sp, and it has to: it
        // makes a call, and the callee owns everything below sp. The kernel
        // preserves every register but x0 across a syscall and compilers rely on
        // it — glibc's inline syscalls declare no clobbers — so x16 and x30 are
        // put back here and the entry it calls preserves the rest.
        if (lx_site) {
            uint32_t body[VEN_MAX_INSN], patch;
            unsigned k = 0, rt = word & 31u, a, b;
            int ok = rt != 31 || in.sysreg == KLX_SYS_SVC;   // xzr as Xt: never emitted
            pick_scratch(in.gp_used, &a, &b);
            if (in.sysreg == KLX_SYS_SVC) {
                body[k++] = enc_push(16, 30);
                body[k++] = enc_mrs_tpidrro(16);
                body[k++] = enc_ldr(16, 16, (unsigned)KLX_SVC_SLOT * 8);
                body[k++] = enc_blr(16);
                body[k++] = enc_pop(16, 30);
            } else if (in.sysreg == KLX_SYS_TP_READ && !in.nfields) {
                body[k++] = enc_mrs_tpidrro(rt);
                body[k++] = enc_ldr(rt, rt, (unsigned)KLX_TP_SLOT * 8);
            } else {
                // A write, or a read into x18: one slot to the other through `b`.
                int rd = in.sysreg == KLX_SYS_TP_READ;
                body[k++] = enc_stp(a, b, -16);
                body[k++] = enc_mrs_tpidrro(a);
                if (in.nfields)
                    body[k++] = enc_ldr(b, a, (unsigned)(rd ? KLX_TP_SLOT : KLX_TSD_SLOT) * 8);
                body[k++] = enc_str(in.nfields ? b : rt, a,
                                    (unsigned)(rd ? KLX_TSD_SLOT : KLX_TP_SLOT) * 8);
                body[k++] = enc_ldp(a, b, -16);
            }
            ok &= out + VEN_MAX_INSN <= pool_end;
            if (ok) { ok &= enc_b(vpc + k * 4, spc + 4, &body[k]); k++; }
            if (ok) ok &= enc_b(spc, vpc, &patch);
            if (!ok) { note_refusal(word); st->lx_refused++; continue; }
            memcpy(out, body, k * 4);
            out += k;
            w[i] = patch;
            if (in.sysreg == KLX_SYS_SVC) st->svc_patched++; else st->tp_patched++;
            continue;
        }

        // `mrs Xt, CTR_EL0`. No spill, no TSD and no scratch — the
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

        // `br x18` — the one terminal case, and the only site shape served
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
        // theoretical one, and it is left standing deliberately: the
        // alternative is refusing the site, which is not a smaller risk but a
        // CERTAIN branch through a zeroed x18 every time it executes. Beat Saber
        // 1.6.0 reaches one of these during scene load and dies on it every run.
        //
        // It is also self-identifying if it ever fires: a fault at address 0 with
        // pc 0 and a guest frame above it is this and essentially nothing else,
        // because a jump-table target is never address 0.
        //
        // The `ldr`/`br` window is NARROWED by a load-use guard:
        //
        //     V+0   mrs  x18, tpidrro_el0
        //     V+4   ldr  x18, [x18, #slot]
        //     V+8   cbz  x18, V+0
        //     V+12  br   x18
        //
        // The guard does not remove the fatal slot — nothing can, for the reason
        // written above — but it makes that slot far narrower, and the mechanism
        // is worth stating because it is the whole argument for the instruction.
        //
        // An interrupt is taken against the OLDEST un-retired instruction, so a
        // slot's exposure is the TIME the pipeline spends with that instruction
        // at the head, not one cycle. Without the guard, `br x18` sits directly
        // on the result of a TSD load and stalls there for the load's whole
        // load-use latency — the fatal slot is the widest one in the veneer.
        // The `cbz` absorbs that stall: it is the instruction that waits for
        // x18, and by the time the `br` issues the value is already in hand, so
        // the `br` retires immediately. The stall moves from the fatal slot to a
        // RECOVERABLE one — an interrupt landing on the `cbz` zeroes x18, the
        // cbz sees the zero and jumps back to the `mrs`, and the veneer simply
        // reloads. No signal, no handler, no cost on the path that does not
        // race.
        //
        // The difference is measurable. Arizona Sunshine 2's FMOD mixer
        // dispatches through a 32-way jump table (libfmod.so+0x85628) on every
        // mix buffer. On the macOS host the unguarded veneer collected 100+
        // `mrs`/`ldr`-window faults in ten minutes and no `br`-window death; on
        // the headset — a real-time audio thread woken on a fixed period, so its
        // preemptions are not randomly phased — the `br` window killed the
        // title about 38 seconds in, two runs out of two.
        //
        // If x18 reads back zero because the TSD slot GENUINELY holds zero, the
        // guard spins instead of branching to 0. That is a guest asking to
        // branch to address 0, which is a guest bug either way; a spin is at
        // least a live process with a stack to look at, where a `br` to 0 on
        // visionOS is an unblockable SIGKILL (CODESIGNING / "Invalid Page"),
        // with no signal delivered and no handler able to see it. It has never
        // been observed.
        if (in.hazard == KLX_HZ_TERMBR) {
            // The jump-table rewrite, when pass A recognised this
            // site. See the block comment above klx_jt_recognise for the whole
            // argument; here is only the emission.
            klx_jt *r = NULL;
            for (unsigned q = 0; q < njt; q++) if (jt[q].idx == i) { r = &jt[q]; break; }
            if (r) {
                // A table already repointed by an earlier site in this chunk is
                // reused rather than rewritten twice — the thunks are identical,
                // and a second pass over the offsets would double-apply them.
                klx_jt *share = NULL;
                for (unsigned q = 0; q < njt; q++)
                    if (&jt[q] != r && jt[q].thunks_va && jt[q].table_va == r->table_va &&
                        jt[q].nent >= r->nent) { share = &jt[q]; break; }

                unsigned need = 4 + (share ? 0 : 2 * r->nent);
                int32_t *ent = (int32_t *)image->data_at(image->ctx, r->table_va,
                                                         (size_t)r->nent * 4);
                if (ent && out + need <= pool_end) {
                    uint64_t thunks_va = share ? share->thunks_va : vpc;
                    uint32_t *tout = out;
                    int ok = 1;

                    // Thunks first, so the dispatch veneer's own address is
                    // known only after them — everything is computed against
                    // `pool_va`, so the order is free either way.
                    if (!share) {
                        for (unsigned e = 0; e < r->nent && ok; e++) {
                            uint64_t tva = thunks_va + (uint64_t)e * 8;
                            uint64_t tgt = r->table_va + (uint64_t)(int64_t)ent[e];
                            tout[e * 2] = enc_ldp(16, 17, -16);
                            ok &= enc_b(tva + 4, tgt, &tout[e * 2 + 1]);
                        }
                        // ...and the table only after every thunk encoded. A
                        // half-repointed table is an image that runs wrong
                        // rather than one that refuses.
                        if (ok)
                            for (unsigned e = 0; e < r->nent; e++) {
                                int64_t d = (int64_t)(thunks_va + (uint64_t)e * 8)
                                          - (int64_t)r->table_va;
                                if (d < INT32_MIN || d > INT32_MAX) { ok = 0; break; }
                            }
                    }

                    // The dispatch sits after this site's own thunks; a site
                    // reusing another's thunks emits only the dispatch.
                    uint32_t body[4], patch;
                    uint64_t dva = share ? vpc : vpc + (uint64_t)r->nent * 8;
                    body[0] = enc_stp(16, 17, -16);
                    body[1] = enc_mrs_tpidrro(16);
                    body[2] = enc_ldr(16, 16, (unsigned)KLX_TSD_SLOT * 8);
                    body[3] = 0xD61F0200u;            // br x16
                    ok &= enc_b(spc, dva, &patch);

                    if (ok) {
                        if (!share) {
                            out += 2 * r->nent;
                            for (unsigned e = 0; e < r->nent; e++)
                                ent[e] = (int32_t)((int64_t)(thunks_va + (uint64_t)e * 8)
                                                 - (int64_t)r->table_va);
                            st->jt_thunks += r->nent;
                        }
                        memcpy(out, body, sizeof body);
                        out += 4;
                        w[i] = patch;
                        r->thunks_va = thunks_va;
                        st->patched++;
                        st->jt_sites++;
                        continue;
                    }
                }
                // Fell through: no room, or a target out of `b` reach. The
                // guarded veneer below is still correct, and the table has not
                // been touched.
            }

            if (out + VEN_MAX_INSN > pool_end) {
                note_refusal(word); st->refused++; continue;
            }
            uint32_t body[VEN_MAX_INSN], patch;
            unsigned k = 0;
            // The spill that makes a repointed table safe for an UNCONVERTED
            // site. This veneer only stores x16/x17 and never writes them, so a
            // raw target sees them unchanged; a thunk's `ldp` restores exactly
            // what was stored. See "WHY THE SPILL IS ON EVERY `br x18` VENEER".
            body[k++] = enc_stp(16, 17, -16);
            body[k++] = enc_mrs_tpidrro(18);
            body[k++] = enc_ldr(18, 18, (unsigned)KLX_TSD_SLOT * 8);
            // The load-use guard (see above): this instruction is what moves the
            // load-use stall off the `br`, and it recovers the slot it lands on.
            body[k++] = enc_cbz(18, -2);       // x18 zeroed under us -> redo the mrs
            body[k++] = word;                  // br x18, against the loaded value
            if (!enc_b(spc, vpc, &patch)) {
                note_refusal(word); st->refused++;
                if (out_of_reach(spc, vpc)) st->far_refused++;
                continue;
            }
            memcpy(out, body, k * 4);
            out += k;
            w[i] = patch;
            st->patched++;
            st->jt_fallback++;
            continue;
        }

        // CASP with x18:x19 as one of its pairs (KLX_HZ_PAIR). The scratch is a
        // PAIR p:p+1, p even, and x19 travels with it:
        //
        //   Rt is x18:x19 (the new value, only read)
        //      stp  p, p+1, [sp, #-16]
        //      mrs  p, tpidrro_el0
        //      ldr  p, [p, #slot*8]
        //      mov  p+1, x19
        //      casp Xs, Xs+1, p, p+1, [Xn]
        //      ldp  p, p+1, [sp, #-16]
        //
        //   Rs is x18:x19 (compared, then overwritten with what memory held)
        //      stp  p, p+1, [sp, #-16]
        //      mrs  p+1, tpidrro_el0
        //      ldr  p, [p+1, #slot*8]
        //      mov  p+1, x19
        //      casp p, p+1, Xt, Xt+1, [Xn]
        //      mrs  x19, tpidrro_el0        x19 is about to be overwritten, so
        //      str  p, [x19, #slot*8]       it is free to hold the TSD base
        //      mov  x19, p+1
        //      ldp  p, p+1, [sp, #-16]
        //
        // Neither touches the flags, which is what the guest tests afterwards
        // (it compares Rs against its own copy). Refused, by name, if x18 turns
        // up in more than one field or the base is x19 itself.
        if (in.hazard == KLX_HZ_PAIR) {
            unsigned rs = (word >> 16) & 31, rn = (word >> 5) & 31, p = 32;
            static const unsigned even[] = {16, 8, 10, 12, 14};
            for (unsigned c = 0; c < sizeof even / sizeof *even && p == 32; c++)
                if (!(in.gp_used & (3u << even[c]))) p = even[c];
            uint32_t body[VEN_MAX_INSN], patch;
            unsigned k = 0;
            int ok = in.nfields == 1 && p != 32 && out + VEN_MAX_INSN <= pool_end;
            if (ok && rs == 18) {
                ok &= rn != 19;
                body[k++] = enc_stp(p, p + 1, -16);
                body[k++] = enc_mrs_tpidrro(p + 1);
                body[k++] = enc_ldr(p, p + 1, (unsigned)KLX_TSD_SLOT * 8);
                body[k++] = enc_mov(p + 1, 19);
                body[k++] = klx_substitute(word, &in, p);
                body[k++] = enc_mrs_tpidrro(19);
                body[k++] = enc_str(p, 19, (unsigned)KLX_TSD_SLOT * 8);
                body[k++] = enc_mov(19, p + 1);
                body[k++] = enc_ldp(p, p + 1, -16);
            } else if (ok) {
                body[k++] = enc_stp(p, p + 1, -16);
                body[k++] = enc_mrs_tpidrro(p);
                body[k++] = enc_ldr(p, p, (unsigned)KLX_TSD_SLOT * 8);
                body[k++] = enc_mov(p + 1, 19);
                body[k++] = klx_substitute(word, &in, p);
                body[k++] = enc_ldp(p, p + 1, -16);
            }
            if (ok) { ok &= enc_b(vpc + k * 4, spc + 4, &body[k]); k++; }
            if (ok) ok &= enc_b(spc, vpc, &patch);
            if (!ok) {
                note_refusal(word); st->refused++;
                if (out_of_reach(spc, vpc)) st->far_refused++;
                continue;
            }
            memcpy(out, body, k * 4);
            out += k;
            w[i] = patch;
            st->patched++;
            continue;
        }

        // `blr x18` — a call through x18. Refused as KLX_HZ_NOFALL for as long
        // as the site had to be a `b`, because then x30 would point into the
        // veneer. It does not have to be: patch the site with `bl` and the link
        // register is the instruction after it, exactly what `blr` would have
        // left, with the return predictor kept in step as well.
        //
        //      mrs  x16, tpidrro_el0
        //      ldr  x16, [x16, #slot*8]
        //      br   x16
        //
        // x16 needs no spill: it is the intra-procedure-call scratch register,
        // which no callee may read on entry. Source 2 has 53 of these.
        if (word == 0xD63F0240u) {
            uint32_t patch;
            if (out + VEN_MAX_INSN > pool_end || !enc_bl(spc, vpc, &patch)) {
                note_refusal(word); st->refused++;
                if (out_of_reach(spc, vpc)) st->far_refused++;
                continue;
            }
            *out++ = enc_mrs_tpidrro(16);
            *out++ = enc_ldr(16, 16, (unsigned)KLX_TSD_SLOT * 8);
            *out++ = 0xD61F0200u;                       // br x16
            w[i] = patch;
            st->patched++;
            continue;
        }

        // `adr x18, label` — the address goes to the shadow slot. The one
        // pc-relative shape that cannot be re-encoded in place of itself (see
        // enc_addr), so it is rebuilt as a pair. Source 2 has 33.
        if (in.pcrel == KLX_PC_ADR && !in.hazard && in.nfields == 1 &&
            out + VEN_MAX_INSN <= pool_end) {
            unsigned a, b;
            pick_scratch(in.gp_used, &a, &b);
            int64_t imm = (int64_t)((((word >> 5) & 0x7ffff) << 2) | ((word >> 29) & 3));
            imm = (imm << 43) >> 43;
            uint32_t body[VEN_MAX_INSN], patch;
            unsigned k = 0;
            body[k++] = enc_stp(a, b, -16);
            body[k++] = enc_mrs_tpidrro(a);
            int ok = enc_addr(b, vpc + k * 4, spc + (uint64_t)imm, body + k);
            k += 2;
            body[k++] = enc_str(b, a, (unsigned)KLX_TSD_SLOT * 8);
            body[k++] = enc_ldp(a, b, -16);
            ok &= enc_b(vpc + k * 4, spc + 4, &body[k]); k++;
            ok &= enc_b(spc, vpc, &patch);
            if (!ok) {
                note_refusal(word); st->refused++;
                if (out_of_reach(spc, vpc)) st->far_refused++;
                continue;
            }
            memcpy(out, body, k * 4);
            out += k;
            w[i] = patch;
            st->patched++;
            continue;
        }

        if (out + VEN_MAX_INSN > pool_end) { note_refusal(word); st->refused++; continue; }

        if (in.hazard) { note_refusal(word); st->refused++; continue; }

        unsigned a, b;
        pick_scratch(in.gp_used, &a, &b);

        // The two pc-relative WRITES, rebuilt rather than relocated. See the
        // block comment above enc_adrp_to for why a refusal here is worse than
        // a refusal anywhere else in this loop.
        //
        // In this fork the plain `adr x18` was already rebuilt by the block
        // above (enc_addr), so what arrives here in practice is the literal
        // load; the adr arm stays for the shapes that block leaves alone.
        if (in.pcrel == KLX_PC_ADR || in.pcrel == KLX_PC_LITERAL) {
            uint32_t body[VEN_MAX_INSN], patch;
            unsigned k = 0;
            int ok = 1;
            uint64_t target = (in.pcrel == KLX_PC_ADR) ? adr_target(word, spc)
                                                       : lit_target(word, spc);
            unsigned off = (unsigned)(target & 0xfff);

            body[k++] = enc_stp(a, b, -16);
            body[k++] = enc_mrs_tpidrro(a);
            body[k]   = enc_adrp_to(b, vpc + k * 4, target, &ok); k++;
            if (in.pcrel == KLX_PC_ADR) {
                body[k++] = enc_add_imm(b, b, off);
            } else {
                switch ((word >> 30) & 3) {
                case 0:  body[k++] = enc_ldr_w(b, b, off); break;   // ldr  w18
                // The scaled offset needs the literal 8-byte aligned. Compilers
                // align 64-bit literals naturally; one that is not is refused
                // rather than loaded from the wrong address.
                case 1:  body[k++] = enc_ldr(b, b, off);                // ldr  x18
                         if (off & 7) ok = 0;
                         break;
                case 2:  body[k++] = enc_ldrsw(b, b, off); break;   // ldrsw x18
                default: ok = 0; break;                             // prfm: not a site
                }
            }
            body[k++] = enc_str(b, a, (unsigned)KLX_TSD_SLOT * 8);
            body[k++] = enc_ldp(a, b, -16);
            ok &= enc_b(vpc + k * 4, spc + 4, &body[k]); k++;
            ok &= (k <= VEN_MAX_INSN);
            ok &= enc_b(spc, vpc, &patch);
            if (!ok) {
                note_refusal(word); st->refused++;
                if (out_of_reach(spc, vpc)) st->far_refused++;
                continue;
            }
            memcpy(out, body, k * 4);
            out += k;
            w[i] = patch;
            st->patched++;
            continue;
        }

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
        if (!ok) {
            note_refusal(word); st->refused++;
            if (out_of_reach(spc, vpc)) st->far_refused++;
            continue;
        }

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

// ------------------------------- pool placement -------------------------------
//
// The patch at a site is a single `b`, so the pool has to be within +/-128 MB of
// every site in the chunk — and this used to be one `mmap` with a hint just past
// the code, on the reasoning that a hint lands where you ask.
//
// **It does not, and RE4 is where that cost a session.** A hint is only a
// starting point: Darwin returns the first free hole at or ABOVE it, and the
// address right after a guest image is not free — the image's own RW segments
// are there, and under the viewer so are ANGLE, Metal and SDL. Measured on
// libUE4 (172 MB, all 8038 of its x18 sites in a 5 MB band at the top): the
// pool landed **240 MB** above the code and `enc_b` refused every single site.
//
// The failure had no error surface at all. A refused site keeps the ORIGINAL
// instruction, so the library runs against Darwin's reserved x18 — which is
// zero after any exception return — and the guest crashes somewhere else
// entirely, intermittently, in whatever code happens to use x18 (here Oodle's
// decompressor, four calls and one thread from the load). Headless runs veneered
// fine because the space above the image was free, which is exactly why this
// read as "the viewer adds a race".
//
// This is the FALLBACK. The real placement is a reservation made with the image
// (kl_image.c), because for a big enough library in a crowded enough process no
// search can succeed — there is simply no free hole in the window. What is left
// here serves callers that reserved nothing, and it is still strictly better
// than the single hint it replaced: it checks its answer.
static uint8_t *klx_place_pool(void *code, size_t size, size_t cap) {
    const uint64_t REACH = 1ULL << 27;                  // +/-128 MB, enc_b's span
    uintptr_t lo = (uintptr_t)code, hi = lo + size;
    // Every veneer must reach every site and back, so the whole pool has to sit
    // inside the window both ends can see. A margin keeps the arithmetic off the
    // exact boundary, where a rounding error is 8038 silent refusals.
    const uint64_t MARGIN = 1u << 20;
    uintptr_t win_lo = (hi + cap + MARGIN > REACH) ? hi + cap + MARGIN - REACH : 0;
    uintptr_t win_hi = lo + REACH - cap - MARGIN;
    if (win_lo >= win_hi) return NULL;                  // chunk wider than 256 MB

    // vm_allocate with VM_FLAGS_FIXED, NOT mmap with a hint, and that is the
    // whole repair. mmap's hint is a place to start SEARCHING: hand it an
    // occupied address and it walks upward past everything in the way — past
    // the guest image itself, which is the one thing guaranteed to be sitting
    // next to the code — and answers hundreds of megabytes out. The first
    // version of this search stepped the hint and still failed for exactly that
    // reason (measured: every candidate skipped to -495 MB).
    //
    // VM_FLAGS_FIXED answers the question actually being asked — "is THIS range
    // free?" — with KERN_NO_SPACE when it is not. It is also the only safe
    // spelling: mmap(MAP_FIXED) would silently REPLACE whatever is there, and
    // what is there is another mapping of ours.
    //
    // Stepped a page-cluster at a time from the code outward in both
    // directions, nearest first, so the pool stays as close to the chunk as the
    // address space allows.
    const uintptr_t STEP = 0x10000;
    for (uintptr_t up = (hi + STEP - 1) & ~(STEP - 1), down = (lo - cap) & ~(STEP - 1);
         up <= win_hi || down >= win_lo; ) {
        uintptr_t cand = 0;
        // Alternate up/down so neither direction starves the other, and prefer
        // the nearer of the two.
        if (up <= win_hi && (down < win_lo || up - hi <= lo - down)) {
            cand = up; up += STEP;
        } else if (down >= win_lo && down <= win_hi) {
            cand = down; down -= STEP;
        } else if (up <= win_hi) {
            cand = up; up += STEP;
        } else break;
        vm_address_t a = (vm_address_t)cand;
        if (vm_allocate(mach_task_self(), &a, cap, VM_FLAGS_FIXED) != KERN_SUCCESS)
            continue;
        if (mprotect((void *)a, cap, PROT_READ | PROT_WRITE) != 0) {
            vm_deallocate(mach_task_self(), a, cap);
            continue;
        }
        return (uint8_t *)a;
    }
    // Nothing in range. Fall back to wherever the kernel will have us so the
    // chunk's ctr veneers (which need no reach — emitted the same way, but a
    // refusal there is a SIGILL) still get a home, and let the
    // per-site refusal count and kl_image.c's report say what was lost.
    uint8_t *p = mmap(NULL, cap, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

// The load-time path: allocate a pool next to the code and emit into it. Here
// the executing addresses *are* the buffer addresses, which is the degenerate
// case of kl_x18_emit.
int kl_x18_patch(void *code, size_t size, void *arena, size_t arena_size,
                 size_t *arena_used, const kl_x18_image *image,
                 kl_x18_stats *st) {
    memset(st, 0, sizeof *st);

    // Same reason as in kl_x18_emit: the data-word tally has to survive the
    // "nothing to veneer here" path, or a chunk of pure data reports nothing.
    unsigned want = klx_survey(code, size, &st->data_words, &st->ctr_sites,
                               &st->tp_sites, &st->svc_sites);
    st->sites = want;
    unsigned veneers = (veneer_enabled() ? want : 0) +
                       (ctr_enabled() ? st->ctr_sites : 0) +
                       st->tp_sites + st->svc_sites;
    if (!veneers) return 0;
    if ((st->tp_sites || st->svc_sites) && kl_x18_init_linux() != 0) return -1;
    // The TSD slot is only needed by the x18 half; a chunk whose only veneer is
    // a CTR_EL0 one does not touch it. Checking it anyway would refuse to fix a
    // SIGILL because of a register the code in question never names.
    if (want && veneer_enabled() && g_slot < 0 && kl_x18_init() != 0) return -1;

    // ...plus what the jump-table rewrite can cost. A `br x18` that converts
    // grows a thunk per table target, so it is the one site shape whose veneer
    // is not VEN_MAX_INSN-bounded. Budgeting the worst case per `br x18` is
    // cheap — there are single digits of them in a real library — and running
    // short is not a crash, only a site that keeps the guarded veneer.
    size_t   termbr = image && image->data_at ? kl_x18_count_termbr(code, size) : 0;
    size_t   cap  = ((size_t)veneers * VEN_MAX_INSN * 4
                     + termbr * KLX_JT_MAX_INSN * 4 + 0xFFFF) & ~(size_t)0xFFFF;
    uint8_t *base = NULL;
    // The caller's reservation first: it is the only placement that is adjacent
    // to the image by CONSTRUCTION rather than by luck. Searching is the
    // fallback, and for a big image in a crowded process it can fail outright —
    // see klx_place_pool.
    if (arena && arena_used && *arena_used + cap <= arena_size) {
        uint8_t *slot = (uint8_t *)arena + *arena_used;
        // ...but adjacent to the image is only "in reach" for the code the pool
        // can actually see. An arena is one reservation beside the whole span,
        // so past ~128 MB of image the far chunks are beyond b's range from
        // every slot in it — Deadpool VR's libUE4.so is 262 MB and lost all
        // 4141 of its sites to exactly this. Taking the slot regardless is the
        // refusal with the worst shape: the space is consumed AND the sites run
        // against Darwin's x18. Check reach on the extremes first and leave the
        // chunk to klx_place_pool, which searches near the CODE.
        uintptr_t lo = (uintptr_t)code, hi = lo + size;
        uintptr_t p0 = (uintptr_t)slot, p1 = p0 + cap;
        if (!out_of_reach(lo, p0) && !out_of_reach(hi, p0) &&
            !out_of_reach(lo, p1) && !out_of_reach(hi, p1)) {
            base = slot;
            *arena_used += cap;
            if (mprotect(base, cap, PROT_READ | PROT_WRITE) != 0) base = NULL;
        }
    }
    if (!base) base = klx_place_pool(code, size, cap);
    if (!base) return -1;

    int rc = kl_x18_emit_img(code, size, (uint64_t)(uintptr_t)code,
                             base, cap, (uint64_t)(uintptr_t)base, image, st, NULL);
    st->pool_va    = (uint64_t)(uintptr_t)base;
    st->pool_bytes = cap;

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


static unsigned g_refused_offline;
unsigned kl_x18_refusals(void) { return g_refused_total + g_refused_offline; }
void kl_x18_note_translated(unsigned refused) { g_refused_offline += refused; }

void kl_x18_report(FILE *f) {
    if (!g_refused_total) return;
    fprintf(f, "  [klepton] x18: %u sites refused (reading Darwin's x18):\n",
            g_refused_total);
    for (unsigned i = 0; i < g_nrefused_kinds; i++)
        fprintf(f, "    %08x  x%u\n", g_refused[i].word, g_refused[i].n);
    if (g_refused_total > g_nrefused_kinds)
        fprintf(f, "    (encodings beyond the first %u kinds are counted, not listed)\n",
                REFUSE_KINDS);
}
