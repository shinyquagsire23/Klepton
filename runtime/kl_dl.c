// Guest dlopen/dlsym/dladdr, backed by klepton's own image registry.
// The guest's dynamic-linking API maps almost one-to-one onto kl_load/kl_sym.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "klepton.h"
#include "kl_env.h"
#include "kl_fault.h"
#include <wchar.h>
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

// Sonames that were looked for and are not here. A guest may ask for the same
// absent library thousands of times a second and get the same answer every time:
// IL2CPP resolves a P/Invoke by dlopen on EVERY call once it has failed, so
// VRChat's `AudioPluginOculusSpatializer` — a plugin its own APK does not ship —
// costs six failed open() calls and six log lines per call, hundreds of times a
// second, which is enough on its own to stop the app making progress.
//
// This changes no answer. The library is still absent and dlopen still returns
// NULL with the same dlerror; what stops is re-deriving it. Nothing here stages
// a library into the guest tree after boot, so "absent" does not become "present"
// — and if that ever changes, this cache is the thing that has to know.
#define KL_DL_MISS_MAX 64
static const char *g_miss[KL_DL_MISS_MAX];
static unsigned g_nmiss;

static int kl_dlopen_missed(const char *path) {
    for (unsigned i = 0; i < g_nmiss; i++)
        if (strcmp(g_miss[i], path) == 0) return 1;
    return 0;
}

static void kl_dlopen_note_miss(const char *path) {
    if (g_nmiss >= KL_DL_MISS_MAX) return;
    char *c = strdup(path);
    if (c) g_miss[g_nmiss++] = c;
}

static void kl_dl_trace_shims(void);

void *klb_dlopen(const char *path, int flags) {
    (void)flags;
    { static int once; if (!once) { once = 1; kl_dl_trace_shims(); } }
    if (!path) return (void *)-1;                    // RTLD_DEFAULT-ish: whole process
    if (kl_dlopen_refused(path)) return NULL;
    // Asked before, and it was not here. The dlerror the guest reads next is set
    // to the same text the first attempt produced, so a caller that reports it
    // cannot tell the two apart.
    pthread_mutex_lock(&g_lock);
    int missed = kl_dlopen_missed(path);
    pthread_mutex_unlock(&g_lock);
    if (missed) {
        snprintf(g_dlerr, sizeof g_dlerr, "klepton: cannot load %s: %s",
                 path, "No such file or directory");
        return NULL;
    }
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
        // KL_TRACE_DLOPEN_FRAMES=1 — who asked. A P/Invoke that cannot resolve
        // throws from IL2CPP's resolver, and the managed method that declared it
        // is named nowhere in the exception; but the resolver runs on the guest's
        // own stack, so the frame walk here reaches the generated wrapper and
        // tools/vrc_code.py --whois turns that address into a method.
        if (kl_env_on("KL_TRACE_DLOPEN_FRAMES", 0)) {
            kl_fault_print_frames(stderr, NULL);
            // ...and the conservative version, because this guest's obfuscated
            // .text keeps no frame chain, so the x29 walk stops before it
            // reaches the managed wrapper. Every saved return address is a stack
            // word pointing into an image, so printing all of them prints the
            // chain with stale words mixed in — a wrong entry here is junk to
            // disassemble, not a misleading claim.
            uintptr_t here;
            uintptr_t *w = (uintptr_t *)(((uintptr_t)&here) & ~(uintptr_t)7);
            fprintf(stderr, "    stack x-ray (conservative):\n");
            for (size_t i = 0, shown = 0; i < 8192 && shown < 40; i++) {
                uintptr_t v = w[i];
                if (v < 0x1000) continue;
                size_t off = 0;
                const char *img = kl_addr_image((void *)v, &off);
                if (!img) continue;
                fprintf(stderr, "      [sp+0x%04zx] %s+0x%zx\n", i * 8, img, off);
                shown++;
            }
        }
        pthread_mutex_lock(&g_lock);
        if (!kl_dlopen_missed(path)) kl_dlopen_note_miss(path);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    fprintf(stderr, "  [klepton] guest dlopen(\"%s\") -> %p\n", path, (void *)img);
    return img;
}

static void *kl_dl_interpose(const char *name, void *real);

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
    if (v) { void *w = kl_dl_interpose(name, v); if (w) return w; }
    return v;
}

// ---------------------------------------------------------------------------
// Interposing a GUEST export, for the questions that only the arguments answer.
//
// A guest-to-guest call is invisible from here — but a P/Invoke is not: managed
// code resolves it by name through klb_dlsym, so a wrapper handed back at THAT
// moment sits on the seam with the real function one call away.
//
// `iplHRTFCreate` gets one permanently, because VRChat's libphonon.so cannot
// answer its own DEFAULT: the shipped `gDefaultHrtfData` is a 40-byte stub
// ("HRTF" magic, version 2, one direction, one sampling rate, a 1-sample
// HRIR of [1.0, 1.0]), and Steam Audio's real ~1 MiB default is simply not in
// the binary. type=DEFAULT therefore yields numSamples == 1, PFFFT refuses it
// (`Unable to create PFFFT setup (size == 1)`), and the managed side throws
// its way into the Error World. This is the APK's own defect — a real Steam
// Frame fails identically — so repairing it with real data is not a host lie.
//
// The repair rides the SOFA path, which IS complete in this binary (a full
// embedded libmysofa; `mysofa_open_data_no_norm` resamples to whatever rate
// the guest asks for): rewrite the settings to type=SOFA with sofa_data
// pointing at CIPIC subject 124, vendored from Valve's open-source
// steam-audio tree and baked in by kl_phonon_hrtf.S. KL_PHONON_HRTF=0 A/Bs
// the substitution off; KL_TRACE_HRTF=1 still prints the arguments either way.
typedef struct { int32_t sampling_rate, frame_size; } kl_ipl_audio_settings;
// IPLHRTFSettings, from Steam Audio's public phonon.h.
typedef struct {
    int32_t     type;            // 0 = IPL_HRTFTYPE_DEFAULT, 1 = ..._SOFA
    const char *sofa_file_name;
    const void *sofa_data;
    int32_t     sofa_data_size;
    float       volume;
    int32_t     norm_type;
} kl_ipl_hrtf_settings;
static int32_t (*g_real_iplHRTFCreate)(void *, kl_ipl_audio_settings *, void *, void *);

