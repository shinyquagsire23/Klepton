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
#include "kl_key.h"
int kl_untested_profile(void);   // guest/kl_driver.c
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
#include <mach-o/loader.h>
#include "klepton.h"
#include "kl_x18.h"
#include "kl_guestpatch.h"
#include "kl_aot_hooks.h"
#include "kl_extension.h"


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
#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHT_NOBITS 8
// The image behind an executable chunk, for the `br x18` jump-table rewrite —
// see kl_x18_image in kl_x18.h. `bias` turns a MAPPED address back into the
// ELF's own VA, because at load time the addresses handed to the veneer pass are
// the mapping's own.
struct x18_img_ctx { const Elf64_Shdr *sh; int shnum; uint64_t bias; };

// Read-only ALLOC data only. Executable would let a mis-recognition write over
// code; writable (.data.rel.ro) is rewritten by relocations at load, which would
// undo the repointing silently. A jump table is in .rodata.
static void *x18_img_data_at(void *vctx, uint64_t va, size_t len) {
    struct x18_img_ctx *c = vctx;
    uint64_t gva = va - c->bias;
    for (int i = 0; i < c->shnum; i++) {
        const Elf64_Shdr *s = &c->sh[i];
        if (s->sh_type != SHT_PROGBITS) continue;
        if (s->sh_flags & (SHF_EXECINSTR | SHF_WRITE)) continue;
        if (!(s->sh_flags & SHF_ALLOC) || !s->sh_addr) continue;
        if (gva < s->sh_addr || gva + len > s->sh_addr + s->sh_size) continue;
        return (void *)(uintptr_t)va;      // the mapping is contiguous from `lo`
    }
    return NULL;
}

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_GNU_RELRO 0x6474e552
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
// Relative-relocation tables this loader must also honour. A library built by a
// current NDK ships NONE of its relative relocs in DT_RELA: they are packed into
// RELR (a bitmap) or Android's APS2 stream, and DT_RELA carries only the handful
// that need a symbol. Miss these and every relative pointer — vtables, GOT, and
// fatally the .init_array function pointers — keeps its raw link-time offset;
// the first constructor call then jumps to that offset and AMFI kills the
// process (CODESIGNING, "Invalid Page"). libmrutilitykitshared (olar) is the
// first guest to use them: 6962 relocs in RELR (all 29 init_array slots) plus 15
// in APS2. RELR has both the standardised tags and the older Android-range ones
// bionic still emits; a library uses one spelling or the other.
#define DT_RELR    0x24
#define DT_RELRSZ  0x23
#define DT_RELRENT 0x25
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001
#define DT_ANDROID_RELRENT 0x6fffe003
#define DT_ANDROID_RELA    0x60000011
#define DT_ANDROID_RELASZ  0x60000012
#define ELF64_R_SYM(i)  ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)(i))
#define R_AARCH64_ABS64      257
#define R_AARCH64_GLOB_DAT  1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE  1027
#define R_AARCH64_TLSDESC   1031
#define PT_TLS 7
#define STB_WEAK 2

struct kl_image {
    uint8_t   *base;        // ELF vaddr V lives at base + V (V relative to the
                            // lowest PT_LOAD); the load bias the guest sees
    size_t     span;        // ...and the vaddr span, base to the last mapped byte
    // Where `base` sits inside the mapping we actually asked the kernel for, and
    // how big that mapping is. Nonzero map_off only for a guest whose p_align is
    // finer than the host page — see choose_bias(). munmap needs both, since
    // base is then not the address mmap returned.
    size_t     map_off, map_size;
    // The x18 veneer pool, reserved on the end of the same mapping so it is
    // within `b`'s reach of the code by construction. munmap frees both.
    size_t     x18_arena_size;
    // ...and a same-sized second pool BEFORE the image, for the code the end
    // pool cannot reach in a library wider than one b window. munmap covers
    // it: the mapping starts this many bytes before base - map_off.
    size_t     x18_pre_arena_size;
    Elf64_Sym *symtab;
    size_t     nsyms;       // from DT_HASH's nchain; 0 if the image had none
    const char *strtab;
    void     (**init_array)(void);
    size_t     init_count;
    kl_stats   stats;
    char       path[512];
    const char **missing;      // unique unresolved import names
    unsigned    missing_n, missing_cap;
    const char **weak;         // ...and the weak ones, deliberately left NULL
    unsigned    weak_n, weak_cap;
    void       *dl_handle;     // set when the image arrived as a translated
                               // Mach-O dylib; then dyld owns the mapping
    int         tls_mod;       // index into g_tls_mod + 1; 0 = no PT_TLS
    const struct mach_header_64 *aot_header;
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

static void record_weak(kl_image *img, const char *nm) {
    if (!nm) return;
    for (unsigned i = 0; i < img->weak_n; i++)
        if (strcmp(img->weak[i], nm) == 0) return;
    if (img->weak_n == img->weak_cap) {
        img->weak_cap = img->weak_cap ? img->weak_cap * 2 : 32;
        img->weak = realloc(img->weak, img->weak_cap * sizeof *img->weak);
    }
    img->weak[img->weak_n++] = nm;
}

const char *const *kl_missing_imports(kl_image *img, unsigned *count) {
    if (count) *count = img->missing_n;
    return img->missing;
}

const char *const *kl_weak_imports(kl_image *img, unsigned *count) {
    if (count) *count = img->weak_n;
    return img->weak;
}

// ---------- unresolved-import stubs ----------
// Every unresolved import gets its OWN cell, so calling one fails BY NAME — the
// name is where the next milestone starts, and one shared abort stub can only
// report "called an unresolved import". The cell loads the name and tail-calls
// the reporter:
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
// An mmap'd pool generated into at runtime is a SIGKILL on visionOS instead —
// AMFI answers "namespace CODESIGNING, Invalid Page" for any pc that did not
// come from a signed file, whatever the page's permissions are.
// Measured on device 2026-08-07; see kl_stub_cells.S for the report.
#define KL_STUB_CELL_BYTES  8              /* mov w16,#i + b — see the .S */
// 2048 was sized for Beat Saber's five libraries. Steam Link's Qt chain is a
// different order of magnitude — libQt6Widgets ALONE exhausted it, and past
// exhaustion an import silently becomes an unnamed stub, so failing BY NAME —
// the property the whole bring-up method rests on — stops working. 16384;
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
// The mutex covers the dedup scan and the bump counter, which are racy under
// M6's concurrent stub traffic (the render thread calling existing stubs while
// the main thread creates new ones). It guards no W^X window: the cells are
// read-only text from the moment the binary is signed, and only the slots array
// is written.
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
// kl_gl_trace_tramp (runtime/gfx/kl_gl_trace.S), which saves the argument registers,
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

// ---------- rewrite `mrs xN, tpidr_el0` -> `mrs xN, tpidrro_el0` ----------
// Darwin keeps the thread pointer in TPIDRRO_EL0; TPIDR_EL0 is clobbered by the
// kernel on preemption. The two encodings differ by exactly one bit (0x20).
// 98% of these sites are -fstack-protector prologues reading bionic slot 5.
// It runs on the same chunks as the x18 pass and carries the same exposure to
// data inside an executable section: a word of a constant table matching this
// pattern gets a bit flipped, and the corrupted constant surfaces somewhere
// else entirely. One in 2^27 words matches by chance. Both rewriters consult
// the same data detector; this one must read the UNMODIFIED buffer, which it
// does, being the first pass over a chunk.
//
// A refusal here is NOT the free outcome it is on the x18 side, and it must
// never again be silent: a `mrs xN, tpidr_el0` left alone reads a register the
// kernel clobbers, so the site returns garbage on every thread and the guest
// faults dereferencing it somewhere else entirely — so a false positive in
// klx_looks_like_data reintroduces the very bug the TLS rewrite fixes. Each one
// is counted apart from the x18 population and
// named by address, because the address is what makes it a five-minute
// disassembly instead of a bisect.
// The guest-patch table's view of this mapping: one contiguous span, so a guest
// vaddr is a subtraction. Out of range answers NULL, which the table reports by
// address rather than treating as a miss.
static uint32_t *gp_at(void *ctx, uint64_t va) {
    struct gp_ctx { uint8_t *base; uint64_t lo, span; } *c = ctx;
    if (va < c->lo || va + 4 > c->lo + c->span) return NULL;
    return (uint32_t *)(c->base + (va - c->lo));
}

static void rewrite_tls(uint8_t *p, size_t n, uint64_t va, const char *path,
                        kl_stats *st) {
    uint32_t *w = (uint32_t *)p;
    size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        if ((w[i] & 0xffffffe0u) == 0xd53bd040u) {
            if (kl_x18_is_data(w, n, i)) {
                st->tls_refused++;
                fprintf(stderr, "  [klepton] %s: TLS site at +0x%llx left alone — its "
                                "neighbourhood reads as data. If it IS code, "
                                "that thread pointer is garbage\n",
                        path, (unsigned long long)(va + i * 4));
                continue;
            }
            w[i] |= 0x20u;
            st->tls_rewrites++;
        } else if (w[i] == 0xd4000001u) { st->svc_sites++; }    // svc #0 — report only
    }
}

