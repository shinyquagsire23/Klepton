// klepton-ld — M1b: translate an Android ARM64 ELF .so into a Mach-O dylib.
//
// This is the offline half of the loader. Today `kl_image.c` mmaps the guest
// ELF and makes its text RWX; on visionOS that is exactly the shape AMFI and
// library validation exist to refuse (PLANNING §12.1, M1b). So the guest text
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

#define PAGE 0x4000ull                 // 16 KB — Apple's page on arm64
#define HDR_RESERVE 0x4000ull          // room for the Mach-O header and load commands

static uint64_t align_up(uint64_t v, uint64_t a)   { return (v + a - 1) & ~(a - 1); }
static uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }

static void die(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    fprintf(stderr, "klepton-ld: ");
    vfprintf(stderr, fmt, a);
    fprintf(stderr, "\n");
    va_end(a);
    exit(1);
}

// ---------- S0.1: the TLS rewrite, moved offline ----------
// Darwin keeps the thread pointer in TPIDRRO_EL0; TPIDR_EL0 is clobbered by the
// kernel on preemption, so a guest read of it is a wrong answer on every
// thread. The two encodings differ by exactly one bit. Instruction count is
// preserved, so nothing in the image moves.
static unsigned rewrite_tls(uint8_t *p, size_t n) {
    unsigned hits = 0;
    uint32_t *w = (uint32_t *)p;
    for (size_t i = 0; i + 4 <= n; i += 4, w++)
        if ((*w & 0xffffffe0u) == 0xd53bd040u) { *w |= 0x20u; hits++; }
    return hits;
}

// x18 sites are found with the real S0.5 decoder, never a bit-field guess.
// Which fields of an encoding are general-purpose registers — as opposed to
// immediates, condition codes or vector registers — is exactly what klx_decode
// exists to decide, and guessing gets it wrong in the unsafe direction: a naive
// "does any 5-bit field equal 18" scan reports 2158 sites in libunityopus.so,
// where the decoder correctly reports 0. kl_x18_count/kl_x18_emit are the same
// code the runtime loader uses, which matters because `make x18` and
// `make guest` can only validate one implementation.

