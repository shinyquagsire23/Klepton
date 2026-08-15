// Klepton runtime — load an Android ARM64 ELF .so into a Darwin arm64 process.
//
// M1a: images are mmap'd and self-relocated at runtime. This works on macOS and
// (per S0.0 P11) on development-signed visionOS builds. klepton-ld's Mach-O
// emitter (M1b) reuses this exact relocation logic offline.
#ifndef KLEPTON_H
#define KLEPTON_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "kl_env.h"

typedef struct kl_image kl_image;

// Load and fully relocate an ELF .so. Returns NULL on error; kl_error() explains.
kl_image *kl_load(const char *path);
// M1b: load a klepton-ld-translated Mach-O dylib instead. dyld maps the guest
// text — nothing is RWX and no guest byte is written at runtime, because the
// TLS and x18 rewrites happened offline. This is the path that has to work on
// visionOS; kl_load() above is the development one. Same kl_image afterwards.
kl_image *kl_load_dylib(const char *path);
// Pick between the two: with KL_DYLIB_DIR set, prefer <dir>/<name>.dylib when a
// translation exists, else the ELF. Lets the boot chain run mixed while some
// libraries are still untranslatable. Every guest-library load goes through it.
kl_image *kl_load_auto(const char *path);

// Could kl_load_auto load a guest library at `path`? Ask this — never
// stat(path) — whenever answering an existence question about a guest library on
// the guest's behalf: with translations embedded, the .so path is a name the
// loader resolves and not a file on disk. See the implementation comment.
int kl_can_load(const char *path);
// ...and the wider question the guest actually asks: could klb_dlopen() open it?
// True for everything kl_can_load covers PLUS the libraries this shim serves
// synthetically, which have no file anywhere. Answering an existence check for
// the guest wants THIS one — see the implementation comment in kl_dl.c for the
// black screen the difference caused. Defined in kl_dl.c.
int kl_can_dlopen(const char *path);
// Look up an exported symbol. NULL if absent.
void      *kl_sym(kl_image *img, const char *name);
// Run DT_INIT_ARRAY. Separate from kl_load so tests can inspect first.
void       kl_run_init(kl_image *img);
void       kl_unload(kl_image *img);
const char *kl_error(void);

// Base address the image was mapped at, and its total vaddr span.
void      *kl_base(kl_image *img);
size_t     kl_span(kl_image *img);

// Stats, for tests and the M1 exit criterion.
typedef struct {
    unsigned relative, abs64, glob_dat, jump_slot;   // relocations applied
    unsigned tls_rewrites;                           // mrs tpidr_el0 -> tpidrro_el0
    // ...and TLS sites the trap-0d data test refused. Counted apart from
    // x18_data_words because the two mean opposite things: a refused x18 site
    // costs nothing, a refused TLS site is trap 1 live in the guest. Nonzero
    // here is a finding, not a statistic — rewrite_tls names each one.
    unsigned tls_refused;
    unsigned svc_sites;                              // inline syscalls found
    unsigned imports_bound, imports_missing;
    // ...and the undefined imports that are WEAK, which is a different answer
    // rather than a subset of imports_missing. A weak undefined is how a guest
    // asks "does this platform have X?": the call site null-tests the slot, so
    // resolving one to a non-null abort trampoline answers YES and then aborts
    // when the guest takes the branch it was offered. Left NULL and counted
    // here. Nonzero is normal (memfd_create, getentropy, __cxa_thread_atexit_impl,
    // jemalloc's mallctl, TSAN's __google_potentially_blocking_region_*), but a
    // NULL-pointer fault with no name in the log starts by reading this list.
    unsigned imports_weak_null;
    // S0.5: guest instructions naming x18, and how many were redirected to a
    // veneer. Anything refused is still exposed to trap 0.
    unsigned x18_sites, x18_patched, x18_refused;
    // Words that decode as an x18 site but sit in DATA inside an
    // executable section (trap 0b). Not sites and not refusals — reported
    // separately because a number moving here means the second detector
    // changed its mind about what is code.
    unsigned x18_data_words;
    // Trap 26: `mrs Xt, CTR_EL0`, which EL0 may not execute on Darwin. Same
    // veneer machinery, counted apart because the x18 numbers above are exact
    // and `make check` gates on them.
    unsigned ctr_sites, ctr_patched, ctr_refused;
} kl_stats;
const kl_stats *kl_get_stats(kl_image *img);

// Unique names of imports the shim could not resolve. Valid until kl_unload().
const char *const *kl_missing_imports(kl_image *img, unsigned *count);
// ...and the WEAK undefined ones, which are left NULL rather than stubbed.
// See stats.imports_weak_null: a different answer, not a subset of the above.
const char *const *kl_weak_imports(kl_image *img, unsigned *count);

// ---- shim ----
// Resolve a bionic/NDK import by name. Returns NULL if unimplemented.
void *kl_shim_lookup(const char *name);
// Install the bionic stack-guard canary into Darwin TSD slot 5 for this thread.
// Must be called on every thread that will execute guest code. (S0.1)
void  kl_thread_init(void);
// Restore default fatal-signal disposition and flush stdio. Call before abort()
// in any diagnostic path: the guest installs handlers that expect a Linux
// ucontext_t, and they swallow our abort and hang instead of dying.
void  kl_fatal_prepare(void);
// Map a guest FILE* (an offset into our fake bionic __sF block) to a host stream.
FILE *kl_host_file(void *guest);

