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
// vm_*, not mach_vm_*: <mach/mach_vm.h> is `#error unsupported` on both the
// xrOS and xrsimulator SDKs, while <mach/vm_map.h> exists on all three and the
// two families are identical for an in-process mach_task_self() call. One code
// path rather than an #ifdef, so `make check` on the host tests what ships.
#include <mach/vm_map.h>
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
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
#define SHT_DYNSYM    11
#define SHF_EXECINSTR 0x4
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_HASH   4
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
    size_t     nsyms;       // from DT_HASH's nchain; 0 if the image had none
    const char *strtab;
    void     (**init_array)(void);
    size_t     init_count;
    kl_stats   stats;
    char       path[512];
    const char **missing;      // unique unresolved import names
    unsigned    missing_n, missing_cap;
    void       *dl_handle;     // set when the image arrived as a translated
                               // Mach-O dylib (M1b); then dyld owns the mapping
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
// gets its own cell, which loads the name and tail-calls the reporter:
//
//   0:  mov w16, #N              // this cell's index, baked in at build time
//   4:  b   kl_stub_named_common // loads slots[N] -> x0, slots[N+1] -> x16, br
//
// Data imports get one too. They are not called, so the guest dereferences the
// cell instead — but a wild pointer into a known page beats a wild pointer into
// nothing, and the index identifies the name.
// The cells are PRE-BUILT and signed with the binary — runtime/kl_stub_cells.S,
// which carries the full rationale. Nothing here writes instructions any more:
// a cell knows only its own index, and the allocator's whole job is to fill in
// the { payload, target } pair the shared dispatcher will read.
//
// This used to be an mmap'd pool that we generated code into, which is a
// SIGKILL on visionOS — AMFI answers "namespace CODESIGNING, Invalid Page" for
// any pc that did not come from a signed file, whatever the page's permissions
// are. Measured on device 2026-08-07; see kl_stub_cells.S for the report.
#define KL_STUB_CELL_BYTES  8              /* mov w16,#i + b — see the .S */
// 2048 was sized for Beat Saber's five libraries. Steam Link's Qt chain is a
// different order of magnitude — libQt6Widgets ALONE exhausted it, and past
// exhaustion an import silently becomes an unnamed stub, i.e. the one thing the
// whole M4 method depends on (failing BY NAME) stops working. Raised to 16384;
// the ceiling is 65536, above which `mov w16,#N` stops being a single movz and
// the cell stride in kl_stub_cells.S would have to change.
#define KL_STUB_NAMED_CELLS 16384
#define KL_STUB_TRACE_CELLS 1024

void kl_unresolved_named(const char *name);   // kl_shim.c

// The cells (text, read-only, signed) and the slots the dispatcher reads
// (data, written here). Both names are referenced from kl_stub_cells.S.
extern char kl_stub_named_cells[];
extern char kl_stub_trace_cells[];
void *kl_stub_named_slots[KL_STUB_NAMED_CELLS * 2];
void *kl_stub_trace_slots[KL_STUB_TRACE_CELLS * 2];

static struct { char *name; void *stub; void *handler; } *g_stubs;
static unsigned g_stub_n, g_stub_cap;
static unsigned g_named_used, g_trace_used;
static pthread_mutex_t g_stub_mu = PTHREAD_MUTEX_INITIALIZER;

