// The guest's address space: mmap, munmap, mprotect.
//
// Two kinds of memory, and the whole file is about keeping them apart.
//
// ORDINARY memory — anonymous mappings and data files — is the host's, with the
// flag words translated. Nothing here is ever executable: on a headset a page
// that did not come from a signed file cannot run, so the host does not allow
// it either, rather than being the one place a JIT works.
//
// What a request for executable anonymous memory gets is the memory, WITHOUT the
// execute bit, and success. Refusing outright was the first answer and it was
// wrong for the commonest caller: Source 2 commits LuaJIT's entire heap
// read-write-execute (one mmap override serves both the Lua heap and its machine
// code), with the JIT compiler switched off — so no instruction is ever fetched
// from those pages, and a refusal only meant the Lua VM had no heap at all. A
// guest that does jump there faults at once, and the fault reporter knows the
// range and says what it was (kl_lx_wanted_exec).
//
// IMAGE memory is a translated ELF, mapped by dyld from a signed Mach-O. The
// guest's loader does not know that. It opens the library, reads the headers and
// asks for the file to be mapped, segment by segment, exactly as it would on
// Linux — and every one of those requests is answered from the mapping dyld
// already made:
//
//   the first mmap of the file    -> here is your image (dlopen, then its base)
//   MAP_FIXED file mmaps inside   -> already there; klepton-ld laid the segments
//                                    out at the same offsets the ELF asks for
//   MAP_FIXED anonymous inside    -> .bss: zero it in place
//   mprotect inside               -> honoured on the writable part (RELRO),
//                                    ignored on the text, which dyld owns
//   munmap inside                 -> ignored; a signed image is not unmapped
//                                    piecemeal
//
// ...with one consequence that has to be undone by hand. dlclose() ends in the
// loader unmapping the library, and a later dlopen() of the same file expects
// what mmap of a file always gives: the data segment as the file has it, and
// writable. Ours is neither by then — it has been relocated once and its RELRO
// part made read-only. So an image whose whole span was unmapped is remembered
// as stale, and the next time the loader asks for that file its writable part is
// put back the way the Mach-O has it on disk. (SDL does exactly this to libudev
// during start-up.)
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "kl_lx.h"
#include "../klepton.h"
#include "../kl_x18.h"
#include "../kl_env.h"

#define LX_MAP_FIXED            0x10
#define LX_MAP_ANONYMOUS        0x20
#define LX_MAP_DENYWRITE        0x800
#define LX_MAP_FIXED_NOREPLACE  0x100000
#define LX_PAGE                 0x4000ull

typedef struct {
    uint64_t lo, hi;        // the embedded ELF's whole span, as mapped
    uint64_t data;          // first writable byte's page; == hi if there is none
    char     path[PATH_MAX];
    char     guest[PATH_MAX];
    uint64_t data_foff, data_fsize;     // __DATA's bytes in the Mach-O file
    int      stale;                     // unmapped by the guest since it was last handed out
} lx_image;

#define LX_IMAGES 256
static lx_image g_img[LX_IMAGES];
static int      g_nimg;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Ranges the guest asked to be executable and was given without the bit.
#define LX_WANTED 512
static struct { uint64_t lo, hi; } g_wanted[LX_WANTED];
static int g_nwanted;

static void wanted_exec(uint64_t addr, uint64_t len) {
    pthread_mutex_lock(&g_lock);
    if (!g_nwanted)
        fprintf(stderr, "  [klepton] linux: the guest asked for executable memory (%#llx, "
                        "%#llx bytes) and was given it without the execute bit — "
                        "unsigned pages cannot run on a headset. A JIT that emits code "
                        "there will fault when it jumps to it.\n",
                (unsigned long long)addr, (unsigned long long)len);
    if (g_nwanted < LX_WANTED) {
        g_wanted[g_nwanted].lo = addr;
        g_wanted[g_nwanted++].hi = addr + len;
    }
    pthread_mutex_unlock(&g_lock);
}

int kl_lx_wanted_exec(uint64_t addr) {
    for (int i = 0; i < g_nwanted; i++)
        if (addr >= g_wanted[i].lo && addr < g_wanted[i].hi) return 1;
    return 0;
}

static lx_image *image_at(uint64_t addr) {
    for (int i = 0; i < g_nimg; i++)
        if (addr >= g_img[i].lo && addr < g_img[i].hi) return &g_img[i];
    return NULL;
}

static int64_t lx_errno(int e) { return -(int64_t)kl_errno_to_linux(e); }

int kl_lx_image_info(int i, uint64_t *lo, uint64_t *data, uint64_t *hi, const char **guest_path) {
    if (i < 0 || i >= g_nimg) return 0;
    *lo = g_img[i].lo; *data = g_img[i].data; *hi = g_img[i].hi; *guest_path = g_img[i].guest;
    return 1;
}

