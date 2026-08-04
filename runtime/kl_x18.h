// S0.5 — x18 veneering.
//
// Android leaves x18 a general-purpose temporary and the Quest toolchain
// allocates into it; Darwin reserves it and the kernel zeroes it on any
// exception return, a timer interrupt included (spikes/s05_x18.c). So the
// architectural x18 cannot hold guest state across any window at all, and the
// fix is to stop using it: every guest instruction that names x18 is redirected
// to a veneer that keeps the value in a per-thread shadow slot instead.
//
// This header is the decoder half. Substituting a register into an instruction
// needs to know exactly which bit-fields of that encoding are general-purpose
// register operands and whether each is read, written, or both — and getting
// that wrong is a silent wrong answer, so the decoder refuses anything it does
// not positively recognise. tests/t_x18.c cross-checks every answer against
// objdump over the real corpus.
#ifndef KL_X18_H
#define KL_X18_H
#include <stddef.h>
#include <stdint.h>

// A64 puts register operands at four canonical bit positions. Which of them are
// actually registers — as opposed to an immediate, a condition code, a prefetch
// op or a vector register — is what the decoder decides.
#define KLX_RD   0     // Rd / Rt  — bits [4:0]
#define KLX_RN   5     // Rn       — bits [9:5]
#define KLX_RA  10     // Ra / Rt2 — bits [14:10]
#define KLX_RM  16     // Rm / Rs  — bits [20:16]

#define KLX_R    1     // field reads the register
#define KLX_W    2     // field writes it

// A veneer executes the instruction somewhere else, so anything whose meaning
// depends on where it sits has to be rebuilt rather than copied. Across the
// whole corpus that is 497 sites and only three shapes: adrp, cbz/cbnz and
// tbz/tbnz. adr and the load-literal forms do not occur — they are classified
// anyway so that the emitter refuses them out loud instead of relocating them
// wrongly if a different target ever has one.
#define KLX_PC_NONE     0
#define KLX_PC_ADRP     1   // page address; re-encodable against the veneer's own pc
#define KLX_PC_ADR      2   // byte address, +/-1 MB
#define KLX_PC_LITERAL  3   // ldr/ldrsw literal, +/-1 MB
#define KLX_PC_CBZ      4   // imm19 at [23:5], condition inverted by bit 24
#define KLX_PC_TBZ      5   // imm14 at [18:5], condition inverted by bit 24

typedef struct {
    int      ok;        // encoding positively recognised; 0 means "refuse to touch"
    uint32_t gp_used;   // every GP register this instruction names (bit n = xn),
                        // so a veneer can pick scratch that does not collide
    unsigned nfields;   // fields naming x18 specifically
    struct {
        uint8_t shift;  // KLX_RD / KLX_RN / KLX_RA / KLX_RM
        uint8_t role;   // KLX_R | KLX_W
    } x18[4];
    uint8_t  roles;     // union of the roles above: does this site read x18, write it, or both
    uint8_t  pcrel;     // KLX_PC_*
    uint8_t  hazard;    // KLX_HZ_*: the veneer shape does not hold for this site
} klx_info;

// A veneer spills scratch below sp, runs the instruction, then restores and
// branches back. Two things break that contract outright, so the emitter
// refuses them and names them rather than producing something plausible.
#define KLX_HZ_NONE     0
#define KLX_HZ_NOFALL   1   // br/ret: control leaves before the restore runs
#define KLX_HZ_SPWRITE  2   // modifies sp, so the restore would read elsewhere

// Decode one instruction word. Returns out->ok. Sites where x18 does not appear
// come back ok with nfields == 0.
int klx_decode(uint32_t insn, klx_info *out);

// Rewrite every x18 field of `insn` to name register `reg` instead.
uint32_t klx_substitute(uint32_t insn, const klx_info *info, unsigned reg);

// ---------- veneering ----------
#include <stdio.h>

typedef struct {
    unsigned sites;     // instructions found naming x18
    unsigned patched;   // veneers installed
    unsigned refused;   // sites left alone — still broken, and reported by name
} kl_x18_stats;

// Allocate the per-thread shadow slot and check the assumptions the emitted code
// depends on. Returns 0 on success; non-zero means veneering must not be used.
int  kl_x18_init(void);

// Patch every x18 site in an executable range, which must still be writable.
// `code` is where it is mapped now. Idempotent per range only in the sense that
// it should be called once, before the segment is made read-execute.
int  kl_x18_patch(void *code, size_t size, kl_x18_stats *st);

// Encodings that were refused, with counts — the work list for extending the
// decoder. Prints nothing when there are none.
void kl_x18_report(FILE *f);

#endif
