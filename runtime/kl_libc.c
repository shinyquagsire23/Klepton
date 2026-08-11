// libc entry points that cannot be forwarded: divergent struct layouts, Android-only
// APIs, and functions whose ABI differs between Linux/aarch64 and Darwin/arm64.
// Everything whose signature *and* layout match is in kl_libc_table.h instead.
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <setjmp.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <sys/utsname.h>
#include <wchar.h>
#include <ctype.h>
#include "klepton.h"
#include "kl_va.h"
#include "kl_jni.h"     // kl_jni_build_string — the Build.* half of the system
                        // properties below, so the two cannot drift apart

static void warn_once(const char *what) {
    static const char *seen[64]; static int n = 0;
    for (int i = 0; i < n; i++) if (seen[i] == what) return;
    if (n < 64) seen[n++] = what;
    fprintf(stderr, "  [klepton] TODO: %s called but not fully implemented\n", what);
}

// ---------- errno / environ ----------
// errno numbering diverges above 34, and the guest compares against Linux's
// constants. This finally bit at IL2CPP's semaphores: sem_timedwait timed out
// legitimately, we set Darwin's ETIMEDOUT (60), IL2CPP tested for Linux's (110),
// did not recognise it as a timeout and took its fatal path — "sem_wait failed",
// abort, with the real cause four layers down. PLANNING listed this as a
// silent-wrong-answer waiting to happen; this is it happening.
//
// Only the codes that actually differ are listed. Everything <= 34 is identical
// on both platforms and falls through unchanged.
static const struct { int darwin, linux_; } g_errno_map[] = {
    {11,  35},   // EDEADLK
    {35,  11},   // EAGAIN / EWOULDBLOCK
    {36,  115},  // EINPROGRESS
    {37,  114},  // EALREADY
    {38,  88},   // ENOTSOCK
    {39,  89},   // EDESTADDRREQ
    {40,  90},   // EMSGSIZE
    {41,  91},   // EPROTOTYPE
    {42,  92},   // ENOPROTOOPT
    {43,  93},   // EPROTONOSUPPORT
    {44,  94},   // ESOCKTNOSUPPORT
    {45,  95},   // ENOTSUP / EOPNOTSUPP
    {46,  96},   // EPFNOSUPPORT
    {47,  97},   // EAFNOSUPPORT
    {48,  98},   // EADDRINUSE
    {49,  99},   // EADDRNOTAVAIL
    {50,  100},  // ENETDOWN
    {51,  101},  // ENETUNREACH
    {52,  102},  // ENETRESET
    {53,  103},  // ECONNABORTED
    {54,  104},  // ECONNRESET
    {55,  105},  // ENOBUFS
    {56,  106},  // EISCONN
    {57,  107},  // ENOTCONN
    {58,  108},  // ESHUTDOWN
    {59,  109},  // ETOOMANYREFS
    {60,  110},  // ETIMEDOUT   <- the one that bit
    {61,  111},  // ECONNREFUSED
    {62,  40},   // ELOOP
    {63,  36},   // ENAMETOOLONG
    {64,  112},  // EHOSTDOWN
    {65,  113},  // EHOSTUNREACH
    {66,  39},   // ENOTEMPTY
    {68,  122},  // EDQUOT
    {69,  116},  // ESTALE
    {77,  37},   // ENOLCK
    {78,  38},   // ENOSYS
    {84,  75},   // EOVERFLOW
    {89,  125},  // ECANCELED
    {90,  43},   // EIDRM
    {91,  42},   // ENOMSG
    {92,  84},   // EILSEQ
    {93,  61},   // ENOATTR / ENODATA
    {96,  87},   // EUSERS
    {100, 71},   // EPROTO
    {101, 74},   // EBADMSG
    {102, 95},   // EOPNOTSUPP
    {104, 76},   // EMULTIHOP
    {105, 72},   // ENOLINK
    {106, 62},   // ETIME
};

int kl_errno_to_linux(int e) {
    for (size_t i = 0; i < sizeof g_errno_map / sizeof g_errno_map[0]; i++)
        if (g_errno_map[i].darwin == e) return g_errno_map[i].linux_;
    return e;
}
int kl_errno_from_linux(int e) {
    for (size_t i = 0; i < sizeof g_errno_map / sizeof g_errno_map[0]; i++)
        if (g_errno_map[i].linux_ == e) return g_errno_map[i].darwin;
    return e;
}

// bionic spells it __errno(), and it returns the *location* rather than the
// value — so the translation has to be staged through a per-thread slot that is
// refreshed on every call. Reads are what matter and they are always fresh; a
// guest that writes here (errno = 0 before a call) sets the slot, and the next
// read re-derives from Darwin's real errno anyway.
static _Thread_local int g_guest_errno;
int *klb_errno(void) {
    g_guest_errno = kl_errno_to_linux(*__error());
    return &g_guest_errno;
}

extern char **environ;
char ***klb_environ_ptr(void) { return &environ; }   // registered as data below

// ---------- Android-only ----------
int  klb_gettid(void) { uint64_t t = 0; pthread_threadid_np(NULL, &t); return (int)t; }
// ---- __system_property_* ----
//
// This used to answer empty for everything, which is not a neutral answer. It
// is a SECOND answer, disagreeing with the first: Build.MANUFACTURER/MODEL over
// JNI already say "Oculus" / "Quest 2" (a settled decision — see g_fields in
// kl_jni.c), and ro.product.manufacturer/model are the same facts asked through
// libc. Steam Link asks both ways, so the disagreement was visible to the guest.
//
// It was also a crash. CShellSystem::GetDeviceName() is
//   mfr = get("ro.product.manufacturer"); mdl = get("ro.product.model");
//   name = mdl.startsWith(mfr) ? mdl : mfr + " " + mdl;
// and with both empty it faulted (SIGBUS in GetDeviceName+0x264, reached from
// BIsVRHeadset <- LoadStreamingSettings <- CShellApplication::BInit <- main) —
// the first thing the 2D shell does after Qt comes up. Trap 6d exactly: a
// silent zero is worse than an error, because nothing near the fault mentions
// a property.
//
// Only ro.product.* is mapped, and each entry names the Build field it must
// agree with rather than repeating its value. Anything else still answers
// empty, which for a property that genuinely is not set IS the truth — bionic
// returns 0 for an unset property and callers handle it.
static const struct { const char *prop, *build_field; } g_sysprops[] = {
    {"ro.product.manufacturer", "MANUFACTURER"},
    {"ro.product.brand",        "BRAND"},
    {"ro.product.model",        "MODEL"},
    {"ro.product.device",       "DEVICE"},
    {"ro.product.name",         "PRODUCT"},
    {"ro.hardware",             "HARDWARE"},
    {"ro.build.id",             "ID"},
    {"ro.build.display.id",     "DISPLAY"},
    {"ro.build.type",           "TYPE"},
    {"ro.build.tags",           "TAGS"},
    {"ro.build.fingerprint",    "FINGERPRINT"},
    {"ro.build.version.release","RELEASE"},
    {"ro.build.version.incremental", "INCREMENTAL"},
    {"ro.build.version.codename",    "CODENAME"},
};

static const char *sysprop_value(const char *n) {
    if (!n) return NULL;
    for (size_t i = 0; i < sizeof g_sysprops / sizeof *g_sysprops; i++)
        if (strcmp(n, g_sysprops[i].prop) == 0)
            return kl_jni_build_string(g_sysprops[i].build_field);
    return NULL;
}

// find/read are the two-step form of the same lookup. The "handle" we hand back
// is the table entry's own name pointer, which is a stable string constant for
// the life of the process — trap 3 does not apply, because nothing stores this
// in guest memory sized for a 32-bit index; bionic's prop_info* is opaque and
// only ever passed straight back to __system_property_read.
const void *klb_sysprop_find(const char *n) {
    if (!n) return NULL;
    for (size_t i = 0; i < sizeof g_sysprops / sizeof *g_sysprops; i++)
        if (strcmp(n, g_sysprops[i].prop) == 0 && sysprop_value(n))
            return g_sysprops[i].prop;
    return NULL;
}

// PROP_VALUE_MAX is 92 in bionic and the caller's buffer is that size. Truncate
// rather than overrun; every value we serve is far shorter.
#define KLB_PROP_VALUE_MAX 92