// ---------- ELF TLS: R_AARCH64_TLSDESC, dynamic model ----------
// A guest library with its own `thread_local`/`__thread` data carries a PT_TLS
// segment and reaches it through TLS descriptors: a 16-byte GOT pair {resolver,
// argument}, called as `blr` with x0 = &pair, returning the variable's offset
// from the thread pointer, which the caller adds to its own `mrs tpidr_el0`
// (tpidrro_el0 after rewrite_tls / klepton-ld). Darwin's thread pointer is its
// TSD block, so there is no static TLS area to place a module at; instead each
// thread gets the module's block lazily on first touch, and the resolver
// answers `block + offset - tp`. bionic's dynamic-TLS path does the same.
//
// Only module-local descriptors (symbol 0, or a symbol this image defines) —
// every guest seen so far: Meta's libovravatar2 / libovrbody /
// libovrplugintracking / libovrgpuskinning (BRINK Traveler) carry nothing else,
// and before this each was refused as "unhandled relocation type 1031". A
// descriptor against an IMPORTED TLS symbol still refuses by name.
//
// The blocks are not freed at thread exit: guest thread_local destructors may
// run after a key destructor would, and a block is a few KB.
#define KL_TLS_MAX 64
typedef struct { const uint8_t *init; size_t filesz, memsz, align; } kl_tls_mod;
typedef struct { unsigned mod; uint64_t off; } kl_tlsdesc_arg;
static kl_tls_mod      g_tls_mod[KL_TLS_MAX];
static unsigned        g_tls_nmod;
static pthread_mutex_t g_tls_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t   g_tls_key;
static pthread_once_t  g_tls_once = PTHREAD_ONCE_INIT;
static void tls_key_make(void) { kl_key_create_or_die(&g_tls_key, NULL, "the guest's TLS blocks"); }

// Register an image's PT_TLS; returns tls_mod (index + 1), or 0 if none/full.
static int tls_register(kl_image *img, const Elf64_Phdr *ph, int phnum, uint64_t lo) {
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_TLS) continue;
        pthread_mutex_lock(&g_tls_mu);
        int idx = g_tls_nmod < KL_TLS_MAX ? (int)g_tls_nmod++ : -1;
        if (idx >= 0)
            g_tls_mod[idx] = (kl_tls_mod){ img->base + ph[i].p_vaddr - lo,
                                           ph[i].p_filesz, ph[i].p_memsz,
                                           ph[i].p_align ? ph[i].p_align : 1 };
        pthread_mutex_unlock(&g_tls_mu);
        if (idx < 0) { err("%s: more than %d TLS modules", img->path, KL_TLS_MAX); return 0; }
        return idx + 1;
    }
    return 0;
}

// The calling thread's copy of the variable. Called only from the resolver
// below, on the guest thread that asked.
void *kl_tls_addr(const kl_tlsdesc_arg *a) __asm__("_kl_tls_addr");
void *kl_tls_addr(const kl_tlsdesc_arg *a) {
    pthread_once(&g_tls_once, tls_key_make);
    void **v = pthread_getspecific(g_tls_key);
    if (!v) {
        v = calloc(KL_TLS_MAX, sizeof *v);
        if (!v) abort();
        pthread_setspecific(g_tls_key, v);
    }
    if (!v[a->mod]) {
        const kl_tls_mod *m = &g_tls_mod[a->mod];
        size_t al = m->align < 16 ? 16 : m->align;
        void *b = NULL;
        if (posix_memalign(&b, al, m->memsz ? m->memsz : 1) != 0) abort();
        memset(b, 0, m->memsz);
        memcpy(b, m->init, m->filesz);           // .tdata, already relocated
        v[a->mod] = b;
    }
    return (uint8_t *)v[a->mod] + a->off;
}

// The descriptor's resolver. The TLSDESC ABI lets it clobber x0 and nothing
// else, so it keeps every general register the C call may touch (x1-x18) and
// all of q0-q31 (AAPCS64 preserves only the low halves of v8-v15).
void kl_tlsdesc_resolve(void) __asm__("_kl_tlsdesc_resolve");
__asm__(
    ".text\n.p2align 2\n"
    "_kl_tlsdesc_resolve:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  sub sp, sp, #656\n"
    "  stp x1, x2, [sp, #0]\n   stp x3, x4, [sp, #16]\n"
    "  stp x5, x6, [sp, #32]\n  stp x7, x8, [sp, #48]\n"
    "  stp x9, x10, [sp, #64]\n stp x11, x12, [sp, #80]\n"
    "  stp x13, x14, [sp, #96]\n stp x15, x16, [sp, #112]\n"
    "  stp x17, x18, [sp, #128]\n"
    "  stp q0, q1, [sp, #144]\n  stp q2, q3, [sp, #176]\n"
    "  stp q4, q5, [sp, #208]\n  stp q6, q7, [sp, #240]\n"
    "  stp q8, q9, [sp, #272]\n  stp q10, q11, [sp, #304]\n"
    "  stp q12, q13, [sp, #336]\n stp q14, q15, [sp, #368]\n"
    "  stp q16, q17, [sp, #400]\n stp q18, q19, [sp, #432]\n"
    "  stp q20, q21, [sp, #464]\n stp q22, q23, [sp, #496]\n"
    "  stp q24, q25, [sp, #528]\n stp q26, q27, [sp, #560]\n"
    "  stp q28, q29, [sp, #592]\n stp q30, q31, [sp, #624]\n"
    "  ldr x0, [x0, #8]\n"
    "  bl _kl_tls_addr\n"
    "  mrs x1, tpidrro_el0\n"
    "  sub x0, x0, x1\n"
    "  ldp q0, q1, [sp, #144]\n  ldp q2, q3, [sp, #176]\n"
    "  ldp q4, q5, [sp, #208]\n  ldp q6, q7, [sp, #240]\n"
    "  ldp q8, q9, [sp, #272]\n  ldp q10, q11, [sp, #304]\n"
    "  ldp q12, q13, [sp, #336]\n ldp q14, q15, [sp, #368]\n"
    "  ldp q16, q17, [sp, #400]\n ldp q18, q19, [sp, #432]\n"
    "  ldp q20, q21, [sp, #464]\n ldp q22, q23, [sp, #496]\n"
    "  ldp q24, q25, [sp, #528]\n ldp q26, q27, [sp, #560]\n"
    "  ldp q28, q29, [sp, #592]\n ldp q30, q31, [sp, #624]\n"
    "  ldp x1, x2, [sp, #0]\n   ldp x3, x4, [sp, #16]\n"
    "  ldp x5, x6, [sp, #32]\n  ldp x7, x8, [sp, #48]\n"
    "  ldp x9, x10, [sp, #64]\n ldp x11, x12, [sp, #80]\n"
    "  ldp x13, x14, [sp, #96]\n ldp x15, x16, [sp, #112]\n"
    "  ldp x17, x18, [sp, #128]\n"
    "  add sp, sp, #656\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n");

// ---------- symbol lookup ----------
// Registered interposers, consulted for defined and undefined symbols alike.
// Small and fixed: this is an instrument, and a table that has to grow is a sign
// the question being asked wants a different tool.
static struct { const char *name; void *fn; } g_interpose[8];
static unsigned g_interpose_n;

