// Guest dlopen/dlsym/dladdr, backed by klepton's own image registry.
// The guest's dynamic-linking API maps almost one-to-one onto kl_load/kl_sym.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "klepton.h"
#include "kl_egl.h"
#include "kl_opensl.h"
#include "kl_ovrp.h"
#include "kl_ovrplat.h"
#include "kl_mediandk.h"
#include "kl_vulkan.h"
#include "kl_aaudio.h"
#include "kl_openxr.h"

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

// ---------- dl_iterate_phdr: how the guest unwinds ----------
// libil2cpp statically links LLVM's libunwind and imports exactly one symbol to
// drive it: dl_iterate_phdr. That call is the *only* way it can find a pc's
// PT_GNU_EH_FRAME, so a stub returning 0 (which this was) means no FDE is ever
// found, _Unwind_RaiseException fails phase 1, and __cxa_throw calls abort().
// The effect is that EVERY guest exception kills the process whether or not a
// handler exists — three separate "the guest threw and died" symptoms (the
// offline Dns SocketException, <Initialize>d__6's TimeoutException, the
// KeyNotFound on an unknown device-config param) were all this one stub.
//
// The struct is bionic's (link.h), not Darwin's — the two have different shapes
// and it is guest code reading these offsets. Only the first four fields are
// load-bearing for the unwinder; the Android-R tail is zeroed and `size` tells
// the callee how much is real.
typedef struct {
    uint64_t     dlpi_addr;          // load bias; vaddr V lives at base + V
    const char  *dlpi_name;
    const void  *dlpi_phdr;
    uint16_t     dlpi_phnum;
    unsigned long long dlpi_adds, dlpi_subs;
    size_t       dlpi_tls_modid;
    void        *dlpi_tls_data;
} kl_dl_phdr_info;

// Takes no lock, for the same reason kl_addr_image does not: this runs on
// arbitrary threads during unwinding, including inside a fault path where some
// dead thread may hold g_lock. Registration is append-only and g_nimgs is read
// once, so a concurrent load is merely not seen yet — never a torn entry.
// The callback is declared void*-first so this definition and kl_shim.c's extern
// are the same type: a mismatch across two TUs compiles silently (trap 6b) and
// the guest's real callback signature is bionic's, not ours, either way.
int kl_dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *data) {
    if (!cb) return 0;
    int n = g_nimgs;
    for (int i = 0; i < n; i++) {
        unsigned phnum = 0;
        const void *phdr = kl_phdrs(g_imgs[i].img, &phnum);
        if (!phdr || !phnum) continue;
        kl_dl_phdr_info info = {0};
        info.dlpi_addr  = (uint64_t)(uintptr_t)kl_base(g_imgs[i].img);
        info.dlpi_name  = g_imgs[i].soname;
        info.dlpi_phdr  = phdr;
        info.dlpi_phnum = (uint16_t)phnum;
        int r = cb((void *)&info, sizeof info, data);
        if (r) return r;                   // non-zero stops iteration, as on Linux
    }
    return 0;
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

// Cross-image export search, used by the loader's relocation-time import
// binding (kl_image.c) for symbols the shim does not serve. Registration order
// is dependencies-first, so the first hit is what Android's linker would have
// bound (nearest dependency wins).
void *kl_guest_sym_global(const char *name) {
    void *v = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nimgs && !v; i++)
        v = kl_sym(g_imgs[i].img, name);
    pthread_mutex_unlock(&g_lock);
    return v;
}

