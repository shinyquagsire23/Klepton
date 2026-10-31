// klepton-ld: translate an Android ARM64 ELF .so into a Mach-O dylib.
//
// This is the offline half of the loader. Today `kl_image.c` mmaps the guest
// ELF and makes its text RWX; on visionOS that is exactly the shape AMFI and
// library validation exist to refuse. So the guest text
// has to arrive as a *signed Mach-O*, mapped by dyld, and this tool is what
// produces it.
//
//
// The runtime finds the embedded image through the __TEXT,__klelf section, so
// no exported symbol — and therefore no exports trie — is needed. See
// kl_load_dylib() in kl_image.c for the other half.
//
//
// klepton-ld also handles x18 -> TLS patching due to macOS vs Android x18
// reservation differences -- macOS clears x18, Android doesn't, and TLS is
// easy enough to dereference in x18's place.
//
// Usage:
//   klepton-ld <input.so> -o <output.dylib> [--platform macos|visionos|
//              visionossim|ios|iossim] [--install-name NAME] [--quiet]
//              [--abi android|linux]
//
// --abi linux is for a glibc guest (see KLX_ABI_LINUX in kl_x18.h): the thread
// pointer and `svc` are veneered rather than TPIDR_EL0 being flipped, and the
// input may be a PIE executable or the dynamic loader as well as a library.

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include "../runtime/kl_x18.h"
#include "../runtime/guest/kl_guestpatch.h"
#include "../runtime/linux/kl_lx_name.h"

// ---------- ELF64 subset (macOS has no <elf.h>) ----------
typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr,
    p_filesz, p_memsz, p_align; } Elf64_Phdr;
typedef struct { uint32_t sh_name, sh_type; uint64_t sh_flags, sh_addr, sh_offset,
    sh_size; uint32_t sh_link, sh_info; uint64_t sh_addralign, sh_entsize; } Elf64_Shdr;

#define PT_LOAD       1
#define PF_X          1
#define PF_W          2
#define SHT_PROGBITS  1
#define SHF_EXECINSTR 0x4
#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2

#define PAGE 0x4000ull                 // 16 KB — Apple's page on arm64
#define HDR_RESERVE 0x4000ull          // room for the Mach-O header and load commands

static uint64_t align_up(uint64_t v, uint64_t a)   { return (v + a - 1) & ~(a - 1); }
static uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void die(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    fprintf(stderr, "klepton-ld: ");
    vfprintf(stderr, fmt, a);
    fprintf(stderr, "\n");
    va_end(a);
    exit(1);
}

// ---------- where a guest vaddr lives in this buffer (kl_guestpatch.h) ----------
// The runtime loader can subtract, because its mapping is contiguous and laid
// out by vaddr. This buffer is the FILE, so the PT_LOAD that contains the
// address is what converts one to the other. Only `p_filesz` counts: a vaddr in
// the .bss tail has no bytes here to patch, and answering NULL for it is the
// same "not in this image" the header already defines.
struct gp_ctx_shape { uint8_t *img; const Elf64_Phdr *ph; int n; };

// The image behind a chunk, for the `br x18` jump-table rewrite. It has to reach
// the TABLE, which lives in .rodata and so in a different section from the code
// being veneered. `shift` is the VA offset the Mach-O layout applies, because
// everything handed to kl_x18_emit is in the shifted (runtime) space while the
// section headers are in the ELF's own.
struct x18_img_ctx {
    uint8_t          *img;
    const Elf64_Shdr *sh;
    int               shnum;
    uint64_t          shift;
};

// Read-only ALLOC data only, and that restriction is load-bearing twice over: an
// executable section would let a mis-recognition write over code, and a WRITABLE
// one (.data.rel.ro) is rewritten by relocations at load, which would undo the
// repointing silently. A jump table is in .rodata.
static void *x18_data_at(void *vctx, uint64_t va, size_t len) {
    struct x18_img_ctx *c = vctx;
    uint64_t gva = va - c->shift;
    for (int i = 0; i < c->shnum; i++) {
        const Elf64_Shdr *s = &c->sh[i];
        if (s->sh_type != SHT_PROGBITS) continue;
        if (s->sh_flags & (SHF_EXECINSTR | SHF_WRITE)) continue;
        if (!(s->sh_flags & SHF_ALLOC) || !s->sh_addr) continue;
        if (gva < s->sh_addr || gva + len > s->sh_addr + s->sh_size) continue;
        return c->img + s->sh_offset + (gva - s->sh_addr);
    }
    return NULL;
}
static uint32_t *gp_at(void *ctx, uint64_t va) {
    struct gp_ctx_shape *c = (struct gp_ctx_shape *)ctx;
    for (int i = 0; i < c->n; i++) {
        if (c->ph[i].p_type != PT_LOAD) continue;
        if (va < c->ph[i].p_vaddr) continue;
        uint64_t off = va - c->ph[i].p_vaddr;
        if (off + 4 > c->ph[i].p_filesz) continue;
        return (uint32_t *)(c->img + c->ph[i].p_offset + off);
    }
    return NULL;
}

// ---------- the TLS rewrite, moved offline ----------
// Darwin keeps the thread pointer in TPIDRRO_EL0; TPIDR_EL0 is clobbered by the
// kernel on preemption, so a guest read of it is a wrong answer on every
// thread. The two encodings differ by exactly one bit. Instruction count is
// preserved, so nothing in the image moves.
// It consults kl_x18_is_data for the same reason the runtime loader does — a
// word of a constant table that happens to match this pattern would get a bit
// flipped — and, more importantly, so that the two passes AGREE. They did not:
// this one rewrote libunity.so+0x3f2118 and the loader refused it, so the same
// guest crashed as an ELF on the host and ran as a dylib on device, which reads
// like a platform difference and is not one. A refusal is named, because a TLS
// site left alone reads a register the kernel clobbers, in whatever runs the
// output (see kl_image.c).
static unsigned rewrite_tls(uint8_t *p, size_t n, uint64_t va, unsigned *refused) {
    unsigned hits = 0;
    uint32_t *w = (uint32_t *)p;
    size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        if ((w[i] & 0xffffffe0u) != 0xd53bd040u) continue;
        if (kl_x18_is_data(w, n, i)) {
            (*refused)++;
            fprintf(stderr, "klepton-ld: TLS site at +0x%llx left alone — its "
                            "neighbourhood reads as data. If it IS code, "
                            "that thread pointer is garbage\n",
                    (unsigned long long)(va + i * 4));
            continue;
        }
        w[i] |= 0x20u;
        hits++;
    }
    return hits;
}