void kl_interpose(const char *name, void *fn) {
    if (!name || !fn || g_interpose_n >= 8) return;
    g_interpose[g_interpose_n].name = name;
    g_interpose[g_interpose_n].fn = fn;
    g_interpose_n++;
}

static uint64_t sym_value(kl_image *img, uint32_t idx, const char **name_out) {
    const Elf64_Sym *s = &img->symtab[idx];
    const char *nm = img->strtab + s->st_name;
    if (name_out) *name_out = nm;
    for (unsigned i = 0; i < g_interpose_n; i++)
        if (!strcmp(nm, g_interpose[i].name))
            return (uint64_t)(uintptr_t)g_interpose[i].fn;
    if (s->st_shndx != 0) {
        // C++ inline data (including its one-time initialization guard) must be
        // shared across Scotland2 modules. Otherwise a later mod gets another
        // copy of an already registered dependency's class metadata. Keep this
        // scoped to module opens and default-visible weak/unique OBJECTs;
        // protected/hidden storage and source-adapted function bodies stay local.
        unsigned binding=s->st_info>>4,type=s->st_info&15,visibility=s->st_other&3;
        if (kl_extension_strict_load() && type==1 && visibility==0 && (binding==2 || binding==10)) {
            void *shared=kl_guest_sym_global(nm);
            if(shared)return (uint64_t)(uintptr_t)shared;
        }
        return (uint64_t)(uintptr_t)(img->base + s->st_value);
    }
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
            if (sidx == 0) {
                // STN_UNDEF: a GLOB_DAT/ABS64 against symbol 0 resolves to 0
                // (plus addend) per the ELF spec, exactly as bionic does — it is
                // not a missing import. Without this the slot would get an abort
                // trampoline and a guest that null-tests the pointer would call
                // straight into it. APS2 streams emit these against symbol 0.
                v = 0;
            } else if (!v && (img->symtab[sidx].st_info >> 4) == STB_WEAK
                   && img->symtab[sidx].st_shndx == 0) {
                // A WEAK undefined must stay NULL — that is the whole mechanism
                // by which a guest asks whether an optional symbol exists. The
                // call site null-tests the slot, so an abort trampoline here
                // answers "present" and the guest then calls it. Beat Saber 1.40
                // is where this stopped being theoretical: libunity weak-imports
                // the entire AMediaCodec / AImageReader / AHardwareBuffer family
                // to probe for NDK video decode, and every name kl_mediandk.c
                // does not implement would have read as supported.
                //
                // Reported, not silent: this trades an abort naming the symbol
                // for a NULL call naming nothing, so record_weak keeps the list.
                img->stats.imports_weak_null++;
                record_weak(img, nm);
            } else if (!v) {
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
        case R_AARCH64_TLSDESC: {
            // See the ELF TLS block above: module-local descriptors only.
            //
            // Not for every guest yet (KL_ELF_TLSDESC, default on for the title
            // this was sent in for: BRINK Traveler, fix 9, 2026-10-11). Until
            // now an image with such a relocation did not load, and where a
            // guest loads it by dlopen that is a NULL it carries on from: a
            // title that works today may ship an optional library it has been
            // running WITHOUT, and would start running it. That wants a look at
            // the libraries of the titles that work (tools/audit_elf.py counts
            // these relocations) before this becomes what the loader does.
            {
                static int on = -1;
                if (on < 0) {
                    extern const char *kl_driver_target_name(void);
                    const char *t = kl_driver_target_name();
                    // ...and Wooorld (x4360608937312293), where the library is
                    // not optional: its sign-in calls into libovravatar2 right
                    // after the platform user arrives, and with the library
                    // refused that is a DllNotFoundException which ends the
                    // chain on a black screen (2026-10-11).
                    // ...and Premium Bowling (x2773034772778845): it carries on
                    // without libovravatar2 ("shutting down AvatarSDK"), but a
                    // NullReferenceException follows when its first menu comes
                    // up; with the library loaded there is none (Mac host,
                    // 2026-10-12).
                    // ...and Demeo (x3634830803298285): libInteractionSdk,
                    // refused, is a DllNotFoundException as its hand-tracking
                    // module comes up; loaded, there is none (Mac host,
                    // 2026-10-12).
                    // ...and BattleGroupVR2 (x9386827918099701): the
                    // LibRuntimeOptimizer it ships has such a relocation and
                    // was refused (the sender's run, fix 38, 2026-10-13).
                    on = kl_env_on("KL_ELF_TLSDESC", t && (strcmp(t, "x3635172946605196") == 0 ||
                                                           strcmp(t, "x4360608937312293") == 0 ||
                                                           strcmp(t, "x2773034772778845") == 0 ||
                                                           strcmp(t, "x3634830803298285") == 0 ||
                                                           strcmp(t, "x2582932495064035") == 0 ||
                                                           strcmp(t, "x9386827918099701") == 0 ||
                    // ...and DreamSpace (x9449316705178989): Meta's MR Utility
                    // Kit (libmrutilitykitshared) is refused without it, and the
                    // title's scene layer then throws a NullReferenceException
                    // (2026-10-16).
                                                           strcmp(t, "x9449316705178989") == 0) ||
                    // ...and every title under the profile for untested games:
                    // without it the library that has one is refused and the
                    // game usually ends on the error that follows.
                                       kl_untested_profile());
                }
                if (!on) {
                    err("unhandled relocation type %u at %#llx (ELF TLS; KL_ELF_TLSDESC=1 serves it)",
                        type, (unsigned long long)r->r_offset);
                    return -1;
                }
            }
            if (!img->tls_mod) {
                err("TLSDESC at %#llx in an image with no PT_TLS", (unsigned long long)r->r_offset);
                return -1;
            }
            if (sidx && img->symtab[sidx].st_shndx == 0) {
                err("TLSDESC at %#llx against imported TLS symbol %s — not supported",
                    (unsigned long long)r->r_offset, img->strtab + img->symtab[sidx].st_name);
                return -1;
            }
            kl_tlsdesc_arg *a = malloc(sizeof *a);
            if (!a) { err("TLSDESC at %#llx: out of memory", (unsigned long long)r->r_offset); return -1; }
            a->mod = (unsigned)(img->tls_mod - 1);
            a->off = (sidx ? img->symtab[sidx].st_value : 0) + (uint64_t)r->r_addend;
            slot[0] = (uint64_t)(uintptr_t)kl_tlsdesc_resolve;
            slot[1] = (uint64_t)(uintptr_t)a;
            break;
        }
        default:
            err("unhandled relocation type %u at %#llx", type, (unsigned long long)r->r_offset);
            return -1;
        }
    }
    return 0;
}

// RELR: a bitmap-packed table of nothing but R_AARCH64_RELATIVE relocations. An
// even entry is an address to relocate and seeds `where` for the bitmaps after
// it; an odd entry is a bitmap whose set bits (from bit 1) relocate the 63 words
// starting at `where`. Every relocated slot holds a link-time offset that the
// load bias (img->base, since lo is 0 for every guest here) is added to — the
// same adjustment R_AARCH64_RELATIVE makes, which is why none of this needs a
// symbol. This is where a current NDK puts the .init_array pointers.
static void apply_relr(kl_image *img, const uint64_t *relr, size_t count) {
    const uintptr_t bias = (uintptr_t)img->base;
    uint64_t *where = NULL;
    for (size_t i = 0; i < count; i++) {
        uint64_t e = relr[i];
        if ((e & 1) == 0) {
            where = (uint64_t *)(bias + (uintptr_t)e);
            *where++ += bias;
            img->stats.relative++;
        } else {
            uint64_t bits = e >> 1;
            for (uint64_t *w = where; bits; bits >>= 1, w++)
                if (bits & 1) { *w += bias; img->stats.relative++; }
            where += 63;
        }
    }
}

static int64_t read_sleb128(const uint8_t **pp, const uint8_t *end) {
    int64_t r = 0; int shift = 0; uint8_t b = 0;
    const uint8_t *p = *pp;
    do {
        if (p >= end) break;
        b = *p++;
        r |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) r |= -((int64_t)1 << shift);
    *pp = p;
    return r;
}

