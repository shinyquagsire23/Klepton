// x18 veneering.
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
// CASP — compare-and-swap PAIR. Its Rs and Rt each name TWO registers, the one
// in the field and the next one up, and both fields must be even. So x18 here
// means the pair x18:x19, and substituting a single scratch register for it is
// wrong twice over: an odd one is an unallocated encoding, and an even one
// silently swaps x19 for its neighbour. The emitter has a shape of its own for
// it (a scratch PAIR, with x19 copied across), which is why it is a hazard for
// the general path rather than something the general path half-handles.
// Android toolchains do not emit it into x18; Valve's Source 2 build does.
#define KLX_HZ_PAIR     4

// System registers that need translating, which is a different job from x18 but
// the same machinery: the site is rewritten in place to a branch into a stub
// that answers, because one instruction cannot materialise a 64-bit constant.
//
// TPIDR_EL0 is not here — it is a one-bit rewrite and needs no stub.
#define KLX_SYS_NONE    0
#define KLX_SYS_CTR_EL0 1   // SIGILL from EL0 on Darwin
// The Linux guest ABI only (KLX_ABI_LINUX) — an Android image never has these
// flagged, so its site counts do not move:
#define KLX_SYS_TP_READ  2  // mrs Xt, TPIDR_EL0 — answered from tsd[KLX_TP_SLOT]
#define KLX_SYS_TP_WRITE 3  // msr TPIDR_EL0, Xt — stored there
#define KLX_SYS_SVC      4  // svc #0 — a call through tsd[KLX_SVC_SLOT]

// ---------------------------------------------------- which guest ABI
//
// An Android guest keeps its TLS in pthread keys, so its thread pointer can BE
// Darwin's and the one-bit TPIDR_EL0 -> TPIDRRO_EL0 flip is the whole rewrite.
// A glibc guest cannot share it: local-exec and initial-exec TLS are link-time
// constant offsets from the thread pointer (`tp + 16 + off`), which against
// Darwin's would land in the middle of its TSD array. So under the Linux ABI the
// guest gets a thread pointer of its own, kept in a TSD slot, and every read or
// write of TPIDR_EL0 is veneered to that slot instead of flipped.
//
// The same ABI has no libc of ours underneath it — the guest brings glibc — so
// the boundary to the host is the kernel's: `svc #0` is veneered into a call
// through a second slot, which holds the syscall entry (runtime/linux/).
//
// Process-wide, and set before anything is decoded: klepton-ld takes it from
// --abi, the runtime from the kind of guest it is loading.
#define KLX_ABI_ANDROID 0
#define KLX_ABI_LINUX   1
void kl_x18_set_abi(int abi);
int  kl_x18_abi(void);

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
// without writing to a dyld-mapped __TEXT, which the translated path exists to
// avoid.
//
// The constant is nevertheless a REAL pthread key, claimed at runtime, not a
// squatted reserved slot. kl_x18_init() calls pthread_key_create in a loop until
// it is handed exactly this key, keeps it, and releases the ones it collected on
// the way. Darwin allocates external keys upward from 258 and never returns one
// that is currently held, so the walk terminates here and the key is genuinely
// ours; if something else already holds it, we fail loudly instead of sharing.
// Surveying the TSD array for a free slot cannot replace this: a slot used
// transiently reads as zero whenever it is sampled, so an empty reading proves
// the slot was unused at an instant, never that it is unowned.
//
// The slot is high because claiming is a RACE an app bundle loses. Darwin issues
// external keys upward from 258 and the TSD array holds 512, so the highest
// slots are the last to be taken: 500 survives a process that created ~240 keys
// before us, where 300 survives about forty. On the visionOS 27 simulator the
// Steam Link app's first pthread_key_create is handed 330 — SwiftUI and UIKit
// burn past 300 during dyld's initialization of THEIR images, before a
// constructor in ours can run, which is why klx_claim_slot_early() exists and is
// still not early enough for 300.
//
// kl_x18_init still proves tsd[SLOT] agrees with pthread_setspecific before
// emitting anything, and a translation built against a different slot is refused
// BY NAME at load (kl_image.c) rather than running against the wrong one.
//
// Changing it means re-translating: `make dylibs` and visionos/mkguest.sh both
// bake it in, and visionos/run.sh rebuilds klepton-ld for exactly this reason.
#define KLX_TSD_SLOT 500

// The Linux ABI's two slots, baked into veneers exactly as KLX_TSD_SLOT is and
// claimed in the same walk. They sit directly above it so the walk that reaches
// 500 is handed these next, while every key below is still held.
//
//   KLX_TP_SLOT   the guest's thread pointer (what TPIDR_EL0 reads as)
//   KLX_SVC_SLOT  the address a veneered `svc #0` calls. A slot rather than a
//                 constant because the veneer is emitted offline, where the
//                 runtime's address does not exist yet.
#define KLX_TP_SLOT  501
#define KLX_SVC_SLOT 502

