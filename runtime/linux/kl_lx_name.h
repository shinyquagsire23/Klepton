// The name a translation of a Linux ELF is stored under — shared by the tool
// that writes it (klepton-ld --print-name, via tools/lx_translate.sh) and the
// runtime that looks for it, so the two cannot disagree.
//
// It took two wrong answers to get here. The stem ("libvorbis") is not an
// identity: the Alyx depot ships libvorbis.so and libvorbis.so.0 as different
// files and glibc loads both. The whole file name is not one either: the depot
// and the Steam Runtime each have a libfreetype.so.6.20.2, and they are not the
// same library. So the name carries a fingerprint of the content as well —
// the file's size and a hash of its first, middle and last 4 KiB, which between
// them hold the ELF header, the build-id note and the section headers.
#ifndef KL_LX_NAME_H
#define KL_LX_NAME_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static inline uint32_t kl_lx_elf_fingerprint(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return 0;
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)st.st_size;
    const off_t at[3] = { 0, st.st_size / 2, st.st_size > 4096 ? st.st_size - 4096 : 0 };
    for (int k = 0; k < 3; k++) {
        uint8_t buf[4096];
        ssize_t n = pread(fd, buf, sizeof buf, at[k]);
        for (ssize_t i = 0; i < n; i++) h = (h ^ buf[i]) * 0x100000001b3ull;
    }
    return (uint32_t)(h ^ (h >> 32));
}

// "…/libvorbis.so.0" -> "libvorbis_so_0-1a2b3c4d.dylib"
static inline void kl_lx_translation_name(const char *elf_path, int fd, char *out, size_t cap) {
    const char *b = strrchr(elf_path, '/');
    char base[256];
    snprintf(base, sizeof base, "%s", b ? b + 1 : elf_path);
    for (char *c = base; *c; c++) if (*c == '.') *c = '_';
    snprintf(out, cap, "%s-%08x.dylib", base, kl_lx_elf_fingerprint(fd));
}

#endif
