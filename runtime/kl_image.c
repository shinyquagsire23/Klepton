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
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "klepton.h"
#include "kl_x18.h"


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
typedef struct { uint32_t sh_name, sh_type; uint64_t sh_flags, sh_addr, sh_offset,
    sh_size; uint32_t sh_link, sh_info; uint64_t sh_addralign, sh_entsize; } Elf64_Shdr;

#define SHT_PROGBITS  1
#define SHF_EXECINSTR 0x4
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

static struct { char *name; void *stub; void *handler; } *g_stubs;
static unsigned g_stub_n, g_stub_cap;
static uint8_t *g_stub_rw, *g_stub_rx;
static size_t   g_stub_off;
static pthread_mutex_t g_stub_mu = PTHREAD_MUTEX_INITIALIZER;

// The pool exists twice: an RX mapping the guest executes from, and an RW
// alias of the same pages that stub_emit writes through. The old single
// mapping had to be flipped RW->RX around every write, and any thread
// executing an earlier stub during that window took an execute-protection
// fault (SIGBUS, pc == fault address, in an anonymous mapping — the "parked"
// fault from CLAUDE.md, which M6's concurrent render-thread stub traffic
// finally reproduced reliably). Dual mapping removes the window entirely:
// the execute view's permissions never change. The mutex is for the
// allocator itself — the dedup scan and g_stub_off were racy in exactly the
// same traffic. visionOS note: the RX/RW pair is a remap of our own
// anonymous allocation, not MAP_JIT; whether that passes W^X policy on
// device is a question for the port, not for this host-side fix.
static int stub_pool_open(void) {
    if (g_stub_rx) return 1;
    mach_vm_address_t rw = 0;
    if (mach_vm_allocate(mach_task_self(), &rw, KL_STUB_POOL,
                         VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return 0;
    mach_vm_address_t rx = 0;
    vm_prot_t cur, max;
    if (mach_vm_remap(mach_task_self(), &rx, KL_STUB_POOL, 0, VM_FLAGS_ANYWHERE,
                      mach_task_self(), rw, FALSE, &cur, &max,
                      VM_INHERIT_NONE) != KERN_SUCCESS ||
        mach_vm_protect(mach_task_self(), rx, KL_STUB_POOL, FALSE,
                        VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS) {
        mach_vm_deallocate(mach_task_self(), rw, KL_STUB_POOL);
        return 0;
    }
    g_stub_rw = (uint8_t *)rw;
    g_stub_rx = (uint8_t *)rx;
    if (getenv("KL_TRACE_IMAGES"))
        fprintf(stderr, "  [klepton] stub pool rx %p..%p (rw alias %p)\n",
                (void *)g_stub_rx, (void *)(g_stub_rx + KL_STUB_POOL),
                (void *)g_stub_rw);
    return 1;
}

// Emit one stub cell and return its *executable* address. Caller holds
// g_stub_mu. `payload` NULL means the owned copy of nm (kl_named_stub's
// argument). i0/i1/i2 are the three instructions before the nop, so both
// stub shapes share this allocator.
static void *stub_emit(const char *nm, void *payload, void *target,
                       uint32_t i0, uint32_t i1, uint32_t i2) {
    for (unsigned i = 0; i < g_stub_n; i++)             // shared across images
        if (g_stubs[i].handler == target && strcmp(g_stubs[i].name, nm) == 0)
            return g_stubs[i].stub;

    if (!stub_pool_open()) return NULL;
    if (g_stub_off + KL_STUB_BYTES > KL_STUB_POOL) return NULL;

    // The name lives in the image's strtab, which kl_unload would take with it.
    char *owned = strdup(nm);
    if (!owned) return NULL;

    if (getenv("KL_TRACE_IMAGES"))
        fprintf(stderr, "  [klepton] stub '%s' at +0x%zx\n", nm, g_stub_off);

    uint8_t *w = g_stub_rw + g_stub_off;
    uint32_t *code = (uint32_t *)w;
    code[0] = i0;
    code[1] = i1;
    code[2] = i2;
    code[3] = 0xD503201Fu;   // nop — the two literals land 8-byte aligned
    if (!payload) payload = owned;
    memcpy(w + 16, &payload, 8);
    memcpy(w + 24, &target, 8);

    uint8_t *s = g_stub_rx + g_stub_off;
    sys_icache_invalidate(s, KL_STUB_BYTES);
    g_stub_off += KL_STUB_BYTES;

    if (g_stub_n == g_stub_cap) {
        g_stub_cap = g_stub_cap ? g_stub_cap * 2 : 64;
        g_stubs = realloc(g_stubs, g_stub_cap * sizeof *g_stubs);
    }
    g_stubs[g_stub_n].name = owned;
    g_stubs[g_stub_n].stub = s;
    g_stubs[g_stub_n].handler = target;
    g_stub_n++;
    return s;
}

// Build a stub that tail-calls `handler` with `nm` in x0. Shared by anything
// that has to name what the guest reached for: unresolved ELF imports, and the
// GL entry points handed out by eglGetProcAddress (kl_egl.c), which have the
// same problem — one anonymous abort tells you nothing about which of two
// hundred functions the guest actually called.
void *kl_named_stub(const char *nm, void *handler) {
    if (!nm || !*nm || !handler) return kl_shim_lookup("__klepton_unresolved");
    pthread_mutex_lock(&g_stub_mu);
    void *s = stub_emit(nm, NULL, handler,
                        0x58000080u,    // ldr x0,  #16   (the name)
                        0x580000B0u,    // ldr x16, #20   (x16 is the intra-
                        0xD61F0200u);   // br  x16         procedure-call scratch)
    pthread_mutex_unlock(&g_stub_mu);
    return s ? s : kl_shim_lookup("__klepton_unresolved");
}

// The other shape of stub: one that logs and then lets the original call proceed
// untouched. kl_named_stub puts the name in x0 and never returns to the guest,
// which is right for "you called something unimplemented" and useless for tracing.
// Here the payload is a descriptor instead of a bare name, and the branch goes to
// kl_gl_trace_tramp (runtime/kl_gl_trace.S), which saves the argument registers,
// logs, restores, and tail-branches to the real function.
//
//    0: ldr x16, #16    // x16 = descriptor    (the trampoline's one input)
//    4: ldr x17, #20    // x17 = trampoline
//    8: br  x17
//   12: nop             // padding, so the two literals land 8-byte aligned
//   16: .quad descriptor
//   24: .quad trampoline
//
// Same 32-byte cell and same pool as kl_named_stub, so the two can be mixed
// freely; dedup is on (trampoline, name) exactly as it is on (handler, name).
void *kl_trace_stub(const char *nm, void *desc, void *tramp) {
    if (!nm || !*nm || !desc || !tramp) return NULL;
    pthread_mutex_lock(&g_stub_mu);
    void *s = stub_emit(nm, desc, tramp,
                        0x58000090u,    // ldr x16, #16   (descriptor)
                        0x580000B1u,    // ldr x17, #20   (trampoline)
                        0xD61F0220u);   // br  x17
    pthread_mutex_unlock(&g_stub_mu);
    return s;
}

static void *unresolved_stub(const char *nm) {
    return kl_named_stub(nm, (void *)kl_unresolved_named);
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

    // ---- rewrite guest text: S0.1 TLS, then S0.5 x18 ----
    //
    // Executable *sections*, not the executable segment. The r-x LOAD segment of
    // these libraries starts at file offset 0 and runs to the end of
    // .gcc_except_table, so it also spans .hash, .dynsym, .rela.dyn, .rodata and
    // .eh_frame — for libunity that is 1.5 MB of read-only data and 720 KB of
    // relocations sitting behind PF_X. Rewriting a word that merely looks like
    // an instruction in there would corrupt data, and the x18 pass patches
    // branches rather than flipping one bit, so it would corrupt it loudly.
    const Elf64_Shdr *sh = (eh->e_shoff && eh->e_shnum)
                         ? (const Elf64_Shdr *)(file + eh->e_shoff) : NULL;
    int veneer = sh != NULL;      // KL_X18 is honoured inside kl_x18_patch, which
                                  // still counts sites when the rewrite is off
    if (!sh)
        fprintf(stderr, "  [klepton] %s: no section headers — cannot separate code "
                        "from rodata; x18 veneering disabled (see trap 0)\n", path);

    for (int i = 0; i < eh->e_phnum && !sh; i++) {
        // Fallback for a stripped image: the old whole-segment scan. Narrower
        // than it should be is not an option for TLS — an unrewritten
        // tpidr_el0 read is a wrong answer on every thread.
        if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X)) continue;
        rewrite_tls(img->base + ph[i].p_vaddr - lo, (size_t)ph[i].p_filesz, &img->stats);
    }
    for (int i = 0; sh && i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_PROGBITS || !(sh[i].sh_flags & SHF_EXECINSTR)) continue;
        uint8_t *p = img->base + sh[i].sh_addr - lo;
        rewrite_tls(p, (size_t)sh[i].sh_size, &img->stats);
        if (!veneer) continue;
        kl_x18_stats xs;
        if (kl_x18_patch(p, (size_t)sh[i].sh_size, &xs) != 0) {
            fprintf(stderr, "  [klepton] %s: x18 veneering failed to start\n", path);
            veneer = 0;
            continue;
        }
        img->stats.x18_sites   += xs.sites;
        img->stats.x18_patched += xs.patched;
        img->stats.x18_refused += xs.refused;
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

    if (getenv("KL_TRACE_IMAGES"))
        fprintf(stderr, "  [klepton] image %s base %p span %#zx\n",
                path, (void *)img->base, img->span);
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
