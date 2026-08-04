// ELF64/AArch64 image loader + self-relocator for Darwin arm64.
// macOS has no <elf.h>, so the (small) subset we need is defined inline.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <libkern/OSCacheControl.h>
#include "klepton.h"

// ---------- ELF64 subset ----------
typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr,
    p_filesz, p_memsz, p_align; } Elf64_Phdr;
typedef struct { uint64_t d_tag; uint64_t d_val; } Elf64_Dyn;
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx;
    uint64_t st_value, st_size; } Elf64_Sym;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_JMPREL 23
#define DT_PLTRELSZ 2
#define ELF64_R_SYM(i)  ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)(i))
#define R_AARCH64_ABS64      257
#define R_AARCH64_GLOB_DAT  1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE  1027

struct kl_image {
    uint8_t   *base;        // mapping base; ELF vaddr V lives at base + V
    size_t     span;
    Elf64_Sym *symtab;
    const char *strtab;
    void     (**init_array)(void);
    size_t     init_count;
    kl_stats   stats;
    char       path[512];
    const char **missing;      // unique unresolved import names
    unsigned    missing_n, missing_cap;
};

static void record_missing(kl_image *img, const char *nm) {
    if (!nm) return;
    for (unsigned i = 0; i < img->missing_n; i++)
        if (strcmp(img->missing[i], nm) == 0) return;      // already seen
    if (img->missing_n == img->missing_cap) {
        img->missing_cap = img->missing_cap ? img->missing_cap * 2 : 64;
        img->missing = realloc(img->missing, img->missing_cap * sizeof *img->missing);
    }
    img->missing[img->missing_n++] = nm;
}

const char *const *kl_missing_imports(kl_image *img, unsigned *count) {
    if (count) *count = img->missing_n;
    return img->missing;
}

// ---------- unresolved-import stubs ----------
// Every unresolved import used to resolve to one shared abort stub, so calling
// one reported "called an unresolved import" and nothing else — the single
// place in this runtime where an unimplemented thing did not fail by name. That
// name is exactly where the next milestone starts, so each missing symbol now
// gets its own 32-byte trampoline that loads it and tail-calls the reporter:
//
//   0:  ldr x0, #16     // the reporter's argument: the symbol name
//   4:  ldr x16, #20    // x16 is the intra-procedure-call scratch register,
//   8:  br  x16         //   which is precisely what it is reserved for
//   12: nop             // padding, so the two literals land 8-byte aligned
//   16: .quad name
//   24: .quad kl_unresolved_named
//
// Data imports get one too. They are not called, so the guest dereferences the
// trampoline instead — but a wild pointer into a known page beats a wild
// pointer into nothing, and the name is right there next to the code.
#define KL_STUB_BYTES 32
#define KL_STUB_POOL  (64 * 1024)          /* 2048 stubs; no image is close */

void kl_unresolved_named(const char *name);   // kl_shim.c

static struct { char *name; void *stub; } *g_stubs;
static unsigned g_stub_n, g_stub_cap;
static uint8_t *g_stub_pool;
static size_t   g_stub_off;