struct platform { const char *name; uint32_t id; uint32_t minos, sdk; };
static const struct platform PLATFORMS[] = {
    { "macos",       PLATFORM_MACOS,              0x0e0000, 0x0e0000 },  // 14.0
    { "ios",         PLATFORM_IOS,                0x110000, 0x110000 },  // 17.0
    { "iossim",      PLATFORM_IOSSIMULATOR,       0x110000, 0x110000 },
    { "visionos",    11 /* PLATFORM_VISIONOS */,  0x010000, 0x010000 },  // 1.0
    { "visionossim", 12 /* ...SIMULATOR */,       0x010000, 0x010000 },
};

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL, *install_name = NULL;
    const struct platform *plat = &PLATFORMS[0];
    int quiet = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--install-name") && i + 1 < argc) install_name = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
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
    // corrupt data (CLAUDE.md trap 0). Work on a private copy of the file.
    uint8_t *img = malloc((size_t)sb.st_size);
    if (!img) die("out of memory");
    memcpy(img, f, (size_t)sb.st_size);

    unsigned tls_rewrites = 0, x18_sites = 0;
    const Elf64_Shdr *sh = (eh->e_shoff && eh->e_shnum)
                         ? (const Elf64_Shdr *)(f + eh->e_shoff) : NULL;
    if (!sh) die("%s has no section headers — cannot separate code from rodata "
                 "(see CLAUDE.md trap 0); refusing to rewrite", in);

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
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_PROGBITS || !(sh[i].sh_flags & SHF_EXECINSTR)) continue;
        uint64_t cursor = sh[i].sh_addr, cva;
        size_t csz;
        while (kl_x18_next_code(sh[i].sh_addr, (size_t)sh[i].sh_size,
                                skip, nskip, &cursor, &cva, &csz)) {
            uint8_t *p = img + sh[i].sh_offset + (cva - sh[i].sh_addr);
            tls_rewrites += rewrite_tls(p, csz);
            x18_sites    += kl_x18_count(p, csz);
        }
    }

    // ---- lay out the Mach-O ----
    // The veneer pool lives *before* the guest image, inside __TEXT, which is
    // why the image shift is computed rather than fixed: S grows to make room.
    // Putting it there rather than in a segment of its own keeps the file to two
    // loadable segments and keeps every `b` well inside its +/-128 MB reach —
    // the furthest any site can be from the pool is the image's own span, and
    // the largest guest library here is libil2cpp at 57 MB.
    uint64_t pool_va   = HDR_RESERVE;                       // right after the header
    uint64_t pool_cap  = (uint64_t)x18_sites * KLX_VEN_MAX_INSN * 4;
    uint64_t image_shift = align_up(pool_va + pool_cap, PAGE);

    uint8_t *pool = NULL;
    if (pool_cap) { pool = calloc(1, (size_t)pool_cap); if (!pool) die("out of memory"); }

    // Pass 2: emit. Each section's veneers take the next slice of the pool, and
    // the addresses handed in are the ones the code will have once mapped — the
    // buffers here are a file image, not a mapping.
    kl_x18_stats x18st = {0};
    uint64_t pool_off = 0;
    for (int i = 0; i < eh->e_shnum && pool_cap; i++) {
        if (sh[i].sh_type != SHT_PROGBITS || !(sh[i].sh_flags & SHF_EXECINSTR)) continue;
        uint64_t cursor = sh[i].sh_addr, cva;
        size_t csz;
        while (kl_x18_next_code(sh[i].sh_addr, (size_t)sh[i].sh_size,
                                skip, nskip, &cursor, &cva, &csz)) {
            kl_x18_stats st;
            size_t used = 0;
            if (kl_x18_emit(img + sh[i].sh_offset + (cva - sh[i].sh_addr), csz,
                            image_shift + cva,
                            pool + pool_off, (size_t)(pool_cap - pool_off),
                            pool_va + pool_off, &st, &used) != 0)
                die("x18 veneer emission failed for section %d", i);
            x18st.sites       += st.sites;
            x18st.patched     += st.patched;
            x18st.refused     += st.refused;
            x18st.ctr_sites   += st.ctr_sites;
            x18st.ctr_patched += st.ctr_patched;
            x18st.ctr_refused += st.ctr_refused;
            pool_off          += used;
        }
    }
    if (x18st.refused) {
        // Same posture as the runtime loader: a refused site keeps its raw x18
        // and stays exposed to trap 0, so it is reported rather than hidden. The
        // two known ones are libunity's `br x18` jump tables (PLANNING S0.5).
        fprintf(stderr, "klepton-ld: %s: %u of %u x18 sites refused — still "
                        "exposed to trap 0:\n", in, x18st.refused, x18st.sites);
        kl_x18_report(stderr);
    }
    // Trap 26 is louder than trap 0 because the failure is immediate: a
    // `mrs Xt, CTR_EL0` left alone is SIGILL the first time it executes, not a
    // wrong value some time later. Announced even when it worked, because one
    // instruction in one library is exactly the kind of thing that goes missing
    // when the guest is re-translated by a different build.
    if (x18st.ctr_sites)
        fprintf(stderr, "klepton-ld: %s: %u CTR_EL0 read(s), %u veneered to "
                        "answer %#llx%s (trap 26)\n", in, x18st.ctr_sites,
                x18st.ctr_patched, (unsigned long long)kl_x18_ctr_value(),
                x18st.ctr_refused ? " — SOME REFUSED, they will SIGILL" : "");

    uint64_t seg_vm[10], seg_vmend[10], seg_foff[10], seg_fsize[10];
    for (int g = 0; g < ngrp; g++) {
        uint64_t want = image_shift + grp[g].vlo;
        seg_vm[g] = (g == 0) ? 0 : align_down(want, PAGE);
        if (g && seg_vm[g] < seg_vmend[g - 1])
            die("segment %d would overlap the previous one (image shift %#llx is "
                "too small for this library's layout)", g, (unsigned long long)image_shift);
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
    uint64_t linkedit_vm   = seg_vmend[ngrp - 1];
    uint64_t linkedit_foff = align_up(seg_foff[ngrp - 1] + seg_fsize[ngrp - 1], PAGE);

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
    uint32_t nsect_text = 2 + (pool_off ? 1 : 0);
    uint64_t stat_va = HDR_RESERVE - 64;
    uint32_t sz_text = sizeof(struct segment_command_64) + nsect_text * sizeof(struct section_64);
    uint32_t sz_data = sizeof(struct segment_command_64) + sizeof(struct section_64);
    uint32_t sz_link = sizeof(struct segment_command_64);
    uint32_t sz_id   = (uint32_t)align_up(sizeof(struct dylib_command) + strlen(iname) + 1, 8);
    uint32_t sz_ld   = (uint32_t)align_up(sizeof(struct dylib_command) + strlen(libsystem) + 1, 8);
    uint32_t sz_bv   = sizeof(struct build_version_command);
    uint32_t sz_st   = sizeof(struct symtab_command);
    uint32_t sz_dst  = sizeof(struct dysymtab_command);
    uint32_t sz_uuid = sizeof(struct uuid_command);

    uint32_t ncmds = 3 /* segments */ + 1 /* id */ + 1 /* load libSystem */ +
                     1 /* build version */ + 1 /* symtab */ + 1 /* dysymtab */ + 1 /* uuid */;
    // __DATA is only present if the library has a writable group.
    if (ngrp < 2) { ncmds--; sz_data = 0; }
    uint32_t sizeofcmds = sz_text + sz_data + sz_link + sz_id + sz_ld + sz_bv +
                          sz_st + sz_dst + sz_uuid;
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

        if (pool_off) {
            strncpy(sec->sectname, "__klx18", 16);
            strncpy(sec->segname,  "__TEXT",  16);
            sec->addr   = pool_va;
            sec->size   = pool_off;
            sec->offset = (uint32_t)(seg_foff[0] + (pool_va - seg_vm[0]));
            sec->align  = 2;
            sec->flags  = S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
            sec++;
        }

        strncpy(sec->sectname, "__klelf", 16);
        strncpy(sec->segname,  "__TEXT",  16);
        sec->addr = image_shift + grp[0].vlo;
        sec->size = grp[0].vhi_file - grp[0].vlo;
        sec->offset = (uint32_t)(seg_foff[0] + (sec->addr - seg_vm[0]));
        sec->align = 14;                       // 16 KB
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
    // LC_BUILD_VERSION — the one field that differs across §4's three rungs.
    {
        struct build_version_command *b = (struct build_version_command *)lc;
        b->cmd = LC_BUILD_VERSION; b->cmdsize = sz_bv;
        b->platform = plat->id; b->minos = plat->minos; b->sdk = plat->sdk;
        b->ntools = 0;
        lc += sz_bv;
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
    if (pool_off) memcpy(o + seg_foff[0] + pool_va, pool, (size_t)pool_off);
    {
        // The reserve the record is parked in (see stat_va) is 64 bytes and the
        // record has grown once already; a silent overrun here would write into
        // the veneer pool.
        _Static_assert(sizeof(klx_stat_section) <= 64,
                       "__klstat no longer fits its reserve — move stat_va");
        klx_stat_section rec = { KLX_STAT_MAGIC, x18st.sites, x18st.patched,
                                 x18st.refused, tls_rewrites, KLX_TSD_SLOT,
                                 x18st.ctr_sites, x18st.ctr_patched,
                                 kl_x18_ctr_value() };
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
        printf("  TLS rewrites  %u\n", tls_rewrites);
        printf("  x18 sites     %u   veneered %u   refused %u   (pool %#llx bytes at %#llx)\n",
               x18st.sites, x18st.patched, x18st.refused,
               (unsigned long long)pool_off, (unsigned long long)pool_va);
        printf("  CTR_EL0 reads %u   veneered %u   refused %u   (answering %#llx)\n",
               x18st.ctr_sites, x18st.ctr_patched, x18st.ctr_refused,
               (unsigned long long)kl_x18_ctr_value());
        printf("  total         %llu bytes\n", (unsigned long long)total);
    }
    return 0;
}
