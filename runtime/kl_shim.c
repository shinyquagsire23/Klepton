// bionic -> Darwin symbol table.
//
// Three tiers:
//   1. kl_libc_table.h  — generated direct forwards (signature + layout both match)
//   2. klb_* / klv_*    — hand-written: divergent layouts (kl_libc.c, kl_pthread.c,
//                          kl_dl.c) and AAPCS64 variadic thunks (kl_va_thunks.S)
//   3. local statics    — small bionic-isms defined right here
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <setjmp.h>
#include <locale.h>
#include <xlocale.h>
#include <libgen.h>
#include <pthread.h>
#include <sched.h>
#include <poll.h>
#include <termios.h>
#include <utime.h>
#include <pwd.h>
#include <zlib.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <net/if.h>
#include "klepton.h"
#include "kl_va.h"
#include "kl_ndk.h"
#include "kl_egl.h"
#include "kl_jni.h"

// ---------- S0.1: bionic stack-guard canary in Darwin TSD slot 5 ----------
#define TLS_SLOT_STACK_GUARD 5
static uint64_t g_canary = 0x0000BEEFCAFE0000ULL;
void kl_thread_init(void) {
    uint64_t tp; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    ((uint64_t *)tp)[TLS_SLOT_STACK_GUARD] = g_canary;
}

// The guest installs its own fatal-signal handlers — IL2CPP does this for crash
// reporting — and they expect a Linux `ucontext_t`. Ours hands them a Darwin one,
// so a diagnostic abort() is caught by guest code that then wanders off and hangs
// instead of dying. Restoring the default disposition first means our own failure
// reports actually terminate the process. Without this every diagnostic below
// turns into a 20-minute mystery hang.
void kl_fatal_prepare(void) {
    // Whatever we are dying of, the graphics surface is context — and unlike the
    // JNI report, plenty of fatal paths (a failed sem_wait, an unresolved
    // import) have no reason to know EGL exists. Idempotent, so paths that
    // already printed it are unaffected.
    kl_egl_report(stderr);
    static const int sigs[] = {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) signal(sigs[i], SIG_DFL);
    fflush(NULL);
}

__attribute__((noreturn)) static void die(const char *what) {
    fprintf(stderr, "[klepton] fatal: %s\n", what);
    // Whatever we died of, the JNI surface is the context: it says how far the
    // guest got and what it had asked for by then. Without it these paths print
    // one line and lose the whole run, which reads as a much earlier failure.
    kl_jni_report(stderr);
    kl_fatal_prepare();
    abort();
}
__attribute__((noreturn)) static void kl_unresolved(void) { die("called an unresolved import"); }

// The named form, reached through the per-symbol trampolines kl_image.c builds.
// Only imports that never got a stub — an allocation failure — still land on the
// anonymous kl_unresolved above.
__attribute__((noreturn)) void kl_unresolved_named(const char *name) {
    char msg[256];
    snprintf(msg, sizeof msg, "called unresolved import '%s'", name ? name : "?");
    die(msg);
}
__attribute__((noreturn)) static void kl_stack_chk_fail(void) { die("stack smashing detected"); }