// Build a code stub that tail-calls `handler` with `name` in x0, so whatever the
// guest reached for names itself instead of arriving as one anonymous abort.
// Used for unresolved ELF imports and for eglGetProcAddress's GL entry points.
void *kl_named_stub(const char *name, void *handler);
// The forwarding variant: a stub that branches to `tramp` with `desc` in x16 and
// nothing else disturbed, so the trampoline can log and then let the original call
// run with its arguments intact. `desc` must be a stable { const char *name;
// void *real; } pair. See runtime/kl_gl_trace.S for the register contract.
void *kl_trace_stub(const char *name, void *desc, void *tramp);

// ---- image registry (backs guest dlopen/dlsym/dladdr) ----
void      kl_set_library_path(const char *dir);
void      kl_register_image(const char *soname, kl_image *img);
kl_image *kl_find_image(const char *soname);
// Load an .so plus its DT_NEEDED chain, dependencies first (kl_dl.c).
kl_image *kl_load_recursive(const char *path);
// Read a file's DT_NEEDED sonames without loading it (kl_image.c).
int       kl_list_needed(const char *path, char names[][128], int max);
// Search every registered guest image for an export (kl_dl.c); used by the
// loader's import binding after the shim misses.
void     *kl_guest_sym_global(const char *name);
// The image's program headers, as mapped. Every guest lib's first PT_LOAD maps
// file offset 0 at vaddr 0, so the phdr array is live at base + e_phoff and the
// load bias is simply the base. This is what dl_iterate_phdr hands the guest's
// unwinder so it can find PT_GNU_EH_FRAME (kl_dl.c).
const void *kl_phdrs(kl_image *img, unsigned *count);

// Which guest image an address is in, and its offset within it. NULL if none.
// Guest libraries land wherever the kernel put them, so this is what makes a
// raw pc from a fault mean anything.
const char *kl_addr_image(const void *addr, size_t *offset);


// Rewrite a guest file path into the synthetic /proc and /sys tree when we serve
// it; returns `path` unchanged otherwise. Every guest file entry point goes
// through this — Unity reads its CPU and memory configuration out of procfs.
const char *kl_guest_path(const char *path, char *buf, size_t cap);
// Register the APK -> unpacked-tree mapping for raw "<apk>/assets/..." opens
// (split assets). See the implementation comment in kl_libc.c.
void kl_guest_path_map(const char *apk, const char *unpacked_dir);
// ...and the guest's public external storage ("/sdcard" and its aliases), for a
// guest that writes there by literal path instead of asking Java. `dir` must be
// what Environment.getExternalStorageDirectory() answers, or the two doors onto
// one directory disagree. See the implementation comment in kl_libc.c.
void kl_guest_extstorage_map(const char *dir);

// How much memory the guest asked us to decommit, and how we served it. Zero
// bytes here on a Unity guest means its VirtualMemory layer's decommit is doing
// nothing and the footprint is a high-water mark (see klb_madvise).
void kl_madv_report(void);

// Memory pressure. The guest is told the room ran out through
// UnityPlayer.nativeLowMemory, which is where a title drops its caches; nothing
// delivered it before, so every cache a Quest build releases under pressure was
// held here forever. _init subscribes to the OS's own signal, _poll checks our
// footprint against the reported budget (cheap enough for once a frame), and
// _fire is the on-demand knob — the way to A/B this without a headset.
void kl_mem_pressure_init(void);
void kl_mem_pressure_poll(void);
void kl_mem_pressure_fire(const char *why);
void kl_mem_report(void);

// The budget and what is left of it, for the other doors a guest asks the same
// question through — ActivityManager.getMemoryInfo is /proc/meminfo asked in
// Java, and the two must not answer differently.
uint64_t kl_mem_budget_bytes(void);
uint64_t kl_mem_available_bytes(void);

// errno numbering diverges above 34 between Linux and Darwin, and guest code
// compares against Linux's values (see kl_libc.c).
int kl_errno_to_linux(int e);
int kl_errno_from_linux(int e);

// KL_TRACE_FS reporting for guest file operations (see kl_libc.c).
void kl_fs_trace_open(const char *path, int flags, int fd);

// A diagnostic's view of the guest's file operations, so RUNTIME_SHIP does not
// have to know that RUNTIME_DIAG exists. NULL unless something installs one;
// `kl_metadump_watch_install` is the only caller today, and it wants exactly
// two events — which fd a path was opened on, and where that fd got mapped.
//
// `path` is set for "open" and NULL for "mmap"; `caller` is the GUEST's return
// address where one is available, which is the whole point of the hook (an
// obfuscated library's call sites are not findable any other way).
typedef void (*kl_file_watch_fn)(const char *op, const char *path, int fd,
                                 void *addr, size_t len, long long off,
                                 void *caller);
extern kl_file_watch_fn kl_file_watch;

// ...and the same idea one layer down: a diagnostic's answer for a shim symbol,
// consulted by kl_shim_lookup before any tier. NULL unless installed. Return
// NULL for a name the diagnostic does not want, which is all of them.
extern void *(*kl_shim_override)(const char *name);

// Mutex owner table dump (kl_pthread.c): who holds each translated guest
// mutex and from where it was locked. Called from t_boot's fault handler so
// a hang names the contested mutex's owner, not just its waiters.
void kl_pthread_report(FILE *out);

#endif
