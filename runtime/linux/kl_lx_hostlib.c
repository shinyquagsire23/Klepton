// Host libraries: the host's code, presented to the guest's loader as an ELF.
//
// The guest binds symbols through its own ld.so, and ld.so resolves only against
// ELF objects it has loaded. So for the guest to call something of ours — the
// Vulkan implementation, later the XR runtime — there has to be an ELF that
// defines the names. This file writes one: no code, no relocations, just a
// dynamic symbol table in which every symbol is SHN_ABS and its value is the
// address of the host function.
//
// SHN_ABS is what makes that a table and nothing more. glibc does not add the
// library's load address to an absolute symbol, so the guest's GOT and PLT slots
// end up holding host addresses directly and a call through one is a plain
// branch into host code — no stub, no veneer, no hypercall, and nothing
// executable in the object, which is therefore ordinary data that can be
// generated at run time on a headset.
//
// What this does NOT do is translate the ABI. It is only right for functions
// that are byte-compatible between AAPCS64 and Darwin's arm64 variant: no
// variadics, no arguments narrower than 32 bits, and nothing the host calls back
// into on a thread the guest did not create. Vulkan is such an API (kl_vulkan.h
// measured it); libc is not, which is why the guest brings its own.
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kl_lx.h"

#define LX_HOSTLIBS 8
static struct {
    char   soname[64];
    size_t (*build)(kl_lx_hostsym **out);
    uint8_t *elf;
    size_t   size;
} g_lib[LX_HOSTLIBS];
static int g_nlib;

void kl_lx_hostlib_register(const char *soname, size_t (*build)(kl_lx_hostsym **out)) {
    if (g_nlib == LX_HOSTLIBS) return;
    snprintf(g_lib[g_nlib].soname, sizeof g_lib[0].soname, "%s", soname);
    g_lib[g_nlib++].build = build;
}

typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version;
    uint64_t entry, phoff, shoff; uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Sym;
typedef struct { int64_t tag; uint64_t val; } Dyn;

static uint32_t sysv_hash(const char *s) {
    uint32_t h = 0, g;
    for (; *s; s++) {
        h = (h << 4) + (uint8_t)*s;
        if ((g = h & 0xf0000000u)) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

// One writable PT_LOAD holding, in order: headers, .dynsym, .hash, .dynstr,
// .dynamic. Writable because the loader relocates .dynamic's pointers in place.
static void synthesize(int idx) {
    kl_lx_hostsym *syms = NULL;
    size_t n = g_lib[idx].build(&syms), strsz = 1 + strlen(g_lib[idx].soname) + 1;
    for (size_t i = 0; i < n; i++) strsz += strlen(syms[i].name) + 1;

    uint32_t nbucket = (uint32_t)(n / 2 + 1), nchain = (uint32_t)n + 1;
    size_t o_sym  = sizeof(Ehdr) + 2 * sizeof(Phdr);
    size_t o_hash = o_sym + (n + 1) * sizeof(Sym);
    size_t o_str  = o_hash + (2 + nbucket + nchain) * 4;
    size_t o_dyn  = (o_str + strsz + 15) & ~(size_t)15;
    size_t total  = o_dyn + 7 * sizeof(Dyn);

    uint8_t *e = calloc(1, total);
    Ehdr *eh = (Ehdr *)e;
    memcpy(eh->ident, "\x7f" "ELF\x02\x01\x01", 7);            // ELF64, little-endian, v1
    eh->type = 3; eh->machine = 183; eh->version = 1;           // ET_DYN, AArch64
    eh->phoff = sizeof(Ehdr); eh->ehsize = sizeof(Ehdr);
    eh->phentsize = sizeof(Phdr); eh->phnum = 2;
    Phdr *ph = (Phdr *)(e + sizeof(Ehdr));
    ph[0] = (Phdr){ 1, 6, 0, 0, 0, total, total, 0x4000 };      // PT_LOAD, RW
    ph[1] = (Phdr){ 2, 6, o_dyn, o_dyn, o_dyn, 7 * sizeof(Dyn), 7 * sizeof(Dyn), 8 };   // PT_DYNAMIC

    Sym *sym = (Sym *)(e + o_sym);
    uint32_t *hash = (uint32_t *)(e + o_hash), *bucket = hash + 2, *chain = bucket + nbucket;
    char *str = (char *)(e + o_str);
    size_t sp = 1;
    hash[0] = nbucket; hash[1] = nchain;
    uint32_t soname = (uint32_t)sp;
    strcpy(str + sp, g_lib[idx].soname); sp += strlen(g_lib[idx].soname) + 1;
    for (size_t i = 0; i < n; i++) {
        Sym *s = &sym[i + 1];
        s->name = (uint32_t)sp;
        s->info = (1 << 4) | 2;                                 // GLOBAL, FUNC
        s->shndx = 0xfff1;                                      // SHN_ABS
        s->value = (uint64_t)(uintptr_t)syms[i].addr;
        strcpy(str + sp, syms[i].name); sp += strlen(syms[i].name) + 1;
        uint32_t b = sysv_hash(syms[i].name) % nbucket;
        chain[i + 1] = bucket[b];
        bucket[b] = (uint32_t)i + 1;
    }
    Dyn *dyn = (Dyn *)(e + o_dyn);
    dyn[0] = (Dyn){ 14, soname };       // DT_SONAME
    dyn[1] = (Dyn){ 4,  o_hash };       // DT_HASH
    dyn[2] = (Dyn){ 5,  o_str };        // DT_STRTAB
    dyn[3] = (Dyn){ 6,  o_sym };        // DT_SYMTAB
    dyn[4] = (Dyn){ 10, strsz };        // DT_STRSZ
    dyn[5] = (Dyn){ 11, sizeof(Sym) };  // DT_SYMENT
    free(syms);
    g_lib[idx].elf = e;
    g_lib[idx].size = total;
    fprintf(stderr, "  [klepton] linux: %s is served from the host — %zu symbol(s)\n",
            g_lib[idx].soname, n);
}

// Is `path` one of ours? By file name alone: wherever the loader looks for
// libvulkan.so.1 first, that is where it finds it.
static int find(const char *path) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    for (int i = 0; i < g_nlib; i++)
        if (!strcmp(b, g_lib[i].soname)) return i;
    return -1;
}

int kl_lx_hostlib_is(const char *path) { return path && find(path) >= 0; }

// A descriptor on the synthesized object, or -1 with errno. Each open gets its
// own unlinked file, so read offsets are independent like any other open's.
int kl_lx_hostlib_open(const char *path) {
    int i = find(path);
    if (i < 0) { errno = ENOENT; return -1; }
    if (!g_lib[i].elf) synthesize(i);
    char tmpl[PATH_MAX];
    const char *tmp = getenv("TMPDIR");
    snprintf(tmpl, sizeof tmpl, "%s/kl-hostlib-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    unlink(tmpl);
    if (write(fd, g_lib[i].elf, g_lib[i].size) != (ssize_t)g_lib[i].size) { close(fd); return -1; }
    lseek(fd, 0, SEEK_SET);
    return fd;
}