int klb_sysprop_get(const char *n, char *v) {
    const char *val = sysprop_value(n);
    if (!v) return val ? (int)strlen(val) : 0;
    if (!val) { v[0] = 0; return 0; }
    int len = snprintf(v, KLB_PROP_VALUE_MAX, "%s", val);
    if (len >= KLB_PROP_VALUE_MAX) len = KLB_PROP_VALUE_MAX - 1;
    return len;
}

void klb_sysprop_read(const void *p, char *n, char *v) {
    const char *prop = (const char *)p;
    if (n) { if (prop) snprintf(n, 32, "%s", prop); else n[0] = 0; }
    if (v) klb_sysprop_get(prop, v);
}
int  klb_prctl(int op, ...)                  { (void)op; return 0; }
int  klb_sched_getaffinity(int p, size_t sz, void *m) {
    (void)p; if (m && sz) memset(m, 0xFF, sz);       // pretend every CPU is available
    return 0;
}
int  klb_sched_setaffinity(int p, size_t sz, const void *m) { (void)p;(void)sz;(void)m; return 0; }

// ---------- struct stat ----------
// bionic/aarch64 layout (128 bytes) is unrelated to Darwin's; translate field by field.
typedef struct {
    uint64_t st_dev, st_ino;
    uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev, __pad1;
    int64_t  st_size;
    int32_t  st_blksize, __pad2;
    int64_t  st_blocks;
    struct { int64_t tv_sec; int64_t tv_nsec; } st_atim, st_mtim, st_ctim;
    uint32_t __unused4, __unused5;
} bionic_stat;

static void stat_to_bionic(const struct stat *d, bionic_stat *b) {
    memset(b, 0, sizeof *b);
    b->st_dev = (uint64_t)d->st_dev;   b->st_ino  = d->st_ino;
    b->st_mode = d->st_mode;           b->st_nlink = d->st_nlink;
    b->st_uid = d->st_uid;             b->st_gid  = d->st_gid;
    b->st_rdev = (uint64_t)d->st_rdev; b->st_size = d->st_size;
    b->st_blksize = d->st_blksize;     b->st_blocks = d->st_blocks;
    b->st_atim.tv_sec = d->st_atimespec.tv_sec; b->st_atim.tv_nsec = d->st_atimespec.tv_nsec;
    b->st_mtim.tv_sec = d->st_mtimespec.tv_sec; b->st_mtim.tv_nsec = d->st_mtimespec.tv_nsec;
    b->st_ctim.tv_sec = d->st_ctimespec.tv_sec; b->st_ctim.tv_nsec = d->st_ctimespec.tv_nsec;
}
// stat/lstat are declared before the /proc layer they use, so the rewriting
// helper is forward-declared rather than reordering the file.
const char *kl_guest_path(const char *path, char *buf, size_t cap);
static void kl_fs_trace(const char *op, const char *path, const char *extra, int failed);

// stat/lstat are traced like open: File.Exists() in managed code is a stat,
// never an open, so an untraced stat makes a probe for a missing file look
// like the guest never asked at all. That blind spot read as "the game never
// checks for settings.cfg" once — it does, it just never opens it.
int klb_stat(const char *p, bionic_stat *b)  { char kp[1024]; const char *q = kl_guest_path(p, kp, sizeof kp);
                                               struct stat s; int r = stat(q, &s);
                                               kl_fs_trace("stat", p, NULL, r != 0);
                                               if (!r) stat_to_bionic(&s, b); return r; }
int klb_lstat(const char *p, bionic_stat *b) { char kp[1024]; const char *q = kl_guest_path(p, kp, sizeof kp);
                                               struct stat s; int r = lstat(q, &s);
                                               kl_fs_trace("lstat", p, NULL, r != 0);
                                               if (!r) stat_to_bionic(&s, b); return r; }
int klb_fstat(int fd, bionic_stat *b)        { struct stat s; int r = fstat(fd, &s);
                                               if (!r) stat_to_bionic(&s, b); return r; }
// ---------- statfs ----------
// bionic/LP64's struct statfs is 120 bytes of uint64_t; Darwin's is a different
// shape entirely (32-bit f_bsize, two embedded 1024-byte path strings). Never
// forward it.
//
// This used to answer with 120 zero bytes, which does not read as "unknown" — it
// reads as a full disk, and Unity refuses to start with "Not enough storage
// space to install required resources." A wrong number here is not a missing
// feature; it is a confidently false one.
typedef struct {
    uint64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t  f_fsid[2];
    uint64_t f_namelen, f_frsize, f_flags, f_spare[4];
} bionic_statfs;
_Static_assert(sizeof(bionic_statfs) == 120, "bionic statfs is 120 bytes on LP64");

int klb_statfs(const char *path, void *out) {
    struct statfs d;
    if (statfs(path, &d) != 0) return -1;
    if (!out) return 0;
    bionic_statfs *b = out;
    memset(b, 0, sizeof *b);
    b->f_type    = 0xEF53;          // EXT4_SUPER_MAGIC — what /data reports on a Quest
    b->f_bsize   = d.f_bsize;
    b->f_frsize  = d.f_bsize;
    b->f_blocks  = d.f_blocks;
    b->f_bfree   = d.f_bfree;
    b->f_bavail  = d.f_bavail;
    b->f_files   = d.f_files;
    b->f_ffree   = d.f_ffree;
    b->f_fsid[0] = d.f_fsid.val[0];
    b->f_fsid[1] = d.f_fsid.val[1];
    b->f_namelen = 255;
    return 0;
}

// ---------- sysconf ----------
// Trap 4 again, and it stayed hidden for a while because it fails quietly:
// bionic's _SC_* numbers are not Darwin's. _SC_NPROCESSORS_ONLN is 0x61 on
// bionic and 58 on Darwin, so a forwarded call asks a different question and
// gets -1 back. That is what made Unity print "SystemInfo CPU = ARM64,
// Cores = 0, Memory = 0mb".
//
// The guest numbers are literals from the NDK's bits/sysconf.h; the host side is
// the macro, so the compiler resolves it and a Darwin header change cannot
// silently desync the two.
static const struct { int guest, host; const char *name; } g_sysconf[] = {
    {0x0000, _SC_ARG_MAX,            "_SC_ARG_MAX"},
    {0x0005, _SC_CHILD_MAX,          "_SC_CHILD_MAX"},
    {0x0006, _SC_CLK_TCK,            "_SC_CLK_TCK"},
    {0x0009, _SC_LINE_MAX,           "_SC_LINE_MAX"},
    {0x000a, _SC_NGROUPS_MAX,        "_SC_NGROUPS_MAX"},
    {0x000b, _SC_OPEN_MAX,           "_SC_OPEN_MAX"},
    {0x001b, _SC_STREAM_MAX,         "_SC_STREAM_MAX"},
    {0x001c, _SC_TZNAME_MAX,         "_SC_TZNAME_MAX"},
    {0x0025, _SC_ATEXIT_MAX,         "_SC_ATEXIT_MAX"},
    {0x0026, _SC_IOV_MAX,            "_SC_IOV_MAX"},
    {0x0027, _SC_PAGESIZE,           "_SC_PAGESIZE"},
    {0x0028, _SC_PAGE_SIZE,          "_SC_PAGE_SIZE"},
    {0x0048, _SC_GETPW_R_SIZE_MAX,   "_SC_GETPW_R_SIZE_MAX"},
    {0x0049, _SC_LOGIN_NAME_MAX,     "_SC_LOGIN_NAME_MAX"},
    {0x004c, _SC_THREAD_STACK_MIN,   "_SC_THREAD_STACK_MIN"},
    {0x0060, _SC_NPROCESSORS_CONF,   "_SC_NPROCESSORS_CONF"},
    {0x0061, _SC_NPROCESSORS_ONLN,   "_SC_NPROCESSORS_ONLN"},
    {0x0062, _SC_PHYS_PAGES,         "_SC_PHYS_PAGES"},
    {0x0064, _SC_MONOTONIC_CLOCK,    "_SC_MONOTONIC_CLOCK"},
    {0x006f, _SC_HOST_NAME_MAX,      "_SC_HOST_NAME_MAX"},
    {0x0079, _SC_SYMLOOP_MAX,        "_SC_SYMLOOP_MAX"},
};