// Claim one pre-built cell and point it at (payload, target). Caller holds
// g_stub_mu. `payload` NULL means the owned copy of nm (kl_named_stub's
// argument). `trace` picks which of the two shapes — and therefore which cell
// array and which dispatcher — the caller wants.
//
// The mutex still matters, for the same reason it always did: the dedup scan
// and the bump counter are racy under M6's concurrent stub traffic (the render
// thread calling existing stubs while the main thread creates new ones). What
// it no longer has to protect is a W^X window, because there is no longer any
// window: the cells are read-only text from the moment the binary is signed,
// and only the slots array is written. That also retires the SIGBUS this code
// used to produce when the pool was flipped RW->RX around each write.
static void *stub_emit(const char *nm, void *payload, void *target, int trace) {
    for (unsigned i = 0; i < g_stub_n; i++)             // shared across images
        if (g_stubs[i].handler == target && strcmp(g_stubs[i].name, nm) == 0)
            return g_stubs[i].stub;

    unsigned cap  = trace ? KL_STUB_TRACE_CELLS : KL_STUB_NAMED_CELLS;
    unsigned *use = trace ? &g_trace_used : &g_named_used;
    if (*use >= cap) {
        // Exhaustion is now a fixed ceiling rather than a 64 KB pool, so say so
        // by name instead of returning a silent NULL that becomes a generic
        // "unresolved" a layer up.
        fprintf(stderr, "  [klepton] out of %s stub cells (%u) — '%s' unnamed\n",
                trace ? "trace" : "named", cap, nm);
        return NULL;
    }

    // The name lives in the image's strtab, which kl_unload would take with it.
    char *owned = strdup(nm);
    if (!owned) return NULL;

    unsigned idx = (*use)++;
    if (!payload) payload = owned;
    void **slots = trace ? kl_stub_trace_slots : kl_stub_named_slots;
    slots[idx * 2]     = payload;
    slots[idx * 2 + 1] = target;

    void *s = (trace ? kl_stub_trace_cells : kl_stub_named_cells)
              + (size_t)idx * KL_STUB_CELL_BYTES;

    if (kl_env_on("KL_TRACE_IMAGES", 0))
        fprintf(stderr, "  [klepton] stub '%s' -> %s cell %u (%p)\n",
                nm, trace ? "trace" : "named", idx, s);

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
    void *s = stub_emit(nm, NULL, handler, 0 /* named */);
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
//    0: mov w16, #N              // this cell's index
//    4: b   kl_stub_trace_common // loads slots[N] -> x16, slots[N+1] -> x17, br
//
// A SEPARATE cell array from kl_named_stub's, because the two shapes differ in
// what they may clobber: this one has to leave x0-x7 alone (the real function
// still needs its arguments), so the payload arrives in x16 rather than x0.
// Dedup is on (trampoline, name) exactly as it is on (handler, name).
void *kl_trace_stub(const char *nm, void *desc, void *tramp) {
    if (!nm || !*nm || !desc || !tramp) return NULL;
    pthread_mutex_lock(&g_stub_mu);
    void *s = stub_emit(nm, desc, tramp, 1 /* trace */);
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

// The mapped phdr array. Every guest lib here has a first PT_LOAD covering file
// offset 0 at vaddr 0 (verified for all five), so the ELF header — and the phdr
// table it points at — is mapped at `base`, and the load bias equals the base.
// Checked rather than assumed: a lib whose phdrs are not mapped reports none,
// and the unwinder then simply does not see it.
const void *kl_phdrs(kl_image *i, unsigned *count) {
    if (count) *count = 0;
    if (!i || !i->base) return NULL;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)i->base;
    if (memcmp(eh->e_ident, "\177ELF", 4) != 0) return NULL;
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) return NULL;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > i->span) return NULL;
    if (count) *count = eh->e_phnum;
    return i->base + eh->e_phoff;
}

// ---------- S0.1: rewrite `mrs xN, tpidr_el0` -> `mrs xN, tpidrro_el0` ----------
// Darwin keeps the thread pointer in TPIDRRO_EL0; TPIDR_EL0 is clobbered by the
// kernel on preemption. The two encodings differ by exactly one bit (0x20).
// 98% of these sites are -fstack-protector prologues reading bionic slot 5.
// It runs on the same chunks as the x18 pass and carries the same trap 0b/0d
// exposure: a word of a constant table that happens to match this pattern gets
// a bit flipped, and the corrupted constant surfaces somewhere else entirely.
// One in 2^27 words matches by chance, so it has not bitten — but the detector
// exists now, and having one of the two rewriters checked and the other not is
// how a class comes back. The order matters too: this must consult the
// unmodified buffer, which it does, since it is the first pass over a chunk.
static void rewrite_tls(uint8_t *p, size_t n, kl_stats *st) {
    uint32_t *w = (uint32_t *)p;
    size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        if ((w[i] & 0xffffffe0u) == 0xd53bd040u) {
            if (kl_x18_is_data(w, n, i)) { st->x18_data_words++; continue; }
            w[i] |= 0x20u;
            st->tls_rewrites++;
        } else if (w[i] == 0xd4000001u) { st->svc_sites++; }    // svc #0 — report only
    }
}

