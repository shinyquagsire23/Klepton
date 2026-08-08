// S0.5 — dump what the x18 decoder makes of a guest library's .text.
//
// This is the measurement half of the veneer work. It does not load or run
// anything: it walks the executable segments of an ELF as a file and reports,
// for every instruction word, either that the decoder found general-purpose x18
// operands or that it refused the encoding. tools/check_x18.py joins that
// against objdump's disassembly and checks both directions —
//
//   * every instruction objdump prints with an x18/w18 operand must be found
//     (a miss means a site we would leave broken), and
//   * every field the decoder calls x18 must survive substitution, checked by
//     disassembling the rewritten word (a false positive means we corrupt an
//     instruction, which is far worse).
//
// Refusals are reported rather than skipped, because a refusal on a real x18
// site is work to do, and a refusal on anything else is free.
//
// Usage: t_x18 <file.so> [more.so ...]
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../runtime/kl_x18.h"

// Same ELF64 subset kl_image.c defines; macOS has no <elf.h>.
typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr,
    p_filesz, p_memsz, p_align; } Elf64_Phdr;
typedef struct { uint32_t sh_name, sh_type; uint64_t sh_flags, sh_addr, sh_offset,
    sh_size; uint32_t sh_link, sh_info; uint64_t sh_addralign, sh_entsize; } Elf64_Shdr;
#define PT_LOAD        1
#define PF_X           1
#define SHT_PROGBITS   1
#define SHF_EXECINSTR  0x4

// The register the test substitutes in. Any value but 18 works: where the
// instruction already names x9 the expected text collapses to x9 twice, which
// is exactly what the rewritten word disassembles to.
#define TEST_REG 9

static unsigned long g_words, g_sites, g_refused, g_rd, g_wr, g_rw;

static void scan(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return; }
    struct stat sb;
    if (fstat(fd, &sb) != 0) { perror("fstat"); close(fd); return; }
    uint8_t *f = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (f == MAP_FAILED) { perror("mmap"); return; }

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)f;
    if (memcmp(eh->e_ident, "\177ELF", 4) != 0) {
        fprintf(stderr, "%s: not an ELF\n", path);
        munmap(f, (size_t)sb.st_size);
        return;
    }
    // Sections, not segments. The r-x LOAD segment of these libraries starts at
    // file offset 0 and runs to the end of .gcc_except_table, so it contains
    // .hash, .dynsym, .rela.dyn, .rodata and .eh_frame as well as code — for
    // libunity that is 1.5 MB of .rodata and 720 KB of relocations. Scanning the
    // segment finds "instructions" in all of it, and patching one would corrupt
    // data that merely happens to sit behind PF_X.
    if (!eh->e_shoff || !eh->e_shnum) {
        fprintf(stderr, "%s: no section headers — cannot tell code from rodata\n", path);
        munmap(f, (size_t)sb.st_size);
        return;
    }
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(f + eh->e_shoff);

    // ...and code inside those sections, not the whole section: a `.text` can
    // hold constant tables (Steam Link's libmain.so has 148 KB of them). The
    // loader and klepton-ld skip the same ranges, so this stays the honest
    // measurement of what actually gets patched.
    static klx_range skip[256];
    unsigned nskip = kl_x18_data_ranges(f, (size_t)sb.st_size, skip,
                                        sizeof skip / sizeof *skip);
    if (nskip) {
        uint64_t bytes = 0;
        for (unsigned i = 0; i < nskip; i++) bytes += skip[i].end - skip[i].start;
        fprintf(stderr, "[t_x18] %s: %u data range(s) inside executable sections, "
                        "%llu bytes — not scanned\n",
                path, nskip, (unsigned long long)bytes);
    }

    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_PROGBITS || !(sh[i].sh_flags & SHF_EXECINSTR)) continue;
        uint64_t cursor = sh[i].sh_addr, cva;
        size_t csz;
        while (kl_x18_next_code(sh[i].sh_addr, (size_t)sh[i].sh_size,
                                skip, nskip, &cursor, &cva, &csz)) {
            const uint32_t *w = (const uint32_t *)(f + sh[i].sh_offset + (cva - sh[i].sh_addr));
            size_t n = csz / 4;
            for (size_t k = 0; k < n; k++) {
                uint64_t va = cva + (uint64_t)k * 4;
                klx_info info;
                klx_decode(w[k], &info);
                g_words++;
                if (info.ok && info.nfields == 0) continue;    // nothing to do here
                if (!info.ok) g_refused++;
                else {
                    g_sites++;
                    if (info.roles == KLX_R) g_rd++;
                    else if (info.roles == KLX_W) g_wr++;
                    else g_rw++;
                }
                printf("%llx %08x %08x %d %u %u\n",
                       (unsigned long long)va, w[k],
                       info.ok ? klx_substitute(w[k], &info, TEST_REG) : w[k],
                       info.ok, info.nfields, info.roles);
            }
        }
    }
    munmap(f, (size_t)sb.st_size);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: t_x18 <file.so> ...\n"); return 2; }
    for (int i = 1; i < argc; i++) scan(argv[i]);
    fprintf(stderr,
            "[t_x18] %lu words scanned, %lu x18 sites (%lu read-only, %lu write-only, "
            "%lu read-write), %lu encodings refused\n",
            g_words, g_sites, g_rd, g_wr, g_rw, g_refused);
    return 0;
}