long klb_sysconf(int guest_name) {
    for (unsigned i = 0; i < sizeof g_sysconf / sizeof g_sysconf[0]; i++)
        if (g_sysconf[i].guest == guest_name) {
            long v = sysconf(g_sysconf[i].host);
            if (kl_env_on("KL_TRACE_SYSCONF", 0))
                fprintf(stderr, "  [libc] sysconf(%s) -> %ld\n", g_sysconf[i].name, v);
            return v;
        }
    // bionic's _SC_AVPHYS_PAGES (0x63) has no Darwin equivalent and lands here.
    // -1 is sysconf's own "no such name", which is the truthful answer; the log
    // line is what keeps it from being a silent zero.
    fprintf(stderr, "[libc] sysconf: no Darwin equivalent for bionic name 0x%x\n", guest_name);
    return -1;
}

// ---------- synthetic /proc and /sys ----------
// Unity reads its CPU and memory configuration straight out of procfs —
// /proc/cpuinfo, /proc/meminfo, /sys/devices/system/cpu/possible — and none of
// those paths exist on Darwin. The failure is silent rather than loud: every
// open returns ENOENT, Unity reports "SystemInfo CPU = ARM64, Cores = 0,
// Memory = 0mb", and then sizes its job system and memory budgets from zero.
//
// The numbers here are the HOST's, and that is a different decision from
// Build.MODEL. The Build fields lie about the device on purpose, because the
// guest's Oculus code branches on them. These are not identity — they are what
// the engine sizes thread pools and heaps from — so the useful answer is the
// machine we are actually running on.
//
// Files are materialised once into a temp directory and the guest's path is
// rewritten to point inside it. Anything under /proc we do not serve falls
// through unrewritten and still fails with ENOENT, exactly as before.
static char g_procroot[512];

static void proc_put(const char *rel, const char *text) {
    char path[1024];
    snprintf(path, sizeof path, "%s%s", g_procroot, rel);
    for (char *p = path + strlen(g_procroot) + 1; *p; p++)
        if (*p == '/') { *p = '\0'; mkdir(path, 0755); *p = '/'; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static uint64_t proc_free_bytes(void) {
    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS) return 0;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &cnt) != KERN_SUCCESS) return 0;
    return ((uint64_t)vm.free_count + vm.inactive_count) * page;
}

static void proc_build(void) {
    // TMPDIR, not /tmp: on visionOS the process is sandboxed into a container
    // and /tmp is not writable, while TMPDIR always names the container's own
    // tmp. Both platforms hand it back with a trailing slash, but appending
    // one only when it is missing costs nothing and a wrong separator here
    // would put the synthetic /proc tree somewhere the guest never finds it.
    char tmpl[512];
    const char *tmp = kl_env_str("TMPDIR", "/tmp");
    size_t n = strlen(tmp);
    snprintf(tmpl, sizeof tmpl, "%s%sklepton-proc.XXXXXX", tmp,
             tmp[n - 1] == '/' ? "" : "/");
    if (!mkdtemp(tmpl)) return;
    snprintf(g_procroot, sizeof g_procroot, "%s", tmpl);

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;

    uint64_t memtotal = 0;
    size_t   sz = sizeof memtotal;
    if (sysctlbyname("hw.memsize", &memtotal, &sz, NULL, 0) != 0) memtotal = 0;
    uint64_t memfree = proc_free_bytes();

    // aarch64 /proc/cpuinfo, in the format Linux emits and Unity parses: one
    // stanza per core keyed on "processor", then a Hardware line. The implementer
    // and part IDs are Qualcomm's, to match the Quest 2 we present elsewhere.
    char  *cpuinfo = malloc((size_t)ncpu * 512 + 256);
    size_t at = 0;
    for (long i = 0; i < ncpu; i++)
        at += (size_t)sprintf(cpuinfo + at,
            "processor\t: %ld\n"
            "BogoMIPS\t: 38.40\n"
            "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp "
            "asimdhp cpuid asimdrdm lrcpc dcpop asimddp\n"
            "CPU implementer\t: 0x51\n"
            "CPU architecture: 8\n"
            "CPU variant\t: 0xd\n"
            "CPU part\t: 0x805\n"
            "CPU revision\t: 14\n\n", i);
    sprintf(cpuinfo + at, "Hardware\t: Qualcomm Technologies, Inc SDM865\n");
    proc_put("/proc/cpuinfo", cpuinfo);
    free(cpuinfo);

    char buf[1024];
    snprintf(buf, sizeof buf,
             "MemTotal:       %8llu kB\n"
             "MemFree:        %8llu kB\n"
             "MemAvailable:   %8llu kB\n"
             "Buffers:               0 kB\n"
             "Cached:                0 kB\n"
             "SwapTotal:             0 kB\n"
             "SwapFree:              0 kB\n",
             (unsigned long long)(memtotal / 1024),
             (unsigned long long)(memfree / 1024),
             (unsigned long long)(memfree / 1024));
    proc_put("/proc/meminfo", buf);

    // "0-N" for more than one CPU, bare "0" for one — Linux's own formatting,
    // and Unity's parser depends on it.
    if (ncpu > 1) snprintf(buf, sizeof buf, "0-%ld\n", ncpu - 1);
    else          snprintf(buf, sizeof buf, "0\n");
    proc_put("/sys/devices/system/cpu/possible", buf);
    proc_put("/sys/devices/system/cpu/present",  buf);
    proc_put("/sys/devices/system/cpu/online",   buf);

    // Every core is described identically, so the guest sees a uniform SMP
    // machine rather than a big.LITTLE one. Apple silicon does have P and E
    // cores, but nothing here can tell them apart from userspace, and inventing
    // a split would be worse than declaring none.
    for (long i = 0; i < ncpu; i++) {
        char rel[256];
        snprintf(rel, sizeof rel, "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", i);
        proc_put(rel, "2840000\n");
        snprintf(rel, sizeof rel, "/sys/devices/system/cpu/cpu%ld/cpu_capacity", i);
        proc_put(rel, "1024\n");
    }
    fprintf(stderr, "[proc] synthetic /proc and /sys at %s (%ld cpus, %llu MB)\n",
            g_procroot, ncpu, (unsigned long long)(memtotal >> 20));
}

// Rewrite a guest path into the synthetic tree when we serve it. Returns `path`
// unchanged for everything else, so this is safe to put in front of every file
// entry point.
//
// One more mapping registered from kl_jni: the APK path itself. Unity mounts
// the APK as a zip for most asset reads, but its *split* assets
// (sharedassets0.assets.splitN) are read by raw open on "<apk>/assets/..." —
// concatenated onto the mount point — which is ENOTDIR on a real file, and
// silently no scene ever loads (the engine pumps frames and draws nothing,
// exactly what the "black frame" hunt measured). On device those bytes come
// from inside the zip; the unpacked tree next to the APK has the identical
// files, so map the prefix there.
static char g_apk_prefix[1024], g_apk_target[1024];

void kl_guest_path_map(const char *apk, const char *unpacked_dir) {
    snprintf(g_apk_prefix, sizeof g_apk_prefix, "%s", apk);
    snprintf(g_apk_target, sizeof g_apk_target, "%s", unpacked_dir);
}

const char *kl_guest_path(const char *path, char *buf, size_t cap) {
    if (!path || path[0] != '/') return path;
    size_t alen = strlen(g_apk_prefix);
    if (alen && strncmp(path, g_apk_prefix, alen) == 0 && path[alen] == '/') {
        snprintf(buf, cap, "%s%s", g_apk_target, path + alen);
        struct stat st;
        if (stat(buf, &st) == 0) return buf;
        // Not in the unpacked tree: fall through to the raw path, which will
        // fail the same way it always did — no invented successes.
        return path;
    }
    if (strncmp(path, "/proc/", 6) != 0 &&
        strncmp(path, "/sys/devices/system/cpu", 23) != 0) return path;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, proc_build);
    if (!g_procroot[0]) return path;
    snprintf(buf, cap, "%s%s", g_procroot, path);
    struct stat st;
    return stat(buf, &st) == 0 ? buf : path;
}

#define KL_GUEST_PATH(p) char _kp[1024]; const char *_p = kl_guest_path((p), _kp, sizeof _kp)