// ---------- symbol lookup ----------
static uint64_t sym_value(kl_image *img, uint32_t idx, const char **name_out) {
    const Elf64_Sym *s = &img->symtab[idx];
    const char *nm = img->strtab + s->st_name;
    if (name_out) *name_out = nm;
    if (s->st_shndx != 0) return (uint64_t)(uintptr_t)(img->base + s->st_value);
    void *ext = kl_shim_lookup(nm);
    // Not a shim: try other already-loaded guest images. Android's linker binds
    // an .so's imports against its DT_NEEDED chain; our chain is the image
    // registry, filled dependencies-first by kl_load_recursive. This is what
    // lets libmain.so's SDL_* imports bind to the guest libSDL3 instead of
    // falling to an unresolved stub.
    if (!ext) ext = kl_guest_sym_global(nm);
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

// Walk PT_DYNAMIC and bind the image: symtab/strtab, DT_INIT_ARRAY, and every
// relocation. Shared by both loaders — the mmap path below and kl_load_dylib()
// — because this half is identical either way. It is the whole reason M1b can
// emit a dylib with *zero dyld fixups*: nothing here needs dyld's help, and no
// relocation target is in __TEXT (M1a's measurement), so the guest image can be
// mapped read-execute by dyld and still relocate correctly.
//
// `lo` is the lowest PT_LOAD vaddr, so `img->base + p_vaddr - lo` addresses the
// mapped copy in both cases (it is 0 for every guest library here).
static int bind_dynamic(kl_image *img, const Elf64_Phdr *ph, int phnum, uint64_t lo) {
    const Elf64_Rela *rela = NULL, *jmprel = NULL;
    size_t relasz = 0, pltsz = 0;
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_DYNAMIC) continue;
        const Elf64_Dyn *dp = (const Elf64_Dyn *)(img->base + ph[i].p_vaddr - lo);
        for (; dp->d_tag != DT_NULL; dp++) {
            switch (dp->d_tag) {
            case DT_SYMTAB: img->symtab = (Elf64_Sym *)(img->base + dp->d_val); break;
            case DT_STRTAB: img->strtab = (const char *)(img->base + dp->d_val); break;
            // The authoritative .dynsym entry count. DT_HASH's second word is
            // nchain, and the chain array is parallel to the symbol table, so
            // nchain IS the number of symbols — this is how a real dynamic
            // linker knows, and it needs no section headers.
            case DT_HASH:   img->nsyms = ((const uint32_t *)(img->base + dp->d_val))[1]; break;
            case DT_RELA:   rela   = (const Elf64_Rela *)(img->base + dp->d_val); break;
            case DT_RELASZ: relasz = dp->d_val; break;
            case DT_JMPREL: jmprel = (const Elf64_Rela *)(img->base + dp->d_val); break;
            case DT_PLTRELSZ: pltsz = dp->d_val; break;
            case DT_INIT_ARRAY:   img->init_array = (void (**)(void))(img->base + dp->d_val); break;
            case DT_INIT_ARRAYSZ: img->init_count = dp->d_val / sizeof(void *); break;
            }
        }
    }
    if (!img->symtab || !img->strtab) { err("missing DT_SYMTAB/DT_STRTAB"); return -1; }

    if ((rela   && apply_relocs(img, rela,   relasz / sizeof(Elf64_Rela)) != 0) ||
        (jmprel && apply_relocs(img, jmprel, pltsz  / sizeof(Elf64_Rela)) != 0))
        return -1;
    return 0;
}