// Load an .so together with its DT_NEEDED chain, dependencies first, so each
// image's relocation-time imports can bind against the images it needs
// (kl_guest_sym_global). Names with no file in the library path are assumed
// shim-served (libc/libm/libdl/liblog/... or a synthetic gateway) and skipped.
kl_image *kl_load_recursive(const char *path) {
    kl_image *have = kl_find_image(path);
    if (have) return have;                       // cycle guard / already loaded

    char names[32][128];
    int nn = kl_list_needed(path, names, 32);
    for (int i = 0; i < nn; i++) {
        char full[1024];
        if (strchr(names[i], '/')) snprintf(full, sizeof full, "%s", names[i]);
        else snprintf(full, sizeof full, "%s/%s", g_libdir, names[i]);
        if (access(full, R_OK) != 0) continue;   // shim-served, not a file
        if (!kl_load_recursive(full)) {
            fprintf(stderr, "  [klepton] dependency %s of %s failed to load: %s\n",
                    names[i], path, kl_error());
            return NULL;
        }
    }

    kl_image *img = kl_load_auto(path);
    if (!img) return NULL;
    kl_register_image(path, img);
    kl_run_init(img);
    return img;
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

// Would klb_dlopen() succeed for `path`? This is the question anything answering
// an existence check on the guest's behalf has to ask, and it is strictly wider
// than kl_can_load(): a library can be loadable two ways, and only one of them
// is a file.
//
//   1. there is an image to load  — a translated dylib, or the ELF   (kl_can_load)
//   2. this shim SERVES the name instead of loading anything         (below)
//
// Case 2 has no file anywhere, by design: OVRPlugin, the platform loader, GLES
// and OpenSL ES are synthesized because the real ones need Quest system
// libraries that do not exist here (PLANNING 3.1). On the host that stayed
// invisible — the APK's own libOVRPlugin.so sits in the guest lib directory and
// access() finds it, even though we never load a byte of it. In the bundle only
// the five *translations* are embedded and the ELF tree is deliberately absent,
// so the same question answered "no".
//
// That is what black-screened the device. ClassLoader.findLibrary("OVRPlugin")
// returned null, so Unity took its fallback branch, could not get a path to
// dlopen for the plugin handle, and gave up with "Oculus Plugin could not be
// loaded." — no VRDevice, no SetupEyeTexture2, so the compositor's eye-texture
// provider was never called and every frame composited black. The renderer was
// healthy the whole time; it had nothing to show. Reproduced on the host in one
// run by moving libOVRPlugin.so aside, which is the A/B if this regresses.
//
// Exactly the same bug as P5.4's "Failed to load Il2CPP." (see kl_can_load), one
// library over and one cause deeper: that one was cured by asking kl_can_load
// instead of stat(), and this is what kl_can_load itself could not see.
int kl_can_dlopen(const char *path) {
    if (!path) return 0;
    return kl_egl_claims(path)  || kl_opensl_claims(path)   || kl_ovrp_claims(path) ||
           kl_ovrplat_claims(path) || kl_mediandk_claims(path) ||
           kl_vulkan_claims(path) || kl_aaudio_claims(path) ||
           kl_openxr_claims(path) || kl_can_load(path);
}

// KL_DLOPEN_REFUSE=<substr>[,<substr>...] — refuse these by name, as if the
// file were not there. A comma-separated list matched against the whole path,
// so "phonon" catches libphonon and libaudioplugin_phonon both.
//
// This is not a way to hide gaps: a guest that dlopens an optional plugin
// ALREADY has a path for the library being absent — VRChat logs
// `DllNotFoundException` for AudioPluginOculusSpatializer and carries on — and
// a NULL here is exactly what a device gives it. What the knob buys is
// isolation: libphonon (Steam Audio) aborts its own worker pool after a failed
// HRTF init, intermittently, which stops the run before anything downstream of
// it can be measured at all. `KL_DLOPEN_REFUSE=phonon` takes that library out
// and makes the rest of the arc reachable, and the A/B is one run.
//
// Nothing defaults to being refused, and a refusal is always named.
static int kl_dlopen_refused(const char *path) {
    static const char *list;
    static int inited;
    if (!inited) { inited = 1; list = kl_env_str("KL_DLOPEN_REFUSE", NULL); }
    if (!list || !*list || !path) return 0;
    for (const char *p = list; *p; ) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        if (n) {
            char want[256];
            if (n >= sizeof want) n = sizeof want - 1;
            memcpy(want, p, n); want[n] = '\0';
            if (strstr(path, want)) {
                fprintf(stderr, "  [klepton] guest dlopen(\"%s\") REFUSED by "
                                "KL_DLOPEN_REFUSE=\"%s\" — the guest sees this as "
                                "a library that is not installed\n", path, want);
                return 1;
            }
        }
        p = comma ? comma + 1 : p + strlen(p);
    }
    return 0;
}