// ---------- file-op tracing ----------
// KL_TRACE_FS=1 reports every guest file operation and its result. The guest
// swallows its own errno and reports something several layers removed ("Not
// enough storage space to install required resources" for a failed extract), so
// the only reliable way to find which call actually failed is to watch them all.
// KL_TRACE_FS=fail narrows it to the failures.
static int kl_fs_trace_mode(void) {
    static int mode = -1;   // 0 off, 1 failures only, 2 everything
    if (mode < 0) {
        const char *e = kl_env_str("KL_TRACE_FS", NULL);
        mode = !e ? 0 : (strcmp(e, "fail") == 0 ? 1 : 2);
    }
    return mode;
}
static void kl_fs_trace(const char *op, const char *path, const char *extra, int failed) {
    if (!extra) extra = "";
    int mode = kl_fs_trace_mode();
    if (!mode || (mode == 1 && !failed)) return;
    if (failed) fprintf(stderr, "[fs] %s(\"%s\", \"%s\") FAILED: %s\n", op, path, extra, strerror(errno));
    else        fprintf(stderr, "[fs] %s(\"%s\", \"%s\") ok\n", op, path, extra);
}

void kl_file_note(void *f, const char *path);
FILE *klb_fopen(const char *path, const char *mode) {
    KL_GUEST_PATH(path);
    FILE *f = fopen(_p, mode);
    kl_fs_trace("fopen", path, mode, f == NULL);
    if (f) kl_file_note(f, path);
    // /proc/cpuinfo gets re-read ~150x/s during the loading crawl — log the
    // caller once so the polling site is an address, not a rumour.
    if (f && strstr(path, "/proc/")) {
        static int said;
        if (!said++) fprintf(stderr, "  [proc] fopen(\"%s\") from guest pc=%p\n",
                             path, __builtin_return_address(0));
    }
    return f;
}

// guest FILE* (here: a host FILE* the guest holds) -> the path it was opened
// with, so KL_TRACE_IO can say WHICH file a slow read stream belongs to.
#define KL_FILE_REGISTRY 256
static struct { void *f; const char *path; } g_file_reg[KL_FILE_REGISTRY];
static int g_file_reg_n;
void kl_file_note(void *f, const char *path) {
    if (g_file_reg_n >= KL_FILE_REGISTRY) return;
    g_file_reg[g_file_reg_n].f = f;
    g_file_reg[g_file_reg_n].path = strdup(path);
    g_file_reg_n++;
}
const char *kl_file_path(void *f) {
    for (int i = 0; i < g_file_reg_n; i++)
        if (g_file_reg[i].f == f) return g_file_reg[i].path;
    return NULL;
}
int klb_access(const char *path, int mode) {
    KL_GUEST_PATH(path);
    int r = access(_p, mode);
    kl_fs_trace("access", path, NULL, r != 0);
    return r;
}
int klb_mkdir(const char *path, mode_t mode) {
    int r = mkdir(path, mode);
    kl_fs_trace("mkdir", path, NULL, r != 0);
    return r;
}
int klb_unlink(const char *path) {
    int r = unlink(path);
    kl_fs_trace("unlink", path, NULL, r != 0);
    return r;
}
int klb_rename(const char *a, const char *b) {
    int r = rename(a, b);
    kl_fs_trace("rename", a, b, r != 0);
    return r;
}

// ---------- struct dirent ----------
typedef struct { uint64_t d_ino; int64_t d_off; uint16_t d_reclen;
                 uint8_t d_type; char d_name[256]; } bionic_dirent;

void *klb_opendir(const char *p) { return opendir(p); }
int   klb_closedir(void *d)      { return closedir((DIR *)d); }
void *klb_readdir(void *d) {
    static _Thread_local bionic_dirent out;    // matches readdir's per-stream lifetime
    struct dirent *e = readdir((DIR *)d);
    if (!e) return NULL;
    memset(&out, 0, sizeof out);
    out.d_ino = e->d_ino;
    out.d_off = 0;
    out.d_reclen = sizeof out;
    out.d_type = e->d_type;
    snprintf(out.d_name, sizeof out.d_name, "%s", e->d_name);
    return &out;
}

// ---------- sigaction ----------
// bionic/LP64 puts sa_flags FIRST, ahead of the handler union. That is specific
// to __LP64__; the 32-bit layout does lead with the handler, which is what this
// struct used to assume. The cost of getting it backwards was invisible for a
// long time: `handler` read the flags word instead, so every guest handler was
// installed at address 0x18000004 — which is not an address, it is Linux's
// SA_RESTART|SA_ONSTACK|SA_SIGINFO. Boehm's GC suspend handler among them.
//
//   bionic/LP64: { int flags; <pad>; handler; unsigned long mask; restorer; } 32B
//   Darwin     : { handler; sigset_t mask; int flags; }                       16B
typedef struct { int32_t flags; int32_t _pad; void *handler;
                 uint64_t mask; void *restorer; } bionic_sigaction;

// Same names, different numbers — trap 4 again, and these are far apart.
#define KL_SA_NOCLDSTOP 0x00000001u
#define KL_SA_NOCLDWAIT 0x00000002u
#define KL_SA_SIGINFO   0x00000004u
#define KL_SA_ONSTACK   0x08000000u
#define KL_SA_RESTART   0x10000000u
#define KL_SA_NODEFER   0x40000000u
#define KL_SA_RESETHAND 0x80000000u

static int kl_sa_flags(uint32_t linux_flags) {
    int f = 0;
    if (linux_flags & KL_SA_NOCLDSTOP) f |= SA_NOCLDSTOP;
    if (linux_flags & KL_SA_NOCLDWAIT) f |= SA_NOCLDWAIT;
    if (linux_flags & KL_SA_SIGINFO)   f |= SA_SIGINFO;
    if (linux_flags & KL_SA_ONSTACK)   f |= SA_ONSTACK;
    if (linux_flags & KL_SA_RESTART)   f |= SA_RESTART;
    if (linux_flags & KL_SA_NODEFER)   f |= SA_NODEFER;
    if (linux_flags & KL_SA_RESETHAND) f |= SA_RESETHAND;
    return f;
}
static uint32_t kl_sa_flags_back(int darwin_flags) {
    uint32_t f = 0;
    if (darwin_flags & SA_NOCLDSTOP) f |= KL_SA_NOCLDSTOP;
    if (darwin_flags & SA_NOCLDWAIT) f |= KL_SA_NOCLDWAIT;
    if (darwin_flags & SA_SIGINFO)   f |= KL_SA_SIGINFO;
    if (darwin_flags & SA_ONSTACK)   f |= KL_SA_ONSTACK;
    if (darwin_flags & SA_RESTART)   f |= KL_SA_RESTART;
    if (darwin_flags & SA_NODEFER)   f |= KL_SA_NODEFER;
    if (darwin_flags & SA_RESETHAND) f |= KL_SA_RESETHAND;
    return f;
}

// A guest handler for a *fatal* signal cannot work here, and that is structural
// rather than a bug we might fix: it expects a Linux ucontext_t, and Darwin's
// _sigtramp hands it a __darwin_mcontext64 instead. IL2CPP's handler then walks
// a garbage context and hangs rather than dying — the process wedges inside
// _sigtramp with a bogus PC, and the actual fault is never reported. Installing
// such a handler therefore buys nothing and costs every crash its diagnosis, so
// it is refused and SIG_DFL is left in place. KL_GUEST_SIGNALS=1 installs it
// anyway, for the case where the question is what the guest handler does.
static int klb_fatal_signal(int sig) {
    switch (sig) {
    case SIGSEGV: case SIGBUS: case SIGILL: case SIGFPE:
    case SIGABRT: case SIGTRAP: case SIGSYS:
        return 1;
    default:
        return 0;
    }
}

int klb_sigaction(int sig, const bionic_sigaction *in, bionic_sigaction *old) {
    struct sigaction d, o;
    if (kl_env_on("KL_TRACE_SIG", 0))
        fprintf(stderr, "  [sig] sigaction(%d, handler=%p)\n", sig,
                in ? in->handler : NULL);
    static int allow_guest = -1;
    if (allow_guest < 0) allow_guest = kl_env_on("KL_GUEST_SIGNALS", 0);
    if (in && in->handler && in->handler != (void *)SIG_DFL &&
        in->handler != (void *)SIG_IGN && klb_fatal_signal(sig) && !allow_guest) {
        fprintf(stderr, "[sig] refusing the guest's handler for signal %d "
                        "(Linux ucontext; set KL_GUEST_SIGNALS=1 to install it)\n", sig);
        if (old) { memset(old, 0, sizeof *old); }
        return 0;                    // the guest believes it is installed
    }
    if (in) {
        memset(&d, 0, sizeof d);
        d.sa_handler = (void (*)(int))in->handler;
        d.sa_flags   = kl_sa_flags((uint32_t)in->flags);
        d.sa_mask    = (sigset_t)(in->mask & 0xFFFFFFFFu);
    }
    int r = sigaction(sig, in ? &d : NULL, old ? &o : NULL);
    if (!r && old) {
        memset(old, 0, sizeof *old);
        old->handler = (void *)o.sa_handler;
        old->flags   = (int32_t)kl_sa_flags_back(o.sa_flags);
        old->mask    = o.sa_mask;
    }
    return r;
}