// ---------- M1b: load a klepton-ld-translated Mach-O dylib ----------
// The shipping loader. dyld maps and (on device) AMFI validates the guest text,
// so nothing here is ever RWX and no guest byte is written by us — the S0.1 TLS
// rewrite and the S0.5 x18 veneers were applied offline by klepton-ld.
//
// The guest ELF image is embedded verbatim in the dylib at a constant shift, and
// found through the __TEXT,__klelf section rather than an exported symbol: that
// is what lets klepton-ld skip the exports trie entirely. getsectiondata()
// applies the image's slide for us.
//
// The mach_header is located by matching the dylib against dyld's loaded-image
// list. dlsym would be the obvious route and is precisely what we cannot use —
// the translated dylib deliberately exports nothing.
kl_image *kl_load_dylib(const char *path) {
    char real[PATH_MAX];
    if (!realpath(path, real)) snprintf(real, sizeof real, "%s", path);

    void *h = dlopen(real, RTLD_NOW | RTLD_LOCAL);
    if (!h) { err("dlopen %s: %s", real, dlerror()); return NULL; }

    const struct mach_header_64 *mh = NULL;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *nm = _dyld_get_image_name(i);
        char nr[PATH_MAX];
        if (!nm) continue;
        if (!realpath(nm, nr)) snprintf(nr, sizeof nr, "%s", nm);
        if (strcmp(nr, real) == 0) {
            mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
            break;
        }
    }
    if (!mh) { err("%s loaded but not found in dyld's image list", real);
               dlclose(h); return NULL; }

    unsigned long secsz = 0;
    uint8_t *elf = getsectiondata(mh, "__TEXT", "__klelf", &secsz);
    if (!elf) { err("%s has no __TEXT,__klelf section — not a klepton-ld image", real);
                dlclose(h); return NULL; }

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 || eh->e_machine != 183) {
        err("%s: __klelf does not hold an AArch64 ELF64 image", real);
        dlclose(h); return NULL;
    }

    kl_image *img = calloc(1, sizeof *img);
    img->base      = elf;
    img->dl_handle = h;
    snprintf(img->path, sizeof img->path, "%s", real);

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(elf + eh->e_phoff);
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) {
            if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
            if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
        }
    if (lo == UINT64_MAX) { err("%s: embedded image has no PT_LOAD", real);
                            free(img); dlclose(h); return NULL; }
    img->span = (size_t)(hi - lo);

    if (bind_dynamic(img, ph, eh->e_phnum, lo) != 0) { free(img); dlclose(h); return NULL; }

    // The TLS rewrite and the x18 veneers were applied offline, so there is
    // nothing left in this image to count — which is precisely why klepton-ld
    // records what it did. Reporting a computed 0 here would read as "this
    // library never needed any", not "this was handled at translation time".
    unsigned long statsz = 0;
    const klx_stat_section *rec =
        (const klx_stat_section *)getsectiondata(mh, "__TEXT", "__klstat", &statsz);
    if (rec && statsz >= sizeof *rec && rec->magic == KLX_STAT_MAGIC) {
        img->stats.tls_rewrites = rec->tls_rewrites;
        img->stats.x18_sites    = rec->sites;
        img->stats.x18_patched  = rec->patched;
        img->stats.x18_refused  = rec->refused;

        // The veneers carry a TSD slot chosen at translation time. If this
        // process cannot have that slot, every one of them reads and writes
        // somebody else's per-thread state — wrong in both directions and
        // silent. It is baked into the text, so there is no recovering: fail
        // here, at load, rather than as a wrong answer later.
        if (rec->patched) {
            if (rec->slot != KLX_TSD_SLOT) {
                err("%s was translated against TSD slot %u but this runtime uses "
                    "%d — rebuild it with a matching klepton-ld",
                    real, rec->slot, KLX_TSD_SLOT);
                free(img); dlclose(h); return NULL;
            }
            if (kl_x18_init() != 0) {
                err("%s needs x18 veneering but TSD slot %d is unavailable in this "
                    "process", real, KLX_TSD_SLOT);
                free(img); dlclose(h); return NULL;
            }
        }
    }

    if (kl_env_on("KL_TRACE_IMAGES", 0))
        fprintf(stderr, "  [klepton] dylib %s: mach header %p, guest image %p span %#zx\n",
                real, (const void *)mh, (void *)img->base, img->span);
    return img;
}

// Pick a loader for a guest library. With KL_DYLIB_DIR set, a klepton-ld
// translation of this library is used when one has been built, and the ELF
// otherwise — so the boot chain can run *mixed* while the offline x18 veneer
// pass is still missing. Three of the five guest libraries have 0 x18 sites and
// translate today; libunity (14,536) and libil2cpp (3,083) do not, and
// klepton-ld refuses them rather than emitting something silently wrong.
//
// A missing translation is normal and quiet. A translation that is *present and
// fails* is not: it returns NULL and says so, because falling back to the ELF
// there would hide exactly the regression this path exists to catch.
// The translation of `path`, if KL_DYLIB_DIR names one. Factored out because two
// callers must agree about it: kl_load_auto, which loads it, and kl_can_load,
// which answers "does this library exist" for the guest. They disagreed once and
// it cost a device run — see kl_can_load.
static int dylib_candidate(const char *path, char *out, size_t cap, char *name_out,
                           size_t name_cap) {
    const char *dir = kl_env_str("KL_DYLIB_DIR", NULL);
    if (!dir || !*dir) return 0;
    const char *stem = strrchr(path, '/');
    stem = stem ? stem + 1 : path;
    char name[256];
    snprintf(name, sizeof name, "%s", stem);
    char *dot = strstr(name, ".so");
    if (dot) *dot = 0;
    if (name_out) snprintf(name_out, name_cap, "%s", name);

    // Two layouts, because the host and the bundle disagree about what a
    // translated library looks like. `make dylibs` writes bare .dylib files; an
    // app bundle carries frameworks (§4.0.1), which is also what P3/P12 actually
    // got past AMFI — Xcode code-signs what it embeds in Frameworks/, and a loose
    // Mach-O elsewhere in the bundle is only sealed, not signed. Same image
    // either way.
    snprintf(out, cap, "%s/%s.dylib", dir, name);
    if (access(out, R_OK) == 0) return 1;
    snprintf(out, cap, "%s/%s.framework/%s", dir, name, name);
    return access(out, R_OK) == 0;
}

