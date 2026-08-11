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
    uint8_t  sysreg;    // KLX_SYS_*: reads a system register EL0 may not read
} klx_info;

// A veneer spills scratch below sp, runs the instruction, then restores and
// branches back. Two things break that contract outright, so the emitter
// refuses them and names them rather than producing something plausible.
#define KLX_HZ_NONE     0
#define KLX_HZ_NOFALL   1   // blr/ret: control leaves before the restore runs
#define KLX_HZ_SPWRITE  2   // modifies sp, so the restore would read elsewhere
// `br x18` alone. Still a no-fallthrough site, but the ONE shape the emitter can
// serve anyway, so it is separated from KLX_HZ_NOFALL rather than folded into it
// (see the emitter for why it is sound, and for the one-instruction window it
// cannot close). blr and ret keep KLX_HZ_NOFALL: blr would leave x30 pointing
// into the veneer, and ret is a return-predictor hint whose register is x30's
// business, not x18's.
#define KLX_HZ_TERMBR   3

// System registers that need translating, which is a different job from x18 but
// the same machinery: the site is rewritten in place to a branch into a stub
// that answers, because one instruction cannot materialise a 64-bit constant.
//
// TPIDR_EL0 is not here — it is a one-bit rewrite (trap 1) and needs no stub.
#define KLX_SYS_NONE    0
#define KLX_SYS_CTR_EL0 1   // trap 26: SIGILL from EL0 on Darwin

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
//
// WHY 500 AND NOT 300. Claiming is a RACE, and in an app bundle we do not get to
// run first. On the visionOS 27 simulator the Steam Link app's first
// pthread_key_create is handed **330**: SwiftUI, UIKit and their dependencies
// burn past 300 during dyld's initialization of THEIR images, which is before a
// constructor in ours can run — measured, not assumed, and it is why
// klx_claim_slot_early() exists and is still not early enough for 300. The guest
// then refuses to load with "TSD slot 300 is unavailable", which reads as a
// platform limit and is a starting-gun problem.
//
// Darwin issues external keys UPWARD from 258 and the TSD array holds 512, so
// the HIGHEST slots are the last to be taken and the most robust choice
// available: 500 survives a process that has created ~240 keys before us, where
// 300 survives about forty. It is not a measured-free slot either — the same
// walk claims it, kl_x18_init still proves tsd[500] agrees with
// pthread_setspecific before emitting anything, and a translation built against
// a different slot is refused BY NAME at load (kl_image.c) rather than running
// against the wrong one.
//
// Changing it means re-translating: `make dylibs` and visionos/mkguest.sh both
// bake it in, and visionos/run.sh rebuilds klepton-ld for exactly this reason.
#define KLX_TSD_SLOT 500

// ---------------------------------------------------- trap 26: CTR_EL0
//
// What a veneered `mrs Xt, CTR_EL0` answers. Baked into the veneer as a
// movz/movk pair, so — like KLX_TSD_SLOT — it must be a constant at translation
// time; KL_CTR_EL0=<hex> overrides it for an A/B, at the cost of re-translating.
//
// **This is Apple silicon's own CTR_EL0, not a convenient invention.** The
// register cannot be read from EL0 on any Darwin kernel (measured: `mrs x0,
// ctr_el0` SIGILLs on macOS 26 as well as on visionOS — trap 26's claim that
// macOS permits it is wrong, and the simulator does not catch the crash only
// because Qt's `__clear_cache` is never reached there), so the value is
// transcribed rather than sampled:
//
//     IminLine 4, DminLine 4   64-byte lines, the minimum across all levels
//     L1Ip 0b11 (PIPT), ERG 4, CWG 4, bit 31 RES1
//     IDC 0, DIC 0             cache maintenance IS required for coherency
//
// **IDC/DIC are deliberately 0, which is the opposite of what the trap-26
// record designed**, and the difference matters. That design assumed `dc cvau`
// and `ic ivau` were as likely to trap as the `mrs`, so it answered IDC=1/DIC=1
// to make `__clear_cache` skip both maintenance loops. Measured on Darwin, both
// instructions are LEGAL from EL0 (only the `mrs` is trapped, which is
// SCTLR_EL1.UCT rather than UCI), and Darwin's own `sys_icache_invalidate` is
// built on them. So claiming coherency we do not have would silently skip the
// maintenance a JIT needs — a wrong answer with no error surface, where letting
// the loops run is simply correct. If a future kernel does trap them, the crash
// moves a few instructions along and kl_fault.c now names it; KL_CTR_EL0
// =0xb444c004 sets IDC and DIC and is the way back.
//
// Under-stating the line size is the safe direction (more maintenance ops, each
// covering its own line), so 64 stands even where the coherency line is 128.
#define KLX_CTR_EL0_VALUE 0x8444c004u

