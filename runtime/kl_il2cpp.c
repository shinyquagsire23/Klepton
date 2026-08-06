// IL2CPP pc -> managed method name resolver (Unity 2019.4, metadata v24).
//
// Beat Saber 1.28 ships metadata version 24.0 (the layout below was verified
// against its global-metadata.dat: header counts are *byte* sizes, and the
// struct sizes fall out of them exactly — Il2CppTypeDefinition 92 B with no
// rgctx fields, Il2CppMethodDefinition 32 B, Il2CppImageDefinition 40 B).
//
// The half Il2CppDumper gets from the binary — which code pointer implements
// which method — comes from libil2cpp's Il2CppCodeGenModule array:
//
//   struct Il2CppCodeGenModule {      // v24, first fields stable across 24.x
//       const char *moduleName;       //   "Main.dll", "mscorlib.dll", ...
//       uint32_t    methodPointerCount;
//       void      **methodPointers;   // indexed by (method token & 0xffffff) - 1
//       ...
//   };
//
// Nothing exports it, so init finds each module by scanning the loaded image
// for structs whose moduleName pointer targets that module's ".dll" string and
// whose methodPointerCount equals the method count the metadata gives that
// image. Both facts checked together make a false positive unlikely; a
// candidate is further required to hold a methodPointers array whose first
// entry lands in the image's executable segment.
//
// A method's index inside its module's methodPointers is its metadata RID
// ((token & 0xffffff) - 1): verified on Beat Saber's metadata, where every
// image's method RIDs are exactly 1..methodCount in table order.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "kl_il2cpp.h"

// ---- global-metadata.dat v24.0 header: (offset, byteCount) pairs ----
// Pair index p lives at header words [2p],[2p+1]. Only the tables we read.
#define P_STRING      3
#define P_METHODS     6
#define P_TYPEDEFS   20
#define P_IMAGES     21

#define SZ_METHODDEF 32
#define SZ_TYPEDEF   92
#define SZ_IMAGEDEF  40

// Field offsets within those structs.
#define MDEF_NAME    0
#define MDEF_TOKEN   20
#define TDEF_NAME    0
#define TDEF_NS      4
#define TDEF_METHODSTART 40
#define TDEF_METHODCOUNT 68
#define IDEF_NAME    0
#define IDEF_TYPESTART 8
#define IDEF_TYPECOUNT 12

static int g_state;   // 0 = not tried, 1 = up, -1 = failed

typedef struct {
    uint64_t    addr;
    const char *name;
} method_rec;
static method_rec *g_methods;
static unsigned    g_nmethods;

static int cmp_rec(const void *pa, const void *pb) {
    uint64_t a = ((const method_rec *)pa)->addr, b = ((const method_rec *)pb)->addr;
    return a < b ? -1 : a > b;
}

static const uint8_t *g_meta;        // mmap'd global-metadata.dat
static size_t         g_meta_len;
static uint32_t       g_str_off;

static const char *meta_str(int32_t idx) {
    if (idx < 0 || (uint32_t)idx >= g_meta_len - g_str_off) return "?";
    return (const char *)(g_meta + g_str_off + idx);
}

static uint32_t meta_u32(uint64_t at) { uint32_t v; memcpy(&v, g_meta + at, 4); return v; }
static int32_t  meta_i32(uint64_t at) { return (int32_t)meta_u32(at); }
static uint16_t meta_u16(uint64_t at) { uint16_t v; memcpy(&v, g_meta + at, 2); return v; }

// The image's executable segment bound, re-read from its in-memory ELF header
// (the loader maps the header too). Used only to validate candidates.
static int exec_bounds(const uint8_t *base, size_t span, uint64_t *lo_v, uint64_t *hi_v) {
    if (span < 0x40 || memcmp(base, "\x7f" "ELF", 4) != 0) return 0;
    uint64_t phoff; memcpy(&phoff, base + 0x20, 8);
    uint16_t phentsize, phnum;
    memcpy(&phentsize, base + 0x36, 2);
    memcpy(&phnum, base + 0x38, 2);
    if (phoff + (uint64_t)phentsize * phnum > span) return 0;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *p = base + phoff + (uint64_t)i * phentsize;
        uint32_t type, flags; uint64_t vaddr, memsz;
        memcpy(&type, p, 4); memcpy(&flags, p + 4, 4);
        memcpy(&vaddr, p + 0x10, 8); memcpy(&memsz, p + 0x28, 8);
        if (type != 1) continue;                 // PT_LOAD
        if (vaddr < lo) lo = vaddr;
        if ((flags & 1) && vaddr + memsz > hi) hi = vaddr + memsz;
    }
    if (lo == UINT64_MAX || !hi) return 0;
    *lo_v = lo; *hi_v = hi;
    return 1;
}