// APS2 (Android packed relocations): a group-compressed stream of RELA entries.
// Each is decoded back to (r_offset, r_info, r_addend) and handed to apply_relocs
// one at a time, so a packed RELATIVE / GLOB_DAT / ABS64 / symbol-0 entry is
// treated exactly as the same entry would be in a plain DT_RELA table. The four
// group flags say which fields are shared across the group vs. read per-entry.
static int apply_android_packed(kl_image *img, const uint8_t *blob, size_t sz) {
    const uint8_t *p = blob, *end = blob + sz;
    if (sz < 4 || memcmp(p, "APS2", 4) != 0) { err("DT_ANDROID_RELA not APS2"); return -1; }
    p += 4;
    int64_t count  = read_sleb128(&p, end);
    int64_t offset = read_sleb128(&p, end);
    int64_t info = 0, addend = 0;
    enum { GRP_BY_INFO = 1, GRP_BY_OFF_DELTA = 2, GRP_BY_ADDEND = 4, GRP_HAS_ADDEND = 8 };
    while (count > 0) {
        int64_t gsz = read_sleb128(&p, end);
        int64_t gf  = read_sleb128(&p, end);
        int64_t goff_delta = (gf & GRP_BY_OFF_DELTA) ? read_sleb128(&p, end) : 0;
        if (gf & GRP_BY_INFO) info = read_sleb128(&p, end);
        if ((gf & GRP_HAS_ADDEND) && (gf & GRP_BY_ADDEND)) addend += read_sleb128(&p, end);
        else if (!(gf & GRP_HAS_ADDEND)) addend = 0;
        for (int64_t k = 0; k < gsz && count > 0; k++, count--) {
            offset += (gf & GRP_BY_OFF_DELTA) ? goff_delta : read_sleb128(&p, end);
            if (!(gf & GRP_BY_INFO)) info = read_sleb128(&p, end);
            if ((gf & GRP_HAS_ADDEND) && !(gf & GRP_BY_ADDEND)) addend += read_sleb128(&p, end);
            Elf64_Rela r; r.r_offset = (uint64_t)offset; r.r_info = (uint64_t)info;
            r.r_addend = addend;
            if (apply_relocs(img, &r, 1) != 0) return -1;
        }
    }
    return 0;
}

// Walk PT_DYNAMIC and bind the image: symtab/strtab, DT_INIT_ARRAY, and every
// relocation. Shared by both loaders — the mmap path below and kl_load_dylib()
// — because this half is identical either way. It is what lets klepton-ld emit a
// dylib with ZERO dyld fixups: nothing here needs dyld's help, and no relocation
// target is in __TEXT (measured across every guest library), so the guest image
// can be mapped read-execute by dyld and still relocate correctly.
//
// `lo` is the lowest PT_LOAD vaddr, so `img->base + p_vaddr - lo` addresses the
// mapped copy in both cases (it is 0 for every guest library here).
static int bind_dynamic(kl_image *img, const Elf64_Phdr *ph, int phnum, uint64_t lo) {
    const Elf64_Rela *rela = NULL, *jmprel = NULL;
    size_t relasz = 0, pltsz = 0;
    const uint64_t *relr = NULL; size_t relrsz = 0;
    const uint8_t *arela = NULL; size_t arelasz = 0;
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
            case DT_RELR:
            case DT_ANDROID_RELR:   relr   = (const uint64_t *)(img->base + dp->d_val); break;
            case DT_RELRSZ:
            case DT_ANDROID_RELRSZ: relrsz = dp->d_val; break;
            case DT_ANDROID_RELA:   arela   = (const uint8_t *)(img->base + dp->d_val); break;
            case DT_ANDROID_RELASZ: arelasz = dp->d_val; break;
            }
        }
    }
    if (!img->symtab || !img->strtab) { err("missing DT_SYMTAB/DT_STRTAB"); return -1; }
    img->tls_mod = tls_register(img, ph, phnum, lo);

    // Order is independent — RELA, RELR and APS2 relocate disjoint slots — but
    // every relative table must run before kl_run_init: the .init_array pointers
    // live in RELR/APS2 on a modern NDK and are raw offsets until relocated.
    if ((rela   && apply_relocs(img, rela,   relasz / sizeof(Elf64_Rela)) != 0) ||
        (jmprel && apply_relocs(img, jmprel, pltsz  / sizeof(Elf64_Rela)) != 0))
        return -1;
    if (relr) apply_relr(img, relr, relrsz / sizeof(uint64_t));
    if (arela && apply_android_packed(img, arela, arelasz) != 0) return -1;
    return 0;
}

// ---------- load a klepton-ld-translated Mach-O dylib ----------
// The shipping loader. dyld maps and (on device) AMFI validates the guest text,
// so nothing here is ever RWX and no guest byte is written by us — the TLS
// rewrite and the x18 veneers were applied offline by klepton-ld.
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
    if (rec && (statsz < sizeof *rec || rec->magic != KLX_STAT_MAGIC)) {
        // A record we cannot read is not a record we may ignore: the TSD-slot
        // check below is the only thing standing between a stale translation
        // and seventeen thousand veneers indexing somebody else's per-thread
        // state. An older klepton-ld wrote a shorter record with a different
        // magic, and both would have failed the test above silently.
        err("%s carries a __klstat record this runtime does not recognise "
            "(magic %#x, %lu bytes; expected %#x, %zu) — re-translate it with "
            "this tree's klepton-ld", real, rec->magic, statsz,
            KLX_STAT_MAGIC, sizeof *rec);
        free(img); dlclose(h); return NULL;
    }
    if (rec) {
        img->stats.tls_rewrites = rec->tls_rewrites;
        img->stats.x18_sites    = rec->sites;
        img->stats.x18_patched  = rec->patched;
        img->stats.x18_refused  = rec->refused;
        img->stats.ctr_sites    = rec->ctr_sites;
        img->stats.ctr_patched  = rec->ctr_patched;

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
        // What the translation did, on the record, on the device. A `br x18`
        // that branches through a zeroed x18 is a codesigning SIGKILL that
        // cannot report itself, and a refused site that WRITES x18 leaves the
        // shadow stale and poisons every later read of it — so the one number
        // that rules that class in or out is how many sites this library's
        // veneer pass refused. klepton-ld recorded it and it was read into
        // img->stats above; print it, including the quiet "0 REFUSED" case: a
        // line that only appears when something is wrong cannot be used to
        // prove that nothing is.
        if (rec->sites || rec->refused)
            fprintf(stderr, "  [klepton] %s: x18 translated offline — %u site(s), "
                            "%u patched, %u REFUSED%s\n",
                    real, rec->sites, rec->patched, rec->refused,
                    rec->refused ? "  <-- refused sites read Darwin's x18; a "
                                   "refused WRITE poisons every later read"
                                 : "");
        kl_x18_note_translated(rec->refused);

        // The CTR_EL0 constant is self-contained — the veneer answers it and
        // nothing here has to agree — so a difference is reported rather than
        // refused. It is reported at all because "the CTR_EL0 value in this
        // dylib is not the one this tree defaults to" is otherwise invisible,
        // and the A/B for it (KL_CTR_EL0) is baked at translation time.
        if (rec->ctr_patched && rec->ctr_value != KLX_CTR_EL0_VALUE)
            fprintf(stderr, "  [klepton] %s: %u CTR_EL0 veneer(s) answer %#x, not "
                            "this tree's default %#x\n",
                    real, rec->ctr_patched, rec->ctr_value, KLX_CTR_EL0_VALUE);
    }

    if (kl_aot_attach(mh, elf) != 0) {
        err("%s: %s",real,kl_aot_error());
        free(img); dlclose(h); return NULL;
    }
    img->aot_header=mh;
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

    // The on-disk framework/dylib can't carry '+' in its name — the App Store rejects
    // a CFBundleExecutable that does (libc++_shared, error 90121) — so mkguest maps it
    // to a filesystem-safe name. Apply the SAME map here to build the path, while the
    // guest keeps calling the library by its real DT_NEEDED name (name_out, above).
    char fsname[256];
    snprintf(fsname, sizeof fsname, "%s", name);
    for (char *p = fsname; *p; p++) if (*p == '+') *p = 'x';

    // Two layouts, because the host and the bundle disagree about what a
    // translated library looks like. `make dylibs` writes bare .dylib files; an
    // app bundle carries frameworks, which is also the layout AMFI accepts —
    // Xcode code-signs what it embeds in Frameworks/, and a loose Mach-O
    // elsewhere in the bundle is only sealed, not signed. Same image either
    // way.
    snprintf(out, cap, "%s/%s.dylib", dir, fsname);
    if (access(out, R_OK) == 0) return 1;
    snprintf(out, cap, "%s/%s.framework/%s", dir, fsname, fsname);
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
        fprintf(stderr, "  [klepton] %s: loaded as a translated dylib\n", name);
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
// A stat(path) here has ClassLoader.findLibrary() return null for libil2cpp, so
// Unity never attempts the dlopen that would have succeeded, and puts up
// "Failed to load Il2CPP." three layers away from the stat that caused it. The
// path stays the .so path in the answer, so there is still exactly one place
// that resolves it: here.
int kl_can_load(const char *path) {
    char cand[1024];
    if (dylib_candidate(path, cand, sizeof cand, NULL, 0)) return 1;
    return access(path, R_OK) == 0;
}