// x18 sites are found with the real decoder, never a bit-field guess.
// Which fields of an encoding are general-purpose registers — as opposed to
// immediates, condition codes or vector registers — is exactly what klx_decode
// exists to decide, and guessing gets it wrong in the unsafe direction: a naive
// "does any 5-bit field equal 18" scan reports 2158 sites in libunityopus.so,
// where the decoder correctly reports 0. kl_x18_count/kl_x18_emit are the same
// code the runtime loader uses, which matters because `make x18` and
// `make guest` can only validate one implementation.

// Can every site in [lo, hi) branch to every byte of a pool in [plo, phi)?
// Both extremes, both directions — the same +/-128 MB `b` window kl_x18.c's
// out_of_reach() tests one site at a time, asked here about a whole region so
// that a pool is CHOSEN in reach rather than discovered out of it. Measured
// against the pool's reserved extent, not its used one, so a pool that grows
// after the choice was made is still within what was checked. Used only by the
// SPLIT placement below.
static int reaches(uint64_t lo, uint64_t hi, uint64_t plo, uint64_t phi) {
    #define B_WINDOW (1ll << 27)
    int64_t most_neg = (int64_t)plo - (int64_t)hi;   // lowest pool byte, highest site
    int64_t most_pos = (int64_t)phi - (int64_t)lo;   // highest pool byte, lowest site
    return most_neg >= -B_WINDOW && most_pos < B_WINDOW;
}