const void *kl_lx_map_image(const char *dylib_path, const char *guest_path) {
    char real[PATH_MAX];
    if (!realpath(dylib_path, real)) snprintf(real, sizeof real, "%s", dylib_path);

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nimg; i++)
        if (!strcmp(g_img[i].path, real)) {
            lx_image *im = &g_img[i];
            if (im->stale && im->hi > im->data) {
                // A fresh mapping of the file, as far as the guest can tell.
                void *d = (void *)(uintptr_t)im->data;
                size_t len = (size_t)(im->hi - im->data);
                int fd = open(real, O_RDONLY);
                mprotect(d, len, PROT_READ | PROT_WRITE);
                memset(d, 0, len);
                if (fd < 0 || pread(fd, d, (size_t)im->data_fsize, (off_t)im->data_foff) != (ssize_t)im->data_fsize)
                    fprintf(stderr, "  [klepton] linux: could not restore the data of %s for a "
                                    "reload\n", real);
                if (fd >= 0) close(fd);
            }
            im->stale = 0;
            pthread_mutex_unlock(&g_lock);
            return (const void *)(uintptr_t)im->lo;
        }
    pthread_mutex_unlock(&g_lock);

    if (!dlopen(real, RTLD_NOW | RTLD_LOCAL)) {
        fprintf(stderr, "  [klepton] linux: dlopen %s: %s\n", real, dlerror());
        return NULL;
    }
    // KL_LX_LOADLOG=<file>: one line per image the guest really loaded. A device
    // bundle carries only those (visionos/mklxguest.sh reads this file).
    const char *ll = getenv("KL_LX_LOADLOG");
    if (ll && *ll) {
        FILE *f = fopen(ll, "a");
        const char *b = strrchr(real, '/');
        if (f) { fprintf(f, "%s\t%s\n", b ? b + 1 : real, guest_path); fclose(f); }
    }
    const struct mach_header_64 *mh = NULL;
    for (uint32_t i = 0; i < _dyld_image_count() && !mh; i++) {
        const char *nm = _dyld_get_image_name(i);
        char nr[PATH_MAX];
        if (!nm) continue;
        if (!realpath(nm, nr)) snprintf(nr, sizeof nr, "%s", nm);
        if (!strcmp(nr, real)) mh = (const void *)_dyld_get_image_header(i);
    }
    unsigned long esz = 0, ssz = 0, dsz = 0;
    const uint8_t *elf = mh ? getsectiondata(mh, "__TEXT", "__klelf", &esz) : NULL;
    const klx_stat_section *rec = mh ? (const void *)getsectiondata(mh, "__TEXT", "__klstat", &ssz) : NULL;
    if (!elf || !rec || rec->magic != KLX_STAT_MAGIC || rec->abi != KLX_ABI_LINUX ||
        memcmp(elf, "\x7f" "ELF", 4)) {
        fprintf(stderr, "  [klepton] linux: %s is not a klepton-ld --abi linux image "
                        "from this tree — re-translate it\n", real);
        return NULL;
    }
    // __DATA straight from its load command. getsegmentdata() would be the
    // obvious call and reports a size one page short for some of these images
    // (libfreetype: 0x4000 for a segment whose vmsize is 0x8000), which is
    // exactly the page the loader's .bss mapping then falls off the end of.
    const uint8_t *data = NULL;
    uint64_t dfoff = 0, dfsize = 0;
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        const struct segment_command_64 *sg = (const void *)lc;
        if (lc->cmd == LC_SEGMENT_64 && !strcmp(sg->segname, "__DATA")) {
            data = (const uint8_t *)mh + sg->vmaddr;        // __TEXT is at vmaddr 0
            dsz = sg->vmsize;
            dfoff = sg->fileoff;
            dfsize = sg->filesize;
        }
    }

    pthread_mutex_lock(&g_lock);
    lx_image *im = g_nimg < LX_IMAGES ? &g_img[g_nimg] : NULL;
    if (im) {
        im->lo   = (uint64_t)(uintptr_t)elf;
        im->hi   = data ? (uint64_t)(uintptr_t)data + dsz : (im->lo + esz + LX_PAGE - 1) & ~(LX_PAGE - 1);
        im->data = data ? (uint64_t)(uintptr_t)data : im->hi;
        snprintf(im->path, sizeof im->path, "%s", real);
        snprintf(im->guest, sizeof im->guest, "%s", guest_path);
        im->data_foff = dfoff;
        im->data_fsize = dfsize;
        im->stale = 0;
        g_nimg++;
    }
    pthread_mutex_unlock(&g_lock);
    if (!im) { fprintf(stderr, "  [klepton] linux: more than %d images\n", LX_IMAGES); return NULL; }
    return elf;
}