int kl_il2cpp_resolver_init(kl_image *libil2cpp, const char *metadata_path) {
    if (g_state) return g_state > 0;
    g_state = -1;

    int fd = open(metadata_path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "  [klepton] il2cpp: open %s failed\n", metadata_path); return 0; }
    struct stat sb;
    if (fstat(fd, &sb) != 0) { close(fd); return 0; }
    g_meta_len = (size_t)sb.st_size;
    g_meta = mmap(NULL, g_meta_len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (g_meta == MAP_FAILED) { g_meta = NULL; return 0; }

    uint32_t sanity = meta_u32(0);
    int32_t  version = meta_i32(4);
    if (sanity != 0xFAB11BAF || version != 24) {
        fprintf(stderr, "  [klepton] il2cpp: metadata sanity %#x version %d — "
                        "resolver only knows v24\n", sanity, version);
        return 0;
    }
    uint32_t m_off = meta_u32(P_METHODS * 8),  m_bytes = meta_u32(P_METHODS * 8 + 4);
    uint32_t t_off = meta_u32(P_TYPEDEFS * 8), t_bytes = meta_u32(P_TYPEDEFS * 8 + 4);
    uint32_t i_off = meta_u32(P_IMAGES * 8),   i_bytes = meta_u32(P_IMAGES * 8 + 4);
    g_str_off = meta_u32(P_STRING * 8);
    if (m_bytes % SZ_METHODDEF || t_bytes % SZ_TYPEDEF || i_bytes % SZ_IMAGEDEF ||
        (uint64_t)i_off + i_bytes > g_meta_len) {
        fprintf(stderr, "  [klepton] il2cpp: metadata tables do not divide by the "
                        "v24.0 struct sizes — layout drifted\n");
        return 0;
    }
    uint32_t n_images = i_bytes / SZ_IMAGEDEF;

    // Per-image method counts from the type definitions.
    uint32_t *want = calloc(n_images, sizeof *want);
    const char **iname = calloc(n_images, sizeof *iname);
    for (uint32_t i = 0; i < n_images; i++) {
        uint64_t id = i_off + (uint64_t)i * SZ_IMAGEDEF;
        iname[i] = meta_str(meta_i32(id + IDEF_NAME));
        int32_t ts = meta_i32(id + IDEF_TYPESTART);
        uint32_t tc = meta_u32(id + IDEF_TYPECOUNT);
        uint32_t n = 0;
        for (uint32_t t = 0; t < tc; t++)
            n += meta_u16(t_off + (uint64_t)(ts + t) * SZ_TYPEDEF + TDEF_METHODCOUNT);
        want[i] = n;
    }

    // Find each module name's string literal inside the loaded image. Pointers
    // are relocated, so a moduleName field holds base + file offset of the
    // string; find the strings by content and compare against runtime pointers.
    uint8_t *base = kl_base(libil2cpp);
    size_t   span = kl_span(libil2cpp);
    uint64_t lo_v, hi_v;
    if (!base || !exec_bounds(base, span, &lo_v, &hi_v)) return 0;
    const uint8_t *exec_end = base + (hi_v - lo_v);

    uint8_t **str_at = calloc(n_images, sizeof *str_at);
    for (uint32_t i = 0; i < n_images; i++) {
        size_t nl = strlen(iname[i]) + 1;
        for (uint8_t *p = base; p + nl <= exec_end; ) {
            uint8_t *hit = memchr(p, iname[i][0], (size_t)(exec_end - p));
            if (!hit || hit + nl > exec_end) break;
            if (memcmp(hit, iname[i], nl) == 0) { str_at[i] = hit; break; }
            p = hit + 1;
        }
    }

    // Scan the image for module structs: word == a known string pointer, the
    // u32 at +8 == that image's metadata method count, and (count == 0 or the
    // methodPointers array at +16 starts inside the executable segment).
    typedef struct { uint32_t img; uint32_t count; void **ptrs; } module;
    module *mods = calloc(n_images, sizeof *mods);
    unsigned n_mods = 0;
    for (uint8_t *a = base; a + 24 <= base + span; a += 8) {
        uint8_t *w; memcpy(&w, a, 8);
        if (w < base || w >= exec_end) continue;
        for (uint32_t i = 0; i < n_images; i++) {   // 87 targets; hits are rare
            if (w != str_at[i]) continue;
            uint32_t cnt; memcpy(&cnt, a + 8, 4);
            void **ptrs; memcpy(&ptrs, a + 16, 8);
            if (cnt != want[i]) continue;
            if (cnt && ((uint8_t *)ptrs < base || (uint8_t *)ptrs >= base + span))
                continue;
            if (cnt) {
                uint8_t *first; memcpy(&first, ptrs, 8);
                if (first && (first < base || first >= exec_end)) continue;
            }
            if (!mods[i].ptrs) { mods[i].img = i; mods[i].count = cnt;
                                 mods[i].ptrs = ptrs; n_mods++; }
        }
    }
    if (!n_mods) {
        fprintf(stderr, "  [klepton] il2cpp: no codegen modules found in libil2cpp\n");
        return 0;
    }

    // Count, then fill, the (address, name) records.
    uint64_t total = 0;
    for (uint32_t m = 0; m < n_images; m++) total += mods[m].count;
    g_methods = malloc((size_t)total * sizeof *g_methods);
    uint32_t m_off_end = m_off + m_bytes;
    for (uint32_t m = 0; m < n_images; m++) {
        if (!mods[m].count) continue;
        uint64_t id = i_off + (uint64_t)m * SZ_IMAGEDEF;
        int32_t ts = meta_i32(id + IDEF_TYPESTART);
        uint32_t tc = meta_u32(id + IDEF_TYPECOUNT);
        for (uint32_t t = 0; t < tc; t++) {
            uint64_t td = t_off + (uint64_t)(ts + t) * SZ_TYPEDEF;
            const char *tns  = meta_str(meta_i32(td + TDEF_NS));
            const char *tnm  = meta_str(meta_i32(td + TDEF_NAME));
            int32_t  ms = meta_i32(td + TDEF_METHODSTART);
            uint16_t mc = meta_u16(td + TDEF_METHODCOUNT);
            for (uint32_t k = 0; k < mc; k++) {
                uint64_t md = m_off + (uint64_t)(ms + k) * SZ_METHODDEF;
                if (md + SZ_METHODDEF > m_off_end) continue;
                uint32_t tok = meta_u32(md + MDEF_TOKEN);
                uint32_t rid = (tok & 0xffffff) - 1;
                if (rid >= mods[m].count) continue;
                uint64_t addr; memcpy(&addr, &mods[m].ptrs[rid], 8);
                if (!addr) continue;              // abstract / interface method
                const char *mnm = meta_str(meta_i32(md + MDEF_NAME));
                char *full;
                if (asprintf(&full, "%s%s%s::%s", tns, *tns ? "." : "",
                             tnm, mnm) < 0) continue;
                g_methods[g_nmethods].addr = addr;
                g_methods[g_nmethods].name = full;
                g_nmethods++;
            }
        }
    }
    free(mods); free(str_at); free(want); free(iname);

    qsort(g_methods, g_nmethods, sizeof *g_methods, cmp_rec);

    fprintf(stderr, "  [klepton] il2cpp: resolver up — %u/%u modules, %u methods\n",
            n_mods, n_images, g_nmethods);
    g_state = 1;
    return 1;
}

const char *kl_il2cpp_method_at(const void *pc) {
    if (g_state != 1 || !pc) return NULL;
    uint64_t a = (uint64_t)pc;
    unsigned lo = 0, hi = g_nmethods;
    while (lo < hi) {                          // first entry with addr > a
        unsigned mid = (lo + hi) / 2;
        if (g_methods[mid].addr <= a) lo = mid + 1; else hi = mid;
    }
    if (!lo) return NULL;
    const method_rec *r = &g_methods[lo - 1];
    if (a - r->addr > 0x100000) return NULL;   // no sizes; refuse silly gaps
    return r->name;
}

unsigned kl_il2cpp_method_count(void) { return g_nmethods; }
