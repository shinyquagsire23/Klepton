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
        addfld(o, w, KLX_RD, KLX_W);
        addfld(o, w, KLX_RN, KLX_R);
        return 1;
    case 0x25:                                 // move wide: imm16 covers [20:5]
        // movk merges into the existing value; movz/movn overwrite it.
        addfld(o, w, KLX_RD, opc == 3 ? (KLX_R | KLX_W) : KLX_W);
        return 1;
    case 0x26:                                 // bitfield
        if (opc == 3) return 0;                // unallocated
        // bfm (and so bfi/bfxil) leaves the untouched bits of Rd in place, which
        // makes Rd a read as well. sbfm/ubfm overwrite the whole register.
        addfld(o, w, KLX_RD, opc == 1 ? (KLX_R | KLX_W) : KLX_W);
        addfld(o, w, KLX_RN, KLX_R);
        return 1;
    case 0x27:                                 // extract (extr, and so ror #imm)
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
        // Two sites exist in the whole corpus, both jump tables in libunity's
        // UTF-8 decoder, and x16/x17 are provably live across them. Refused.
        o->hazard = KLX_HZ_NOFALL;
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
    if (!o->ok) { o->nfields = 0; o->roles = 0; o->gp_used = 0; }
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
#define VEN_MAX_INSN 8
#define REFUSE_KINDS 24

static int g_slot = -1;
static struct { uint32_t word; unsigned n; } g_refused[REFUSE_KINDS];
static unsigned g_nrefused_kinds, g_refused_total;

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

    pthread_key_t k;
    if (pthread_key_create(&k, NULL) != 0) return -1;
    if ((unsigned)k >= 4096) return -1;          // ldr's scaled imm12 caps the slot
    // The veneer reads the slot inline as [tsd + k*8]. That is the same storage
    // pthread_setspecific uses only for the direct range, so prove it on this
    // key before emitting seventeen thousand copies of the assumption.
    pthread_setspecific(k, (void *)(uintptr_t)0x5A5A5A5AU);
    if (((void **)(uintptr_t)tp)[k] != (void *)(uintptr_t)0x5A5A5A5AU) return -1;
    pthread_setspecific(k, NULL);

    g_slot = (int)k;
    return 0;
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
        const char *v = getenv("KL_X18");
        cached = !(v && (v[0] == '0' || v[0] == 'n' || v[0] == 'N'));
    }
    return cached;
}

int kl_x18_patch(void *code, size_t size, kl_x18_stats *st) {
    memset(st, 0, sizeof *st);

    uint32_t *w = (uint32_t *)code;
    size_t    n = size / 4;

    unsigned want = 0;
    for (size_t i = 0; i < n; i++) {
        klx_info in;
        klx_decode(w[i], &in);
        if (in.nfields) want++;
    }
    st->sites = want;
    if (!want || !veneer_enabled()) return 0;
    if (g_slot < 0 && kl_x18_init() != 0) return -1;

    // The pool goes immediately after the code so that `b` reaches it from
    // anywhere in the range. mmap treats the address as a hint, so the result
    // is range-checked per site rather than assumed.
    size_t   cap  = ((size_t)want * VEN_MAX_INSN * 4 + 0xFFFF) & ~(size_t)0xFFFF;
    uint8_t *hint = (uint8_t *)(((uintptr_t)code + size + 0xFFFF) & ~(uintptr_t)0xFFFF);
    uint8_t *base = mmap(hint, cap, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) return -1;

    uint32_t *out = (uint32_t *)base, *pool_end = (uint32_t *)(base + cap);

    for (size_t i = 0; i < n; i++) {
        klx_info in;
        klx_decode(w[i], &in);
        if (!in.nfields) continue;

        uint64_t spc = (uint64_t)(uintptr_t)&w[i];
        uint64_t vpc = (uint64_t)(uintptr_t)out;
        uint32_t word = w[i];

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
            body[k++] = enc_ldr(b, a, (unsigned)g_slot * 8);
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
            if (in.roles & KLX_R) body[k++] = enc_ldr(b, a, (unsigned)g_slot * 8);
            if (in.pcrel == KLX_PC_ADRP) {
                int fits;
                sub = reenc_adrp(sub, spc, vpc + k * 4, &fits);
                ok &= fits;
            }
            body[k++] = sub;
            if (in.roles & KLX_W) body[k++] = enc_str(b, a, (unsigned)g_slot * 8);
            body[k++] = enc_ldp(a, b, -16);
            ok &= enc_b(vpc + k * 4, spc + 4, &body[k]); k++;
        }

        uint32_t patch;
        ok &= enc_b(spc, vpc, &patch);
        if (!ok) { note_refusal(word); st->refused++; continue; }

        memcpy(out, body, k * 4);
        out += k;
        w[i] = patch;
        st->patched++;
    }

    mprotect(base, cap, PROT_READ | PROT_EXEC);
    sys_icache_invalidate(base, cap);
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