// kl_phonon_hrtf.S (.incbin of runtime/data/phonon_hrtf_cipic_124.sofa).
extern const uint8_t kl_phonon_hrtf_sofa[] __asm__("_kl_phonon_hrtf_sofa");
extern const uint8_t kl_phonon_hrtf_sofa_end[] __asm__("_kl_phonon_hrtf_sofa_end");

static int32_t kl_trace_iplHRTFCreate(void *ctx, kl_ipl_audio_settings *as,
                                      void *hs, void *out) {
    const kl_ipl_hrtf_settings *h = (const kl_ipl_hrtf_settings *)hs;
    int trace = kl_env_on("KL_TRACE_HRTF", 0);
    if (trace)
        fprintf(stderr, "  [phonon] iplHRTFCreate(samplingRate=%d, frameSize=%d) "
                        "hrtf{type=%d sofaFile=%s sofaData=%p size=%d volume=%.3f}\n",
                as ? as->sampling_rate : -1, as ? as->frame_size : -1,
                h ? h->type : -1,
                h && h->sofa_file_name ? h->sofa_file_name : "(null)",
                h ? h->sofa_data : NULL, h ? h->sofa_data_size : -1,
                h ? (double)h->volume : 0.0);
    int injected = kl_env_on("KL_PHONON_HRTF", 1) && h && h->type == 0 &&
                   !h->sofa_file_name && !h->sofa_data;
    kl_ipl_hrtf_settings fix;
    if (injected) {
        fix = *h;
        fix.type = 1;                                   // IPL_HRTFTYPE_SOFA
        fix.sofa_data = kl_phonon_hrtf_sofa;
        fix.sofa_data_size = (int32_t)(kl_phonon_hrtf_sofa_end - kl_phonon_hrtf_sofa);
        fprintf(stderr, "  [phonon] DEFAULT HRTF is a 1-sample stub; substituting "
                        "CIPIC 124 SOFA (%d bytes)\n", fix.sofa_data_size);
        hs = &fix;
    }
    int32_t r = g_real_iplHRTFCreate(ctx, as, hs, out);
    if (trace || injected)
        fprintf(stderr, "  [phonon] iplHRTFCreate -> %d\n", r);
    return r;
}


// KL_TRACE_WMEMCHR=1 — Steam Audio picks its HRIR length by searching a table of
// supported sampling rates with std::find over an int array, which the compiler
// lowers to `wmemchr` (wchar_t is 32-bit on both sides). A miss there is not an
// error anywhere: the search returns the END pointer, the index equals the
// count, and the length that falls out is 1 — which is what libphonon then
// reports as `Unable to create PFFFT setup (size == 1)`, several frames and two
// call levels away from the lookup that actually failed.
//
// So the lookup is worth being able to SEE. Installed through kl_shim_override,
// which is consulted before any shim tier, and it forwards unchanged.
static wchar_t *(*g_real_wmemchr)(const wchar_t *, wchar_t, size_t);

static wchar_t *kl_trace_wmemchr(const wchar_t *s, wchar_t c, size_t n) {
    wchar_t *r = g_real_wmemchr(s, c, n);
    fprintf(stderr, "  [wmemchr] find %d in %zu entr%s ->%s", (int)c, n,
            n == 1 ? "y" : "ies", r ? " HIT at " : " MISS  [");
    if (r) fprintf(stderr, "%zu\n", (size_t)(r - s));
    else {
        for (size_t i = 0; i < n && i < 16; i++)
            fprintf(stderr, "%s%d", i ? ", " : "", (int)s[i]);
        fprintf(stderr, "]\n");
    }
    return r;
}

static void *kl_dl_shim_override(const char *name) {
    if (strcmp(name, "wmemchr") == 0 && g_real_wmemchr)
        return (void *)kl_trace_wmemchr;
    return NULL;
}

static void kl_dl_trace_shims(void) {
    if (!kl_env_on("KL_TRACE_WMEMCHR", 0)) return;
    g_real_wmemchr = wmemchr;
    kl_shim_override = kl_dl_shim_override;
}

static void *kl_dl_interpose(const char *name, void *real) {
    if (strcmp(name, "iplHRTFCreate") == 0) {
        g_real_iplHRTFCreate = (int32_t (*)(void *, kl_ipl_audio_settings *,
                                            void *, void *))real;
        return (void *)kl_trace_iplHRTFCreate;
    }
    return NULL;
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