// ---------- segment protection, when the HOST page is coarser than p_align ----------
//
// Apply PT_LOAD permissions at HOST page granularity, as the union of every
// segment sharing a page — and then resolve the one union that cannot be
// applied.
//
// A per-segment mprotect() rounded out to the page is correct only while no two
// segments share one, which held while every guest's p_align was at least
// Apple's 16 KB page: Beat Saber 1.28 ships 64 KB, Steam Link 16 KB. Beat Saber
// 1.40 ships
// **4 KB** — eleven of its thirteen libraries — so segments now share pages,
// the last one written wins, and libmain's r-x segment was left RW because the
// RW segment 0x14e4 bytes above it is processed after it. JNI_OnLoad then took
// an instruction fetch from a non-executable page: SIGBUS with the fault
// address equal to the pc, which reads like a wild jump rather than a
// permission. Nothing about it named the loader.
//
// The union is W|X on exactly one page per library (where the r-x segment's
// tail meets the RW segment's head), and macOS refuses mprotect(RWX) on
// ordinary anonymous memory with EACCES, so "just OR them" does not survive
// contact. Two rules settle every case in the corpus, and both are readings of
// what the file already says rather than heuristics:
//
//   1. Drop X if no SHF_EXECINSTR SECTION lands on the page. The r-x LOAD
//      segment spans .rodata, .dynsym, .rela.dyn and .eh_frame as well as
//      .text — the same distinction the rewrite passes above rely on — so the
//      X on such a page was never for code. 8 of 11 libraries, libunity,
//      libil2cpp and libenet among them.
//   2. Otherwise drop W if every writable byte on the page is inside
//      PT_GNU_RELRO. RELRO is by definition read-only once relocation is done,
//      and relocation IS done here, so this is the ELF's own instruction rather
//      than a concession. The remaining 3 — libmain, lib_burst_generated,
//      libhaptics_sdk.
//
// Anything left is a page that genuinely needs both, which no permission can
// express. It fails by name rather than silently mis-protecting, because the
// symptom on either side is a fault a long way from here.
// How many host pages an image of `span` bytes placed `bias` into its mapping
// covers, and which one a guest vaddr lands on. Both are here rather than
// inline so the planner and the applier cannot disagree about the arithmetic.
static size_t page_count(size_t span, size_t bias, uintptr_t pg) {
    return (span + bias + pg - 1) / pg;
}

// Fill `want` — the ELF PF_* bits each host page needs, as the union of every
// segment on it — and then resolve the unions that cannot be applied, by the
// two rules above. Returns 0 if every page came out expressible, -1 otherwise;
// on -1 `*bad_va` is the guest vaddr the first unresolvable page starts at.
//
// `want` may be NULL, which is the bias search asking whether a placement is
// loadable at all rather than what its protections would be.
static int plan_pages(const Elf64_Ehdr *eh, const Elf64_Phdr *ph, const Elf64_Shdr *sh,
                      uint64_t lo, size_t span, size_t bias, uintptr_t pg,
                      uint8_t *want, int64_t *bad_va) {
    const size_t npages = page_count(span, bias, pg);
    uint8_t *bits = want ? want : calloc(npages, 1);
    if (!bits) return -1;
    if (want) memset(want, 0, npages);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !ph[i].p_memsz) continue;
        size_t off = (size_t)(ph[i].p_vaddr - lo) + bias;
        size_t s = off / pg, e = (off + (size_t)ph[i].p_memsz + pg - 1) / pg;
        for (size_t p = s; p < e && p < npages; p++) bits[p] |= (uint8_t)ph[i].p_flags;
    }

    int rc = 0;
    for (size_t p = 0; p < npages; p++) {
        if ((bits[p] & (PF_W | PF_X)) != (PF_W | PF_X)) continue;
        // Signed, because with a bias the first page starts BELOW the image's
        // lowest vaddr — the bytes there are padding we own and nothing claims.
        int64_t ps = (int64_t)lo + (int64_t)(p * pg) - (int64_t)bias;
        int64_t pe = ps + (int64_t)pg;

        int has_code = 0;
        for (int i = 0; sh && i < eh->e_shnum; i++)
            if ((sh[i].sh_flags & SHF_EXECINSTR) && sh[i].sh_type != SHT_NOBITS
                && (int64_t)sh[i].sh_addr < pe
                && (int64_t)(sh[i].sh_addr + sh[i].sh_size) > ps)
                has_code = 1;
        if (!has_code) { bits[p] &= (uint8_t)~PF_X; continue; }

        // Every writable byte on this page must be inside a PT_GNU_RELRO range.
        int relro_covers = 1;
        for (int i = 0; i < eh->e_phnum && relro_covers; i++) {
            if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_W)) continue;
            int64_t ws = (int64_t)ph[i].p_vaddr, we = ws + (int64_t)ph[i].p_memsz;
            if (ws >= pe || we <= ps) continue;
            if (ws < ps) ws = ps;
            if (we > pe) we = pe;
            int covered = 0;
            for (int j = 0; j < eh->e_phnum; j++)
                if (ph[j].p_type == PT_GNU_RELRO && (int64_t)ph[j].p_vaddr <= ws
                    && (int64_t)(ph[j].p_vaddr + ph[j].p_memsz) >= we)
                    covered = 1;
            if (!covered) relro_covers = 0;
        }
        if (relro_covers) { bits[p] &= (uint8_t)~PF_W; continue; }

        if (bad_va) *bad_va = ps;
        rc = -1;
        break;
    }
    if (!want) free(bits);
    return rc;
}

// ...and the third way out, which is not a protection at all: MOVE THE IMAGE.
//
// The general way out, and BONELAB needs it. Its libmain.so has just TWO PT_LOADs,
// r-x at 0 and rw- at 0x2d80, so both live in host page 0; PT_GNU_RELRO covers
// 0x2d80..0x3000 and the 0x40 bytes above it are ordinary .data. Rule 1 cannot
// apply (there is real code on the page) and rule 2 cannot (those 0x40 bytes are
// writable and outside RELRO), so the library that IS the entry point failed to
// load, before any of the interesting questions about the title could be asked.
//
// What no permission can express, placement can: the mapping is ours to choose,
// and shifting the image inside it moves where the host page boundaries FALL
// without moving any segment relative to any other. Put a boundary in the gap
// between the last executable byte and the first writable one and the collision
// is gone rather than resolved. klepton_ld.c does exactly this offline for the
// dylib path (see its `image_shift` search) — this is the runtime's half, and
// the two now agree that a guest's placement is part of loading it.
//
// **The shift must be a multiple of 4096**, whatever else it does: `adrp`
// resolves against a 4 KB page, so every PC-relative reference the linker
// encoded survives only if the whole image moves by a multiple of that. A shift
// that is not costs a run to diagnose — klepton_ld.c's comment records what it
// looks like (a static table read one page out, and nothing naming the shift).
//
// Stepping by 4 KB through one host page is exhaustive, because the layout
// repeats mod the host page size. Bias 0 is tried FIRST and wins whenever it is
// loadable, so every guest that loads today is placed exactly where it is
// placed today — a 16 KB-or-coarser guest never reaches the second candidate.
#define KL_ADRP_PAGE 0x1000u
static size_t choose_bias(const Elf64_Ehdr *eh, const Elf64_Phdr *ph,
                          const Elf64_Shdr *sh, uint64_t lo, size_t span,
                          uintptr_t pg) {
    for (size_t bias = 0; bias < (size_t)pg; bias += KL_ADRP_PAGE)
        if (plan_pages(eh, ph, sh, lo, span, bias, pg, NULL, NULL) == 0)
            return bias;
    return 0;   // none works; protect_pages reports which page and why
}