// ---------- bionic FILE ----------
// The guest computes stderr as &__sF[2] using bionic's sizeof(FILE), which we do not
// know, so anything landing inside the block routes to stderr.
// TODO(M3): recover the true stride from the GLOB_DAT addend.
static char g_sF[4096] __attribute__((aligned(16)));
FILE *kl_host_file(void *guest) {
    if ((char *)guest >= g_sF && (char *)guest < g_sF + sizeof g_sF) return stderr;
    // A FILE * small enough to be a page-zero offset is not a FILE * — it is an
    // argument that landed in the wrong slot (trap 6b) or an uninitialised one.
    // Passing it through means dying inside flockfile with no idea who called,
    // so it is named here instead and the call is dropped.
    // A FILE * small enough to be a page-zero offset is not a FILE * — it is an
    // argument that landed in the wrong slot (trap 6b) or an uninitialised one.
    // Passing it through dies inside flockfile with no clue who called; naming it
    // and routing to stderr keeps the run going, and a write that lands in the
    // log is a better outcome than a dropped one. Reads simply fail, which is
    // the honest answer for a handle that was never valid.
    if ((uintptr_t)guest < 0x10000) {
        static void *seen[8];
        static unsigned n;
        unsigned i = 0;
        for (; i < n; i++) if (seen[i] == guest) break;
        if (i == n && n < 8) {
            seen[n++] = guest;
            fprintf(stderr, "[klepton] bogus guest FILE * %p — routed to stderr\n", guest);
        }
        return stderr;
    }
    return (FILE *)guest;
}
static int kl_fputc(int c, void *f)                { return fputc(c, kl_host_file(f)); }
static int kl_fputs(const char *s, void *f)        { return fputs(s, kl_host_file(f)); }
static size_t kl_fwrite(const void *p, size_t a, size_t b, void *f) { return fwrite(p, a, b, kl_host_file(f)); }
static size_t kl_fread(void *p, size_t a, size_t b, void *f)        { return fread(p, a, b, kl_host_file(f)); }
static int kl_fclose(void *f)                      { return fclose(kl_host_file(f)); }
static int kl_fflush(void *f)                      { return fflush(f ? kl_host_file(f) : NULL); }
static char *kl_fgets(char *s, int n, void *f)     { return fgets(s, n, kl_host_file(f)); }
static int kl_getc_(void *f)                       { return getc(kl_host_file(f)); }
static int kl_feof(void *f)                        { return feof(kl_host_file(f)); }
static int kl_ferror(void *f)                      { return ferror(kl_host_file(f)); }
static void kl_clearerr(void *f)                   { clearerr(kl_host_file(f)); }
static int kl_fseek(void *f, long o, int w)        { return fseek(kl_host_file(f), o, w); }
static int kl_fseeko(void *f, off_t o, int w)      { return fseeko(kl_host_file(f), o, w); }
static long kl_ftell(void *f)                      { return ftell(kl_host_file(f)); }
static off_t kl_ftello(void *f)                    { return ftello(kl_host_file(f)); }
static int kl_setvbuf(void *f, char *b, int m, size_t n) { return setvbuf(kl_host_file(f), b, m, n); }

// ---------- guest va_list consumers that live here ----------
static int kl_vfprintf(void *f, const char *fmt, void *gva) {
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, (kl_va *)gva, m, sizeof m, KL_VA_PRINTF) == (size_t)-1) return -1;
    return vfprintf(kl_host_file(f), fmt, (va_list)m);
}
static int kl_vsnprintf(char *b, size_t n, const char *fmt, void *gva) {
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, (kl_va *)gva, m, sizeof m, KL_VA_PRINTF) == (size_t)-1) return -1;
    return vsnprintf(b, n, fmt, (va_list)m);
}
static int kl_vasprintf(char **o, const char *fmt, void *gva) {
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, (kl_va *)gva, m, sizeof m, KL_VA_PRINTF) == (size_t)-1) return -1;
    return vasprintf(o, fmt, (va_list)m);
}

// ---------- fortify (_chk) variants ----------
static void *kl_memcpy_chk(void *d, const void *s, size_t n, size_t dl) {
    if (n > dl) die("__memcpy_chk overflow"); return memcpy(d, s, n); }
static void *kl_memset_chk(void *d, int c, size_t n, size_t dl) {
    if (n > dl) die("__memset_chk overflow"); return memset(d, c, n); }
static char *kl_strcpy_chk(char *d, const char *s, size_t dl) {
    if (strlen(s) + 1 > dl) die("__strcpy_chk overflow"); return strcpy(d, s); }

// ---------- small bionic-isms ----------
static void kl_set_abort_message(const char *m) { fprintf(stderr, "[guest abort] %s\n", m ? m : ""); }
static int  kl_dl_iterate_phdr(void *cb, void *d) { (void)cb; (void)d; return 0; }
static void kl_openlog(const char *a, int b, int c) { (void)a; (void)b; (void)c; }
static void kl_closelog(void) {}
static int  kl_cxa_atexit(void (*f)(void *), void *a, void *d) { (void)f;(void)a;(void)d; return 0; }
static void kl_cxa_finalize(void *d) { (void)d; }
static void kl_android_log_write(int p, const char *t, const char *m) {
    fprintf(stderr, "[%d/%s] %s\n", p, t ? t : "", m ? m : ""); }

