// execve, for a translated Linux program.
//
// What the kernel does for a dynamically linked ELF is small: map the program
// and its interpreter, build one stack that describes them, and jump to the
// interpreter. Everything else — relocation, TLS, symbol versions, init order —
// is the guest's own ld.so, which is the point of running it.
//
// "Map" here means dlopen: the images were translated by klepton-ld into signed
// Mach-O, so dyld maps them and the embedded ELF is found by its section, the
// same way kl_load_dylib() finds an Android library. Unlike that path nothing is
// relocated here.
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "kl_lx.h"
#include "../kl_x18.h"

typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr,
    p_filesz, p_memsz, p_align; } Elf64_Phdr;
#define PT_INTERP 3

enum { AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6, AT_BASE = 7,
       AT_FLAGS = 8, AT_ENTRY = 9, AT_UID = 11, AT_EUID = 12, AT_GID = 13, AT_EGID = 14,
       AT_PLATFORM = 15, AT_HWCAP = 16, AT_CLKTCK = 17, AT_SECURE = 23, AT_RANDOM = 25,
       AT_HWCAP2 = 26, AT_EXECFN = 31 };

// What we admit to. FP, ASIMD, AES, PMULL, SHA1, SHA2, CRC32 and LSE atomics are
// all real on Apple silicon. HWCAP_CPUID is deliberately absent: with it glibc
// reads MIDR_EL1 from EL0, which Linux emulates and Darwin does not.
#define LX_HWCAP 0x1fbull

#define LX_STACK_SIZE (8ull << 20)

// Map the translation of the ELF a guest path finally names.
static const Elf64_Ehdr *map_guest(const char *guest_path) {
    char host[PATH_MAX], real[PATH_MAX], dylib[PATH_MAX];
    if (kl_lx_resolve(guest_path, 1, host, sizeof host, real) != 0) strlcpy(real, guest_path, sizeof real);
    int fd = open(host, O_RDONLY);
    int found = fd >= 0 && kl_lx_find_dylib(real, fd, dylib, sizeof dylib) == 0;
    if (fd >= 0) close(fd);
    if (!found) {
        fprintf(stderr, "  [klepton] linux: no translation of %s in any dylib "
                        "directory (run tools/lx_translate.sh)\n", real);
        return NULL;
    }
    return kl_lx_map_image(dylib, real);
}

int kl_lx_exec(const char *prog, int argc, char **argv, char **envp) {
    kl_x18_set_abi(KLX_ABI_LINUX);
    if (kl_x18_init_linux() != 0) return -1;

    // The program must exist in the guest's namespace even though its bytes come
    // from the translation: its path is what $ORIGIN and /proc/self/exe answer.
    char host[PATH_MAX], guest[PATH_MAX];
    if (kl_lx_resolve(prog, 1, host, sizeof host, guest) != 0 || access(host, R_OK) != 0) {
        fprintf(stderr, "  [klepton] linux: %s: not found in the guest namespace\n", prog);
        return -1;
    }
    kl_lx_set_exe(guest);

    const Elf64_Ehdr *exe = map_guest(guest);
    if (!exe) return -1;
    const Elf64_Phdr *ph = (const void *)((const uint8_t *)exe + exe->e_phoff);

    // The interpreter, if the program names one. A program without one is
    // either static or the loader itself being run directly, and enters at its
    // own entry with AT_BASE 0, as the kernel would have it.
    const Elf64_Ehdr *interp = NULL;
    for (int i = 0; i < exe->e_phnum; i++)
        if (ph[i].p_type == PT_INTERP) {
            interp = map_guest((const char *)exe + ph[i].p_offset);
            if (!interp) return -1;
        }

    uint8_t *stack = mmap(NULL, LX_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stack == MAP_FAILED) { perror("  [klepton] linux: stack"); return -1; }

    // Strings and the AT_RANDOM bytes at the very top, the vectors below them.
    uint8_t *top = stack + LX_STACK_SIZE;
    #define PUSH_STR(s) (top -= strlen(s) + 1, (uint64_t)(uintptr_t)strcpy((char *)top, (s)))
    int envc = 0;
    while (envp && envp[envc]) envc++;
    uint64_t *gargv = calloc((size_t)argc + envc + 2, sizeof *gargv), *genvp = gargv + argc + 1;
    for (int i = argc - 1; i >= 0; i--) gargv[i] = PUSH_STR(argv[i]);
    for (int i = envc - 1; i >= 0; i--) genvp[i] = PUSH_STR(envp[i]);
    uint64_t platform = PUSH_STR("aarch64"), execfn = PUSH_STR(argc ? argv[0] : prog);
    top -= 16;
    arc4random_buf(top, 16);
    uint64_t random = (uint64_t)(uintptr_t)top;

    const uint64_t auxv[][2] = {
        { AT_PHDR,   (uint64_t)(uintptr_t)ph },      { AT_PHENT,  sizeof(Elf64_Phdr) },
        { AT_PHNUM,  exe->e_phnum },                 { AT_PAGESZ, 0x4000 },
        { AT_BASE,   (uint64_t)(uintptr_t)interp },  { AT_FLAGS,  0 },
        { AT_ENTRY,  (uint64_t)(uintptr_t)exe + exe->e_entry },
        { AT_UID, 1000 }, { AT_EUID, 1000 }, { AT_GID, 1000 }, { AT_EGID, 1000 },
        { AT_PLATFORM, platform }, { AT_HWCAP, LX_HWCAP }, { AT_HWCAP2, 0 },
        { AT_CLKTCK, 100 }, { AT_SECURE, 0 }, { AT_RANDOM, random },
        { AT_EXECFN, execfn }, { AT_NULL, 0 } };

    size_t words = 1 + (size_t)argc + 1 + (size_t)envc + 1 + sizeof auxv / 8;
    uint64_t *sp = (uint64_t *)(((uintptr_t)top - words * 8) & ~(uintptr_t)15), *w = sp;
    *w++ = (uint64_t)argc;
    memcpy(w, gargv, ((size_t)argc + 1) * 8); w += argc + 1;
    memcpy(w, genvp, ((size_t)envc + 1) * 8); w += envc + 1;
    memcpy(w, auxv, sizeof auxv);
    free(gargv);

    kl_lx_proc_init(argc, argv, (uint64_t)(uintptr_t)stack, (uint64_t)(uintptr_t)stack + LX_STACK_SIZE);
    kl_lx_signal_init();
    pthread_setspecific(KLX_TP_SLOT, NULL);
    pthread_setspecific(KLX_SVC_SLOT, (void *)kl_lx_svc_entry);

    const Elf64_Ehdr *start = interp ? interp : exe;
    kl_lx_enter((uint64_t)(uintptr_t)start + start->e_entry, (uint64_t)(uintptr_t)sp);
}
