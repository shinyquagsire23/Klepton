// Guest dlopen/dlsym/dladdr, backed by klepton's own image registry.
// The guest's dynamic-linking API maps almost one-to-one onto kl_load/kl_sym.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "klepton.h"
#include "kl_egl.h"
#include "kl_opensl.h"

#define KL_MAX_IMAGES 64
typedef struct { char soname[128]; kl_image *img; } entry;
static entry  g_imgs[KL_MAX_IMAGES];
static int    g_nimgs = 0;
static char   g_libdir[512] = ".";
static char   g_dlerr[256];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void kl_set_library_path(const char *dir) { snprintf(g_libdir, sizeof g_libdir, "%s", dir); }

static const char *basename_of(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

// Register an image so guest dlsym/dladdr can see it (also used for the root libs).
void kl_register_image(const char *soname, kl_image *img) {
    pthread_mutex_lock(&g_lock);
    if (g_nimgs < KL_MAX_IMAGES) {
        snprintf(g_imgs[g_nimgs].soname, sizeof g_imgs[0].soname, "%s", basename_of(soname));
        g_imgs[g_nimgs].img = img;
        g_nimgs++;
    }
    pthread_mutex_unlock(&g_lock);
}

kl_image *kl_find_image(const char *soname) {
    const char *b = basename_of(soname);
    for (int i = 0; i < g_nimgs; i++)
        if (strcmp(g_imgs[i].soname, b) == 0) return g_imgs[i].img;
    return NULL;
}

// Which guest image an address falls in, and how far into it. Guest libraries
// are mapped at whatever address the kernel picked, so a raw pc from a fault is
// unusable on its own — this is what turns it into a "libil2cpp+0x1234" that can
// be disassembled. Takes no lock: the callers are diagnostic paths running in an
// already-broken process, where blocking on a mutex some dead thread holds would
// lose the report entirely.
const char *kl_addr_image(const void *addr, size_t *offset) {
    for (int i = 0; i < g_nimgs; i++) {
        const char *base = (const char *)kl_base(g_imgs[i].img);
        if (!base) continue;
        if ((const char *)addr >= base && (const char *)addr < base + kl_span(g_imgs[i].img)) {
            if (offset) *offset = (size_t)((const char *)addr - base);
            return g_imgs[i].soname;
        }
    }
    if (offset) *offset = 0;
    return NULL;
}

void *klb_dlopen(const char *path, int flags) {
    (void)flags;
    if (!path) return (void *)-1;                    // RTLD_DEFAULT-ish: whole process
    // GL libraries have no file to open — they are served by kl_egl.c. This has
    // to come first: falling through would look for libGLESv2.so on disk, fail,
    // and hand the guest a NULL it goes on to call.
    void *gl = kl_egl_dlopen(path);
    if (gl) return gl;
    void *sl = kl_opensl_dlopen(path);
    if (sl) return sl;
    pthread_mutex_lock(&g_lock);
    kl_image *found = kl_find_image(path);           // already loaded? refcount is coarse
    pthread_mutex_unlock(&g_lock);
    if (found) return found;

    char full[1024];
    if (strchr(path, '/')) snprintf(full, sizeof full, "%s", path);
    else                   snprintf(full, sizeof full, "%s/%s", g_libdir, path);

    kl_image *img = kl_load(full);
    if (!img) {
        snprintf(g_dlerr, sizeof g_dlerr, "klepton: cannot load %s: %s", full, kl_error());
        fprintf(stderr, "  [klepton] guest dlopen(\"%s\") FAILED: %s\n", path, kl_error());
        return NULL;
    }
    kl_register_image(path, img);
    kl_run_init(img);
    fprintf(stderr, "  [klepton] guest dlopen(\"%s\") -> %p\n", path, (void *)img);
    return img;
}

void *klb_dlsym(void *handle, const char *name) {
    if (kl_egl_is_handle(handle)) return kl_egl_sym(name);
    if (kl_opensl_is_handle(handle)) return kl_opensl_sym(name);
    if (handle == NULL || handle == (void *)-1) {    // RTLD_DEFAULT / RTLD_NEXT
        void *s = kl_shim_lookup(name);
        if (s) return s;
        for (int i = 0; i < g_nimgs; i++) {
            void *v = kl_sym(g_imgs[i].img, name);
            if (v) return v;
        }
        snprintf(g_dlerr, sizeof g_dlerr, "klepton: undefined symbol: %s", name);
        return NULL;
    }
    void *v = kl_sym((kl_image *)handle, name);
    if (!v) snprintf(g_dlerr, sizeof g_dlerr, "klepton: undefined symbol: %s", name);
    return v;
}

int klb_dlclose(void *handle) { (void)handle; return 0; }   // images are never unloaded

const char *klb_dlerror(void) {
    if (!g_dlerr[0]) return NULL;
    static char out[256];
    memcpy(out, g_dlerr, sizeof out);
    g_dlerr[0] = 0;
    return out;
}

// Linux Dl_info; layout matches Darwin's, so this can be filled directly.
typedef struct { const char *dli_fname; void *dli_fbase;
                 const char *dli_sname; void *dli_saddr; } kl_dl_info;

int klb_dladdr(const void *addr, kl_dl_info *info) {
    for (int i = 0; i < g_nimgs; i++) {
        uint8_t *b = kl_base(g_imgs[i].img);
        if ((const uint8_t *)addr >= b && (const uint8_t *)addr < b + kl_span(g_imgs[i].img)) {
            info->dli_fname = g_imgs[i].soname;
            info->dli_fbase = b;
            info->dli_sname = NULL;      // TODO: reverse-lookup nearest .dynsym entry
            info->dli_saddr = NULL;
            return 1;
        }
    }
    return 0;
}