// ---------- externs ----------
#define X(n) extern int n(void);
X(klv_printf) X(klv_fprintf) X(klv_sprintf) X(klv_snprintf) X(klv_asprintf)
X(klv_dprintf) X(klv_syslog) X(klv_android_log_print) X(klv_sscanf) X(klv_fscanf)
X(klv_open) X(klv_fcntl) X(klv_ioctl) X(klv_strtold) X(klv_wcstold) X(klv_strtold_l)
X(klb_errno) X(klb_gettid) X(klb_sysprop_find) X(klb_sysprop_get) X(klb_sysprop_read)
X(klb_prctl) X(klb_sched_getaffinity) X(klb_sched_setaffinity)
X(klb_stat) X(klb_lstat) X(klb_fstat) X(klb_statfs) X(klb_uname) X(klb_sigaction)
X(klb_opendir) X(klb_readdir) X(klb_closedir)
X(klb_FD_ISSET_chk) X(klb_FD_SET_chk) X(klb_ctype_mb_cur_max) X(klb_lseek64)
X(klb_sysconf) X(klb_fopen) X(klb_access) X(klb_mkdir) X(klb_unlink) X(klb_rename)
X(klh_android_log_print)
X(klb_getpwuid) X(klb_getpwuid_r) X(klb_execl) X(klb_syscall) X(klb_swprintf)
X(klb_vprintf) X(klb_vsscanf) X(klb_memrchr) X(klb_memalign)
X(klb_mmap) X(klb_madvise)
X(klb_pthread_mutex_init) X(klb_pthread_mutex_lock) X(klb_pthread_mutex_unlock)
X(klb_pthread_mutex_trylock) X(klb_pthread_mutex_destroy)
X(klb_pthread_mutexattr_init) X(klb_pthread_mutexattr_destroy) X(klb_pthread_mutexattr_settype)
X(klb_pthread_cond_init) X(klb_pthread_cond_destroy) X(klb_pthread_cond_signal)
X(klb_pthread_cond_broadcast) X(klb_pthread_cond_wait) X(klb_pthread_cond_timedwait)
X(klb_pthread_condattr_init) X(klb_pthread_condattr_destroy) X(klb_pthread_condattr_setclock)
X(klb_pthread_rwlock_init) X(klb_pthread_rwlock_destroy) X(klb_pthread_rwlock_rdlock)
X(klb_pthread_rwlock_wrlock) X(klb_pthread_rwlock_unlock)
X(klb_pthread_attr_init) X(klb_pthread_attr_destroy) X(klb_pthread_attr_setstacksize)
X(klb_pthread_attr_setdetachstate) X(klb_pthread_attr_getstack) X(klb_pthread_getattr_np)
X(klb_pthread_key_create) X(klb_pthread_key_delete) X(klb_pthread_getspecific)
X(klb_pthread_setspecific) X(klb_pthread_once)
X(klb_pthread_create) X(klb_pthread_join) X(klb_pthread_detach) X(klb_pthread_exit)
X(klb_pthread_self) X(klb_pthread_equal) X(klb_pthread_kill) X(klb_pthread_sigmask)
X(klb_pthread_setname_np) X(klb_pthread_atfork)
X(klb_sem_init) X(klb_sem_destroy) X(klb_sem_post) X(klb_sem_wait)
X(klb_sem_timedwait) X(klb_sem_getvalue)
X(klb_dlopen) X(klb_dlsym) X(klb_dlclose) X(klb_dlerror) X(klb_dladdr)
#undef X
extern char **environ;

// ---------- table ----------
typedef struct { const char *name; void *fn; } kl_entry;
#define E(n, f)   { n, (void *)(f) }
#define KL_FWD(n) { #n, (void *)(n) },