kl_image *kl_load_auto(const char *path) {
    char cand[1024], name[256];
    if (dylib_candidate(path, cand, sizeof cand, name, sizeof name)) {
        kl_image *img = kl_load_dylib(cand);
        if (!img) {
            fprintf(stderr, "  [klepton] %s: translated dylib present but failed "
                            "to load: %s\n", cand, kl_error());
            return NULL;
        }
        fprintf(stderr, "  [klepton] %s: loaded as a translated dylib (M1b)\n", name);
        return img;
    }
    return kl_load(path);
}

// Could kl_load_auto load a guest library at `path`? Anything that answers an
// existence question about a guest library on the guest's behalf must ask this
// and not stat(path) — because on device the ELF tree is deliberately NOT in the
// bundle (only the translations are, which is 80 MB saved), so the .so path is a
// name the loader resolves rather than a file that exists.
//
// This is what P5.4's first device lifecycle run died on.
// ClassLoader.findLibrary() stat'ed the .so and returned null for libil2cpp, so
// Unity never even attempted the dlopen that would have succeeded — and put up
// "Failed to load Il2CPP." three layers away from the stat that caused it. The
// path stays the .so path in the answer, so there is still exactly one place
// that resolves it: here.
int kl_can_load(const char *path) {
    char cand[1024];
    if (dylib_candidate(path, cand, sizeof cand, NULL, 0)) return 1;
    return access(path, R_OK) == 0;
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

    // ---- PT_DYNAMIC, then relocate ----
    // Every relocation target is in the RW segment; __TEXT is never written.
    if (bind_dynamic(img, ph, eh->e_phnum, lo) != 0) {
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

    // The authoritative .dynsym entry count, and the reason it is taken from the
    // SECTION rather than from DT_HASH: DT_HASH is optional. Steam Link's VR APK
    // builds libSDL3/libmain with `.gnu.hash` only — no `.hash` section, so no
    // DT_HASH — and `.gnu.hash` then sits between `.dynsym` and `.dynstr`, which
    // is exactly the adjacency the old inference assumed. Section headers are
    // required here anyway (x18 cannot separate code from rodata without them),
    // so this is free. DT_HASH's nchain, set in bind_dynamic, is the fallback.
    for (int i = 0; sh && i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_DYNSYM || !sh[i].sh_entsize) continue;
        img->nsyms = (size_t)(sh[i].sh_size / sh[i].sh_entsize);
        break;
    }

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
    // ...and code inside those sections, not whole sections. An executable
    // section can itself contain data: Steam Link's libmain.so carries
    // BoringSSL's 148 KB `ecp_nistz256_precomputed` table inside .text, and
    // 1059 of its 1080 apparent x18 sites are words of that table. Patching
    // them would replace elliptic-curve constants with branches — a wrong
    // answer that surfaces as a failing TLS handshake, nowhere near here.
    static klx_range skip[256];
    unsigned nskip = sh ? kl_x18_data_ranges(file, (size_t)sb.st_size, skip,
                                             sizeof skip / sizeof *skip) : 0;
    if (nskip) {
        uint64_t bytes = 0;
        for (unsigned i = 0; i < nskip; i++) bytes += skip[i].end - skip[i].start;
        fprintf(stderr, "  [klepton] %s: %u data range(s) inside executable sections, "
                        "%llu bytes — left alone\n",
                path, nskip, (unsigned long long)bytes);
    }
    for (int i = 0; sh && i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_PROGBITS || !(sh[i].sh_flags & SHF_EXECINSTR)) continue;
        uint64_t cursor = sh[i].sh_addr, cva;
        size_t csz;
        while (kl_x18_next_code(sh[i].sh_addr, (size_t)sh[i].sh_size,
                                skip, nskip, &cursor, &cva, &csz)) {
            uint8_t *p = img->base + cva - lo;
            rewrite_tls(p, csz, &img->stats);
            if (!veneer) continue;
            kl_x18_stats xs;
            if (kl_x18_patch(p, csz, &xs) != 0) {
                fprintf(stderr, "  [klepton] %s: x18 veneering failed to start\n", path);
                veneer = 0;
                continue;
            }
            img->stats.x18_sites   += xs.sites;
            img->stats.x18_patched += xs.patched;
            img->stats.x18_refused += xs.refused;
            img->stats.x18_data_words += xs.data_words;
        }
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

    if (kl_env_on("KL_TRACE_IMAGES", 0))
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
    //
    // The count comes from DT_HASH's nchain. It used to be inferred from the
    // gap between symtab and strtab, which silently assumes .dynstr directly
    // follows .dynsym — true of every Beat Saber library and false of every
    // Steam Link one, where the linker puts .gnu.hash and .hash between them.
    // libc++_shared.so has 2506 symbols and a gap of 4198 entries, so the scan
    // ran off the end of .dynsym into the hash tables, read their words as
    // st_name offsets and dereferenced strtab + garbage. It faulted inside
    // strcmp during a cross-image import bind — a wild read that a slightly
    // different layout would have turned into a wrong answer instead.
    size_t n = img->nsyms;
    if (!n)     // neither sections nor DT_HASH: infer, rather than bind nothing
        n = ((const char *)img->strtab - (const char *)img->symtab) / sizeof(Elf64_Sym);

    // Belt and braces, and this is the part that makes the class of bug above
    // impossible rather than merely fixed: an over-long count must degrade to
    // "no match", never to a wild read. Everything an ELF symbol table can name
    // lives inside the mapping, so a st_name or st_value outside it is a symbol
    // this table does not really have — which is precisely what a miscounted
    // scan reads once it walks off the end into the hash tables.
    const char *lo_p = (const char *)img->base, *hi_p = lo_p + img->span;
    for (size_t i = 0; i < n; i++) {
        const Elf64_Sym *s = &img->symtab[i];
        if ((const char *)(s + 1) > hi_p) break;             // past the mapping
        if (s->st_shndx == 0 || !s->st_name) continue;
        const char *nm = img->strtab + s->st_name;
        if (nm < lo_p || nm >= hi_p) continue;               // not a real name
        if (strcmp(nm, name) == 0)
            return img->base + s->st_value;
    }
    return NULL;
}

