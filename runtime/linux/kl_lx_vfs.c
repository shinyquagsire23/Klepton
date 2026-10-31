// The guest's file namespace.
//
// A Linux guest names files by absolute path in a root that is not the host's:
// /lib/aarch64-linux-gnu/libc.so.6 is the runtime's glibc, not anything on this
// machine. So paths are resolved the way a chroot would resolve them — against a
// table of mounts, component by component, with a symlink's target read as a
// GUEST path — and only the final answer is handed to the host. Resolving on the
// host instead is wrong the first time a symlink inside the sysroot is absolute,
// and a Debian sysroot is full of those.
//
// Guest fds ARE host fds; nothing is renumbered. What a descriptor needs
// remembered about it (is it an ELF we hold a translation of) sits in a side
// table indexed by the number.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "kl_lx.h"
#include "kl_lx_name.h"
#include "../klepton.h"
#include "../kl_env.h"

#define LX_MOUNTS 32
#define LX_DIRS   8
#define LX_FDS    4096

static struct { char guest[256]; size_t glen; char host[PATH_MAX]; int ro; } g_mount[LX_MOUNTS];
static int  g_nmount;
static char g_dylib_dir[LX_DIRS][PATH_MAX];
static int  g_ndylib_dir;
static char g_cwd[PATH_MAX] = "/";
static char g_exe[PATH_MAX];
static kl_lx_fdinfo g_fd[LX_FDS];

void kl_lx_mount_ro(const char *guest, const char *host) {
    kl_lx_mount(guest, host);
    if (g_nmount) g_mount[g_nmount - 1].ro = 1;
}

// Whether a resolved host path lies under a read-only mount.
int kl_lx_host_ro(const char *host) {
    for (int i = 0; i < g_nmount; i++) {
        size_t n = strlen(g_mount[i].host);
        if (g_mount[i].ro && !strncmp(host, g_mount[i].host, n) && (!host[n] || host[n] == '/'))
            return 1;
    }
    return 0;
}

void kl_lx_mount(const char *guest, const char *host) {
    if (g_nmount == LX_MOUNTS) return;
    snprintf(g_mount[g_nmount].guest, sizeof g_mount[0].guest, "%s", guest);
    g_mount[g_nmount].glen = strlen(guest);
    if (!realpath(host, g_mount[g_nmount].host))
        snprintf(g_mount[g_nmount].host, PATH_MAX, "%s", host);
    g_nmount++;
}
void kl_lx_add_dylib_dir(const char *dir) {
    if (g_ndylib_dir < LX_DIRS && !realpath(dir, g_dylib_dir[g_ndylib_dir]))
        snprintf(g_dylib_dir[g_ndylib_dir], PATH_MAX, "%s", dir);
    if (g_ndylib_dir < LX_DIRS) g_ndylib_dir++;
}
void        kl_lx_set_exe(const char *guest_path) { snprintf(g_exe, sizeof g_exe, "%s", guest_path); }
const char *kl_lx_exe(void) { return g_exe; }
const char *kl_lx_cwd(void) { return g_cwd; }

int kl_lx_find_dylib(const char *elf_name, int fd, char *out, size_t cap) {
    char file[NAME_MAX], fw[NAME_MAX];
    kl_lx_translation_name(elf_name, fd, file, sizeof file);
    // The same translation inside an app bundle is a framework, because that is
    // the only shape a device accepts signed code in (visionos/mklxguest.sh).
    // '+' becomes 'x' there, as it does for every other guest's frameworks.
    snprintf(fw, sizeof fw, "%.*s", (int)(strlen(file) - strlen(".dylib")), file);
    for (char *c = fw; *c; c++) if (*c == '+') *c = 'x';
    for (int i = 0; i < g_ndylib_dir; i++) {
        snprintf(out, cap, "%s/%s", g_dylib_dir[i], file);
        if (access(out, R_OK) == 0) return 0;
        snprintf(out, cap, "%s/%s.framework/%s", g_dylib_dir[i], fw, fw);
        if (access(out, R_OK) == 0) return 0;
    }
    return -1;
}

// One lexical component step on a normalised absolute guest path: the longest
// mount that covers it, or -1 if the guest's root simply has nothing there.
static int to_host(const char *guest, char *out, size_t cap) {
    int best = -1;
    for (int i = 0; i < g_nmount; i++) {
        size_t n = g_mount[i].glen;
        if (strncmp(guest, g_mount[i].guest, n)) continue;
        if (n > 1 && guest[n] && guest[n] != '/') continue;     // "/lib" must not claim "/lib64"
        if (best < 0 || n > g_mount[best].glen) best = i;
    }
    if (best < 0) return -1;
    const char *tail = guest + g_mount[best].glen;
    while (*tail == '/') tail++;
    snprintf(out, cap, "%s%s%s", g_mount[best].host, *tail ? "/" : "", tail);
    return 0;
}