// ---------- uname ----------
// bionic fields are 65 bytes each; Darwin's are 256. Never forward the struct.
typedef struct { char sysname[65], nodename[65], release[65],
                      version[65], machine[65], domainname[65]; } bionic_utsname;
int klb_uname(bionic_utsname *u) {
    memset(u, 0, sizeof *u);
    snprintf(u->sysname,  65, "Linux");
    snprintf(u->nodename, 65, "localhost");
    snprintf(u->release,  65, "4.14.0-klepton");
    snprintf(u->version,  65, "#1 SMP klepton");
    snprintf(u->machine,  65, "aarch64");
    return 0;
}

// ---------- misc ----------
int    klb_FD_ISSET_chk(int fd, void *set) { return FD_ISSET(fd, (fd_set *)set); }
void   klb_FD_SET_chk(int fd, void *set)   { FD_SET(fd, (fd_set *)set); }
size_t klb_ctype_mb_cur_max(void)          { return MB_CUR_MAX; }
off_t  klb_lseek64(int fd, off_t o, int w) { return lseek(fd, o, w); }
void  *klb_getpwuid(unsigned uid)          { (void)uid; warn_once("getpwuid"); return NULL; }
int    klb_getpwuid_r(unsigned uid, void *pw, char *buf, size_t n, void **res) {
    (void)uid; (void)pw; (void)buf; (void)n; if (res) *res = NULL;
    warn_once("getpwuid_r"); return 0;
}
int    klb_execl(const char *p, const char *a, ...) { (void)p; (void)a;
                                                      warn_once("execl"); errno = ENOSYS; return -1; }
// system(): hand-written rather than forwarded, because visionOS marks it
// __API_UNAVAILABLE — a generated KL_FWD does not compile for xrOS at all,
// which is how the Steam Link libc sweep broke `make xros`. Refusing is also
// the right answer on every platform: there is no shell to run a command in
// under AMFI, and execl already answers ENOSYS for the same reason. 127 is
// what a real system() returns when the shell cannot execute the command.
int    klb_system(const char *cmd) { (void)cmd; warn_once("system"); return 127; }
// ---------- raw syscalls ----------
// Note the signature: fixed arity, not `...`. AAPCS64 passes variadic arguments
// in x0-x7, which is exactly where a fixed-arity Darwin function reads its own,
// so up to eight arguments arrive correctly without a thunk. Declaring this
// variadic on Darwin would send it looking on the stack instead — trap 2.
//
// Almost every raw syscall is refused, but futex is not optional. Unity's
// Baselib builds its locks, semaphores and events on futex directly rather than
// through pthreads, so refusing it fails silently rather than loudly: the waiter
// gets ENOSYS back and spins. One run refused 13,846 of them.
//
// The emulation is a bucket table of condition variables keyed on the futex
// address. A wake broadcasts its bucket, so addresses that hash together produce
// spurious wakeups — which is allowed, because futex explicitly permits them and
// every correct user rechecks its own predicate. Correctness comes from the
// waiter holding the bucket lock across the compare-and-sleep while the waker
// takes that same lock: the futex contract stores the new value before waking,
// so a wakeup cannot slip through the gap.
#define KL_SYS_futex 98                 // aarch64 Linux

enum {
    KL_FUTEX_WAIT = 0, KL_FUTEX_WAKE = 1, KL_FUTEX_WAKE_OP = 5,
    KL_FUTEX_WAIT_BITSET = 9, KL_FUTEX_WAKE_BITSET = 10,
    KL_FUTEX_PRIVATE_FLAG = 128, KL_FUTEX_CLOCK_REALTIME = 256,
};

// FUTEX_WAKE_OP's `val3` packs a whole little instruction:
//   op(4) | cmp(4) | oparg(12) | cmparg(12)
// and the kernel performs `*uaddr2 = *uaddr2 OP oparg` atomically, wakes
// `val` waiters on uaddr, and — if the value uaddr2 held BEFORE the operation
// satisfies `oldval CMP cmparg` — wakes `val2` more on uaddr2.
enum {
    KL_FUTEX_OP_SET = 0, KL_FUTEX_OP_ADD = 1, KL_FUTEX_OP_OR = 2,
    KL_FUTEX_OP_ANDN = 3, KL_FUTEX_OP_XOR = 4,
    KL_FUTEX_OP_OPARG_SHIFT = 8,        // OR'd into op: oparg means (1 << oparg)
    KL_FUTEX_OP_CMP_EQ = 0, KL_FUTEX_OP_CMP_NE = 1, KL_FUTEX_OP_CMP_LT = 2,
    KL_FUTEX_OP_CMP_LE = 3, KL_FUTEX_OP_CMP_GT = 4, KL_FUTEX_OP_CMP_GE = 5,
};

#define KL_FUTEX_BUCKETS 256
static struct { pthread_mutex_t m; pthread_cond_t c; } g_futex[KL_FUTEX_BUCKETS];

static void futex_init(void) {
    for (int i = 0; i < KL_FUTEX_BUCKETS; i++) {
        pthread_mutex_init(&g_futex[i].m, NULL);
        pthread_cond_init(&g_futex[i].c, NULL);
    }
}
static unsigned futex_bucket(const void *a) {
    uint64_t v = (uint64_t)(uintptr_t)a >> 2;
    return (unsigned)((v * 0x9E3779B97F4A7C15ULL) >> 56) & (KL_FUTEX_BUCKETS - 1);
}
static void ts_normalise(struct timespec *t) {
    while (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
    while (t->tv_nsec < 0)            { t->tv_nsec += 1000000000L; t->tv_sec--; }
}

// KL_TRACE_TIME=1: log the guest-visible answers of the wall-clock family.
// Diagnostic for the class-init deadlock: the abstime the guest feeds
// pthread_cond_timedwait lands ~9.5 hours out, so either its clock source is
// wrong going in (this trace) or its arithmetic corrupts it after (disasm).
static int t_time(void) { static int t = -1; if (t < 0) t = kl_env_on("KL_TRACE_TIME", 0); return t; }

// bionic clock ids differ from Darwin's; forwarding them verbatim makes
// clock_gettime(CLOCK_MONOTONIC=1) fail EINVAL, and libunity reads the
// (unwritten) output buffer anyway — poisoning every monotonic-time consumer.
//   bionic: 0 REALTIME 1 MONOTONIC 2 PROCESS_CPUTIME 3 THREAD_CPUTIME
//           4 MONOTONIC_RAW 5 REALTIME_COARSE 6 MONOTONIC_COARSE 7 BOOTTIME
//           8 REALTIME_ALARM 9 BOOTTIME_ALARM
//   Darwin: 0 REALTIME 4 MONOTONIC_RAW 6 MONOTONIC 8 UPTIME_RAW
//           12 PROCESS_CPUTIME_ID
static int kl_clock_to_darwin(int clk) {
    switch (clk) {
    case 0: case 5: case 8: return 0;    // realtime (coarse/alarm degrade to it)
    case 1: case 6:         return 6;    // monotonic (coarse degrades)
    case 2: case 3:         return 12;   // cpu-time ids degrade to process
    case 4:                 return 4;    // monotonic raw
    case 7: case 9:         return 8;    // boottime ~ uptime raw
    }
    return -1;
}

int klb_clock_getres(int clk, struct timespec *ts) {
    int dclk = kl_clock_to_darwin(clk);
    return dclk < 0 ? (errno = EINVAL, -1) : clock_getres(dclk, ts);
}

int klb_clock_gettime(int clk, struct timespec *ts) {
    int dclk = kl_clock_to_darwin(clk);
    int r = dclk < 0 ? (errno = EINVAL, -1) : clock_gettime(dclk, ts);
    if (t_time()) {
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 40)
            fprintf(stderr, "  [klb] clock_gettime(%d->%d) ra=%p -> %d: %lld.%09ld\n",
                    clk, dclk, __builtin_return_address(0), r,
                    (long long)ts->tv_sec, ts->tv_nsec);
    }
    return r;
}

