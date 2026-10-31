// Diagnostics that need to stand between the guest and its own libc.
//
// KL_LX_TSDTRACE=<key>: every pthread_setspecific on that key, with the value
// and the caller. Thread-specific data has no other instrument: a wrong value in
// a slot is found long after it was stored, by code that did not store it, and
// the store itself is an ordinary call into glibc that no system call marks.
//
// It works the way a preloaded library would on Linux, because that is what it
// is: a host library (kl_lx_hostlib.c) exporting pthread_setspecific, named in
// LD_PRELOAD by whoever sets the variable, so the guest's calls bind to the
// function below, which reports and then calls glibc's own. Run with
//
//     --env LD_PRELOAD=libkl_tsdtrace.so      (and KL_LX_TSDTRACE in the host's)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kl_lx.h"

// The ELF structures this needs; the host has no <elf.h>.
typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
                 uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
                 uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
               } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz,
                 p_memsz, p_align; } Elf64_Phdr;
typedef struct { int64_t d_tag; union { uint64_t d_val, d_ptr; } d_un; } Elf64_Dyn;
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx;
                 uint64_t st_value, st_size; } Elf64_Sym;
#define PT_DYNAMIC 2
#define DT_NULL    0
#define DT_STRTAB  5
#define DT_SYMTAB  6

static int g_key = -1;
static int (*g_real)(unsigned key, const void *value);

// A dynamic symbol of a loaded guest image, by name. The symbol table ends
// where the string table begins in everything GNU ld writes, which is what
// gives its length without the section headers (they are not mapped).
static void *guest_sym(const char *image, const char *name) {
    uint64_t lo, data, hi;
    const char *path;
    for (int i = 0; kl_lx_image_info(i, &lo, &data, &hi, &path); i++) {
        const char *b = strrchr(path, '/');
        if (strcmp(b ? b + 1 : path, image)) continue;
        const Elf64_Ehdr *eh = (const void *)(uintptr_t)lo;
        const Elf64_Phdr *ph = (const void *)(uintptr_t)(lo + eh->e_phoff);
        const Elf64_Dyn *dyn = NULL;
        for (int k = 0; k < eh->e_phnum; k++)
            if (ph[k].p_type == PT_DYNAMIC) dyn = (const void *)(uintptr_t)(lo + ph[k].p_vaddr);
        if (!dyn) return NULL;
        uint64_t symtab = 0, strtab = 0;
        for (; dyn->d_tag != DT_NULL; dyn++) {
            if (dyn->d_tag == DT_SYMTAB) symtab = dyn->d_un.d_ptr;
            if (dyn->d_tag == DT_STRTAB) strtab = dyn->d_un.d_ptr;
        }
        // The loader may or may not have relocated these in place.
        if (symtab < lo) symtab += lo;
        if (strtab < lo) strtab += lo;
        if (!symtab || strtab <= symtab) return NULL;
        const Elf64_Sym *sym = (const void *)(uintptr_t)symtab;
        size_t n = (size_t)(strtab - symtab) / sizeof *sym;
        for (size_t k = 0; k < n; k++)
            if (sym[k].st_value && sym[k].st_shndx &&
                !strcmp((const char *)(uintptr_t)strtab + sym[k].st_name, name))
                return (void *)(uintptr_t)(lo + sym[k].st_value);
        return NULL;
    }
    return NULL;
}

static int traced_setspecific(unsigned key, const void *value) {
    void *ra = __builtin_return_address(0);
    if (!g_real) g_real = guest_sym("libc.so.6", "pthread_setspecific");
    if ((int)key == g_key) {
        uint64_t lo, data, hi;
        const char *path, *who = "?";
        uint64_t off = (uint64_t)(uintptr_t)ra;
        for (int i = 0; kl_lx_image_info(i, &lo, &data, &hi, &path); i++)
            if ((uint64_t)(uintptr_t)ra >= lo && (uint64_t)(uintptr_t)ra < hi) {
                const char *b = strrchr(path, '/');
                who = b ? b + 1 : path;
                off -= lo;
                break;
            }
        fprintf(stderr, "  [klepton] linux: tid %d pthread_setspecific(%u, %p) from %s+%#llx\n",
                kl_lx_gettid(), key, value, who, (unsigned long long)off);
    }
    return g_real ? g_real(key, value) : 22;
}

static size_t build(kl_lx_hostsym **out) {
    kl_lx_hostsym *s = calloc(1, sizeof *s);
    s[0] = (kl_lx_hostsym){ "pthread_setspecific", (void *)traced_setspecific };
    *out = s;
    return 1;
}

void kl_lx_diag_init(void) {
    const char *k = getenv("KL_LX_TSDTRACE");
    if (!k || !*k) return;
    g_key = atoi(k);
    kl_lx_hostlib_register("libkl_tsdtrace.so", build);
}