static void *unresolved_stub(const char *nm) {
    if (!nm || !*nm) return kl_shim_lookup("__klepton_unresolved");
    for (unsigned i = 0; i < g_stub_n; i++)             // shared across images
        if (strcmp(g_stubs[i].name, nm) == 0) return g_stubs[i].stub;

    if (!g_stub_pool) {
        g_stub_pool = mmap(NULL, KL_STUB_POOL, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
        if (g_stub_pool == MAP_FAILED) { g_stub_pool = NULL; return kl_shim_lookup("__klepton_unresolved"); }
    }
    if (g_stub_off + KL_STUB_BYTES > KL_STUB_POOL) return kl_shim_lookup("__klepton_unresolved");

    // The name lives in the image's strtab, which kl_unload would take with it.
    char *owned = strdup(nm);
    if (!owned) return kl_shim_lookup("__klepton_unresolved");

    uint8_t *s = g_stub_pool + g_stub_off;
    if (mprotect(g_stub_pool, KL_STUB_POOL, PROT_READ | PROT_WRITE) != 0) { free(owned); return kl_shim_lookup("__klepton_unresolved"); }
    uint32_t *code = (uint32_t *)s;
    code[0] = 0x58000080u;   // ldr x0,  #16
    code[1] = 0x580000B0u;   // ldr x16, #20
    code[2] = 0xD61F0200u;   // br  x16
    code[3] = 0xD503201Fu;   // nop
    memcpy(s + 16, &owned, 8);
    void (*fn)(const char *) = kl_unresolved_named;
    memcpy(s + 24, &fn, 8);
    mprotect(g_stub_pool, KL_STUB_POOL, PROT_READ | PROT_EXEC);
    sys_icache_invalidate(s, KL_STUB_BYTES);
    g_stub_off += KL_STUB_BYTES;

    if (g_stub_n == g_stub_cap) {
        g_stub_cap = g_stub_cap ? g_stub_cap * 2 : 64;
        g_stubs = realloc(g_stubs, g_stub_cap * sizeof *g_stubs);
    }
    g_stubs[g_stub_n].name = owned;
    g_stubs[g_stub_n].stub = s;
    g_stub_n++;
    return s;
}

static char g_err[512];
static void err(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vsnprintf(g_err, sizeof g_err, fmt, a); va_end(a);
}
const char *kl_error(void) { return g_err; }
void  *kl_base(kl_image *i) { return i->base; }
size_t kl_span(kl_image *i) { return i->span; }
const kl_stats *kl_get_stats(kl_image *i) { return &i->stats; }

// ---------- S0.1: rewrite `mrs xN, tpidr_el0` -> `mrs xN, tpidrro_el0` ----------
// Darwin keeps the thread pointer in TPIDRRO_EL0; TPIDR_EL0 is clobbered by the
// kernel on preemption. The two encodings differ by exactly one bit (0x20).
// 98% of these sites are -fstack-protector prologues reading bionic slot 5.
static void rewrite_tls(uint8_t *p, size_t n, kl_stats *st) {
    uint32_t *w = (uint32_t *)p;
    for (size_t i = 0; i + 4 <= n; i += 4, w++) {
        if ((*w & 0xffffffe0u) == 0xd53bd040u) { *w |= 0x20u; st->tls_rewrites++; }
        else if (*w == 0xd4000001u) { st->svc_sites++; }        // svc #0 — report only
    }
}

// ---------- symbol lookup ----------
static uint64_t sym_value(kl_image *img, uint32_t idx, const char **name_out) {
    const Elf64_Sym *s = &img->symtab[idx];
    const char *nm = img->strtab + s->st_name;
    if (name_out) *name_out = nm;
    if (s->st_shndx != 0) return (uint64_t)(uintptr_t)(img->base + s->st_value);
    void *ext = kl_shim_lookup(nm);
    return (uint64_t)(uintptr_t)ext;      // 0 if unresolved
}

static int apply_relocs(kl_image *img, const Elf64_Rela *r, size_t count) {
    for (size_t i = 0; i < count; i++, r++) {
        uint64_t *slot = (uint64_t *)(img->base + r->r_offset);
        uint32_t type = ELF64_R_TYPE(r->r_info);
        uint32_t sidx = ELF64_R_SYM(r->r_info);
        const char *nm = NULL;
        switch (type) {
        case R_AARCH64_RELATIVE:
            *slot = (uint64_t)(uintptr_t)img->base + (uint64_t)r->r_addend;
            img->stats.relative++;
            break;
        case R_AARCH64_ABS64:
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT: {
            uint64_t v = sym_value(img, sidx, &nm);
            if (!v) {
                // Dedupe: libil2cpp has 711k relocations; per-site logging would flood.
                img->stats.imports_missing++;
                record_missing(img, nm);
                v = (uint64_t)(uintptr_t)unresolved_stub(nm);
            } else if (img->symtab[sidx].st_shndx == 0) {
                img->stats.imports_bound++;
            }
            *slot = v + (uint64_t)r->r_addend;
            if (type == R_AARCH64_ABS64) img->stats.abs64++;
            else if (type == R_AARCH64_GLOB_DAT) img->stats.glob_dat++;
            else img->stats.jump_slot++;
            break;
        }
        default:
            err("unhandled relocation type %u at %#llx", type, (unsigned long long)r->r_offset);
            return -1;
        }
    }
    return 0;
}

kl_image *kl_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { err("open %s: %s", path, strerror(errno)); return NULL; }
    struct stat sb;
    if (fstat(fd, &sb) != 0) { err("fstat: %s", strerror(errno)); close(fd); return NULL; }
    uint8_t *file = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) { err("mmap file: %s", strerror(errno)); return NULL; }

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 || eh->e_ident[4] != 2) {
        err("not an ELF64 file"); munmap(file, sb.st_size); return NULL;
    }
    if (eh->e_machine != 183) { err("not AArch64 (e_machine=%u)", eh->e_machine);
                                munmap(file, sb.st_size); return NULL; }

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(file + eh->e_phoff);
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) {
            if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
            if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
        }
    if (lo == UINT64_MAX) { err("no PT_LOAD"); munmap(file, sb.st_size); return NULL; }

    kl_image *img = calloc(1, sizeof *img);
    img->span = (size_t)(hi - lo);
    snprintf(img->path, sizeof img->path, "%s", path);

    // One RW mapping for the whole vaddr span; permissions tightened after relocation.
    img->base = mmap(NULL, img->span, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    if (img->base == MAP_FAILED) { err("mmap image: %s", strerror(errno));
                                   free(img); munmap(file, sb.st_size); return NULL; }
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        memcpy(img->base + ph[i].p_vaddr - lo, file + ph[i].p_offset, ph[i].p_filesz);
        // remainder of memsz is .bss — already zero from MAP_ANON
    }

    // ---- PT_DYNAMIC ----
    const Elf64_Rela *rela = NULL, *jmprel = NULL;
    size_t relasz = 0, pltsz = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_DYNAMIC) continue;
        const Elf64_Dyn *dp = (const Elf64_Dyn *)(img->base + ph[i].p_vaddr - lo);
        for (; dp->d_tag != DT_NULL; dp++) {
            switch (dp->d_tag) {
            case DT_SYMTAB: img->symtab = (Elf64_Sym *)(img->base + dp->d_val); break;
            case DT_STRTAB: img->strtab = (const char *)(img->base + dp->d_val); break;
            case DT_RELA:   rela   = (const Elf64_Rela *)(img->base + dp->d_val); break;
            case DT_RELASZ: relasz = dp->d_val; break;
            case DT_JMPREL: jmprel = (const Elf64_Rela *)(img->base + dp->d_val); break;
            case DT_PLTRELSZ: pltsz = dp->d_val; break;
            case DT_INIT_ARRAY:   img->init_array = (void (**)(void))(img->base + dp->d_val); break;
            case DT_INIT_ARRAYSZ: img->init_count = dp->d_val / sizeof(void *); break;
            }
        }
    }
    if (!img->symtab || !img->strtab) { err("missing DT_SYMTAB/DT_STRTAB");
                                        munmap(file, sb.st_size); return NULL; }

    // ---- relocate (every target is in the RW segment; __TEXT is never written) ----
    if ((rela   && apply_relocs(img, rela,   relasz / sizeof(Elf64_Rela)) != 0) ||
        (jmprel && apply_relocs(img, jmprel, pltsz  / sizeof(Elf64_Rela)) != 0)) {
        munmap(file, sb.st_size); return NULL;
    }

    // ---- S0.1 TLS rewrite, then lock down permissions per segment ----
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint8_t *segp = img->base + ph[i].p_vaddr - lo;
        if (ph[i].p_flags & PF_X) rewrite_tls(segp, (size_t)ph[i].p_filesz, &img->stats);
    }
    sys_icache_invalidate(img->base, img->span);

    uintptr_t pg = (uintptr_t)getpagesize();
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uintptr_t s = (uintptr_t)(img->base + ph[i].p_vaddr - lo) & ~(pg - 1);
        uintptr_t e = ((uintptr_t)(img->base + ph[i].p_vaddr - lo + ph[i].p_memsz) + pg - 1) & ~(pg - 1);
        int prot = ((ph[i].p_flags & PF_R) ? PROT_READ : 0) |
                   ((ph[i].p_flags & PF_W) ? PROT_WRITE : 0) |
                   ((ph[i].p_flags & PF_X) ? PROT_EXEC : 0);
        if (mprotect((void *)s, (size_t)(e - s), prot) != 0)
            fprintf(stderr, "  [klepton] mprotect(%#lx,%#lx,%d) failed: %s\n",
                    (unsigned long)s, (unsigned long)(e - s), prot, strerror(errno));
    }

    munmap(file, (size_t)sb.st_size);
    return img;
}

void kl_run_init(kl_image *img) {
    for (size_t i = 0; i < img->init_count; i++)
        if (img->init_array[i]) img->init_array[i]();
}

void *kl_sym(kl_image *img, const char *name) {
    // Linear scan of .dynsym. Fine for M1; klepton-ld will emit a hash table.
    // Symbol count is bounded by the gap between symtab and strtab.
    size_t n = ((const char *)img->strtab - (const char *)img->symtab) / sizeof(Elf64_Sym);
    for (size_t i = 0; i < n; i++) {
        const Elf64_Sym *s = &img->symtab[i];
        if (s->st_shndx == 0 || !s->st_name) continue;
        if (strcmp(img->strtab + s->st_name, name) == 0)
            return img->base + s->st_value;
    }
    return NULL;
}

void kl_unload(kl_image *img) {
    if (!img) return;
    munmap(img->base, img->span);
    free(img);
}