// bionic struct timeval is { int64 sec; int64 usec }; Darwin's tv_usec is a
// 32-bit suseconds_t. Forwarding the guest's buffer straight to gettimeofday
// leaves the top half of its tv_usec as stack garbage — observed at il2cpp's
// gettimeofday-based condvar wait (libil2cpp+0x1273398), whose computed
// abstime read ~10 h out with tv_nsec > 1e9 because the stale half-word
// happened to hold a monotonic-ms value. Write both fields with bionic's
// 64-bit layout.
struct klb_timeval { int64_t tv_sec; int64_t tv_usec; };
int klb_gettimeofday(struct klb_timeval *tv, void *tz) {
    struct timeval h;
    int r = gettimeofday(&h, tz);
    if (tv) { tv->tv_sec = h.tv_sec; tv->tv_usec = h.tv_usec; }
    if (t_time() && tv) {
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 40)
            fprintf(stderr, "  [klb] gettimeofday ra=%p -> %d: %lld.%06lld\n",
                    __builtin_return_address(0), r,
                    (long long)tv->tv_sec, (long long)tv->tv_usec);
    }
    return r;
}


// The tail three arguments are overloaded by op, exactly as the kernel
// overloads them: `ts` is a timeout for the WAIT family and the second wake
// count (val2) for WAKE_OP, and uaddr2/val3 are only read by WAKE_OP.
static long kl_futex_impl(int32_t *uaddr, int op, uint32_t val, const struct timespec *ts,
                          int32_t *uaddr2, uint32_t val3);
static long kl_futex(int32_t *uaddr, int op, uint32_t val, const struct timespec *ts,
                     int32_t *uaddr2, uint32_t val3) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, futex_init);
    // KL_TRACE_FUTEX=1: once a second, waits (split by timeout expiry vs
    // wakeup) and wakes. The async loader's pace smells like a wake that
    // never arrives, every iteration paying the full timeout.
    static int trace = -1;
    if (trace < 0) trace = kl_env_on("KL_TRACE_FUTEX", 0);
    if (trace) {
        static _Atomic unsigned long long waits, timeouts, woken, wakes;
        static _Atomic time_t last;
        int is_wait = (op & 0xf) == 0 || (op & 0xf) == 9;   // WAIT / WAIT_BITSET base
        unsigned long long w  = atomic_fetch_add(&waits, is_wait) + is_wait;
        unsigned long long wk = atomic_fetch_add(&wakes, !is_wait) + !is_wait;
        (void)wk;
        time_t now = time(NULL), prev = atomic_load(&last);
        if (now != prev && atomic_compare_exchange_strong(&last, &prev, now))
            fprintf(stderr, "  [futex] waits=%llu (timeout=%llu woken=%llu) wakes=%llu\n",
                    w, (unsigned long long)atomic_load(&timeouts),
                    (unsigned long long)atomic_load(&woken), wk);
        // Result accounting happens on the way out below via these two.
        long r = kl_futex_impl(uaddr, op, val, ts, uaddr2, val3);
        if (is_wait) {
            if (r == 0) atomic_fetch_add(&woken, 1);
            else if (errno == ETIMEDOUT) atomic_fetch_add(&timeouts, 1);
        }
        return r;
    }
    return kl_futex_impl(uaddr, op, val, ts, uaddr2, val3);
}

// The read-modify-write half of FUTEX_WAKE_OP, returning the value uaddr2 held
// BEFORE it — which is what the comparison is made against. Every case is a
// single atomic so it composes with the guest's own atomics on the same word;
// a load/store pair would lose a concurrent release.
static int32_t futex_op_apply(int opcode, int32_t *uaddr2, int32_t oparg) {
    switch (opcode) {
    case KL_FUTEX_OP_SET:  return __atomic_exchange_n(uaddr2, oparg, __ATOMIC_SEQ_CST);
    case KL_FUTEX_OP_ADD:  return __atomic_fetch_add(uaddr2, oparg, __ATOMIC_SEQ_CST);
    case KL_FUTEX_OP_OR:   return __atomic_fetch_or(uaddr2, oparg, __ATOMIC_SEQ_CST);
    case KL_FUTEX_OP_ANDN: return __atomic_fetch_and(uaddr2, ~oparg, __ATOMIC_SEQ_CST);
    case KL_FUTEX_OP_XOR:  return __atomic_fetch_xor(uaddr2, oparg, __ATOMIC_SEQ_CST);
    }
    return 0;
}

static int futex_op_cmp(int cmp, int32_t oldval, int32_t cmparg) {
    switch (cmp) {
    case KL_FUTEX_OP_CMP_EQ: return oldval == cmparg;
    case KL_FUTEX_OP_CMP_NE: return oldval != cmparg;
    case KL_FUTEX_OP_CMP_LT: return oldval <  cmparg;
    case KL_FUTEX_OP_CMP_LE: return oldval <= cmparg;
    case KL_FUTEX_OP_CMP_GT: return oldval >  cmparg;
    case KL_FUTEX_OP_CMP_GE: return oldval >= cmparg;
    }
    return 0;
}

static long kl_futex_impl(int32_t *uaddr, int op, uint32_t val, const struct timespec *ts,
                          int32_t *uaddr2, uint32_t val3) {
    int      base = op & ~(KL_FUTEX_PRIVATE_FLAG | KL_FUTEX_CLOCK_REALTIME);
    unsigned b    = futex_bucket(uaddr);

    switch (base) {
    case KL_FUTEX_WAIT:
    case KL_FUTEX_WAIT_BITSET: {
        pthread_mutex_lock(&g_futex[b].m);
        if ((uint32_t)__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val) {
            pthread_mutex_unlock(&g_futex[b].m);
            errno = EAGAIN;
            return -1;
        }
        int r;
        if (!ts) {
            r = pthread_cond_wait(&g_futex[b].c, &g_futex[b].m);
        } else {
            // FUTEX_WAIT's timeout is relative; FUTEX_WAIT_BITSET's is an
            // absolute deadline, on CLOCK_MONOTONIC unless FUTEX_CLOCK_REALTIME
            // is set. pthread_cond_timedwait only speaks CLOCK_REALTIME, so a
            // monotonic deadline is carried across as the interval that remains.
            struct timespec now, abs;
            clock_gettime(CLOCK_REALTIME, &now);
            if (base == KL_FUTEX_WAIT) {
                abs.tv_sec  = now.tv_sec  + ts->tv_sec;
                abs.tv_nsec = now.tv_nsec + ts->tv_nsec;
            } else if (op & KL_FUTEX_CLOCK_REALTIME) {
                abs = *ts;
            } else {
                struct timespec mono;
                clock_gettime(CLOCK_MONOTONIC, &mono);
                abs.tv_sec  = now.tv_sec  + (ts->tv_sec  - mono.tv_sec);
                abs.tv_nsec = now.tv_nsec + (ts->tv_nsec - mono.tv_nsec);
            }
            ts_normalise(&abs);
            r = pthread_cond_timedwait(&g_futex[b].c, &g_futex[b].m, &abs);
        }
        pthread_mutex_unlock(&g_futex[b].m);
        if (r == ETIMEDOUT) { errno = ETIMEDOUT; return -1; }
        return 0;
    }
    case KL_FUTEX_WAKE:
    case KL_FUTEX_WAKE_BITSET:
        pthread_mutex_lock(&g_futex[b].m);
        pthread_cond_broadcast(&g_futex[b].c);
        pthread_mutex_unlock(&g_futex[b].m);
        // A broadcast makes the true count unobservable. Every caller uses this
        // only as "at least one was woken", so the requested count is returned.
        return (long)val;

    // "Modify a second word, wake on the first, and conditionally wake on the
    // second too" — one syscall so the release side of a semaphore needs no
    // lock. Qt's QSemaphore::release() is built on it, and refusing it is a
    // wake that never arrives: libshell's 2D frontend deadlocked in
    // QImage::smoothScaled (Qt parallelises the scale across QThreadPool and
    // joins on a QSemaphore) while constructing its very first panel. Nothing
    // in the hang mentions futex — see the trap record.
    case KL_FUTEX_WAKE_OP: {
        if (!uaddr2) { errno = EFAULT; return -1; }
        // `ts` is not a pointer for this op — the kernel reuses the timeout
        // slot as the second wake count.
        uint32_t val2   = (uint32_t)(uintptr_t)ts;
        int      opcode = (int)((val3 >> 28) & 0xf);
        int      cmp    = (int)((val3 >> 24) & 0xf);
        int32_t  oparg  = (int32_t)((val3 >> 12) & 0xfff);
        int32_t  cmparg = (int32_t)(val3 & 0xfff);
        if (opcode & KL_FUTEX_OP_OPARG_SHIFT) {
            opcode &= ~KL_FUTEX_OP_OPARG_SHIFT;
            oparg = (int32_t)(1u << (oparg & 31));
        }
        unsigned b2 = futex_bucket(uaddr2);

        // The RMW and the wake of uaddr2's own waiters share one critical
        // section, because a waiter on uaddr2 compares and sleeps holding this
        // same lock — that is what closes the gap the futex contract otherwise
        // leaves. Broadcasting unconditionally (rather than only when the
        // comparison holds) costs a recheck in threads that then sleep again,
        // and futex explicitly permits spurious wakeups.
        pthread_mutex_lock(&g_futex[b2].m);
        int32_t oldval = futex_op_apply(opcode, uaddr2, oparg);
        pthread_cond_broadcast(&g_futex[b2].c);
        pthread_mutex_unlock(&g_futex[b2].m);

        if (b != b2) {
            pthread_mutex_lock(&g_futex[b].m);
            pthread_cond_broadcast(&g_futex[b].c);
            pthread_mutex_unlock(&g_futex[b].m);
        }
        return (long)val + (futex_op_cmp(cmp, oldval, cmparg) ? (long)val2 : 0);
    }
    default: {
        // Trap 6d, in the one place it is most expensive: a futex op we do not
        // implement is a WAKE that never arrives, and ENOSYS is returned to a
        // caller that has already decided a waiter needs waking. Nothing near
        // the hang mentions futex. Name the op, once each.
        static _Atomic unsigned seen;
        unsigned bit = 1u << (base & 31);
        if (!(atomic_fetch_or(&seen, bit) & bit))
            fprintf(stderr, "  [futex] unimplemented op %d (raw 0x%x) — refused; "
                            "a waiter on %p will not be woken\n", base, op, (void *)uaddr);
        errno = ENOSYS;
        return -1;
    }
    }
}