// ...the value actually in force, KL_CTR_EL0 applied. Exposed so klepton-ld can
// record what it baked into the veneers it emitted.
uint64_t kl_x18_ctr_value(void);

// Longest veneer, in instructions. Public because klepton-ld sizes its pool from
// the site count before it emits anything.
#define KLX_VEN_MAX_INSN 8

typedef struct {
    unsigned sites;     // instructions found naming x18
    unsigned patched;   // veneers installed
    unsigned refused;   // sites left alone — still broken, and reported by name
    unsigned data_words; // words that decode as an x18 site but sit in data
                         // (trap 0b's second detector — NOT refusals: these
                         // were never instructions, and patching them is the
                         // corruption the detector exists to prevent)
    // trap 26, counted APART from the x18 numbers on purpose: those are exact
    // and `make check` gates on them, so a second population sharing the same
    // counters would move a number that is supposed to be stable.
    unsigned ctr_sites, ctr_patched, ctr_refused;
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

// Trap 0d's second detector, exposed so tests/t_x18.c reports the same verdict
// the loader acts on. `index` is a WORD index into `code`; the window is
// clamped to the buffer, so pass the same chunk the scanner is walking.
int kl_x18_is_data(const void *code, size_t size, size_t index);

// Count sites that will need a VENEER — x18 sites plus trap 26's CTR_EL0 reads
// — without emitting anything. klepton-ld sizes its pool with it, so it has to
// be the total rather than the x18 half; the stats keep the two apart.
unsigned kl_x18_count(const void *code, size_t size);

// What an offline translation did, recorded in the emitted image's
// __TEXT,__klstat section. A translated library has no sites left to count at
// load time — the veneers are already in its text — so without this the loader
// would report "x18 sites: 0", which reads as "this library never needed any"
// rather than "this was handled at translation time". Silent zeros are worse
// than errors (CLAUDE.md trap 6d).
// The magic carries a VERSION, and the loader refuses a record it does not
// recognise BY NAME rather than ignoring it. A record that merely got longer
// would fail the `statsz >= sizeof` test and be skipped silently — which also
// skips the TSD-slot check that is the whole reason it is read.
#define KLX_STAT_MAGIC 0x39315838u   /* "8X19" — was "8X18" before trap 26 */
typedef struct {
    uint32_t magic;
    uint32_t sites, patched, refused;
    uint32_t tls_rewrites;
    uint32_t slot;          // KLX_TSD_SLOT the veneers were built against
    uint32_t ctr_sites, ctr_patched;   // trap 26
    uint32_t ctr_value;     // ...and what they were built to answer
} klx_stat_section;

// Encodings that were refused, with counts — the work list for extending the
// decoder. Prints nothing when there are none.
void kl_x18_report(FILE *f);

// ---------------------------------------------------- data inside code
//
// "Executable sections, not the executable segment" (trap 0) is one level too
// shallow: a `.text` SECTION can itself contain data. Steam Link's libmain.so
// carries BoringSSL's `ecp_nistz256_precomputed` — 148 KB of elliptic-curve
// constants — as an STT_OBJECT at 0xb23000, inside .text, and 1059 of that
// library's 1080 apparent x18 sites are words of that table. Veneering them
// replaces the constants with branches, so the P-256 tables are silently wrong
// and every TLS handshake fails somewhere far away from here.
//
// Beat Saber has none of this (measured: 0 bytes of STT_OBJECT inside any
// executable section, across all five libraries), which is why it never showed
// up. Note the corollary: the objdump cross-check cannot see this class either
// unless the disassembler can — it renders such a symbol as bytes only because
// the symbol table says it is data, so a stripped library hides the hazard from
// BOTH tools. Symbol coverage is the ceiling on what can be detected here.
typedef struct { uint64_t start, end; } klx_range;

// Ranges of an ELF's address space that are data despite living in an
// executable section: STT_OBJECT symbols from .symtab/.dynsym, plus `$d`
// mapping symbols where the toolchain emitted them. Returned sorted, merged and
// outward-aligned to 4 bytes, so the code chunks between them stay instruction
// aligned. `elf` is the whole mapped file. Returns the count written, which is
// capped at `max`; a truncated list is reported by kl_x18_data_ranges itself.
unsigned kl_x18_data_ranges(const void *elf, size_t size,
                            klx_range *out, unsigned max);

// Iterate the sub-ranges of [sec_va, sec_va+sec_size) that `skip` does NOT
// cover — i.e. the parts of an executable section that are really code. Set
// *cursor to sec_va before the first call; returns 1 and fills *out_va/*out_size
// per chunk, 0 when the section is exhausted. With nskip == 0 this yields the
// whole section once, which is what every previously-working library gets.
int kl_x18_next_code(uint64_t sec_va, size_t sec_size,
                     const klx_range *skip, unsigned nskip,
                     uint64_t *cursor, uint64_t *out_va, size_t *out_size);

#endif
