// klepton-ld — M1b: translate an Android ARM64 ELF .so into a Mach-O dylib.
//
// This is the offline half of the loader. Today `kl_image.c` mmaps the guest
// ELF and makes its text RWX; on visionOS that is exactly the shape AMFI and
// library validation exist to refuse (PLANNING §12.1, M1b). So the guest text
// has to arrive as a *signed Mach-O*, mapped by dyld, and this tool is what
// produces it.
//
// WHAT IT EMITS, AND WHY IT IS SHAPED THIS WAY
//
// PLANNING §4.0.2 originally proposed emitting MH_OBJECT and letting ld64 do
// the final link, so that chained fixups, the exports trie and function starts
// would be the linker's problem. M1a's measurement retired that: **zero
// relocations target __TEXT** — every one of them lands in the RW image — so
// there is nothing for dyld to fix up at all. The runtime already applies the
// 1795 ELF relocations itself and will keep doing so. That removes the only
// reason to want ld64 in the loop, and ld64 in exchange gives us far too little
// control over the one thing that actually constrains this layout:
//
//   THE GUEST'S text->data DELTA MUST BE PRESERVED EXACTLY.
//
// ADRP+ADD pairs in guest text bake that delta in as an immediate. For
// libunityopus.so the RW image sits at ELF vaddr 0x97010 — which is *not*
// 16 KB-aligned, so the RW content cannot be placed at a Mach-O segment
// boundary. The resolution is to shift the whole ELF image by a constant S and
// let the RW content land at a non-zero offset *inside* __DATA:
//
//   macho vmaddr of ELF vaddr V   ==   S + V          (one constant, all V)
//
//   S = 0x4000        Mach-O header and load commands live below the image
//   __TEXT   vmaddr 0        the ELF r-x LOADs, at S
//   __DATA   vmaddr 0x98000  16 KB-aligned; RW content at +0x3010 within it
//
// Segment boundaries stay aligned, the delta is untouched, and the image is
// byte-for-byte the ELF's. Segments are made contiguous in VM (each extends to
// the next one's start) rather than leaving holes, because dyld is happier with
// a gapless image and the padding is a few zeroed pages.
//
// The runtime finds the embedded image through the __TEXT,__klelf section, so
// no exported symbol — and therefore no exports trie — is needed. See
// kl_load_dylib() in kl_image.c for the other half.
//
// WHAT IS DONE OFFLINE HERE THAT THE RUNTIME USED TO DO
//
// The S0.1 TLS rewrite (`mrs xN, tpidr_el0` -> `tpidrro_el0`) patches guest
// text, which a mapped-by-dyld __TEXT will not permit. It moves here. The S0.5
// x18 veneer pass has the same problem and will move here too (PLANNING §12.2);
// libunityopus.so has 0 x18 sites, so P1 does not need it yet, and this tool
// refuses an image that does have them rather than emitting one that would be
// silently wrong.
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
#define IMAGE_SHIFT 0x4000ull          // S: room for the Mach-O header below the image

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