struct platform { const char *name; uint32_t id; uint32_t minos, sdk; };
static const struct platform PLATFORMS[] = {
    { "macos",       PLATFORM_MACOS,              0x0e0000, 0x0e0000 },  // 14.0
    { "ios",         PLATFORM_IOS,                0x110000, 0x110000 },  // 17.0
    { "iossim",      PLATFORM_IOSSIMULATOR,       0x110000, 0x110000 },
    { "visionos",    11 /* PLATFORM_VISIONOS */,  0x1a0000, 0x1a0000 },  // 26.0
    { "visionossim", 12 /* ...SIMULATOR */,       0x1a0000, 0x1a0000 },
};

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL, *install_name = NULL;
    const struct platform *plat = &PLATFORMS[0];
    int quiet = 0;

    // --print-name <elf>: the file name the Linux runtime will look for this
    // ELF's translation under (kl_lx_name.h). Nothing is translated.
    if (argc == 3 && !strcmp(argv[1], "--print-name")) {
        char name[512];
        int nfd = open(argv[2], O_RDONLY);
        if (nfd < 0) die("open %s: %s", argv[2], strerror(errno));
        kl_lx_translation_name(argv[2], nfd, name, sizeof name);
        puts(name);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--install-name") && i + 1 < argc) install_name = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--abi") && i + 1 < argc) {
            const char *w = argv[++i];
            if (!strcmp(w, "linux")) kl_x18_set_abi(KLX_ABI_LINUX);
            else if (strcmp(w, "android")) die("unknown abi '%s'", w);
        }
        else if (!strcmp(argv[i], "--platform") && i + 1 < argc) {
            const char *w = argv[++i];
            plat = NULL;
            for (size_t k = 0; k < sizeof PLATFORMS / sizeof *PLATFORMS; k++)
                if (!strcmp(PLATFORMS[k].name, w)) plat = &PLATFORMS[k];
            if (!plat) die("unknown platform '%s'", w);
        }
        else if (argv[i][0] == '-') die("unknown option '%s'", argv[i]);
        else in = argv[i];
    }
    if (!in || !out) die("usage: klepton-ld <input.so> -o <output.dylib> "
                         "[--platform macos|visionos|visionossim|ios|iossim] "
                         "[--install-name NAME]");

    // ---- read the ELF ----
    int fd = open(in, O_RDONLY);
    if (fd < 0) die("open %s: %s", in, strerror(errno));
    struct stat sb;
    if (fstat(fd, &sb) != 0) die("fstat: %s", strerror(errno));
    uint8_t *f = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (f == MAP_FAILED) die("mmap %s: %s", in, strerror(errno));

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)f;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) || eh->e_ident[4] != 2)
        die("%s is not an ELF64 file", in);
    if (eh->e_machine != 183) die("%s is not AArch64 (e_machine=%u)", in, eh->e_machine);

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(f + eh->e_phoff);

    // ---- group the PT_LOADs by writability: one Mach-O segment per group ----
    // Every guest library here has exactly one r-x LOAD and one rw LOAD, but the
    // grouping is general so a third does not silently land in the wrong place.
    struct group { int writable; uint64_t vlo, vhi_file, vhi_mem; } grp[8];
    int ngrp = 0;
    uint64_t lo = UINT64_MAX;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
    }
    if (lo == UINT64_MAX) die("%s has no PT_LOAD", in);
    if (lo != 0) die("%s: first PT_LOAD is at vaddr %#llx, expected 0", in,
                     (unsigned long long)lo);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        int w = (ph[i].p_flags & PF_W) != 0;
        if (ngrp && grp[ngrp - 1].writable == w) {
            struct group *g = &grp[ngrp - 1];
            if (ph[i].p_vaddr + ph[i].p_filesz > g->vhi_file) g->vhi_file = ph[i].p_vaddr + ph[i].p_filesz;
            if (ph[i].p_vaddr + ph[i].p_memsz  > g->vhi_mem)  g->vhi_mem  = ph[i].p_vaddr + ph[i].p_memsz;
        } else {
            if (ngrp == 8) die("more PT_LOAD groups than expected");
            grp[ngrp].writable = w;
            grp[ngrp].vlo      = ph[i].p_vaddr;
            grp[ngrp].vhi_file = ph[i].p_vaddr + ph[i].p_filesz;
            grp[ngrp].vhi_mem  = ph[i].p_vaddr + ph[i].p_memsz;
            ngrp++;
        }
    }

    // ---- the text rewrites ----
    // Executable *sections*, not the executable segment: the r-x LOAD starts at
    // file offset 0 and also spans .rodata, .rela.dyn and .eh_frame, and
    // rewriting a word that merely looks like an instruction in there would
    // corrupt data. Work on a private copy of the file.
    uint8_t *img = malloc((size_t)sb.st_size);
    if (!img) die("out of memory");
    memcpy(img, f, (size_t)sb.st_size);

    unsigned tls_rewrites = 0, tls_refused = 0, x18_sites = 0, x18_termbr = 0;
    const Elf64_Shdr *sh = (eh->e_shoff && eh->e_shnum)
                         ? (const Elf64_Shdr *)(f + eh->e_shoff) : NULL;
    if (!sh) die("%s has no section headers — cannot separate code from rodata "
                 "; refusing to rewrite", in);

    // ...and code inside those sections, not whole sections: a `.text` can hold
    // constant tables (Steam Link's libmain.so has 148 KB of BoringSSL's P-256
    // tables in there, which account for 1059 of its 1080 apparent x18 sites).
    // Same ranges the runtime loader skips — kl_x18.c owns the answer so the
    // offline and load-time passes cannot drift apart.
    static klx_range skip[256];
    unsigned nskip = kl_x18_data_ranges(f, (size_t)sb.st_size, skip,
                                        sizeof skip / sizeof *skip);

    // Pass 1: the TLS rewrite, and a site count so the veneer pool can be sized
    // before the layout is chosen. TLS first, then count — the same order the
    // runtime loader uses, and it matters: `mrs x18, tpidr_el0` is itself an x18
    // site, and rewriting it changes the word the decoder then sees.
    // ...and the SPAN of the executable sections, because it is what decides
    // where the veneer pool can go. See the placement below.
    uint64_t text_lo = UINT64_MAX, text_hi = 0;
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_PROGBITS || !(sh[i].sh_flags & SHF_EXECINSTR)) continue;
        if (sh[i].sh_addr < text_lo) text_lo = sh[i].sh_addr;
        if (sh[i].sh_addr + sh[i].sh_size > text_hi) text_hi = sh[i].sh_addr + sh[i].sh_size;
        uint64_t cursor = sh[i].sh_addr, cva;
        size_t csz;
        while (kl_x18_next_code(sh[i].sh_addr, (size_t)sh[i].sh_size,
                                skip, nskip, &cursor, &cva, &csz)) {
            uint8_t *p = img + sh[i].sh_offset + (cva - sh[i].sh_addr);
            // The flip is the Android ABI's answer to TPIDR_EL0. Under the
            // Linux ABI those reads are veneer sites, counted just below.
            if (kl_x18_abi() != KLX_ABI_LINUX)
                tls_rewrites += rewrite_tls(p, csz, cva, &tls_refused);
            x18_sites    += kl_x18_count(p, csz);
            x18_termbr   += kl_x18_count_termbr(p, csz);
        }
    }
    if (text_lo == UINT64_MAX) text_lo = text_hi = 0;

    // ---- lay out the Mach-O ----
    // The veneer pool lives *before* the guest image, inside __TEXT, which is
    // why the image shift is computed rather than fixed: S grows to make room.
    // Putting it there rather than in a segment of its own keeps the file to two
    // loadable segments and keeps every `b` well inside its +/-128 MB reach —
    // the furthest any site can be from the pool is the image's own span, and
    // the largest guest library here is libil2cpp at 57 MB.
    uint64_t pool_va   = HDR_RESERVE;                       // right after the header
    // ...plus the jump-table rewrite's worst case per `br x18`. That is the one
    // site shape whose veneer grows with the guest — a thunk per table target —
    // and it is a handful of sites per library, so budgeting the maximum costs
    // nothing worth measuring. Running short is not a failure, only a `br x18`
    // that keeps the guarded veneer and its residual window.
    uint64_t pool_cap  = (uint64_t)x18_sites * KLX_VEN_MAX_INSN * 4
                       + (uint64_t)x18_termbr * KLX_JT_MAX_INSN * 4;
    uint64_t image_shift = align_up(pool_va + pool_cap, PAGE);

    // ...and then the shift is CHOSEN, not merely made large enough, because a
    // 4 KB-aligned guest puts the writable bytes less than a host page above
    // the executable ones.
    //
    // Beat Saber 1.40 ships p_align 4096 and four PT_LOADs (r--, r-x, rw-, rw-)
    // with a 4 KB gap between the last read-only byte and the first writable
    // one — libmain 0x1f00 -> 0x2f00, libunity 0x130e430 -> 0x130f430. Group
    // them by writability and __TEXT's end rounds UP through that gap while
    // __DATA's start rounds DOWN through it, so the two segments overlap and no
    // amount of extra shift helps: S is added to both, so it moves them
    // together. What it does move is where the 16 KB boundaries FALL, and
    // kl_image.c says the same thing from the other side, about the mprotect
    // version of this collision: "the only general way out is to place the
    // image so that a host page boundary falls in the GAP between the
    // executable segment and the writable bytes ... a property of the mapping
    // rather than of the protections."
    //
    // So move the shift until a boundary lands in the gap — in steps of 4 KB,
    // and that step is the whole subtlety.
    //
    // **The shift must stay a multiple of 4096 whatever else it does**, because
    // `adrp` resolves against a 4 KB page: the linker encoded every
    // PC-relative reference in this guest as a delta between the 4 KB page of
    // the instruction and the 4 KB page of its target, and that delta survives
    // relocation only if the whole image moves by a multiple of 4096. It cost a
    // run to find out the interesting way: a shift of 0x5100 places the segments
    // perfectly, produces a byte-faithful copy (one byte differs, the TLS
    // rewrite), relocates identically to the ELF path — 20 relocations, same
    // four kinds — and then libmain's JNI_OnLoad reads its own static table
    // through an adrp/add pair that is now a page out and calls FindClass("").
    // Nothing about that names the shift.
    //
    // Stepping by 4096 through one host page is exhaustive: if no offset in
    // [0, PAGE) works, no shift does, since the layout repeats mod PAGE.
    // For a guest aligned to 16 KB or more (1.28 at 64 KB, Steam Link at 16 KB)
    // the first candidate is the old answer, so nothing about those changes.
    #define ADRP_PAGE 0x1000ull
    uint64_t first = image_shift;
    for (uint64_t try = 0; ; try += ADRP_PAGE) {
        if (try >= PAGE) {
            image_shift = first;         // no offset works; let the layout below
            break;                       // report which gap is too narrow
        }
        uint64_t s = first + try, prev_end = 0;
        int ok = 1;
        for (int g = 0; g < ngrp && ok; g++) {
            uint64_t start = (g == 0) ? 0 : align_down(s + grp[g].vlo, PAGE);
            if (g && start < prev_end) ok = 0;
            prev_end = align_up(s + grp[g].vhi_mem, PAGE);
        }
        if (ok) { image_shift = s; break; }
    }

    // ---- the segment layout, which the pool placement below needs -----------
    // Depends on nothing but `image_shift` and the PT_LOAD groups, so it is
    // computed here rather than after the rewrites: a pool that goes ABOVE the
    // image has to know where the image ends before the veneers are emitted,
    // because every veneer's address is baked into the branch that reaches it.
    uint64_t seg_vm[10], seg_vmend[10], seg_foff[10], seg_fsize[10];
    for (int g = 0; g < ngrp; g++) {
        uint64_t want = image_shift + grp[g].vlo;
        seg_vm[g] = (g == 0) ? 0 : align_down(want, PAGE);
        if (g && seg_vm[g] < seg_vmend[g - 1])
            // The shift above places a page boundary at the FIRST writable
            // group, which settles every library in this corpus. A third group
            // that needs one too cannot have it — one shift, one boundary — so
            // say which gap is too narrow rather than blaming the shift.
            die("segment %d starts %#llx after the previous one ends, which is "
                "less than the %#llx host page: no image shift can put a page "
                "boundary between them", g,
                (unsigned long long)(grp[g].vlo - grp[g - 1].vhi_mem),
                (unsigned long long)PAGE);
        seg_vmend[g] = align_up(image_shift + grp[g].vhi_mem, PAGE);
    }
    // Make them gapless: each segment runs up to the next one's start.
    for (int g = 0; g + 1 < ngrp; g++) seg_vmend[g] = seg_vm[g + 1];

    for (int g = 0; g < ngrp; g++) {
        seg_foff[g]  = (g == 0) ? 0 : seg_foff[g - 1] + seg_fsize[g - 1];
        seg_foff[g]  = align_up(seg_foff[g], PAGE);
        // File content stops at the end of initialised data; the rest is .bss,
        // which dyld supplies as zero pages beyond filesize.
        seg_fsize[g] = (image_shift + grp[g].vhi_file) - seg_vm[g];
        if (g == 0) seg_fsize[g] = seg_vmend[0];   // __TEXT: file covers the whole segment
    }

    // ---- WHERE the pool goes, which is decided by REACH ---------------------
    // The patch at each site is a single `b` — it has to be, because the veneer
    // pass preserves instruction count — and `b` spans +/-128 MB. So the pool
    // must be within 128 MB of every site, and the only two places that do not
    // disturb the guest's own layout are below the image and above it. (Nothing
    // may go BETWEEN: a `b` could be re-encoded but an `adrp` in the guest's own
    // text could not, so the text-to-data delta is fixed.)
    //
    // Below was the only choice until 2026-08-15, under "the largest guest
    // library here is libil2cpp at 57 MB". **libUE4.so is 172 MB**, its `.text`
    // runs from 0x54ce000 to 0x9c53900, and every one of its 8038 x18 sites is
    // in a 5.2 MB band at the very top — 151 MB above a pool at 0x4000. So all
    // 8038 were refused, silently as far as any gate here was concerned, and RE4
    // ran on the HOST (where kl_image.c places its pool next to the code) and
    // could not boot on a headset, where a klepton-ld dylib is the only path.
    // Refused veneers are not a crash at a named place: they are a guest
    // reading Darwin's reserved x18.
    //
    // The reach is measured against the executable sections' span rather than
    // against the sites, because it has to hold for a site anywhere in them.
    //
    // Then Deadpool VR's libUE4.so arrived at 262 MB with a 128.0 MB `.text`, and
    // ONE pool stopped being enough in either place: 239.9 MB below, 150.2 MB
    // above. That library — and only a library no single pool can serve — is
    // translated in SPLIT mode: both pools exist at once (the __TEXT one below
    // the image, the __KLX18 one above it), executable chunks are sliced at
    // 32 MB, and each slice takes its veneers from whichever pool reaches it,
    // HALVING a slice neither reaches until one does (down to 64 KB, fatal only
    // if that slice still has x18 sites). kl_image.c slices the same way at
    // load time with its pre-image and end-of-image arenas.
    //
    // Every library a single pool CAN serve keeps the single-pool path below
    // exactly as it was — same pool, one emit per chunk — so its output is
    // byte-identical to what this tool produced before SPLIT mode existed.
    #define BR_REACH (1ull << 27)                 // +/-128 MB, `b`'s imm26 << 2
    #define SLICE_MAX (32ull << 20)               // kl_image.c slices the same
    #define SLICE_MIN (64ull << 10)               // ...and stops halving here
    uint64_t last_end = seg_vmend[ngrp - 1];
    int pool_high = 0, pool_split = 0;
    if (pool_cap) {
        // Below: the furthest site is the top of .text, reaching down to the
        // pool's first byte.
        uint64_t down = (image_shift + text_hi) - HDR_RESERVE;
        // Above: the pool sits past the last segment, and the furthest site is
        // the BOTTOM of .text reaching up to the pool's last byte.
        uint64_t up = align_up(last_end, PAGE) + pool_cap - (image_shift + text_lo);
        if (down >= BR_REACH && up < BR_REACH) pool_high = 1;
        else if (down >= BR_REACH) {
            pool_split = 1;
            fprintf(stderr, "klepton-ld: %s: .text spans %#llx..%#llx (%.1f MB) and "
                            "no single veneer pool reaches all of it (%.1f MB below, "
                            "%.1f MB above) — SPLIT placement, a pool per region\n",
                    in, (unsigned long long)text_lo, (unsigned long long)text_hi,
                    (double)(text_hi - text_lo) / (1 << 20),
                    (double)down / (1 << 20), (double)up / (1 << 20));
        }
    }
    if (pool_high) pool_va = align_up(last_end, PAGE);

    uint8_t *pool = NULL;
    if (pool_cap) { pool = calloc(1, (size_t)pool_cap); if (!pool) die("out of memory"); }
    // SPLIT mode's second pool, above the image. Sized for the WHOLE site
    // population, like the low one, because the share is not known until the
    // slices have been assigned; only the bytes used are written out.
    uint64_t low_va  = HDR_RESERVE;               // reserved by image_shift above
    uint64_t high_va = align_up(last_end, PAGE);  // past the last guest segment
    uint8_t *hpool = NULL;
    uint64_t hpool_off = 0;
    if (pool_split) { hpool = calloc(1, (size_t)pool_cap); if (!hpool) die("out of memory"); }

    // Pass 2: emit. Each section's veneers take the next slice of the pool, and
    // the addresses handed in are the ones the code will have once mapped — the
    // buffers here are a file image, not a mapping.
    kl_x18_stats x18st = {0};
    uint64_t pool_off = 0;
    unsigned halvings = 0, halvings_odd = 0;   // SPLIT slice halvings, and how many hit an odd-word tail
    struct x18_img_ctx ictx = { img, sh, eh->e_shnum, image_shift };
    kl_x18_image ximg = { x18_data_at, &ictx };
    for (int i = 0; i < eh->e_shnum && pool_cap; i++) {
        if (sh[i].sh_type != SHT_PROGBITS || !(sh[i].sh_flags & SHF_EXECINSTR)) continue;
        uint64_t cursor = sh[i].sh_addr, cva;
        size_t csz;
        while (kl_x18_next_code(sh[i].sh_addr, (size_t)sh[i].sh_size,
                                skip, nskip, &cursor, &cva, &csz)) {
            uint8_t *chunk = img + sh[i].sh_offset + (cva - sh[i].sh_addr);
            for (size_t off = 0; off < csz; ) {
                // Single-pool mode: the whole chunk in one emit, as always.
                size_t slice = csz - off;
                uint8_t *pbuf = pool; uint64_t *pused = &pool_off, pva = pool_va;
                uint64_t slo = image_shift + cva + off;
                if (pool_split) {
                    if (slice > SLICE_MAX) slice = SLICE_MAX;
                    pbuf = NULL; pused = NULL; pva = 0;
                    while (1) {
                        uint64_t shi = slo + slice;
                        if (reaches(slo, shi, low_va, low_va + pool_cap)) {
                            pbuf = pool;  pused = &pool_off;  pva = low_va;  break;
                        }
                        if (reaches(slo, shi, high_va, high_va + pool_cap)) {
                            pbuf = hpool; pused = &hpool_off; pva = high_va; break;
                        }
                        if (slice > SLICE_MIN) {
                            // Keep slices whole instructions: an odd-word tail
                            // halved plainly leaves the next slice mid-insn.
                            size_t half = slice / 2;
                            halvings++; if (half & 3) halvings_odd++;
                            slice = half & ~(size_t)3;
                            continue;
                        }
                        // The middle band of an image wider than two `b`
                        // windows, narrowed as far as the halving goes. Only
                        // fatal if there is anything in it to veneer.
                        if (kl_x18_count(chunk + off, slice) == 0) break;
                        die("%s: the code at %#llx..%#llx has x18 sites and neither "
                            "veneer pool is within `b`'s +/-128 MB of it — %.1f MB "
                            "below (pool at %#llx), %.1f MB above (pool at %#llx). "
                            "This image is wider than two `b` windows and the middle "
                            "band cannot be served from either end.",
                            in, (unsigned long long)(cva + off),
                            (unsigned long long)(cva + off + slice),
                            (double)(shi - low_va) / (1 << 20),
                            (unsigned long long)low_va,
                            (double)(high_va + pool_cap - slo) / (1 << 20),
                            (unsigned long long)high_va);
                    }
                    if (!pbuf) { off += slice; continue; }   // nothing here to serve
                }
                kl_x18_stats st;
                size_t used = 0;
                if (kl_x18_emit_img(chunk + off, slice, slo,
                                    pbuf + *pused, (size_t)(pool_cap - *pused),
                                    pva + *pused, &ximg, &st, &used) != 0)
                    die("x18 veneer emission failed for section %d", i);
                x18st.sites       += st.sites;
                x18st.patched     += st.patched;
                x18st.refused     += st.refused;
                x18st.ctr_sites   += st.ctr_sites;
                x18st.ctr_patched += st.ctr_patched;
                x18st.ctr_refused += st.ctr_refused;
                x18st.jt_sites    += st.jt_sites;
                x18st.jt_thunks   += st.jt_thunks;
                x18st.jt_fallback += st.jt_fallback;
                // ...and the Linux ABI's populations (the Alyx work in this fork).
                x18st.tp_sites    += st.tp_sites;
                x18st.tp_patched  += st.tp_patched;
                x18st.svc_sites   += st.svc_sites;
                x18st.svc_patched += st.svc_patched;
                x18st.lx_refused  += st.lx_refused;
                x18st.far_refused += st.far_refused;
                *pused            += used;
                off               += slice;
            }
        }
    }
    if (halvings)
        fprintf(stderr, "[klepton-ld] %s: slice halved %u times (%u from an odd-word tail, re-aligned)\n",
                in, halvings, halvings_odd);
    if (x18st.refused) {
        // Same posture as the runtime loader: a refused site keeps its raw x18
        // reads Darwin's, so it is reported rather than hidden.
        fprintf(stderr, "klepton-ld: %s: %u of %u x18 sites refused — these "
                        "read Darwin's reserved x18:\n", in, x18st.refused, x18st.sites);
        kl_x18_report(stderr);
    }
    // The jump-table rewrite, announced whenever there is a `br x18` at all —
    // both halves of it. A CONVERTED site no longer branches through x18 and so
    // cannot be killed by the kernel zeroing it; a FALLBACK site still can, and
    // on device that is a SIGKILL with no report, so it must never be a silent
    // zero.
    if (x18st.jt_sites || x18st.jt_fallback)
        fprintf(stderr, "klepton-ld: %s: br x18 — %u converted to thunk dispatch "
                        "(%u targets), %u left on the guarded veneer%s\n",
                in, x18st.jt_sites, x18st.jt_thunks, x18st.jt_fallback,
                x18st.jt_fallback ? " (those keep the residual x18 window)" : "");
    // The CTR_EL0 half is louder than the x18 half because the failure is
    // immediate: a
    // `mrs Xt, CTR_EL0` left alone is SIGILL the first time it executes, not a
    // wrong value some time later. Announced even when it worked, because one
    // instruction in one library is exactly the kind of thing that goes missing
    // when the guest is re-translated by a different build.
    if (x18st.ctr_sites)
        fprintf(stderr, "klepton-ld: %s: %u CTR_EL0 read(s), %u veneered to "
                        "answer %#llx%s\n", in, x18st.ctr_sites,
                x18st.ctr_patched, (unsigned long long)kl_x18_ctr_value(),
                x18st.ctr_refused ? " — SOME REFUSED, they will SIGILL" : "");

    // A refused thread-pointer or svc site is not survivable either way: one
    // reads a register the kernel clobbers, the other enters Darwin's syscall
    // table with a Linux number. Named, with the encodings, like the x18 ones.
    if (x18st.lx_refused) {
        fprintf(stderr, "klepton-ld: %s: %u thread-pointer/svc site(s) REFUSED:\n",
                in, x18st.lx_refused);
        kl_x18_report(stderr);
    }

    // ---- the measured guest patches (kl_guestpatch.c) ----
    // The same table the runtime loader applies, at the same point in the same
    // order — after the TLS and x18 rewrites, so both passes see identical
    // words and an `expect` measured on one path can never be wrong on the
    // other. This is the OFFLINE half, and it is the only half a headset ever
    // gets: a klepton-ld dylib's __TEXT is signed, so kl_image.c cannot write
    // it there. Without this a device run behaves as `KL_GUEST_PATCH=0` while
    // the host does not, which is the worst kind of difference — one that only
    // shows up on the hardware.
    struct gp_ctx_shape gpc = { img, ph, eh->e_phnum };
    unsigned patched = kl_guest_patch_apply(in, gp_at, &gpc);
    (void)patched;

    // Where the pool's bytes live in the FILE, and where __LINKEDIT then starts.
    // With the pool below the image both are what they always were: __TEXT
    // starts at vmaddr 0 and fileoff 0, so the pool's file offset is its address.
    //
    // From here on the pools are named by WHERE they are: the low one (in
    // __TEXT) and the high one (__KLX18). Single-pool mode fills exactly one of
    // the two, so its output is what it always was; SPLIT mode may fill both.
    uint8_t *low  = pool_high ? NULL : pool;
    uint64_t low_off  = pool_high ? 0 : pool_off;
    uint8_t *high = pool_high ? pool : hpool;
    uint64_t high_off = pool_high ? pool_off : hpool_off;
    if (pool_high) high_va = pool_va;
    uint64_t low_foff  = seg_foff[0] + low_va;
    uint64_t high_foff = 0;
    uint64_t after_vm  = seg_vmend[ngrp - 1];
    uint64_t after_foff = align_up(seg_foff[ngrp - 1] + seg_fsize[ngrp - 1], PAGE);
    if (high_off) {
        high_foff  = after_foff;
        after_vm   = align_up(high_va + high_off, PAGE);
        after_foff = align_up(high_foff + high_off, PAGE);
    }
    uint64_t linkedit_vm   = after_vm;
    uint64_t linkedit_foff = after_foff;

    // ---- load commands ----
    char iname[512];
    const char *stem = strrchr(in, '/'); stem = stem ? stem + 1 : in;
    char base[256]; snprintf(base, sizeof base, "%s", stem);
    char *dot = strstr(base, ".so"); if (dot) *dot = 0;
    if (install_name) snprintf(iname, sizeof iname, "%s", install_name);
    else snprintf(iname, sizeof iname, "@rpath/%s.framework/%s", base, base);

    const char *libsystem = "/usr/lib/libSystem.B.dylib";
    // __TEXT carries the guest image (__klelf), the veneer pool (__klx18, absent
    // when the library has no x18 sites), and a small record of what this
    // translation did (__klstat) parked at the top of the header reserve.
    //
    // ...unless the pool had to go ABOVE the image to be in `b`'s reach (see the
    // placement), in which case it is a segment of its own — __KLX18, r-x, after
    // __DATA. A third segment rather than an extension of __TEXT because the
    // guest's own text-to-data delta is fixed by its `adrp`s: nothing of ours
    // may be inserted between them.
    // SPLIT mode (a library wider than two `b` windows) can carry both: either
    // pool may be empty.
    uint32_t nsect_text = 2 + (low_off ? 1 : 0);
    int      have_x18_seg = high_off != 0;
    uint64_t stat_va = HDR_RESERVE - 64;
    uint32_t sz_text = sizeof(struct segment_command_64) + nsect_text * sizeof(struct section_64);
    uint32_t sz_x18  = have_x18_seg
                     ? sizeof(struct segment_command_64) + sizeof(struct section_64) : 0;
    uint32_t sz_data = sizeof(struct segment_command_64) + sizeof(struct section_64);
    uint32_t sz_link = sizeof(struct segment_command_64);
    uint32_t sz_id   = (uint32_t)align_up(sizeof(struct dylib_command) + strlen(iname) + 1, 8);
    uint32_t sz_ld   = (uint32_t)align_up(sizeof(struct dylib_command) + strlen(libsystem) + 1, 8);
    uint32_t sz_bv   = sizeof(struct build_version_command);
    uint32_t sz_enc  = sizeof(struct encryption_info_command_64);
    uint32_t sz_st   = sizeof(struct symtab_command);
    uint32_t sz_dst  = sizeof(struct dysymtab_command);
    uint32_t sz_uuid = sizeof(struct uuid_command);

    // LC_ENCRYPTION_INFO_64 goes on DEVICE binaries only: the App Store requires it
    // (error 90125 without), but the simulator loader rejects a binary that carries
    // it. Keyed on the platform name so device/sim each get the right shape.
    int enc = (strstr(plat->name, "sim") == NULL);
    uint32_t ncmds = 3 /* segments */ + 1 /* id */ + 1 /* load libSystem */ +
                     1 /* build version */ + 1 /* symtab */ + 1 /* dysymtab */ + 1 /* uuid */;
    if (enc) ncmds++;                            /* encryption info */
    // __DATA is only present if the library has a writable group.
    if (ngrp < 2) { ncmds--; sz_data = 0; }
    if (have_x18_seg) ncmds++;
    uint32_t sizeofcmds = sz_text + sz_x18 + sz_data + sz_link + sz_id + sz_ld + sz_bv +
                          sz_st + sz_dst + sz_uuid;
    if (enc) sizeofcmds += sz_enc;
    if (sizeof(struct mach_header_64) + sizeofcmds > HDR_RESERVE)
        die("load commands (%u bytes) do not fit below the image shift %#llx",
            sizeofcmds, (unsigned long long)HDR_RESERVE);

    // __LINKEDIT holds a minimal (empty) symbol table. There is nothing for
    // dyld to bind — MH_NOUNDEFS — and nothing to export, because the runtime
    // finds the image through its section rather than through dlsym.
    const uint32_t strsize = 8;
    uint64_t linkedit_size = strsize;
    uint64_t total = linkedit_foff + linkedit_size;

    uint8_t *o = calloc(1, (size_t)total);
    if (!o) die("out of memory");

    struct mach_header_64 *mh = (struct mach_header_64 *)o;
    mh->magic      = MH_MAGIC_64;
    mh->cputype    = CPU_TYPE_ARM64;
    mh->cpusubtype = CPU_SUBTYPE_ARM64_ALL;
    mh->filetype   = MH_DYLIB;
    mh->ncmds      = ncmds;
    mh->sizeofcmds = sizeofcmds;
    mh->flags      = MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL;

    uint8_t *lc = o + sizeof *mh;

    // __TEXT
    {
        struct segment_command_64 *s = (struct segment_command_64 *)lc;
        s->cmd = LC_SEGMENT_64; s->cmdsize = sz_text;
        strncpy(s->segname, "__TEXT", 16);
        s->vmaddr = seg_vm[0]; s->vmsize = seg_vmend[0] - seg_vm[0];
        s->fileoff = seg_foff[0]; s->filesize = seg_fsize[0];
        s->maxprot = s->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
        s->nsects = nsect_text; s->flags = 0;
        struct section_64 *sec = (struct section_64 *)(s + 1);

        strncpy(sec->sectname, "__klstat", 16);
        strncpy(sec->segname,  "__TEXT",   16);
        sec->addr   = stat_va;
        sec->size   = sizeof(klx_stat_section);
        sec->offset = (uint32_t)(seg_foff[0] + (stat_va - seg_vm[0]));
        sec->align  = 3;
        sec->flags  = S_REGULAR;
        sec++;

        if (low_off) {
            strncpy(sec->sectname, "__klx18", 16);
            strncpy(sec->segname,  "__TEXT",  16);
            sec->addr   = low_va;
            sec->size   = low_off;
            sec->offset = (uint32_t)(seg_foff[0] + (low_va - seg_vm[0]));
            sec->align  = 2;
            sec->flags  = S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
            sec++;
        }

        strncpy(sec->sectname, "__klelf", 16);
        strncpy(sec->segname,  "__TEXT",  16);
        sec->addr = image_shift + grp[0].vlo;
        sec->size = grp[0].vhi_file - grp[0].vlo;
        sec->offset = (uint32_t)(seg_foff[0] + (sec->addr - seg_vm[0]));
        // What the address IS, not what it used to be: the shift is chosen to
        // put a page boundary at the writable group, so on a 4 KB-aligned guest
        // this section is no longer 16 KB aligned and claiming 14 would be a
        // field that disagrees with the addr beside it.
        sec->align = 0;
        for (uint64_t a = sec->addr; a && !(a & 1) && sec->align < 14; a >>= 1)
            sec->align++;
        sec->flags = S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
        lc += sz_text;
    }
    // __DATA
    if (ngrp >= 2) {
        struct segment_command_64 *s = (struct segment_command_64 *)lc;
        s->cmd = LC_SEGMENT_64; s->cmdsize = sz_data;
        strncpy(s->segname, "__DATA", 16);
        s->vmaddr = seg_vm[1]; s->vmsize = seg_vmend[1] - seg_vm[1];
        s->fileoff = seg_foff[1]; s->filesize = seg_fsize[1];
        s->maxprot = s->initprot = VM_PROT_READ | VM_PROT_WRITE;
        s->nsects = 1; s->flags = 0;
        struct section_64 *sec = (struct section_64 *)(s + 1);
        strncpy(sec->sectname, "__kldata", 16);
        strncpy(sec->segname,  "__DATA",   16);
        sec->addr = image_shift + grp[1].vlo;
        sec->size = grp[1].vhi_file - grp[1].vlo;
        sec->offset = (uint32_t)(seg_foff[1] + (sec->addr - seg_vm[1]));
        sec->align = 4;
        sec->flags = S_REGULAR;
        lc += sz_data;
    }
    // __KLX18 — the veneer pool, when it had to go above the image to be within
    // `b`'s reach of the code. Executable and read-only, like __TEXT: these are
    // real instructions the guest branches into and returns from.
    if (have_x18_seg) {
        struct segment_command_64 *s = (struct segment_command_64 *)lc;
        s->cmd = LC_SEGMENT_64; s->cmdsize = sz_x18;
        strncpy(s->segname, "__KLX18", 16);
        s->vmaddr = high_va; s->vmsize = align_up(high_off, PAGE);
        s->fileoff = high_foff; s->filesize = high_off;
        s->maxprot = s->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
        s->nsects = 1; s->flags = 0;
        struct section_64 *sec = (struct section_64 *)(s + 1);
        strncpy(sec->sectname, "__klx18", 16);
        strncpy(sec->segname,  "__KLX18", 16);
        sec->addr   = high_va;
        sec->size   = high_off;
        sec->offset = (uint32_t)high_foff;
        sec->align  = 2;
        sec->flags  = S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
        lc += sz_x18;
    }
    // __LINKEDIT
    {
        struct segment_command_64 *s = (struct segment_command_64 *)lc;
        s->cmd = LC_SEGMENT_64; s->cmdsize = sz_link;
        strncpy(s->segname, "__LINKEDIT", 16);
        s->vmaddr = linkedit_vm; s->vmsize = align_up(linkedit_size, PAGE);
        s->fileoff = linkedit_foff; s->filesize = linkedit_size;
        s->maxprot = s->initprot = VM_PROT_READ;
        s->nsects = 0; s->flags = 0;
        lc += sz_link;
    }
    // LC_ID_DYLIB
    {
        struct dylib_command *d = (struct dylib_command *)lc;
        d->cmd = LC_ID_DYLIB; d->cmdsize = sz_id;
        d->dylib.name.offset = sizeof(struct dylib_command);
        d->dylib.timestamp = 1;
        d->dylib.current_version = d->dylib.compatibility_version = 0x10000;
        memcpy(lc + sizeof(struct dylib_command), iname, strlen(iname) + 1);
        lc += sz_id;
    }
    // LC_LOAD_DYLIB libSystem — nothing is bound against it (MH_NOUNDEFS), but
    // dyld expects a dylib to declare its dependency on libSystem.
    {
        struct dylib_command *d = (struct dylib_command *)lc;
        d->cmd = LC_LOAD_DYLIB; d->cmdsize = sz_ld;
        d->dylib.name.offset = sizeof(struct dylib_command);
        d->dylib.timestamp = 1;
        d->dylib.current_version = d->dylib.compatibility_version = 0x10000;
        memcpy(lc + sizeof(struct dylib_command), libsystem, strlen(libsystem) + 1);
        lc += sz_ld;
    }
    // LC_BUILD_VERSION — the one field that differs across the three platforms.
    {
        struct build_version_command *b = (struct build_version_command *)lc;
        b->cmd = LC_BUILD_VERSION; b->cmdsize = sz_bv;
        b->platform = plat->id; b->minos = plat->minos; b->sdk = plat->sdk;
        b->ntools = 0;
        lc += sz_bv;
    }
    // LC_ENCRYPTION_INFO_64 — Apple's App Store validation refuses a Mach-O without
    // it ("the encryption info ... is either missing or invalid ... not built with
    // Apple's linker", error 90125). cryptid 0 = not encrypted; the encryptable
    // region is __TEXT past the header reserve, the same convention a linker-built
    // dylib uses (cryptoff = HDR_RESERVE, cryptsize = the rest of __TEXT's file bytes).
    if (enc) {
        struct encryption_info_command_64 *e = (struct encryption_info_command_64 *)lc;
        e->cmd = LC_ENCRYPTION_INFO_64; e->cmdsize = sz_enc;
        e->cryptoff  = (uint32_t)(seg_foff[0] + HDR_RESERVE);
        e->cryptsize = (uint32_t)(seg_fsize[0] > HDR_RESERVE ? seg_fsize[0] - HDR_RESERVE : 0);
        e->cryptid   = 0;
        e->pad       = 0;
        lc += sz_enc;
    }
    // LC_SYMTAB / LC_DYSYMTAB — empty. Nothing imported, nothing exported.
    {
        struct symtab_command *st = (struct symtab_command *)lc;
        st->cmd = LC_SYMTAB; st->cmdsize = sz_st;
        st->symoff = 0; st->nsyms = 0;
        st->stroff = (uint32_t)linkedit_foff; st->strsize = strsize;
        lc += sz_st;

        struct dysymtab_command *ds = (struct dysymtab_command *)lc;
        ds->cmd = LC_DYSYMTAB; ds->cmdsize = sz_dst;
        lc += sz_dst;
    }
    // LC_UUID — derived from the content so the same input gives the same
    // output; a random uuid would make the build non-reproducible.
    {
        struct uuid_command *u = (struct uuid_command *)lc;
        u->cmd = LC_UUID; u->cmdsize = sz_uuid;
        uint64_t h1 = 0xcbf29ce484222325ull, h2 = 0x9e3779b97f4a7c15ull;
        for (size_t i = 0; i < (size_t)sb.st_size; i++) {
            h1 = (h1 ^ img[i]) * 0x100000001b3ull;
            h2 = (h2 + img[i]) * 0x9e3779b97f4a7c15ull;
        }
        memcpy(u->uuid,     &h1, 8);
        memcpy(u->uuid + 8, &h2, 8);
        u->uuid[6] = (u->uuid[6] & 0x0f) | 0x40;      // version 4 shape
        u->uuid[8] = (u->uuid[8] & 0x3f) | 0x80;
        lc += sz_uuid;
    }

    // ---- the veneer pool and the translation record ----
    // Both live in __TEXT below the image, so seg_foff[0] + vmaddr is the file
    // offset (segment 0 starts at vmaddr 0 and fileoff 0).
    // (The high pool, when anything was emitted into it, has its own segment
    // past the guest's last one.)
    if (low_off)  memcpy(o + low_foff,  low,  (size_t)low_off);
    if (high_off) memcpy(o + high_foff, high, (size_t)high_off);
    {
        // The reserve the record is parked in (see stat_va) is 64 bytes and the
        // record has grown once already; a silent overrun here would write into
        // the veneer pool.
        _Static_assert(sizeof(klx_stat_section) <= 64,
                       "__klstat no longer fits its reserve — move stat_va");
        klx_stat_section rec = { KLX_STAT_MAGIC, x18st.sites, x18st.patched,
                                 x18st.refused, tls_rewrites, KLX_TSD_SLOT,
                                 x18st.ctr_sites, x18st.ctr_patched,
                                 kl_x18_ctr_value(), (uint32_t)kl_x18_abi(),
                                 x18st.tp_patched, x18st.svc_patched };
        memcpy(o + seg_foff[0] + stat_va, &rec, sizeof rec);
    }

    // ---- copy the image, LOAD by LOAD, at its shifted address ----
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t vm = image_shift + ph[i].p_vaddr;
        int g = 0;
        for (int k = 0; k < ngrp; k++)
            if (vm >= seg_vm[k] && vm < seg_vmend[k]) { g = k; break; }
        uint64_t at = seg_foff[g] + (vm - seg_vm[g]);
        if (at + ph[i].p_filesz > total)
            die("internal: LOAD %d overruns the output (%#llx + %#llx > %#llx)",
                i, (unsigned long long)at, (unsigned long long)ph[i].p_filesz,
                (unsigned long long)total);
        memcpy(o + at, img + ph[i].p_offset, (size_t)ph[i].p_filesz);
    }

    // ---- write it out ----
    int ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (ofd < 0) die("open %s: %s", out, strerror(errno));
    if (write(ofd, o, (size_t)total) != (ssize_t)total) die("write %s: %s", out, strerror(errno));
    close(ofd);

    if (!quiet) {
        printf("klepton-ld: %s -> %s\n", in, out);
        printf("  platform      %s (LC_BUILD_VERSION platform=%u)\n", plat->name, plat->id);
        printf("  install name  %s\n", iname);
        printf("  image shift   %#llx   (ELF vaddr V lives at macho vmaddr S+V)\n",
               (unsigned long long)image_shift);
        for (int g = 0; g < ngrp; g++)
            printf("  %-10s vm %#09llx..%#09llx  file %#09llx+%#llx  (ELF %#llx..%#llx)\n",
                   g == 0 ? "__TEXT" : "__DATA",
                   (unsigned long long)seg_vm[g], (unsigned long long)seg_vmend[g],
                   (unsigned long long)seg_foff[g], (unsigned long long)seg_fsize[g],
                   (unsigned long long)grp[g].vlo, (unsigned long long)grp[g].vhi_mem);
        if (ngrp >= 2)
            printf("  text->data delta preserved: ELF %#llx, macho %#llx (same)\n",
                   (unsigned long long)(grp[1].vlo - grp[0].vlo),
                   (unsigned long long)((image_shift + grp[1].vlo) - (image_shift + grp[0].vlo)));
        printf("  TLS rewrites  %u   refused %u\n", tls_rewrites, tls_refused);
        printf("  x18 sites     %u   veneered %u   refused %u   "
               "(%#llx bytes at %#llx in __TEXT below, %#llx bytes at %#llx "
               "in __KLX18 above%s)\n",
               x18st.sites, x18st.patched, x18st.refused,
               (unsigned long long)low_off,  (unsigned long long)low_va,
               (unsigned long long)high_off, (unsigned long long)high_va,
               pool_split ? ", SPLIT" : "");
        printf("  CTR_EL0 reads %u   veneered %u   refused %u   (answering %#llx)\n",
               x18st.ctr_sites, x18st.ctr_patched, x18st.ctr_refused,
               (unsigned long long)kl_x18_ctr_value());
        if (kl_x18_abi() == KLX_ABI_LINUX)
            printf("  linux abi     tp sites %u veneered %u   svc sites %u veneered %u   "
                   "refused %u\n", x18st.tp_sites, x18st.tp_patched,
                   x18st.svc_sites, x18st.svc_patched, x18st.lx_refused);
        printf("  total         %llu bytes\n", (unsigned long long)total);
    }
    return 0;
}