void kl_unload(kl_image *img) {
    if (!img) return;
    if (img->dl_handle) dlclose(img->dl_handle);   // M1b: dyld owns the mapping
    else                munmap(img->base, img->span);
    free(img);
}

// Read an image's DT_NEEDED sonames straight out of the file, without loading
// it. kl_load_recursive uses this to pull the dependency chain in before the
// image that imports it, so relocation-time binding (sym_value's global guest
// search) has the exports in place. d_val entries are vaddrs; converting them
// to file offsets goes through the PT_LOAD table, which on these libraries is
// identity-mapped (p_vaddr == p_offset) but is not required to be.
int kl_list_needed(const char *path, char names[][128], int max) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat sb;
    if (fstat(fd, &sb) != 0) { close(fd); return -1; }
    uint8_t *file = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) return -1;

    int n = 0;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) == 0 && eh->e_machine == 183) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(file + eh->e_phoff);
        const Elf64_Dyn *dyn = NULL;
        for (int i = 0; i < eh->e_phnum; i++)
            if (ph[i].p_type == PT_DYNAMIC) { dyn = (const Elf64_Dyn *)(file + ph[i].p_offset); break; }
        const char *strtab = NULL;
        if (dyn)
            for (const Elf64_Dyn *dp = dyn; dp->d_tag != DT_NULL; dp++)
                if (dp->d_tag == DT_STRTAB) {
                    // vaddr -> file offset via PT_LOAD
                    for (int i = 0; i < eh->e_phnum; i++)
                        if (ph[i].p_type == PT_LOAD &&
                            dp->d_val >= ph[i].p_vaddr &&
                            dp->d_val < ph[i].p_vaddr + ph[i].p_filesz) {
                            strtab = (const char *)(file + ph[i].p_offset +
                                                    (dp->d_val - ph[i].p_vaddr));
                            break;
                        }
                    break;
                }
        if (dyn && strtab)
            for (const Elf64_Dyn *dp = dyn; dp->d_tag != DT_NULL && n < max; dp++)
                if (dp->d_tag == DT_NEEDED) {
                    snprintf(names[n], 128, "%s", strtab + dp->d_val);
                    n++;
                }
    }
    munmap(file, (size_t)sb.st_size);
    return n;
}