static int protect_pages(kl_image *img, const Elf64_Ehdr *eh, const Elf64_Phdr *ph,
                         const Elf64_Shdr *sh, uint64_t lo) {
    const uintptr_t pg = (uintptr_t)getpagesize();
    const size_t npages = page_count(img->span, img->map_off, pg);
    uint8_t *want = calloc(npages, 1);          // ELF PF_* bits, per host page
    if (!want) { err("out of memory for %zu page protections", npages); return -1; }

    int64_t bad_va = 0;
    if (plan_pages(eh, ph, sh, lo, img->span, img->map_off, pg, want, &bad_va) != 0) {
        uint64_t minalign = 0;
        for (int i = 0; i < eh->e_phnum; i++)
            if (ph[i].p_type == PT_LOAD && (!minalign || ph[i].p_align < minalign))
                minalign = ph[i].p_align;
        // Reached only when NO placement works either — choose_bias has already
        // tried every 4 KB offset through a host page. So the gap between the
        // executable bytes and the writable ones is narrower than a page
        // everywhere, which is a property of the file and not of this run.
        err("%s: page at vaddr %#llx needs W and X together and neither rule "
            "applies — executable sections are present AND writable bytes "
            "outside PT_GNU_RELRO share it, at every 4 KB placement of the "
            "image (LOAD p_align %#llx < host page %#lx)",
            img->path, (unsigned long long)bad_va,
            (unsigned long long)minalign, (unsigned long)pg);
        free(want);
        return -1;
    }

    // Coalesce equal-protection runs: libil2cpp is 5380 pages and would
    // otherwise cost 5380 syscalls and as many VM map entries.
    uint8_t *map = img->base - img->map_off;
    for (size_t p = 0; p < npages; ) {
        if (!want[p]) { p++; continue; }        // a gap between segments; leave RW
        size_t q = p;
        while (q < npages && want[q] == want[p]) q++;
        int prot = ((want[p] & PF_R) ? PROT_READ  : 0) |
                   ((want[p] & PF_W) ? PROT_WRITE : 0) |
                   ((want[p] & PF_X) ? PROT_EXEC  : 0);
        if (mprotect(map + p * pg, (q - p) * pg, prot) != 0)
            fprintf(stderr, "  [klepton] mprotect(%p,%#zx,%d) failed: %s\n",
                    (void *)(map + p * pg), (q - p) * pg, prot, strerror(errno));
        p = q;
    }
    free(want);
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

    // Section headers are read here rather than at the rewrite passes below
    // because the placement decision needs them too: rule 1 asks which pages
    // carry executable SECTIONS, not which carry the executable segment.
    const Elf64_Shdr *sh = (eh->e_shoff && eh->e_shnum)
                         ? (const Elf64_Shdr *)(file + eh->e_shoff) : NULL;

    // One RW mapping for the whole vaddr span; permissions tightened after
    // relocation. The span is padded by a host page and the image placed
    // `map_off` into it, so that a guest whose p_align is finer than the host
    // page can have a page boundary land between its executable bytes and its
    // writable ones — see choose_bias(). map_off is 0 for every guest that does
    // not need it, which is every guest aligned to 16 KB or coarser.
    const uintptr_t pg = (uintptr_t)getpagesize();
    img->map_off  = choose_bias(eh, ph, sh, lo, img->span, pg);
    img->map_size = (img->span + img->map_off + pg - 1) & ~((size_t)pg - 1);

    // ...and the x18 veneer pool is reserved WITH the image, on the end of the
    // same mapping.
    //
    // A separate mmap with a hint just past the code does NOT work: a hint is
    // only a hint, Darwin returns the first free hole at or above it,
    // and the address after a guest image is exactly the one guaranteed to be
    // taken. For libUE4 (172 MB, every one of its 8038 x18 sites in a 5 MB band
    // at the top) under a viewer run — ANGLE, Metal and SDL already mapped — the
    // nearest hole measures 240 MB away, past `b`'s +/-128 MB, so every veneer
    // is refused and the library runs against Darwin's reserved x18. Searching
    // harder does not help: there is no free hole in the window at all. Only a
    // reservation made at the same moment as the image is guaranteed to be
    // within reach of it.
    //
    // Sized by counting the sites in the FILE's executable sections, which is
    // exact enough to be tight: relocations never touch guest text, so the
    // words counted here are the words the rewrite pass will see. The count is
    // an over-estimate only in that it includes data words the pass will refuse.
    size_t x18_arena = 0;
    for (int i = 0; sh && i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_PROGBITS || !(sh[i].sh_flags & SHF_EXECINSTR)) continue;
        if (sh[i].sh_offset + sh[i].sh_size > (uint64_t)sb.st_size) continue;
        x18_arena += (size_t)kl_x18_count(file + sh[i].sh_offset, (size_t)sh[i].sh_size);
        // ...plus the jump-table rewrite's worst case per `br x18`, the one site
        // shape whose veneer grows a thunk per table target.
        x18_arena += (size_t)kl_x18_count_termbr(file + sh[i].sh_offset,
                                                 (size_t)sh[i].sh_size)
                   * (KLX_JT_MAX_INSN / KLX_VEN_MAX_INSN + 2);
    }
    if (x18_arena) {
        // Per-chunk pools each round up to 64 KB and one section can be several
        // chunks (data ranges inside .text split it), so the reservation
        // carries slack for that rounding rather than exactly the veneer bytes.
        x18_arena = x18_arena * KLX_VEN_MAX_INSN * 4 + (64u << 10) * 64;
        x18_arena = (x18_arena + pg - 1) & ~((size_t)pg - 1);
    }
    img->x18_arena_size = x18_arena;

    // A second arena of the same size BEFORE the image. The end-of-image arena
    // is within b's +/-128 MB of the top of the code only; for a library wider
    // than one window (Deadpool VR's libUE4.so is 262 MB) the bottom half needs
    // a pool below the image just as much, and "somewhere free nearby" is luck —
    // one ASLR roll gives a 436 MB gap below, the next gives 15 MB and every
    // veneer is REACH-refused. Reserved here, it is in reach by construction,
    // which is the whole argument the end arena was added on. Virtual-address
    // space only; untouched pages are never committed.
    size_t x18_pre_arena = x18_arena;
    uint8_t *map = mmap(NULL, x18_pre_arena + img->map_size + x18_arena,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) { err("mmap image: %s", strerror(errno));
                             free(img); munmap(file, sb.st_size); return NULL; }
    img->x18_pre_arena_size = x18_pre_arena;
    img->base = map + x18_pre_arena + img->map_off;
    if (img->map_off)
        fprintf(stderr, "  [klepton] %s: placed %#zx into its mapping so a host "
                        "page boundary falls between the executable bytes and "
                        "the writable ones\n", path, img->map_off);
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

    // ---- rewrite guest text: TLS, then x18 ----
    //
    // Executable *sections*, not the executable segment. The r-x LOAD segment of
    // these libraries starts at file offset 0 and runs to the end of
    // .gcc_except_table, so it also spans .hash, .dynsym, .rela.dyn, .rodata and
    // .eh_frame — for libunity that is 1.5 MB of read-only data and 720 KB of
    // relocations sitting behind PF_X. Rewriting a word that merely looks like
    // an instruction in there corrupts data — the x18 pass patches a branch
    // rather than flipping one bit, so it corrupts several words at a time.
    // (`sh` is read above — the placement decision needs the same distinction.)

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
                        "from rodata; x18 veneering disabled\n", path);

    for (int i = 0; i < eh->e_phnum && !sh; i++) {
        // Fallback for a stripped image: the old whole-segment scan. Narrower
        // than it should be is not an option for TLS — an unrewritten
        // tpidr_el0 read is a wrong answer on every thread.
        if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X)) continue;
        rewrite_tls(img->base + ph[i].p_vaddr - lo, (size_t)ph[i].p_filesz,
                    ph[i].p_vaddr, path, &img->stats);
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
    // The reservation made with the mapping above, carved across every chunk of
    // every executable section. Running out is not silent: kl_x18_patch falls
    // back to searching for a hole, and a search that fails leaves the refusals
    // that the report below names.
    uint8_t *x18_arena_base = img->x18_arena_size
                            ? img->base - img->map_off + img->map_size : NULL;
    size_t   x18_arena_used = 0;
    uint8_t *x18_pre_base = img->x18_pre_arena_size
                          ? img->base - img->map_off - img->x18_pre_arena_size : NULL;
    size_t   x18_pre_used = 0;

    // The image behind each chunk, so the `br x18` jump-table rewrite can reach
    // the table it has to repoint. Here the chunk addresses ARE the mapped
    // addresses, so resolving is only a bounds check against the section that
    // owns the VA — and the section must be read-only ALLOC data: executable
    // would let a mis-recognition write over code, and writable is rewritten by
    // relocations, which would undo the repointing without a word.
    struct x18_img_ctx ictx = { sh, eh->e_shnum, (uint64_t)(uintptr_t)img->base - lo };
    kl_x18_image ximg = { x18_img_data_at, &ictx };

    for (int i = 0; sh && i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_PROGBITS || !(sh[i].sh_flags & SHF_EXECINSTR)) continue;
        uint64_t cursor = sh[i].sh_addr, cva;
        size_t csz;
        while (kl_x18_next_code(sh[i].sh_addr, (size_t)sh[i].sh_size,
                                skip, nskip, &cursor, &cva, &csz)) {
            uint8_t *p = img->base + cva - lo;
            rewrite_tls(p, csz, cva, path, &img->stats);
            if (!veneer) continue;
            // One more split on top of kl_x18_next_code's. A veneer pool has to
            // sit within b's +/-128 MB of EVERY site it serves, and a single
            // executable section of a 262 MB library is wider than two of those
            // windows put together: one pool per section is in reach of the
            // section's ends only. Slicing gives each slice its own pool — an
            // arena's where an arena is near enough, klx_place_pool's search
            // elsewhere — so a pool only has to reach its own slice's sites.
            // 32 MB keeps a slice's window wide enough that the pool search has
            // somewhere to land. A chunk under 32 MB is one slice, exactly as
            // before.
            for (size_t off = 0; off < csz; ) {
            size_t slice = csz - off;
            if (slice > (32u << 20)) slice = 32u << 20;
            // Pick the arena whose next slot this slice can actually branch to:
            // the end arena for the top of the image, the pre-image arena for
            // the bottom. 120 MB rather than the exact 128 — the pool grows
            // past the slot and kl_x18_patch re-checks the extremes itself,
            // falling back to klx_place_pool's search when neither arena
            // answers (the unservable middle band of a >256 MB image).
            void   *arena = NULL; size_t *aused = NULL, asize = 0;
            int64_t slo = (int64_t)(uintptr_t)(p + off);
            int64_t shi = slo + (int64_t)slice;
            const int64_t REACH = 120LL << 20;
            if (x18_arena_base) {
                int64_t slot = (int64_t)(uintptr_t)(x18_arena_base + x18_arena_used);
                if (slot - shi < REACH && slot - shi >= 0) {
                    arena = x18_arena_base; aused = &x18_arena_used;
                    asize = img->x18_arena_size;
                }
            }
            if (!arena && x18_pre_base) {
                int64_t slot = (int64_t)(uintptr_t)(x18_pre_base + x18_pre_used);
                if (slo - slot < REACH && slo - slot >= 0) {
                    arena = x18_pre_base; aused = &x18_pre_used;
                    asize = img->x18_pre_arena_size;
                }
            }
            kl_x18_stats xs;
            if (kl_x18_patch(p + off, slice, arena, asize,
                             aused, &ximg, &xs) != 0) {
                fprintf(stderr, "  [klepton] %s: x18 veneering failed to start\n", path);
                veneer = 0;
                break;
            }
            // The jump-table rewrite, both halves. A CONVERTED `br x18` no
            // longer branches through x18 at all and cannot be killed by the
            // kernel zeroing it; a FALLBACK one still can, and that residual is
            // a silent SIGKILL on device — so it is printed rather than left to
            // be inferred from silence.
            if (xs.jt_sites || xs.jt_fallback)
                fprintf(stderr, "  [klepton] %s: br x18 — %u converted to thunk "
                                "dispatch (%u targets), %u on the guarded veneer%s\n",
                        path, xs.jt_sites, xs.jt_thunks, xs.jt_fallback,
                        xs.jt_fallback ? " (residual x18 window)" : "");
            // A REFUSED x18 site runs against Darwin's reserved x18, which is
            // zero after any exception return, and it has no other error
            // surface: libUE4's 8038 sites all refused for REACH surface as an
            // intermittent SIGSEGV inside Oodle's decompressor on a pak worker
            // thread, hours from the load that caused it.
            //
            // So it is printed unconditionally when non-zero, with the pool's
            // own distance beside it, because the two refusal kinds want
            // different fixes and only one of them is about the decoder.
            if (xs.refused) {
                int64_t d = (int64_t)xs.pool_va - (int64_t)(uintptr_t)(p + off);
                fprintf(stderr,
                        "  [klepton] %s: x18 REFUSED %u of %u sites (%u for REACH"
                        " — pool at %#llx, %+lld MB from the code at %p). Those "
                        "sites run against Darwin's reserved x18 and it is zero "
                        "after any exception return.\n",
                        path, xs.refused, xs.sites, xs.far_refused,
                        (unsigned long long)xs.pool_va,
                        (long long)(d / (1024 * 1024)), (void *)(p + off));
            }
            img->stats.x18_sites   += xs.sites;
            img->stats.x18_patched += xs.patched;
            img->stats.x18_refused += xs.refused;
            img->stats.x18_data_words += xs.data_words;
            img->stats.ctr_sites   += xs.ctr_sites;
            img->stats.ctr_patched += xs.ctr_patched;
            img->stats.ctr_refused += xs.ctr_refused;
            off += slice;
            }
        }
    }
    // Measured guest patches, here because this is the last moment the image is
    // writable and the icache flush below already covers whatever they wrote.
    // Every row is guarded by the word it expects to find, so a library this
    // table has never heard of costs one strcmp — see kl_guestpatch.c.
    struct gp_ctx { uint8_t *base; uint64_t lo, span; } gpc = {
        img->base, lo, (uint64_t)img->span
    };
    kl_guest_patch_apply(path, gp_at, &gpc);

    sys_icache_invalidate(img->base, img->span);

    if (protect_pages(img, eh, ph, sh, lo) != 0) {
        munmap(file, (size_t)sb.st_size); return NULL;
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
    // Linear scan of .dynsym.
    //
    // The count comes from DT_HASH's nchain, never from the gap between symtab
    // and strtab: that assumes .dynstr directly follows .dynsym, which
    // is true of every Beat Saber library and false of every Steam Link one,
    // where the linker puts .gnu.hash and .hash between them. libc++_shared.so
    // has 2506 symbols and a gap of 4198 entries, so the scan runs off the end
    // of .dynsym into the hash tables, reads their words as st_name offsets and
    // dereferences strtab + garbage — a fault inside strcmp during a
    // cross-image import bind, and a wrong answer under a different layout.
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

// Repoint the slots `img` bound for `name` from `from` to `to`, after the fact.
// This is ELF symbol preemption done late: our chain binds each library as it
// loads, dependencies first, so a symbol a dependency DEFINES binds to its own
// copy — where bionic, binding the whole group at once, would hand it the copy
// in the library that pulled it in. Only slots still holding `from` move, so a
// slot an interposer or a shim claimed stays where it is. RELA and JMPREL only:
// the import slots live there; RELR and APS2 are relative-only here. Must run
// before the guest's initializers do. Returns the number of slots moved.
int kl_rebind(kl_image *img, const char *name, void *from, void *to) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img->base;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0) return -1;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(img->base + eh->e_phoff);
    uint64_t lo = UINT64_MAX;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
    const Elf64_Rela *tab[2] = { NULL, NULL };
    size_t cnt[2] = { 0, 0 };
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_DYNAMIC) continue;
        for (const Elf64_Dyn *dp = (const Elf64_Dyn *)(img->base + ph[i].p_vaddr - lo);
             dp->d_tag != DT_NULL; dp++)
            switch (dp->d_tag) {
            case DT_RELA:     tab[0] = (const Elf64_Rela *)(img->base + dp->d_val); break;
            case DT_RELASZ:   cnt[0] = dp->d_val / sizeof(Elf64_Rela); break;
            case DT_JMPREL:   tab[1] = (const Elf64_Rela *)(img->base + dp->d_val); break;
            case DT_PLTRELSZ: cnt[1] = dp->d_val / sizeof(Elf64_Rela); break;
            }
    }
    int moved = 0;
    for (int t = 0; t < 2; t++) {
        if (!tab[t]) continue;
        for (size_t i = 0; i < cnt[t]; i++) {
            const Elf64_Rela *r = &tab[t][i];
            uint32_t type = ELF64_R_TYPE(r->r_info), sidx = ELF64_R_SYM(r->r_info);
            if (!sidx || (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT &&
                          type != R_AARCH64_ABS64)) continue;
            if (strcmp(img->strtab + img->symtab[sidx].st_name, name) != 0) continue;
            uint64_t *slot = (uint64_t *)(img->base + r->r_offset);
            if (*slot != (uint64_t)(uintptr_t)from + (uint64_t)r->r_addend) continue;
            *slot = (uint64_t)(uintptr_t)to + (uint64_t)r->r_addend;
            moved++;
        }
    }
    return moved;
}

