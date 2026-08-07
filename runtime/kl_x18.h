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

// The per-thread shadow slot, as a Darwin TSD index.
//
// It is baked into every veneer as the immediate of `ldr b, [a, #SLOT*8]`, so it
// MUST be a build-time constant: klepton-ld emits veneers offline, where no
// pthread key exists yet, and a runtime-chosen slot cannot be patched in later
// without writing to a dyld-mapped __TEXT — the exact thing M1b exists to stop.
//
// The constant is nevertheless a REAL pthread key, claimed at runtime, not a
// squatted reserved slot. kl_x18_init() calls pthread_key_create in a loop until
// it is handed exactly this key, keeps it, and releases the ones it collected on
// the way. Darwin allocates external keys upward from 258 and never returns one
// that is currently held, so the walk terminates here and the key is genuinely
// ours; if something else already holds it, we fail loudly instead of sharing.
//
// SQUATTING A RESERVED SLOT WAS TRIED FIRST AND IS WRONG. Slot 200 looked ideal:
// Darwin's assigned blocks are the pthread/libc internals at 0-9 and per-
// framework blocks running to roughly 130, the dynamic range starts at 258, and
// surveying the live TSD array with ANGLE, Metal and QuartzCore loaded showed
// only 0,1,2,3,4,7,20,25,40,43,55,114 occupied. It still corrupted the process:
// a 30-frame lifecycle run died in `malloc: pointer being freed was not
// allocated: 0x3` — 0x3 being a guest x18 value that some libSystem consumer of
// that slot then read back as its own pointer.
//
// The survey was the bug. It proved the slot was empty at three *instants*, not
// that it was unowned; a slot used transiently reads as zero whenever you are
// not looking. There is no sampling schedule that establishes ownership, which
// is why this is now a claimed key rather than a measured-free one.
#define KLX_TSD_SLOT 300

// Longest veneer, in instructions. Public because klepton-ld sizes its pool from
// the site count before it emits anything.
#define KLX_VEN_MAX_INSN 8

typedef struct {
    unsigned sites;     // instructions found naming x18
    unsigned patched;   // veneers installed
    unsigned refused;   // sites left alone — still broken, and reported by name
} kl_x18_stats;

// Check the assumptions the emitted code depends on: thread-pointer alignment
// and that KLX_TSD_SLOT is actually free. Returns 0 on success; non-zero means
// veneering must not be used. The runtime loader calls it before patching, and
// kl_load_dylib() calls it too — a translated image arrives with the slot
// already baked into its text, so a collision there is fatal rather than
// recoverable, and has to be found at load time instead of as a wrong answer.
int  kl_x18_init(void);

// Emit veneers for every x18 site in an executable range. Pure with respect to
// memory: it writes only into `code` and `pool`, and takes the addresses those
// buffers will have *when executed* separately, so the same emitter serves the
// runtime loader (where they are equal to the pointers) and klepton-ld (where
// the image is a file buffer being laid out for a future mapping).
//
// Splitting this out is what lets the offline pass and the load-time pass share
// one implementation — which matters because `make x18` and `make guest` only
// validate the one they can reach.
int  kl_x18_emit(void *code, size_t size, uint64_t code_va,
                 void *pool, size_t poolcap, uint64_t pool_va,
                 kl_x18_stats *st, size_t *pool_used);

// Patch every x18 site in an executable range, which must still be writable.
// `code` is where it is mapped now. Idempotent per range only in the sense that
// it should be called once, before the segment is made read-execute.
int  kl_x18_patch(void *code, size_t size, kl_x18_stats *st);

// Count x18 sites without emitting anything — klepton-ld sizes its pool with it.
unsigned kl_x18_count(const void *code, size_t size);

// What an offline translation did, recorded in the emitted image's
// __TEXT,__klstat section. A translated library has no sites left to count at
// load time — the veneers are already in its text — so without this the loader
// would report "x18 sites: 0", which reads as "this library never needed any"
// rather than "this was handled at translation time". Silent zeros are worse
// than errors (CLAUDE.md trap 6d).
#define KLX_STAT_MAGIC 0x38315838u   /* "8X18" */
typedef struct {
    uint32_t magic;
    uint32_t sites, patched, refused;
    uint32_t tls_rewrites;
    uint32_t slot;          // KLX_TSD_SLOT the veneers were built against
} klx_stat_section;

// Encodings that were refused, with counts — the work list for extending the
// decoder. Prints nothing when there are none.
void kl_x18_report(FILE *f);

#endif