// ---------------------------------------------------- CTR_EL0
//
// What a veneered `mrs Xt, CTR_EL0` answers. Baked into the veneer as a
// movz/movk pair, so — like KLX_TSD_SLOT — it must be a constant at translation
// time; KL_CTR_EL0=<hex> overrides it for an A/B, at the cost of re-translating.
//
// This is Apple silicon's own CTR_EL0, not a convenient invention. The register
// cannot be read from EL0 on ANY Darwin kernel — measured: `mrs x0, ctr_el0`
// SIGILLs on macOS 26 as well as on visionOS, and the simulator stays silent
// only because Qt's `__clear_cache` is never reached there — so the value is
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
// the site count before it emits anything. The longest is the CASP one whose
// compared pair is x18:x19 (ten, see KLX_HZ_PAIR); everything else fits in eight.
#define KLX_VEN_MAX_INSN 10

typedef struct {
    unsigned sites;     // instructions found naming x18
    unsigned patched;   // veneers installed
    unsigned refused;   // sites left alone — still broken, and reported by name
    // ...and how many of those refusals were REACH rather than encoding: the
    // patch at a site is a single `b`, so a pool further than +/-128 MB from it
    // refuses every site it cannot reach. Counted apart because the two have
    // nothing in common — an unteachable encoding is one instruction and a pool
    // out of range is the whole library, and only the second is fixable by
    // moving something. pool_va/pool_bytes are what say WHERE it landed.
    unsigned far_refused;
    uint64_t pool_va, pool_bytes;
    unsigned data_words; // words that decode as an x18 site but sit in data
                         // (found by the data detector — NOT refusals: these
                         // were never instructions, and patching them is the
                         // corruption the detector exists to prevent)
    // CTR_EL0, counted APART from the x18 numbers on purpose: those are exact
    // and `make check` gates on them, so a second population sharing the same
    // counters would move a number that is supposed to be stable.
    unsigned ctr_sites, ctr_patched, ctr_refused;
    // ...and the Linux ABI's two populations, apart for the same reason.
    unsigned tp_sites, tp_patched, svc_sites, svc_patched, lx_refused;
    // The `br x18` jump-table rewrite. `jt_sites` counts `br x18` dispatches
    // recognised and converted to branch through x16 into a per-target thunk;
    // `jt_thunks` is how many targets those tables held, which is what the
    // rewrite costs in pool. `jt_fallback` is the `br x18` sites that could NOT
    // be converted and still carry the guarded veneer — those keep the residual
    // one-instruction window in which an exception return zeroes x18 before
    // the branch, so a non-zero count here is the number to look at.
    unsigned jt_sites, jt_thunks, jt_fallback;
} kl_x18_stats;

// The image a chunk belongs to, so the jump-table rewrite can reach the TABLE.
//
// A `br x18` dispatch reads its targets from a table of 32-bit self-relative
// offsets that lives in .rodata — a different section from the code being
// veneered — and the whole point of the rewrite is to point those offsets at
// thunks. kl_x18_emit is handed one executable chunk and nothing else, so the
// caller supplies the two questions the rewrite has to ask about addresses
// outside it. Pass NULL for `image` and the rewrite is simply not attempted:
// every `br x18` keeps the guarded veneer it had before.
typedef struct {
    // A writable pointer to `len` bytes at guest VA `va`, or NULL. It must
    // answer ONLY for non-executable data. Answering for text would let a
    // mis-recognition write over code, which is the one failure mode this pass
    // must not have.
    void *(*data_at)(void *ctx, uint64_t va, size_t len);
    void   *ctx;
} kl_x18_image;

// How far back the recogniser looks for each part of the dispatch idiom, and the
// largest table it will convert. The scan bound is generous because the table
// base (`adrp`/`add`) is routinely hoisted out of a loop, tens of instructions
// above the dispatch that uses it; the entry bound is what keeps the pool
// budget finite. KLX_JT_MAX_SITES bounds the per-chunk record array.
#define KLX_JT_SCAN         128
#define KLX_JT_MAX_ENTRIES  256
#define KLX_JT_MAX_SITES    64

// Pool words a single converted site can cost: the dispatch veneer plus two
// instructions per thunk. klepton-ld and kl_x18_patch size their pools with it.
#define KLX_JT_MAX_INSN     (KLX_VEN_MAX_INSN + 2 * KLX_JT_MAX_ENTRIES)

// Check the assumptions the emitted code depends on: thread-pointer alignment
// and that KLX_TSD_SLOT is actually free. Returns 0 on success; non-zero means
// veneering must not be used. The runtime loader calls it before patching, and
// kl_load_dylib() calls it too — a translated image arrives with the slot
// already baked into its text, so a collision there is fatal rather than
// recoverable, and has to be found at load time instead of as a wrong answer.
int  kl_x18_init(void);

// The Linux ABI's half of that check: KLX_TP_SLOT and KLX_SVC_SLOT were claimed
// too, and read back through the raw TSD array. Non-zero means a Linux-ABI image
// must not be run in this process.
int  kl_x18_init_linux(void);

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