static void syscall_warn_once(long n) {
    static long seen[32]; static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == n) return;
    if (nseen < 32) seen[nseen++] = n;
    fprintf(stderr, "  [klepton] guest raw syscall(%ld) — refusing (logged once)\n", n);
}

long klb_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    // futex is the only syscall here that reads past the fourth argument, and
    // the two it reads used to be discarded — which made FUTEX_WAKE_OP
    // impossible to implement rather than merely unimplemented.
    if (n == KL_SYS_futex)
        return kl_futex((int32_t *)(uintptr_t)a1, (int)a2, (uint32_t)a3,
                        (const struct timespec *)(uintptr_t)a4,
                        (int32_t *)(uintptr_t)a5, (uint32_t)a6);
    syscall_warn_once(n);
    errno = ENOSYS;
    return -1;
}
int    klb_swprintf(wchar_t *s, size_t n, const wchar_t *f, ...) {
    (void)f; warn_once("swprintf (wide format strings unsupported)");
    if (s && n) s[0] = 0; return 0;
}

// ---------- guest va_list consumers ----------
int klb_vprintf(const char *fmt, void *gva) {
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, (kl_va *)gva, m, sizeof m, KL_VA_PRINTF) == (size_t)-1) return -1;
    return vprintf(fmt, (va_list)m);
}
int klb_vsscanf(const char *s, const char *fmt, void *gva) {
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, (kl_va *)gva, m, sizeof m, KL_VA_SCANF) == (size_t)-1) return -1;
    return vsscanf(s, fmt, (va_list)m);
}

// ---------- long double ----------
// Linux/aarch64 long double is IEEE binary128 returned in q0; Darwin's is a plain
// 64-bit double. klv_strtold (kl_va_thunks.S) calls strtod then this widener.
__uint128_t kl_f64_to_f128(double d) {
    uint64_t b; memcpy(&b, &d, 8);
    uint64_t sign = b >> 63;
    int64_t  exp  = (int64_t)((b >> 52) & 0x7FF);
    uint64_t man  = b & 0xFFFFFFFFFFFFFull;
    uint64_t hi, lo;
    if (exp == 0 && man == 0) { hi = sign << 63; lo = 0; }            // +-0
    else if (exp == 0x7FF)    { hi = (sign << 63) | 0x7FFFull << 48 | (man >> 4); lo = man << 60; }
    else {
        int64_t e128 = exp - 1023 + 16383;
        hi = (sign << 63) | ((uint64_t)e128 << 48) | (man >> 4);
        lo = man << 60;
    }
    return ((__uint128_t)hi << 64) | lo;
}

// ---------- not present on Darwin ----------
void *klb_memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) if (p[n] == (unsigned char)c) return (void *)(p + n);
    return NULL;
}
void *klb_memalign(size_t align, size_t size) {
    void *p = NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    if (align & (align - 1)) return NULL;             // must be a power of two
    return posix_memalign(&p, align, size) == 0 ? p : NULL;
}

// ---------- flag-value translation ----------
// Same names, different numbers. MAP_ANONYMOUS is 0x20 on Linux and 0x1000 on
// Darwin, so an untranslated guest anonymous mmap becomes a file mapping of fd -1
// and fails -- which is exactly how IL2CPP's Boehm GC died at startup.
#define LX_MAP_SHARED 0x01
#define LX_MAP_PRIVATE 0x02
#define LX_MAP_FIXED 0x10
#define LX_MAP_ANONYMOUS 0x20
#define LX_MAP_NORESERVE 0x4000

int kl_mmap_flags(int lx) {
    int d = 0;
    if (lx & LX_MAP_SHARED)     d |= MAP_SHARED;
    if (lx & LX_MAP_PRIVATE)    d |= MAP_PRIVATE;
    if (lx & LX_MAP_FIXED)      d |= MAP_FIXED;
    if (lx & LX_MAP_ANONYMOUS)  d |= MAP_ANON;
    if (lx & LX_MAP_NORESERVE)  d |= MAP_NORESERVE;
    return d;
}
// ---- executable anonymous memory: a guest JIT, and why it must fail LOUDLY --
//
// Trap 12 says AMFI kills any pc not backed by a signed file. What that means
// for a guest that generates code is worse than "the JIT does not work": the
// mapping succeeds, the writes succeed, `__clear_cache` succeeds, and the
// process is killed on the first BRANCH into it — `EXC_BAD_ACCESS /
// KERN_PROTECTION_FAILURE`, termination namespace CODESIGNING, "Invalid Page".
// That is a SIGKILL, so no handler of ours reports it and the log simply stops.
// SL-19 spent a device run and two crash reports to find one of these
// (PCRE2's JIT inside libQt6Core, reached through QRegularExpression).
//
// Refusing the request is strictly better, because a JIT that cannot get
// executable memory is a case its authors already handle — PCRE2 falls back to
// its interpreter, and so do most others. So an anonymous PROT_EXEC mapping
// fails with EPERM and says so ONCE, by name, which is the difference between a
// fallback and a corpse.
//
// File-backed PROT_EXEC is untouched: that is a real signed image and it is how
// dlopen works. KL_GUEST_JIT=1 restores the old behaviour for a host run, where
// there is no AMFI and a JIT works fine — and where this would otherwise make
// the host quietly slower than the device is honest about.
static int guest_jit_allowed(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GUEST_JIT", 0);
    return on;
}

static int klb_refuse_exec(const char *what, int anon, int prot) {
    if (!anon || !(prot & PROT_EXEC) || guest_jit_allowed()) return 0;
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [klepton] refusing the guest's %s of anonymous "
                        "PROT_EXEC memory — this platform will not execute an "
                        "unsigned page (trap 12), and a mapping that succeeds "
                        "here is a SIGKILL on the first branch into it. A JIT "
                        "that is told no takes its interpreter; one that is not "
                        "takes the process down. KL_GUEST_JIT=1 to allow it.\n",
                what);
    }
    errno = EPERM;
    return 1;
}