int64_t kl_lx_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, int64_t off) {
    int anon  = (flags & LX_MAP_ANONYMOUS) != 0;
    // KL_LX_LOWMEM_BT=1: who is asking for memory below 4 GB. Nothing can be
    // mapped there on Darwin arm64 (it is all __PAGEZERO), so a guest that needs
    // it is a guest that needs changing, and this says which part.
    if (addr && addr < (1ull << 32) && !(flags & LX_MAP_FIXED) && kl_lx_cur) {
        static int said;
        if (said < 3 && kl_env_on("KL_LX_LOWMEM_BT", 0)) {
            said++;
            fprintf(stderr, "  [klepton] linux: mmap hint %#llx (+%#llx, prot %d) is below 4 GB:\n",
                    (unsigned long long)addr, (unsigned long long)len, prot);
            kl_lx_backtrace(kl_lx_cur);
        }
    }
    int fixed = (flags & (LX_MAP_FIXED | LX_MAP_FIXED_NOREPLACE)) != 0;
    lx_image *im = fixed ? image_at(addr) : NULL;

    if (im) {
        // The loader rounds to pages in the ELF's address space; an image whose
        // base is not itself on a host page (a 4 KB-aligned guest, shifted so
        // the boundary falls between its segments) makes that rounding overshoot
        // the real end by less than a page. More than that means the
        // translation's layout and the ELF's disagree, which nothing downstream
        // would survive.
        uint64_t end = addr + len;
        if (end > im->hi + LX_PAGE) {
            fprintf(stderr, "  [klepton] linux: fixed mapping %#llx+%#llx runs past "
                            "the image %s\n", (unsigned long long)addr,
                    (unsigned long long)len, im->path);
            return lx_errno(ENOMEM);
        }
        if (end > im->hi) end = im->hi;
        if (anon && addr >= im->data && end > addr)
            memset((void *)(uintptr_t)addr, 0, (size_t)(end - addr));
        return (int64_t)addr;
    }

    // The loader's mapping of a library, as opposed to anyone else's mapping of
    // the same file: glibc asks with MAP_DENYWRITE, and nothing else does. A
    // program that maps an ELF to READ it — a backtrace printer looking up its
    // own symbols is the usual one — gets the file, like any other file.
    kl_lx_fdinfo *fi = anon ? NULL : kl_lx_fd(fd);
    int loading = fi && (flags & LX_MAP_DENYWRITE);
    if (loading && fi->elf == 1 && !fixed && off == 0) {
        const void *base = kl_lx_map_image(fi->dylib, fi->guest);
        if (!base) return lx_errno(ENOEXEC);
        lx_image *got = image_at((uint64_t)(uintptr_t)base);
        if ((uint64_t)(uintptr_t)base + len > got->hi) {
            fprintf(stderr, "  [klepton] linux: %s wants %#llx bytes mapped but its "
                            "translation spans %#llx\n", fi->guest, (unsigned long long)len,
                    (unsigned long long)(got->hi - got->lo));
            return lx_errno(ENOMEM);
        }
        return (int64_t)(uintptr_t)base;
    }
    if (loading && fi->elf && fi->elf != 2) {
        // An ELF nobody translated. Named, because it is a missing
        // tools/lx_translate.sh run and nothing else.
        fprintf(stderr, "  [klepton] linux: refusing to map %s — no translation of it was "
                        "found (run tools/lx_translate.sh)\n", fi->guest);
        return lx_errno(ENOEXEC);
    }

    int dflags = (flags & 3) | (anon ? MAP_ANON : 0) | (fixed ? MAP_FIXED : 0);
    void *p = mmap((void *)(uintptr_t)addr, (size_t)len, prot & ~PROT_EXEC, dflags,
                   anon ? -1 : fd, off);
    if (p == MAP_FAILED) return lx_errno(errno);
    if (prot & PROT_EXEC) wanted_exec((uint64_t)(uintptr_t)p, len);
    return (int64_t)(uintptr_t)p;
}

int64_t kl_lx_munmap(uint64_t addr, uint64_t len) {
    lx_image *im = image_at(addr);
    if (im) {
        // The loader letting go of the whole library, which is what dlclose
        // comes to. See the top of the file for what that obliges.
        if (addr <= im->lo && addr + len >= im->data) im->stale = 1;
        return 0;
    }
    return munmap((void *)(uintptr_t)addr, (size_t)len) ? lx_errno(errno) : 0;
}

