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

typedef struct kl_image kl_image;

// Load and fully relocate an ELF .so. Returns NULL on error; kl_error() explains.
kl_image *kl_load(const char *path);
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
    unsigned svc_sites;                              // inline syscalls found
    unsigned imports_bound, imports_missing;
} kl_stats;
const kl_stats *kl_get_stats(kl_image *img);

// Unique names of imports the shim could not resolve. Valid until kl_unload().
const char *const *kl_missing_imports(kl_image *img, unsigned *count);

// ---- shim ----
// Resolve a bionic/NDK import by name. Returns NULL if unimplemented.
void *kl_shim_lookup(const char *name);
// Install the bionic stack-guard canary into Darwin TSD slot 5 for this thread.
// Must be called on every thread that will execute guest code. (S0.1)
void  kl_thread_init(void);
// Map a guest FILE* (an offset into our fake bionic __sF block) to a host stream.
FILE *kl_host_file(void *guest);

// ---- image registry (backs guest dlopen/dlsym/dladdr) ----
void      kl_set_library_path(const char *dir);
void      kl_register_image(const char *soname, kl_image *img);
kl_image *kl_find_image(const char *soname);

#endif