void *klb_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    int df = kl_mmap_flags(flags);
    if (df & MAP_ANON) fd = -1;                  // Darwin insists on -1 for anonymous
    if (klb_refuse_exec("mmap", (df & MAP_ANON) != 0, prot)) return MAP_FAILED;
    return mmap(addr, len, prot, df, fd, off);   // PROT_* values match on both sides
}

// ...and the other half, which is how a JIT usually asks: map RW, write, then
// mprotect to RX. Refusing only mmap would let that one through.
//
// Unconditional on PROT_EXEC, and that is narrower than it looks: this entry
// point serves the GUEST only. Our own loader makes guest text executable by
// calling mprotect() directly (kl_image.c), and dlopen is dyld's business, so
// nothing that legitimately maps a signed image comes through here. A guest
// asking to make memory executable is a guest generating code.
int klb_mprotect(void *addr, size_t len, int prot) {
    if (klb_refuse_exec("mprotect", 1, prot)) return -1;
    return mprotect(addr, len, prot);
}

#define LX_O_CREAT 0x40
#define LX_O_EXCL 0x80
#define LX_O_NOCTTY 0x100
#define LX_O_TRUNC 0x200
#define LX_O_APPEND 0x400
#define LX_O_NONBLOCK 0x800
#define LX_O_DIRECTORY 0x4000
#define LX_O_NOFOLLOW 0x8000
#define LX_O_CLOEXEC 0x80000

int kl_open_flags(int lx) {
    int d = lx & 0x3;                            // O_RDONLY/WRONLY/RDWR agree
    if (lx & LX_O_CREAT)     d |= O_CREAT;
    if (lx & LX_O_EXCL)      d |= O_EXCL;
    if (lx & LX_O_NOCTTY)    d |= O_NOCTTY;
    if (lx & LX_O_TRUNC)     d |= O_TRUNC;
    if (lx & LX_O_APPEND)    d |= O_APPEND;
    if (lx & LX_O_NONBLOCK)  d |= O_NONBLOCK;
    if (lx & LX_O_DIRECTORY) d |= O_DIRECTORY;
    if (lx & LX_O_NOFOLLOW)  d |= O_NOFOLLOW;
    if (lx & LX_O_CLOEXEC)   d |= O_CLOEXEC;
    return d;
}
// ---- fcntl / ioctl: trap 4, in the two calls that had been left raw ----
//
// open() has translated its flags since M2. fcntl() did not, and it is the SAME
// flag words: F_SETFL takes exactly what open() takes. The guest passes Linux's
// O_NONBLOCK (0x800); on Darwin 0x800 is O_EXCL. So
//   fcntl(fd, F_SETFL, O_NONBLOCK)
// SUCCEEDED, returned 0, set a meaningless bit, and left the socket BLOCKING.
//
// Measured cost: Steam Link's 2D shell hung with `main` stuck in
// CDiscoverySocket::BReceive -> recvfrom, four frames under
// CShellApplication::BInit, waiting forever for a LAN datagram that a
// non-blocking read would have declined in a microsecond. Nothing errored, and
// the failure looked like "the app has no UI" rather than "a flag was wrong".
// Same shape as trap 6d and trap 16b: a wrong number that SUCCEEDS meaning
// something else.
//
// The reverse direction matters too — F_GETFL's RESULT is a flag word the guest
// will read with Linux's numbering, and the read-modify-write
// `fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)` is the common idiom, so
// getting only one side right corrupts the other flags.
// Unlike warn_once() this keys on the NUMBER, because the interesting thing
// about an unrecognised command is which one it was.
static void fcntl_warn_once(const char *fn, long cmd) {
    static struct { const char *fn; long cmd; } seen[32]; static int n = 0;
    for (int i = 0; i < n; i++) if (seen[i].fn == fn && seen[i].cmd == cmd) return;
    if (n < 32) seen[n++] = (typeof(seen[0])){fn, cmd};
    fprintf(stderr, "  [klepton] %s: unrecognised Linux request 0x%lx — refused "
                    "(add a translation rather than passing it through)\n", fn, cmd);
}

static int open_flags_to_linux(int d) {
    int lx = d & 0x3;
    if (d & O_CREAT)     lx |= LX_O_CREAT;
    if (d & O_EXCL)      lx |= LX_O_EXCL;
    if (d & O_NOCTTY)    lx |= LX_O_NOCTTY;
    if (d & O_TRUNC)     lx |= LX_O_TRUNC;
    if (d & O_APPEND)    lx |= LX_O_APPEND;
    if (d & O_NONBLOCK)  lx |= LX_O_NONBLOCK;
    if (d & O_DIRECTORY) lx |= LX_O_DIRECTORY;
    if (d & O_NOFOLLOW)  lx |= LX_O_NOFOLLOW;
    if (d & O_CLOEXEC)   lx |= LX_O_CLOEXEC;
    return lx;
}

// The command numbers diverge too, and not uniformly: the first five agree and
// then Linux and Darwin swap the {GETOWN,SETOWN} pair against the lock family.
#define LX_F_DUPFD 0
#define LX_F_GETFD 1
#define LX_F_SETFD 2
#define LX_F_GETFL 3
#define LX_F_SETFL 4
#define LX_F_GETLK 5
#define LX_F_SETLK 6
#define LX_F_SETLKW 7
#define LX_F_SETOWN 8
#define LX_F_GETOWN 9
#define LX_F_DUPFD_CLOEXEC 1030

int kl_fcntl(int fd, int cmd, uintptr_t arg) {
    switch (cmd) {
    case LX_F_SETFL:
        // The only translated-argument case, and the one that bit.
        return fcntl(fd, F_SETFL, kl_open_flags((int)arg));
    case LX_F_GETFL: {
        int d = fcntl(fd, F_GETFL);
        return d < 0 ? d : open_flags_to_linux(d);
    }
    // FD_CLOEXEC is 1 on both, and these three take no flag word we own.
    case LX_F_DUPFD:         return fcntl(fd, F_DUPFD, (int)arg);
    case LX_F_GETFD:         return fcntl(fd, F_GETFD);
    case LX_F_SETFD:         return fcntl(fd, F_SETFD, (int)arg);
    case LX_F_DUPFD_CLOEXEC: return fcntl(fd, F_DUPFD_CLOEXEC, (int)arg);
    case LX_F_SETOWN:        return fcntl(fd, F_SETOWN, (int)arg);
    case LX_F_GETOWN:        return fcntl(fd, F_GETOWN);
    // struct flock is laid out differently on the two platforms (Linux leads
    // with l_type/l_whence, Darwin with l_start/l_len), so forwarding the
    // pointer would silently lock a different range. Trap 7's class. Refuse by
    // name until something actually needs it — no guest has yet.
    case LX_F_GETLK: case LX_F_SETLK: case LX_F_SETLKW:
        warn_once("fcntl file locking (struct flock layout differs) — refused");
        errno = EINVAL;
        return -1;
    default:
        fcntl_warn_once("fcntl", cmd);
        errno = EINVAL;
        return -1;
    }
}

// Same class of bug, same fix. Only the two request codes any guest here has
// used are translated; anything else is reported rather than passed through,
// because an untranslated ioctl request is not a near miss — the encoding packs
// direction and size, so a wrong code addresses a different driver operation.
#define LX_FIONREAD 0x541B
#define LX_FIONBIO  0x5421

int kl_ioctl(int fd, unsigned long req, uintptr_t arg) {
    switch (req) {
    case LX_FIONREAD: return ioctl(fd, FIONREAD, (void *)arg);
    case LX_FIONBIO:  return ioctl(fd, FIONBIO,  (void *)arg);
    default:
        fcntl_warn_once("ioctl", (long)req);
        errno = ENOTTY;
        return -1;
    }
}

int klb_madvise(void *a, size_t n, int advice) {
    if (advice == 8) advice = MADV_FREE;         // Linux MADV_FREE
    return madvise(a, n, advice);
}

// open() lives behind a variadic thunk in kl_va_handlers.c, so its trace call
// comes back here rather than duplicating the mode lookup there.
void kl_fs_trace_open(const char *path, int flags, int fd) {
    (void)flags;
    kl_fs_trace("open", path, NULL, fd < 0);
}