// ...and the same emitter with the image behind the chunk, which is what the
// `br x18` jump-table rewrite needs to read and repoint the table. Callers that
// can answer for the whole image (klepton-ld offline, kl_image.c at load time)
// use this; kl_x18_emit is this with `image` NULL.
int  kl_x18_emit_img(void *code, size_t size, uint64_t code_va,
                     void *pool, size_t poolcap, uint64_t pool_va,
                     const kl_x18_image *image,
                     kl_x18_stats *st, size_t *pool_used);

// Patch every x18 site in an executable range, which must still be writable.
// `code` is where it is mapped now. Idempotent per range only in the sense that
// it should be called once, before the segment is made read-execute.
// `arena`/`arena_size` is the caller's own reservation for the veneer pool, and
// passing one is how a big library gets veneered at all. The patch at a site is
// a single `b`, so the pool has to sit within +/-128 MB of it — and for a 172 MB
// image with 232 MB of other mappings stacked above it (which is what an ANGLE +
// SDL process looks like) there is NO free hole in that window to find. The only
// address guaranteed to be adjacent to an image is one reserved WITH it, so
// kl_image.c extends its own mapping and hands the tail in here.
//
// `*arena_used` carries the running offset across the chunks of one image, so
// several chunks share one reservation. Pass arena = NULL to fall back to
// searching for a hole, which is right for a caller that has not reserved one
// and is still correct for every library small enough to find one.
// `image` may be NULL, and then the `br x18` jump-table rewrite is not
// attempted for this chunk — see kl_x18_image.
int  kl_x18_patch(void *code, size_t size, void *arena, size_t arena_size,
                  size_t *arena_used, const kl_x18_image *image,
                  kl_x18_stats *st);

// The data-inside-code detector, exposed so tests/t_x18.c reports the verdict
// the loader acts on. `index` is a WORD index into `code`; the window is
// clamped to the buffer, so pass the same chunk the scanner is walking.
int kl_x18_is_data(const void *code, size_t size, size_t index);

// Count sites that will need a VENEER — x18 sites plus the CTR_EL0 reads
// — without emitting anything. klepton-ld sizes its pool with it, so it has to
// be the total rather than the x18 half; the stats keep the two apart.
unsigned kl_x18_count(const void *code, size_t size);

// How many of those are `br x18` — the terminal-branch shape, and the only one
// whose veneer can grow a thunk per jump-table target. Pool sizing needs it
// apart from the total because one of these can cost KLX_JT_MAX_INSN words
// where every other site costs KLX_VEN_MAX_INSN.
unsigned kl_x18_count_termbr(const void *code, size_t size);

// What an offline translation did, recorded in the emitted image's
// __TEXT,__klstat section. A translated library has no sites left to count at
// load time — the veneers are already in its text — so without this the loader
// would report "x18 sites: 0", which reads as "this library never needed any"
// rather than "this was handled at translation time". Silent zeros are worse
// than errors.
// The magic carries a VERSION, and the loader refuses a record it does not
// recognise BY NAME rather than ignoring it. A record that merely got longer
// would fail the `statsz >= sizeof` test and be skipped silently — which also
// skips the TSD-slot check that is the whole reason it is read.
#define KLX_STAT_MAGIC 0x41315838u   /* "8X1A" */
typedef struct {
    uint32_t magic;
    uint32_t sites, patched, refused;
    uint32_t tls_rewrites;
    uint32_t slot;          // KLX_TSD_SLOT the veneers were built against
    uint32_t ctr_sites, ctr_patched;   // the CTR_EL0 population
    uint32_t ctr_value;     // ...and what they were built to answer
    uint32_t abi;           // KLX_ABI_*: whose thread pointer the text reads
    uint32_t tp_patched, svc_patched;  // the Linux ABI populations
} klx_stat_section;

// Encodings that were refused, with counts — the work list for extending the
// decoder. Prints nothing when there are none.
void kl_x18_report(FILE *f);

// How many sites the veneer pass REFUSED, across every library loaded so far —
// patched at load time in this process, or translated offline by klepton-ld
// and recorded in the dylib's __klstat (kl_x18_note_translated). A refusal is
// not a site left merely unpatched: a refused site that WRITES x18 leaves the
// shadow slot stale, and every later read of it is wrong. The count therefore
// has to be answerable from a running process and not only from `make check`
// — the host patches at load time and the device runs translations, so a
// clean host sweep says nothing about the libraries the device actually runs.
unsigned kl_x18_refusals(void);

// Add an offline translation's refusals to kl_x18_refusals(). Called by the
// loader when it reads a translated library's record.
void kl_x18_note_translated(unsigned refused);

// ---------------------------------------------------- data inside code
//
// "Executable sections, not the executable segment" is one level too shallow:
// a `.text` SECTION can itself contain data. Steam Link's libmain.so
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