void *klb_dlopen(const char *path, int flags) {
    (void)flags;
    if (!path) return (void *)-1;                    // RTLD_DEFAULT-ish: whole process
    if (kl_dlopen_refused(path)) return NULL;
    // GL libraries have no file to open — they are served by kl_egl.c. This has
    // to come first: falling through would look for libGLESv2.so on disk, fail,
    // and hand the guest a NULL it goes on to call.
    void *gl = kl_egl_dlopen(path);
    if (gl) return gl;
    void *sl = kl_opensl_dlopen(path);
    if (sl) return sl;
    void *xr = kl_ovrp_dlopen(path);
    if (xr) return xr;
    // The Oculus Platform loader. Served rather than loaded for the same reason as
    // OVRPlugin: the real one only forwards to a system service that is not here.
    void *plat = kl_ovrplat_dlopen(path);
    if (plat) return plat;
    void *md = kl_mediandk_dlopen(path);
    if (md) return md;
    // Vulkan (BONELAB). Same reasoning as the GL line above: there is no
    // libvulkan.so on disk to fall through to, and Unity dlopens it as the FIRST
    // thing it tries — it is the only graphics API this build probes.
    void *vk = kl_vulkan_dlopen(path);
    if (vk) return vk;
    // AAudio. FMOD dlopens it by name on every Unity guest and there is no file
    // to fall through to, so a miss here is silently no audio at all — see
    // kl_aaudio.c. (Steam Link reaches the same code through DT_NEEDED instead,
    // which is why this door was missing for so long.)
    void *aa = kl_aaudio_dlopen(path);
    if (aa) return aa;
    // The OpenXR loader. This one is NOT like the lines above it: the file
    // really is in the guest tree, so this must come first to keep the real
    // Khronos loader from loading successfully and then failing at the Android
    // runtime broker. See kl_openxr.c.
    void *xrl = kl_openxr_dlopen(path);
    if (xrl) return xrl;
    pthread_mutex_lock(&g_lock);
    kl_image *found = kl_find_image(path);           // already loaded? refcount is coarse
    pthread_mutex_unlock(&g_lock);
    if (found) return found;

    char full[1024];
    if (strchr(path, '/')) snprintf(full, sizeof full, "%s", path);
    else                   snprintf(full, sizeof full, "%s/%s", g_libdir, path);

    kl_image *img = kl_load_recursive(full);
    if (!img) {
        snprintf(g_dlerr, sizeof g_dlerr, "klepton: cannot load %s: %s", full, kl_error());
        fprintf(stderr, "  [klepton] guest dlopen(\"%s\") FAILED: %s\n", path, kl_error());
        return NULL;
    }
    fprintf(stderr, "  [klepton] guest dlopen(\"%s\") -> %p\n", path, (void *)img);
    return img;
}

void *klb_dlsym(void *handle, const char *name) {
    if (kl_egl_is_handle(handle)) return kl_egl_sym(name);
    if (kl_opensl_is_handle(handle)) return kl_opensl_sym(name);
    if (kl_ovrp_is_handle(handle)) return kl_ovrp_sym(name);
    if (kl_ovrplat_is_handle(handle)) return kl_ovrplat_sym(name);
    if (kl_mediandk_is_handle(handle)) return kl_mediandk_sym(name);
    if (kl_vulkan_is_handle(handle)) return kl_vulkan_sym(name);
    if (kl_aaudio_is_handle(handle)) return kl_aaudio_sym(name);
    if (kl_openxr_is_handle(handle)) return kl_openxr_sym(name);
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