void kl_unload(kl_image *img) {
    if (!img) return;
    if (img->aot_header) kl_aot_detach(img->aot_header);
    if (img->dl_handle) dlclose(img->dl_handle);   // translated: dyld owns it
    else                munmap(img->base - img->map_off - img->x18_pre_arena_size,
                               img->x18_pre_arena_size + img->map_size +
                               img->x18_arena_size);
    free(img);
}

// Read an image's DT_NEEDED sonames straight out of the file, without loading
// it. kl_load_recursive uses this to pull the dependency chain in before the
// image that imports it, so relocation-time binding (sym_value's global guest
// search) has the exports in place. d_val entries are vaddrs; converting them
// to file offsets goes through the PT_LOAD table, which on these libraries is
// identity-mapped (p_vaddr == p_offset) but is not required to be.
// Map an ELF vaddr to a pointer inside an in-memory image. Two layouts, because
// the two sources below present the ELF differently: a raw .so on disk is in FILE
// layout (index by the containing PT_LOAD's p_offset), while the ELF baked into a
// translated dylib's __klelf is in LOAD/vaddr layout — the same one kl_load_dylib
// and bind_dynamic read with `base + p_vaddr - lo`. Getting this wrong resolves
// DT_STRTAB to garbage and every DT_NEEDED reads back empty.
static const uint8_t *elf_vptr(const uint8_t *img, const Elf64_Phdr *ph, int phnum,
                               uint64_t lo, int vaddr_layout, uint64_t va) {
    if (vaddr_layout) return img + (va - lo);
    for (int i = 0; i < phnum; i++)
        if (ph[i].p_type == PT_LOAD && va >= ph[i].p_vaddr &&
            va < ph[i].p_vaddr + ph[i].p_filesz)
            return img + ph[i].p_offset + (va - ph[i].p_vaddr);
    return NULL;
}