// Append `comp` to the normalised path in `acc`, honouring "." and "..".
static void push(char *acc, const char *comp, size_t len) {
    if (!len || (len == 1 && comp[0] == '.')) return;
    if (len == 2 && comp[0] == '.' && comp[1] == '.') {
        char *s = strrchr(acc, '/');
        if (s == acc) acc[1] = 0; else if (s) *s = 0;
        return;
    }
    size_t n = strlen(acc);
    if (n > 1) acc[n++] = '/';
    if (n + len < PATH_MAX) { memcpy(acc + n, comp, len); acc[n + len] = 0; }
}

int kl_lx_resolve(const char *path, int follow_last, char *host, size_t cap, char *guest_out) {
    if (!path || !*path) { errno = ENOENT; return -1; }

    // What the kernel's own namespaces provide, which no sysroot contains.
    static const char *const passthrough[] = { "/dev/null", "/dev/zero", "/dev/urandom",
                                               "/dev/random", "/dev/tty" };
    for (size_t i = 0; i < sizeof passthrough / sizeof *passthrough; i++)
        if (!strcmp(path, passthrough[i])) {
            snprintf(host, cap, "%s", path);
            if (guest_out) strcpy(guest_out, path);
            return 0;
        }

    char todo[PATH_MAX * 2], acc[PATH_MAX] = "/";
    if (path[0] == '/') snprintf(todo, sizeof todo, "%s", path);
    else snprintf(todo, sizeof todo, "%s/%s", g_cwd, path);

    for (int links = 0; ; ) {
        const char *p = todo;
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char prev[PATH_MAX], rest[PATH_MAX * 2];
        strcpy(prev, acc);
        snprintf(rest, sizeof rest, "%s", e ? e : "");
        push(acc, p, len);

        char h[PATH_MAX], target[PATH_MAX];
        int last = !rest[0] || !rest[strspn(rest, "/")];
        ssize_t n = -1;
        if ((!last || follow_last) && to_host(acc, h, sizeof h) == 0)
            n = readlink(h, target, sizeof target - 1);
        if (n < 0) { strcpy(todo, rest); continue; }
        if (++links > 40) { errno = ELOOP; return -1; }
        target[n] = 0;
        // A symlink: its target is a guest path, absolute or relative to the
        // directory the link sits in.
        if (target[0] == '/') strcpy(acc, "/"); else strcpy(acc, prev);
        char next[PATH_MAX * 2];
        snprintf(next, sizeof next, "%s%s", target, rest);
        strcpy(todo, next);
    }
    if (guest_out) strcpy(guest_out, acc);
    if (to_host(acc, host, cap) != 0) { errno = ENOENT; return -1; }
    return 0;
}