static const kl_entry g_shim[] = {
#include "kl_libc_table.h"

    // stdio needing guest-FILE translation
    E("__sF", g_sF),
    E("fputc", kl_fputc), E("fputs", kl_fputs), E("fwrite", kl_fwrite), E("fread", kl_fread),
    E("fclose", kl_fclose), E("fflush", kl_fflush), E("fgets", kl_fgets), E("getc", kl_getc_),
    E("feof", kl_feof), E("ferror", kl_ferror), E("clearerr", kl_clearerr),
    E("fseek", kl_fseek), E("fseeko", kl_fseeko), E("ftell", kl_ftell), E("ftello", kl_ftello),
    E("setvbuf", kl_setvbuf),

    // AAPCS64 variadic thunks
    E("printf", klv_printf), E("fprintf", klv_fprintf), E("sprintf", klv_sprintf),
    E("snprintf", klv_snprintf), E("asprintf", klv_asprintf), E("dprintf", klv_dprintf),
    E("sscanf", klv_sscanf), E("fscanf", klv_fscanf), E("syslog", klv_syslog),
    E("__android_log_print", klv_android_log_print),
    E("open", klv_open), E("fcntl", klv_fcntl), E("ioctl", klv_ioctl),
    E("strtold", klv_strtold), E("wcstold", klv_wcstold), E("strtold_l", klv_strtold_l),
    E("swprintf", klb_swprintf), E("execl", klb_execl), E("syscall", klb_syscall),

    // guest va_list consumers
    E("vfprintf", kl_vfprintf), E("vsnprintf", kl_vsnprintf), E("vasprintf", kl_vasprintf),
    E("vprintf", klb_vprintf), E("vsscanf", klb_vsscanf),

    // fortify
    E("__memcpy_chk", kl_memcpy_chk), E("__memset_chk", kl_memset_chk),
    E("__strcpy_chk", kl_strcpy_chk), E("__stack_chk_fail", kl_stack_chk_fail),
    E("__FD_ISSET_chk", klb_FD_ISSET_chk), E("__FD_SET_chk", klb_FD_SET_chk),

    // divergent layouts / Android-only
    E("__errno", klb_errno), E("environ", &environ), E("gettid", klb_gettid),
    E("stat", klb_stat), E("lstat", klb_lstat), E("fstat", klb_fstat), E("statfs", klb_statfs),
    E("uname", klb_uname), E("sigaction", klb_sigaction),
    E("sysconf", klb_sysconf),
    E("fopen", klb_fopen), E("access", klb_access),
    E("mkdir", klb_mkdir), E("unlink", klb_unlink), E("rename", klb_rename),
    E("opendir", klb_opendir), E("readdir", klb_readdir), E("closedir", klb_closedir),
    E("lseek64", klb_lseek64), E("__ctype_get_mb_cur_max", klb_ctype_mb_cur_max),
    E("getpwuid", klb_getpwuid), E("getpwuid_r", klb_getpwuid_r),
    E("prctl", klb_prctl),
    E("memrchr", klb_memrchr), E("memalign", klb_memalign),
    E("mmap", klb_mmap), E("madvise", klb_madvise),
    E("sched_getaffinity", klb_sched_getaffinity), E("sched_setaffinity", klb_sched_setaffinity),
    E("__system_property_find", klb_sysprop_find),
    E("__system_property_get", klb_sysprop_get),
    E("__system_property_read", klb_sysprop_read),

    // setjmp/longjmp forward directly: bionic's jmp_buf (256B) is LARGER than
    // Darwin's (192B), so the guest's buffer is safe, and registering the host
    // function itself avoids interposing a stack frame that longjmp would destroy.
    E("setjmp", setjmp), E("longjmp", longjmp),

    // pthread / sem (bionic layouts)
    E("pthread_mutex_init", klb_pthread_mutex_init),
    E("pthread_mutex_lock", klb_pthread_mutex_lock),
    E("pthread_mutex_unlock", klb_pthread_mutex_unlock),
    E("pthread_mutex_trylock", klb_pthread_mutex_trylock),
    E("pthread_mutex_destroy", klb_pthread_mutex_destroy),
    E("pthread_mutexattr_init", klb_pthread_mutexattr_init),
    E("pthread_mutexattr_destroy", klb_pthread_mutexattr_destroy),
    E("pthread_mutexattr_settype", klb_pthread_mutexattr_settype),
    E("pthread_cond_init", klb_pthread_cond_init),
    E("pthread_cond_destroy", klb_pthread_cond_destroy),
    E("pthread_cond_signal", klb_pthread_cond_signal),
    E("pthread_cond_broadcast", klb_pthread_cond_broadcast),
    E("pthread_cond_wait", klb_pthread_cond_wait),
    E("pthread_cond_timedwait", klb_pthread_cond_timedwait),
    E("pthread_condattr_init", klb_pthread_condattr_init),
    E("pthread_condattr_destroy", klb_pthread_condattr_destroy),
    E("pthread_condattr_setclock", klb_pthread_condattr_setclock),
    E("pthread_rwlock_init", klb_pthread_rwlock_init),
    E("pthread_rwlock_destroy", klb_pthread_rwlock_destroy),
    E("pthread_rwlock_rdlock", klb_pthread_rwlock_rdlock),
    E("pthread_rwlock_wrlock", klb_pthread_rwlock_wrlock),
    E("pthread_rwlock_unlock", klb_pthread_rwlock_unlock),
    E("pthread_attr_init", klb_pthread_attr_init),
    E("pthread_attr_destroy", klb_pthread_attr_destroy),
    E("pthread_attr_setstacksize", klb_pthread_attr_setstacksize),
    E("pthread_attr_setdetachstate", klb_pthread_attr_setdetachstate),
    E("pthread_attr_getstack", klb_pthread_attr_getstack),
    E("pthread_getattr_np", klb_pthread_getattr_np),
    E("pthread_key_create", klb_pthread_key_create),
    E("pthread_key_delete", klb_pthread_key_delete),
    E("pthread_getspecific", klb_pthread_getspecific),
    E("pthread_setspecific", klb_pthread_setspecific),
    E("pthread_once", klb_pthread_once),
    E("pthread_create", klb_pthread_create), E("pthread_join", klb_pthread_join),
    E("pthread_detach", klb_pthread_detach), E("pthread_exit", klb_pthread_exit),
    E("pthread_self", klb_pthread_self), E("pthread_equal", klb_pthread_equal),
    E("pthread_kill", klb_pthread_kill), E("pthread_sigmask", klb_pthread_sigmask),
    E("pthread_setname_np", klb_pthread_setname_np), E("pthread_atfork", klb_pthread_atfork),
    E("sem_init", klb_sem_init), E("sem_destroy", klb_sem_destroy),
    E("sem_post", klb_sem_post), E("sem_wait", klb_sem_wait),
    E("sem_timedwait", klb_sem_timedwait), E("sem_getvalue", klb_sem_getvalue),

    // dl
    E("dlopen", klb_dlopen), E("dlsym", klb_dlsym), E("dlclose", klb_dlclose),
    E("dlerror", klb_dlerror), E("dladdr", klb_dladdr),
    E("dl_iterate_phdr", kl_dl_iterate_phdr),

    // logging / misc
    E("__android_log_write", kl_android_log_write),
    // __android_log_vprint(prio, tag, fmt, va_list) — NOT vfprintf's shape. It
    // was bound to kl_vfprintf once, which put `prio` in the FILE* slot and
    // segfaulted in flockfile the first time Unity logged through it. The
    // variadic handler already has exactly this signature, because an AAPCS64
    // va_list is a 32-byte descriptor and so arrives by reference — which is
    // what a kl_va * is.
    E("__android_log_vprint", klh_android_log_print),
    E("android_set_abort_message", kl_set_abort_message),
    E("openlog", kl_openlog), E("closelog", kl_closelog),
    E("__cxa_atexit", kl_cxa_atexit), E("__cxa_finalize", kl_cxa_finalize),

    E("__klepton_unresolved", kl_unresolved),
};

void *kl_shim_lookup(const char *name) {
    for (size_t i = 0; i < sizeof g_shim / sizeof g_shim[0]; i++)
        if (strcmp(g_shim[i].name, name) == 0) return g_shim[i].fn;
    // Tier 4: the NDK surface (M3) lives in its own table — it is a different
    // API family with its own lifetimes, not more bionic libc.
    void *ndk = kl_ndk_lookup(name);
    if (ndk) return ndk;
    // Tier 5: EGL (M5). Same reasoning again, and it is the door to GLES —
    // eglGetProcAddress hands out the rest of the graphics surface.
    return kl_egl_lookup(name);
}