// DT_NEEDED out of an in-memory AArch64 ELF64 image. vaddr_layout selects how the
// dynamic section and its string table are addressed (see elf_vptr).
static int elf_list_needed(const uint8_t *img, int vaddr_layout, char names[][128], int max) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 || eh->e_machine != 183) return 0;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(img + eh->e_phoff);
    uint64_t lo = UINT64_MAX;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
    if (lo == UINT64_MAX) lo = 0;

    const Elf64_Dyn *dyn = NULL;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) {
            dyn = (const Elf64_Dyn *)elf_vptr(img, ph, eh->e_phnum, lo, vaddr_layout, ph[i].p_vaddr);
            break;
        }
    const char *strtab = NULL;
    if (dyn)
        for (const Elf64_Dyn *dp = dyn; dp->d_tag != DT_NULL; dp++)
            if (dp->d_tag == DT_STRTAB) {
                strtab = (const char *)elf_vptr(img, ph, eh->e_phnum, lo, vaddr_layout, dp->d_val);
                break;
            }
    int n = 0;
    if (dyn && strtab)
        for (const Elf64_Dyn *dp = dyn; dp->d_tag != DT_NULL && n < max; dp++)
            if (dp->d_tag == DT_NEEDED) { snprintf(names[n], 128, "%s", strtab + dp->d_val); n++; }
    return n;
}

// The DT_NEEDED sonames of an image, without loading it. Two sources, because the
// ELF .so tree is on disk for a HOST run but NOT bundled on DEVICE — there only
// the translated frameworks ship, with the original ELF baked into their
// __TEXT,__klelf. Read the .so directly when it exists; otherwise resolve the
// framework the same way kl_load_auto will and read DT_NEEDED out of __klelf.
// Missing this second source silently dropped every bundled dependency on device
// (vicecity/libmiamivr's libc++_shared.so was never loaded, and its first
// allocation aborted in kl_unresolved_named on operator new / _Znwm).
int kl_list_needed(const char *path, char names[][128], int max) {
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        struct stat sb;
        if (fstat(fd, &sb) == 0) {
            uint8_t *file = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (file != MAP_FAILED) {
                const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;
                if (memcmp(eh->e_ident, "\x7f" "ELF", 4) == 0 && eh->e_machine == 183) {
                    int n = elf_list_needed(file, 0, names, max);   // raw .so: file layout
                    munmap(file, (size_t)sb.st_size);
                    return n;
                }
                munmap(file, (size_t)sb.st_size);   // a Mach-O at this path: fall through
            }
        } else close(fd);
    }
    // Device: no .so on disk. Read DT_NEEDED from the translated dylib's __klelf.
    char cand[1024];
    if (!dylib_candidate(path, cand, sizeof cand, NULL, 0)) return -1;
    int mfd = open(cand, O_RDONLY);
    if (mfd < 0) return -1;
    struct stat sb;
    if (fstat(mfd, &sb) != 0) { close(mfd); return -1; }
    uint8_t *mo = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, mfd, 0);
    close(mfd);
    if (mo == MAP_FAILED) return -1;
    int n = -1;
    const struct mach_header_64 *mh = (const struct mach_header_64 *)mo;
    if ((size_t)sb.st_size >= sizeof *mh && mh->magic == MH_MAGIC_64) {
        const uint8_t *lc = mo + sizeof *mh;
        for (uint32_t i = 0; i < mh->ncmds && n < 0; i++) {
            const struct load_command *cmd = (const struct load_command *)lc;
            if (cmd->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
                if (strcmp(seg->segname, "__TEXT") == 0) {
                    const struct section_64 *sec = (const struct section_64 *)(lc + sizeof *seg);
                    for (uint32_t s = 0; s < seg->nsects; s++)
                        if (strcmp(sec[s].sectname, "__klelf") == 0) {
                            // __klelf is in LOAD/vaddr layout (see elf_vptr / how
                            // kl_load_dylib reads it), not raw file layout.
                            n = elf_list_needed(mo + sec[s].offset, 1, names, max);
                            break;
                        }
                }
            }
            lc += cmd->cmdsize;
        }
    }
    munmap(mo, (size_t)sb.st_size);
    return n;
}