// madvise(MADV_DONTNEED) — the one piece of advice that is not advice.
//
// On Linux it is a promise about CONTENTS: private anonymous pages given back
// this way read as zero the next time they are touched. glibc depends on it —
// a thread arena that shrinks its heap hands the tail back with exactly this
// call and later grows into it again assuming zeros, which is what lets calloc
// skip the clear — and so does every allocator written against Linux. Answered
// with a bare 0 the memory keeps its old bytes, and a "zeroed" object on some
// thread other than the main one comes back holding whatever was freed there:
// Alyx's video library read a stale 'profiling is on' flag out of one and
// followed the null pointer beside it (libvideo.so+0x1180b0, on loading the
// first level).
//
// Darwin has no call with that contract, so the pages are replaced: whole host
// pages by a fresh anonymous mapping with the protection they had — which also
// gives the memory back, the other half of what the caller wanted — and the
// partial pages at either end, which cannot be remapped, by clearing them.
// Only private anonymous memory is touched; a file's pages and a translated
// image are left as they are, where Linux would re-read them from the file.
int64_t kl_lx_madvise_dontneed(uint64_t addr, uint64_t len) {
    uint64_t end = addr + len;
    while (addr < end) {
        vm_address_t ra = (vm_address_t)addr;
        vm_size_t rs = 0;
        vm_region_extended_info_data_t info;
        mach_msg_type_number_t cnt = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t obj = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &ra, &rs, VM_REGION_EXTENDED_INFO,
                         (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS || !rs)
            break;
        if ((uint64_t)ra >= end) break;
        uint64_t lo = (uint64_t)ra > addr ? (uint64_t)ra : addr;
        uint64_t hi = (uint64_t)ra + rs < end ? (uint64_t)ra + rs : end;
        int priv = info.share_mode == SM_PRIVATE || info.share_mode == SM_COW ||
                   info.share_mode == SM_PRIVATE_ALIASED;
        if (priv && !info.external_pager && !image_at(lo)) {
            uint64_t alo = (lo + LX_PAGE - 1) & ~(LX_PAGE - 1), ahi = hi & ~(LX_PAGE - 1);
            int prot = info.protection & (PROT_READ | PROT_WRITE);
            int w = (info.protection & VM_PROT_WRITE) != 0;
            if (alo < ahi) {
                if (mmap((void *)(uintptr_t)alo, (size_t)(ahi - alo), prot,
                         MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED && w)
                    memset((void *)(uintptr_t)alo, 0, (size_t)(ahi - alo));
                if (w && lo < alo) memset((void *)(uintptr_t)lo, 0, (size_t)(alo - lo));
                if (w && ahi < hi) memset((void *)(uintptr_t)ahi, 0, (size_t)(hi - ahi));
            } else if (w) {
                memset((void *)(uintptr_t)lo, 0, (size_t)(hi - lo));
            }
        }
        addr = (uint64_t)ra + rs;
    }
    return 0;
}

int64_t kl_lx_mprotect(uint64_t addr, uint64_t len, int prot) {
    lx_image *im = image_at(addr);
    if (im) {
        // Only the writable part, only on whole host pages, and never to
        // executable. RELRO is the one caller that matters.
        if (addr < im->data || (prot & PROT_EXEC)) return 0;
        uint64_t lo = (addr + LX_PAGE - 1) & ~(LX_PAGE - 1), hi = (addr + len) & ~(LX_PAGE - 1);
        if (hi > im->hi) hi = im->hi;
        if (lo < hi) mprotect((void *)(uintptr_t)lo, (size_t)(hi - lo), prot);
        return 0;
    }
    // A range that is not on host pages. The guest was told the page is 16 KB,
    // and glibc believes it — but Source 2's own page allocators (Panorama's
    // reserves 64 KB and commits it 4 KB at a time) have 4096 compiled in, and
    // Darwin answers EINVAL to an mprotect that does not start on a page. The
    // protection cannot be finer than the host's, so it is rounded in the
    // direction that keeps the guest running: a request that GRANTS access is
    // widened to the pages it touches, one that takes access away is narrowed to
    // the pages it wholly covers. The cost is that a 4 KB guard page guards
    // nothing, which is the lesser failure.
    uint64_t lo = addr, hi = addr + len;
    if ((lo | hi) & (LX_PAGE - 1)) {
        if (prot & (PROT_WRITE | PROT_EXEC)) {
            lo &= ~(LX_PAGE - 1);
            hi = (hi + LX_PAGE - 1) & ~(LX_PAGE - 1);
        } else {
            lo = (lo + LX_PAGE - 1) & ~(LX_PAGE - 1);
            hi &= ~(LX_PAGE - 1);
            if (lo >= hi) return 0;
        }
    }
    if (mprotect((void *)(uintptr_t)lo, (size_t)(hi - lo), prot & ~PROT_EXEC) != 0) return lx_errno(errno);
    if (prot & PROT_EXEC) wanted_exec(addr, len);
    return 0;
}