int kl_lx_chdir(const char *path) {
    char host[PATH_MAX], guest[PATH_MAX];
    struct stat st;
    if (kl_lx_resolve(path, 1, host, sizeof host, guest) != 0) return -1;
    if (stat(host, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    strcpy(g_cwd, guest);
    return 0;
}

kl_lx_fdinfo *kl_lx_fd(int fd) { return fd >= 0 && fd < LX_FDS ? &g_fd[fd] : NULL; }

// Linux's O_* as arm64 spells them; only the low access bits agree with Darwin.
static int open_flags(int f) {
    int d = f & 3;
    if (f & 0100)     d |= O_CREAT;
    if (f & 0200)     d |= O_EXCL;
    if (f & 0400)     d |= O_NOCTTY;
    if (f & 01000)    d |= O_TRUNC;
    if (f & 02000)    d |= O_APPEND;
    if (f & 04000)    d |= O_NONBLOCK;
    if (f & 040000)   d |= O_DIRECTORY;
    if (f & 0100000)  d |= O_NOFOLLOW;
    if (f & 02000000) d |= O_CLOEXEC;
    return d;
}

int kl_lx_open(const char *path, int lx_flags, int mode) {
    char host[PATH_MAX], guest[PATH_MAX];
    if (path && kl_lx_hostlib_is(path)) {
        int hfd = kl_lx_hostlib_open(path);
        kl_lx_fdinfo *hi = hfd >= 0 ? kl_lx_fd(hfd) : NULL;
        if (hi) {
            memset(hi, 0, sizeof *hi);
            snprintf(hi->guest, sizeof hi->guest, "%s", path);
            hi->elf = 2;
        }
        return hfd;
    }
    if (kl_lx_audio_is(path)) {                     // SDL's disk driver: kl_lx_audio.c
        int afd = kl_lx_audio_open();
        kl_lx_fdinfo *ai = afd >= 0 ? kl_lx_fd(afd) : NULL;
        if (ai) { memset(ai, 0, sizeof *ai); snprintf(ai->guest, sizeof ai->guest, "%s", path); }
        return afd;
    }
    if (kl_lx_interstitial_is(path)) {              // a game's loading screen, said rather than drawn
        int ifd = kl_lx_interstitial_open();
        kl_lx_fdinfo *ii = ifd >= 0 ? kl_lx_fd(ifd) : NULL;
        if (ii) { memset(ii, 0, sizeof *ii); snprintf(ii->guest, sizeof ii->guest, "%s", path); }
        return ifd;
    }
    if (kl_lx_hand_is(path)) {                      // the hand a game's "weapon hand" option names
        int dfd = kl_lx_hand_open();
        kl_lx_fdinfo *di = dfd >= 0 ? kl_lx_fd(dfd) : NULL;
        if (di) { memset(di, 0, sizeof *di); snprintf(di->guest, sizeof di->guest, "%s", path); }
        return dfd;
    }
    if (path && kl_lx_proc_is(path)) {
        int pfd = kl_lx_proc_open(path);
        kl_lx_fdinfo *pi = pfd >= 0 ? kl_lx_fd(pfd) : NULL;
        if (pi) { memset(pi, 0, sizeof *pi); snprintf(pi->guest, sizeof pi->guest, "%s", path); }
        return pfd;
    }
    if (kl_lx_resolve(path, !(lx_flags & 0100000), host, sizeof host, guest) != 0) return -1;
    if ((lx_flags & (3 | 0100 | 01000)) && kl_lx_host_ro(host)) { errno = EROFS; return -1; }
    int fd = open(host, open_flags(lx_flags), mode);
    if (fd < 0) return -1;
    kl_lx_fdinfo *fi = kl_lx_fd(fd);
    if (!fi) return fd;
    memset(fi, 0, sizeof *fi);
    snprintf(fi->guest, sizeof fi->guest, "%s", guest);

    // An AArch64 ELF we hold a translation of is remembered as one: its mmap is
    // answered with the translation (kl_lx_mm.c), and reads of its program
    // headers are edited (kl_lx_elf_view).
    uint8_t eh[64];
    if ((lx_flags & 3) == 0 && pread(fd, eh, sizeof eh, 0) == sizeof eh &&
        !memcmp(eh, "\x7f" "ELF", 4) && eh[4] == 2 && eh[18] == 183 && eh[19] == 0) {
        memcpy(&fi->phoff, eh + 0x20, 8);
        memcpy(&fi->phnum, eh + 0x38, 2);
        fi->elf = kl_lx_find_dylib(guest, fd, fi->dylib, sizeof fi->dylib) == 0 ? 1 : -1;
    }
    // KL_LX_OPENLOG=<file>: every guest path that was really opened. What a
    // device needs staged before a run can get as far as this one did
    // (visionos/stage_sync.py --opened).
    static int logfd = -2;
    if (logfd == -2) {
        const char *ol = getenv("KL_LX_OPENLOG");
        logfd = ol && *ol ? open(ol, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
    }
    if (logfd >= 0) dprintf(logfd, "%s\n", guest);
    return fd;
}

void kl_lx_close(int fd) {
    kl_lx_fdinfo *fi = kl_lx_fd(fd);
    if (!fi) return;
    if (fi->dir) closedir(fi->dir);
    if (fi->efd) kl_lx_eventfd_close(fi->efd);
    memset(fi, 0, sizeof *fi);
}

// getdents64. Darwin has no call that fills a buffer with directory entries on
// arm64, so the descriptor grows a DIR* over a dup of itself and entries are
// re-packed into the kernel's linux_dirent64 as they are read. d_type values
// are the same on both sides.
int64_t kl_lx_getdents64(int fd, void *buf, size_t cap) {
    kl_lx_fdinfo *fi = kl_lx_fd(fd);
    if (!fi) { errno = EBADF; return -1; }
    if (!fi->dir) {
        int d = dup(fd);
        if (d < 0 || !(fi->dir = fdopendir(d))) { if (d >= 0) close(d); return -1; }
    }
    size_t used = 0;
    for (;;) {
        long pos = telldir(fi->dir);
        struct dirent *de = readdir(fi->dir);
        if (!de) break;
        size_t nl = strlen(de->d_name), rec = (19 + nl + 1 + 7) & ~(size_t)7;
        if (used + rec > cap) {
            seekdir(fi->dir, pos);                  // does not fit: hand it out next time
            if (!used) { errno = EINVAL; return -1; }
            break;
        }
        uint8_t *o = (uint8_t *)buf + used;
        uint64_t ino = de->d_ino; int64_t off = telldir(fi->dir); uint16_t rl = (uint16_t)rec;
        memcpy(o, &ino, 8); memcpy(o + 8, &off, 8); memcpy(o + 16, &rl, 2);
        o[18] = de->d_type;
        memcpy(o + 19, de->d_name, nl + 1);
        used += rec;
    }
    return (int64_t)used;
}

// What the guest's loader may read of a translated ELF's program headers.
//
// The loader does not map these files — kl_lx_mm.c answers its mmaps from the
// translation — but it does validate them first, against the page size it was
// told, and two of its checks are about a mapping that will never happen:
//
//   p_align larger than a page (every Steam Runtime library says 0x10000) makes
//   glibc reserve address space itself and map the segments into it, at an
//   address of its choosing. A signed image mapped by dyld cannot be moved
//   there. The translation is already laid out on 16 KB boundaries, so the
//   honest answer is the page size.
//
//   p_vaddr - p_offset not a multiple of the page (libsteam_api.so is linked for
//   4 KB pages) is refused outright: "address/offset not page-aligned". The file
//   offset only matters to someone who is going to map the file, so the view
//   gives it the one value that is always congruent, the address.
//
//   Two segments that share a 16 KB page (same library: its text ends and its
//   data begins 0x1eb8 apart) are refused after the first mmap, under the same
//   message, because the loader's page arithmetic makes them overlap. They do
//   not overlap in the translation — klepton-ld shifts the image until a host
//   page boundary falls in that gap — so the view ends the earlier segment at
//   the page the later one starts on. Only the loader's bookkeeping reads that
//   size; the phdrs the guest finds IN MEMORY (dl_iterate_phdr, the unwinder)
//   are the real ones.
//
// Nothing else in the file is touched, and reads of anything that is not a
// PT_LOAD entry pass through.
void kl_lx_elf_view(int fd, uint8_t *buf, size_t n, uint64_t off) {
    kl_lx_fdinfo *fi = kl_lx_fd(fd);
    if (!fi || fi->elf != 1 || !fi->phnum || fi->phnum > 64) return;
    struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } ph[64];
    size_t bytes = (size_t)fi->phnum * 56;
    if (off >= fi->phoff + bytes || off + n <= fi->phoff) return;
    if (pread(fd, ph, bytes, (off_t)fi->phoff) != (ssize_t)bytes) return;

    for (unsigned i = 0; i < fi->phnum; i++) {
        if (ph[i].type != 1) continue;                               // PT_LOAD
        if ((ph[i].vaddr - ph[i].offset) & 0x3fff) ph[i].offset = ph[i].vaddr;
        ph[i].align = 0x4000;
        for (unsigned k = i + 1; k < fi->phnum; k++) {
            if (ph[k].type != 1) continue;
            uint64_t limit = ph[k].vaddr & ~0x3fffull;               // the next segment's page
            if (ph[i].vaddr + ph[i].filesz > limit && limit > ph[i].vaddr) {
                ph[i].filesz = limit - ph[i].vaddr;
                if (ph[i].memsz > ph[i].filesz) ph[i].memsz = ph[i].filesz;
            }
            break;
        }
    }
    for (size_t b = 0; b < bytes; b++)
        if (fi->phoff + b >= off && fi->phoff + b < off + n)
            buf[fi->phoff + b - off] = ((uint8_t *)ph)[b];
}

// The kernel's struct stat on arm64 (asm-generic), which is not Darwin's.
struct lx_stat {
    uint64_t st_dev, st_ino; uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev, pad1; int64_t st_size; int32_t st_blksize, pad2; int64_t st_blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns; uint32_t unused[2];
};
_Static_assert(sizeof(struct lx_stat) == 128, "arm64 struct stat is 128 bytes");

void kl_lx_stat_out(const struct stat *s, void *out) {
    struct lx_stat *l = out;
    memset(l, 0, sizeof *l);
    l->st_dev = (uint64_t)s->st_dev;   l->st_ino = s->st_ino;
    l->st_mode = s->st_mode;           l->st_nlink = s->st_nlink;
    l->st_uid = 1000;                  l->st_gid = 1000;
    l->st_rdev = (uint64_t)s->st_rdev; l->st_size = s->st_size;
    l->st_blksize = 0x4000;            l->st_blocks = s->st_blocks;
    l->atime = s->st_atimespec.tv_sec; l->atime_ns = s->st_atimespec.tv_nsec;
    l->mtime = s->st_mtimespec.tv_sec; l->mtime_ns = s->st_mtimespec.tv_nsec;
    l->ctime = s->st_ctimespec.tv_sec; l->ctime_ns = s->st_ctimespec.tv_nsec;
}