// Count x18 sites with the real S0.5 decoder, not a bit-field guess. Which
// fields of an encoding are general-purpose registers — as opposed to
// immediates, condition codes or vector registers — is exactly what klx_decode
// exists to decide, and guessing gets it wrong in the unsafe direction: a naive
// "does any 5-bit field equal 18" scan reports 2158 sites in libunityopus.so,
// where the decoder correctly reports 0. This is a count only; emitting the
// veneers offline is the P4 work (PLANNING §12.2), and until then an image with
// real sites is refused rather than translated wrongly.
static unsigned count_x18(const uint8_t *p, size_t n) {
    unsigned hits = 0;
    const uint32_t *w = (const uint32_t *)p;
    for (size_t i = 0; i + 4 <= n; i += 4, w++) {
        klx_info info;
        klx_decode(*w, &info);
        if (info.nfields) hits++;
    }
    return hits;
}

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

    // ---- the text rewrites that used to happen at load time ----
    // Executable *sections*, not the executable segment: the r-x LOAD starts at
    // file offset 0 and also spans .rodata, .rela.dyn and .eh_frame, and
    // rewriting a word that merely looks like an instruction in there would
    // corrupt data (CLAUDE.md trap 0). Work on a private copy of the file.
    uint8_t *img = malloc((size_t)sb.st_size);
    if (!img) die("out of memory");
    memcpy(img, f, (size_t)sb.st_size);

    unsigned tls_rewrites = 0, x18_suspect = 0;
    const Elf64_Shdr *sh = (eh->e_shoff && eh->e_shnum)
                         ? (const Elf64_Shdr *)(f + eh->e_shoff) : NULL;
    if (!sh) die("%s has no section headers — cannot separate code from rodata "
                 "(see CLAUDE.md trap 0); refusing to rewrite", in);
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_PROGBITS || !(sh[i].sh_flags & SHF_EXECINSTR)) continue;
        tls_rewrites += rewrite_tls(img + sh[i].sh_offset, (size_t)sh[i].sh_size);
        x18_suspect  += count_x18(img + sh[i].sh_offset, (size_t)sh[i].sh_size);
    }
    if (x18_suspect)
        die("%s has %u instructions naming x18; the offline veneer pass is not "
            "wired up yet (PLANNING §12.2). Refusing rather than emitting an "
            "image that would be silently wrong.", in, x18_suspect);

    // ---- lay out the Mach-O ----
    // Segment k covers [seg_vm[k], seg_vm[k+1]) — contiguous, no holes.
    uint64_t seg_vm[10], seg_vmend[10], seg_foff[10], seg_fsize[10];
    for (int g = 0; g < ngrp; g++) {
        uint64_t want = IMAGE_SHIFT + grp[g].vlo;
        seg_vm[g] = (g == 0) ? 0 : align_down(want, PAGE);
        if (g && seg_vm[g] < seg_vmend[g - 1])
            die("segment %d would overlap the previous one (image shift %#llx is "
                "too small for this library's layout)", g, (unsigned long long)IMAGE_SHIFT);
        seg_vmend[g] = align_up(IMAGE_SHIFT + grp[g].vhi_mem, PAGE);
    }
    // Make them gapless: each segment runs up to the next one's start.
    for (int g = 0; g + 1 < ngrp; g++) seg_vmend[g] = seg_vm[g + 1];

    for (int g = 0; g < ngrp; g++) {
        seg_foff[g]  = (g == 0) ? 0 : seg_foff[g - 1] + seg_fsize[g - 1];
        seg_foff[g]  = align_up(seg_foff[g], PAGE);
        // File content stops at the end of initialised data; the rest is .bss,
        // which dyld supplies as zero pages beyond filesize.
        seg_fsize[g] = (IMAGE_SHIFT + grp[g].vhi_file) - seg_vm[g];
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
    uint32_t sz_text = sizeof(struct segment_command_64) + sizeof(struct section_64);
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
    if (sizeof(struct mach_header_64) + sizeofcmds > IMAGE_SHIFT)
        die("load commands (%u bytes) do not fit below the image shift %#llx",
            sizeofcmds, (unsigned long long)IMAGE_SHIFT);

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
        s->nsects = 1; s->flags = 0;
        struct section_64 *sec = (struct section_64 *)(s + 1);
        strncpy(sec->sectname, "__klelf", 16);
        strncpy(sec->segname,  "__TEXT",  16);
        sec->addr = IMAGE_SHIFT + grp[0].vlo;
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
        sec->addr = IMAGE_SHIFT + grp[1].vlo;
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

    // ---- copy the image, LOAD by LOAD, at its shifted address ----
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t vm = IMAGE_SHIFT + ph[i].p_vaddr;
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
               (unsigned long long)IMAGE_SHIFT);
        for (int g = 0; g < ngrp; g++)
            printf("  %-10s vm %#09llx..%#09llx  file %#09llx+%#llx  (ELF %#llx..%#llx)\n",
                   g == 0 ? "__TEXT" : "__DATA",
                   (unsigned long long)seg_vm[g], (unsigned long long)seg_vmend[g],
                   (unsigned long long)seg_foff[g], (unsigned long long)seg_fsize[g],
                   (unsigned long long)grp[g].vlo, (unsigned long long)grp[g].vhi_mem);
        if (ngrp >= 2)
            printf("  text->data delta preserved: ELF %#llx, macho %#llx (same)\n",
                   (unsigned long long)(grp[1].vlo - grp[0].vlo),
                   (unsigned long long)((IMAGE_SHIFT + grp[1].vlo) - (IMAGE_SHIFT + grp[0].vlo)));
        printf("  TLS rewrites  %u   x18 sites %u\n", tls_rewrites, x18_suspect);
        printf("  total         %llu bytes\n", (unsigned long long)total);
    }
    return 0;
}
