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
#include "guest/kl_driver.h"
#include "kl_jni.h"
#include "kl_opensl.h"
#include "kl_ovrp.h"
#include "kl_ovrplat.h"
#include "kl_mediandk.h"
#include "kl_vulkan.h"
#include "kl_aaudio.h"
#include "kl_ndk.h"
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
// are the same type: a mismatch across two TUs compiles silently and
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
    const char *base = basename_of(soname);
    pthread_mutex_lock(&g_lock);
    if (g_nimgs < KL_MAX_IMAGES) {
        snprintf(g_imgs[g_nimgs].soname, sizeof g_imgs[0].soname, "%s", base);
        g_imgs[g_nimgs].img = img;
        g_nimgs++;
    }
    pthread_mutex_unlock(&g_lock);
    // libgl4es IS the guest's GL whenever it is present — it is a GL 1.x-over-GLES
    // translator that only works if it sees the WHOLE gl* stream (kl_shim.c Tier
    // 4b). JKXR set this from its own loader; targets that pull gl4es in as a plain
    // DT_NEEDED (hl1's GLES3JNI engine, whose menu is immediate-mode glBegin/
    // glVertex2f/glOrtho) never did, so those calls fell through to ANGLE's
    // abort-stub and the menu drew nothing. Detecting the ONE unambiguous name
    // here — not searching for any GL library, which klepton.h warns breaks Steam
    // Link/VRChat/RE4 — routes them to gl4es for every driver.
    if (!strcmp(base, "libgl4es.so")) kl_shim_set_guest_gl(img);
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
// KL_TRACE_FMOD=1: interpose FMOD Studio's bank-load and event-start calls to
// answer the one question a silent FMOD guest (vampire) poses — do the banks
// LOAD and do events START? FMOD runs as real translated code and binds these
// against libfmodstudio at relocation time, so the only seam is here, as the
// import is resolved. Off by default (an indirection on every FMOD call is not
// something a normal run should carry); when on, each is logged a few times and
// forwarded to the real function, which is stored per-name at bind time.
static int kl_fmod_trace_default(void);   // defined below

static int (*g_fmod_loadBankMemory)(void *, const char *, int, int, uint32_t, void **);
static int (*g_fmod_loadBankFile)(void *, const char *, uint32_t, void **);
static int (*g_fmod_evtStart)(void *);
static int (*g_fmod_createInstance)(void *, void **);
static int (*g_fmod_studioCreate)(void **, uint32_t);
static int (*g_fmod_studioInit)(void *, int, uint32_t, uint32_t, void *);
static int (*g_fmod_setOutput)(void *, int);
static int (*g_fmod_mixerSuspend)(void *);
static int (*g_fmod_mixerResume)(void *);
static int (*g_fmod_busSetMute)(void *, int);
static int (*g_fmod_busSetVolume)(void *, float);

// Trace decision, taken at CALL time (not install time). The wrappers are now
// installed unconditionally the moment the FMOD symbol resolves — that is the
// only chance to catch it, and it happens during libUE4's relocation which can
// precede kl_driver_init() setting the target. By call time the target is always
// set, so the per-target default (vampire) resolves correctly here.
static int klf_trace(void) {
    static int on = -1;
    if (on < 0) {   // do NOT latch to the default before the target is known
        if (getenv("KL_TRACE_FMOD")) on = kl_env_on("KL_TRACE_FMOD", 0);
        else if (kl_driver_target_name()) on = kl_fmod_trace_default();
    }
    return on > 0;
}
static int klf_loadBankMemory(void *self, const char *buf, int len, int mode,
                              uint32_t flags, void **bank) {
    int r = g_fmod_loadBankMemory ? g_fmod_loadBankMemory(self, buf, len, mode, flags, bank) : -1;
    static int said; if (klf_trace() && said < 32) { said++;
        fprintf(stderr, "  [fmod] loadBankMemory(len=%d) -> result %d, bank %p\n",
                len, r, bank ? *bank : NULL); }
    return r;
}
static int klf_loadBankFile(void *self, const char *fn, uint32_t flags, void **bank) {
    int r = g_fmod_loadBankFile ? g_fmod_loadBankFile(self, fn, flags, bank) : -1;
    static int said; if (klf_trace() && said < 32) { said++;
        fprintf(stderr, "  [fmod] loadBankFile(\"%s\") -> result %d, bank %p\n",
                fn ? fn : "(null)", r, bank ? *bank : NULL); }
    return r;
}
static int klf_evtStart(void *self) {
    int r = g_fmod_evtStart ? g_fmod_evtStart(self) : -1;
    static unsigned n; if (klf_trace() && n < 32) { n++;
        fprintf(stderr, "  [fmod] EventInstance::start() -> result %d (event #%u)\n", r, n); }
    return r;
}
static int klf_createInstance(void *self, void **inst) {
    int r = g_fmod_createInstance ? g_fmod_createInstance(self, inst) : -1;
    static unsigned n; if (klf_trace() && n < 16) { n++;
        fprintf(stderr, "  [fmod] EventDescription::createInstance() -> result %d\n", r); }
    return r;
}
// Studio::System lifecycle — to see whether the plugin even brings the Studio
// system up (banks/events live under it). If create/initialize appear but no
// loadBankFile does, the plugin inits FMOD then declines to load banks (the game
// side is inert); if they never appear, the FMOD plugin itself never started.
static int klf_studioCreate(void **sys, uint32_t hdr) {
    int r = g_fmod_studioCreate ? g_fmod_studioCreate(sys, hdr) : -1;
    static int said; if (klf_trace() && !said) { said = 1;
        fprintf(stderr, "  [fmod] Studio::System::create() -> result %d, sys %p\n",
                r, sys ? *sys : NULL); }
    return r;
}
static int klf_studioInit(void *self, int max, uint32_t sflags, uint32_t cflags, void *ed) {
    int r = g_fmod_studioInit ? g_fmod_studioInit(self, max, sflags, cflags, ed) : -1;
    static int said; if (klf_trace() && !said) { said = 1;
        fprintf(stderr, "  [fmod] Studio::System::initialize(max=%d, sflags=0x%x, "
                "cflags=0x%x) -> result %d\n", max, sflags, cflags, r); }
    return r;
}
// Audio-control traces: is the game muting/suspending the mix, or picking a
// silent output driver? "Banks load + events start but the mix is all-zero"
// points here. setOutput's arg is FMOD_OUTPUTTYPE (2=NOSOUND, and the Android
// output types above it); mixerSuspend with no resume = a deliberately silenced
// mixer; Bus::setMute(true)/setVolume(0) on the master = a muted master bus.
static int klf_setOutput(void *self, int type) {
    int r = g_fmod_setOutput ? g_fmod_setOutput(self, type) : -1;
    if (klf_trace()) fprintf(stderr, "  [fmod] System::setOutput(type=%d) -> result %d\n", type, r);
    return r;
}
static int klf_mixerSuspend(void *self) {
    int r = g_fmod_mixerSuspend ? g_fmod_mixerSuspend(self) : -1;
    if (klf_trace()) fprintf(stderr, "  [fmod] System::mixerSuspend() -> result %d\n", r);
    return r;
}
static int klf_mixerResume(void *self) {
    int r = g_fmod_mixerResume ? g_fmod_mixerResume(self) : -1;
    if (klf_trace()) fprintf(stderr, "  [fmod] System::mixerResume() -> result %d\n", r);
    return r;
}
static int klf_busSetMute(void *self, int mute) {
    int r = g_fmod_busSetMute ? g_fmod_busSetMute(self, mute) : -1;
    if (klf_trace()) fprintf(stderr, "  [fmod] Bus::setMute(%d) -> result %d\n", mute, r);
    return r;
}
static int klf_busSetVolume(void *self, float v) {
    int r = g_fmod_busSetVolume ? g_fmod_busSetVolume(self, v) : -1;
    if (klf_trace()) fprintf(stderr, "  [fmod] Bus::setVolume(%.3f) -> result %d\n", (double)v, r);
    return r;
}
// Default the FMOD trace ON for titles whose audio is a confirmed FMOD-internal
// silence — vampire mixes 1700+ all-zero buffers though the whole OpenSL ->
// CoreAudio path is proven working (40 s played, 0 underruns), so the one thing
// left to see is whether its banks LOAD and its events START, which only this
// interposition can answer. On device the user cannot set env, so it has to be
// the default there; KL_TRACE_FMOD still overrides in either direction.
static int kl_fmod_trace_default(void) {
    static const char *on[] = { "vampire" };
    const char *t = kl_driver_target_name();
    if (t) for (unsigned i = 0; i < sizeof on / sizeof on[0]; i++)
        if (strcmp(t, on[i]) == 0) return 1;
    return 0;
}

static void *kl_fmod_interpose(const char *name, void *real) {
    // Install UNCONDITIONALLY when an FMOD symbol resolves — this is the only
    // moment we can substitute the address, and it happens during libUE4's
    // relocation, which can precede kl_driver_init() setting the target (that
    // timing is why the earlier on-gated versions never armed for vampire). The
    // wrappers merely forward and only LOG when klf_trace() says so, checked at
    // call time when the target is certainly set — so installing for every FMOD
    // title is harmless (an unmeasurable indirection on bank-load/event-start,
    // never a per-sample path). The one-time install line proves the seam caught
    // FMOD at all, independent of whether the game ever calls the function.
    if (!real) return NULL;
    void *wrap = NULL;
    if (!strcmp(name, "_ZN4FMOD6Studio6System14loadBankMemoryEPKci28FMOD_STUDIO_LOAD_MEMORY_MODEjPPNS0_4BankE")) {
        g_fmod_loadBankMemory = (int (*)(void *, const char *, int, int, uint32_t, void **))real;
        wrap = (void *)klf_loadBankMemory; }
    else if (!strcmp(name, "_ZN4FMOD6Studio6System12loadBankFileEPKcjPPNS0_4BankE")) {
        g_fmod_loadBankFile = (int (*)(void *, const char *, uint32_t, void **))real;
        wrap = (void *)klf_loadBankFile; }
    else if (!strcmp(name, "_ZN4FMOD6Studio13EventInstance5startEv")) {
        g_fmod_evtStart = (int (*)(void *))real;
        wrap = (void *)klf_evtStart; }
    else if (!strcmp(name, "_ZNK4FMOD6Studio16EventDescription14createInstanceEPPNS0_13EventInstanceE")) {
        g_fmod_createInstance = (int (*)(void *, void **))real;
        wrap = (void *)klf_createInstance; }
    else if (!strcmp(name, "_ZN4FMOD6Studio6System6createEPPS1_j")) {
        g_fmod_studioCreate = (int (*)(void **, uint32_t))real;
        wrap = (void *)klf_studioCreate; }
    else if (!strcmp(name, "_ZN4FMOD6Studio6System10initializeEijjPv")) {
        g_fmod_studioInit = (int (*)(void *, int, uint32_t, uint32_t, void *))real;
        wrap = (void *)klf_studioInit; }
    else if (!strcmp(name, "_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE")) {
        g_fmod_setOutput = (int (*)(void *, int))real;
        wrap = (void *)klf_setOutput; }
    else if (!strcmp(name, "_ZN4FMOD6System12mixerSuspendEv")) {
        g_fmod_mixerSuspend = (int (*)(void *))real;
        wrap = (void *)klf_mixerSuspend; }
    else if (!strcmp(name, "_ZN4FMOD6System11mixerResumeEv")) {
        g_fmod_mixerResume = (int (*)(void *))real;
        wrap = (void *)klf_mixerResume; }
    else if (!strcmp(name, "_ZN4FMOD6Studio3Bus7setMuteEb")) {
        g_fmod_busSetMute = (int (*)(void *, int))real;
        wrap = (void *)klf_busSetMute; }
    else if (!strcmp(name, "_ZN4FMOD6Studio3Bus9setVolumeEf")) {
        g_fmod_busSetVolume = (int (*)(void *, float))real;
        wrap = (void *)klf_busSetVolume; }
    if (wrap)
        // Unconditional (once per symbol): a run always shows the seam fired even
        // if the game never calls the function — which, for a silent FMOD title
        // that loads no banks, is itself the answer.
        fprintf(stderr, "  [fmod] interpose installed for %s\n", name);
    return wrap;
}

void *kl_guest_sym_global(const char *name) {
    void *v = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nimgs && !v; i++)
        v = kl_sym(g_imgs[i].img, name);
    pthread_mutex_unlock(&g_lock);
    if (v) { void *w = kl_fmod_interpose(name, v); if (w) return w; }
    return v;
}

// Load an .so together with its DT_NEEDED chain, dependencies first, so each
// image's relocation-time imports can bind against the images it needs
// (kl_guest_sym_global). Names with no file in the library path are assumed
// shim-served (libc/libm/libdl/liblog/... or a synthetic gateway) and skipped.
// Names Klepton serves from its own shims rather than from a bundled file, so
// a missing file for one of these is expected, not a packaging error.
static int kl_dep_is_shim_served(const char *name) {
    static const char *shim[] = {
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libstdc++.so",
        "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libEGL.so",
        "libandroid.so", "libOpenSLES.so", "libaaudio.so", "libvulkan.so",
        "libz.so", "libjnigraphics.so", "libnativewindow.so", "libmediandk.so",
        "libamidi.so", "libcamera2ndk.so", "libnativehelper.so",
        // Served synthetically by Klepton's own shims / replaced outright, so
        // there is deliberately no file for them and their symbols bind through
        // kl_guest_sym_global — not a packaging gap.
        "libOVRPlugin.so", "libovrplatformloader.so", "libvrapi.so",
        "libopenxr_loader.so", "libOpenMAXAL.so",
    };
    for (unsigned i = 0; i < sizeof shim / sizeof shim[0]; i++)
        if (!strcmp(name, shim[i])) return 1;
    return 0;
}

// FMOD's Android platform layer (the AudioTrack/AudioManager output, and the
// JavaVM it needs for them) is set up in libfmod's JNI_OnLoad — which on Android
// is called by System.loadLibrary("fmod"). Here libfmod is pulled in through
// libUE4's DT_NEEDED chain and loaded eagerly, so only its DT_INIT runs, never
// JNI_OnLoad; FMOD then has no platform object and FMOD::getGlobals returns
// FMOD_ERR_INTERNAL (28), which cascades to Studio::System::create failing and
// the whole game going silent (vampire: create -> 28, no banks, no events).
// Disassembling libfmod pinned it to a null object at getGlobals+0xbc that only
// JNI_OnLoad populates. So call it once, with the synthetic JavaVM, for the FMOD
// core library. Targeted by basename: libfmodstudio exports no JNI_OnLoad (a
// no-op lookup), and no other DT_NEEDED lib is auto-driven this way.
static void kl_maybe_jni_onload(const char *path, kl_image *img) {
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    if (strncmp(b, "libfmod", 7) != 0) return;
    int (*onload)(void *, void *) = (int (*)(void *, void *))kl_sym(img, "JNI_OnLoad");
    if (!onload) return;
    int r = onload(kl_jni_vm(), NULL);
    fprintf(stderr, "  [klepton] %s JNI_OnLoad(vm) -> 0x%x (FMOD Android platform "
            "init; without this FMOD::getGlobals fails INTERNAL and audio is silent)\n",
            b, r);
}

kl_image *kl_load_recursive(const char *path) {
    kl_image *have = kl_find_image(path);
    if (have) return have;                       // cycle guard / already loaded

    char names[32][128];
    int nn = kl_list_needed(path, names, 32);
    if (kl_env_on("KL_TRACE_NEEDED", 1)) {
        fprintf(stderr, "  [dl] %s: %d DT_NEEDED\n", path, nn);
        for (int i = 0; i < nn; i++) fprintf(stderr, "  [dl]     needs %s\n", names[i]);
        fflush(stderr);
    }
    for (int i = 0; i < nn; i++) {
        // The OpenXR loader ships as a real file, and some guests
        // pull it in through DT_NEEDED rather than dlopen. Loading it would run
        // the real Khronos loader, which then hunts for an Android runtime
        // broker that does not exist here and fails the app's OpenXR init with
        // no call ever reaching us. Treat it as shim-served exactly as the
        // dlopen door does (kl_openxr_dlopen): skip the file so its xr* imports
        // bind to the synthetic runtime through kl_guest_sym_global.
        if (kl_openxr_claims(names[i])) {
            if (kl_env_on("KL_TRACE_NEEDED", 1))
                fprintf(stderr, "  [dl]     %s -> openxr-served (skip)\n", names[i]);
            continue;
        }
        // Shim-served libraries (libGLES*/libEGL/libvulkan/libc/liblog/...) are
        // provided by Klepton's own gateways and must NOT be loaded as guest
        // images — their imports bind through kl_guest_sym_global to the shims,
        // exactly as the dlopen door serves them. Checked BEFORE kl_can_load
        // because a host framework of the same name may exist in the app bundle
        // (ANGLE ships libGLESv2.framework for Klepton's own GL): kl_can_load
        // would then call it "loadable" and we would try to load the host ANGLE
        // dylib as a guest image, which has no __TEXT,__klelf section and fails —
        // taking the dependent plugin down with it. That is exactly how Steam
        // Link's Qt "virtual" platform plugin (DT_NEEDED libGLESv2.so) died.
        if (kl_dep_is_shim_served(names[i])) {
            if (kl_env_on("KL_TRACE_NEEDED", 1))
                fprintf(stderr, "  [dl]     %s -> shim-served (skip)\n", names[i]);
            continue;
        }
        char full[1024];
        if (strchr(names[i], '/')) snprintf(full, sizeof full, "%s", names[i]);
        else snprintf(full, sizeof full, "%s/%s", g_libdir, names[i]);
        // Framework-aware, NOT a raw access() on "<dir>/libfoo.so": on device the
        // guest libraries ship as translated frameworks (libfoo.framework/libfoo),
        // so a plain access() finds NONE of them and every bundled dependency is
        // silently skipped — which is how Wrath2 lost libfmod (its FMOD imports
        // went unresolved and the first FMOD call aborted in kl_unresolved_named).
        // kl_can_load resolves the framework/dylib the same way kl_load_auto will.
        if (!kl_can_load(full)) {
            // Shim-served names already continued above, so anything reaching here
            // that is not loadable is a genuine packaging gap worth naming.
            fprintf(stderr, "  [klepton] DT_NEEDED %s of %s is neither loadable "
                    "nor shim-served — its imports will be UNRESOLVED\n",
                    names[i], path);
            continue;
        }
        if (kl_env_on("KL_TRACE_NEEDED", 1))
            fprintf(stderr, "  [dl]     %s -> loading (%s)\n", names[i], full);
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
    kl_maybe_jni_onload(path, img);
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

// Every guest image and where it landed, one line each.
//
// **A guest pc is meaningless without this.** The libraries are mapped wherever
// the kernel put them, so a crash address, an `lldb` breakpoint on a guest
// function, and a `sample` profile all need the base to be subtracted before
// any of them can be matched against a disassembly of the .so — and re-deriving
// it from a fault report only works if there was a fault. Printed once at boot
// so the number is in the log before it is needed rather than after.
void kl_dl_report_images(FILE *f) {
    if (!f) return;
    fprintf(f, "=== guest image map (subtract the base to disassemble) ===\n");
    for (int i = 0; i < g_nimgs; i++) {
        const void *base = kl_base(g_imgs[i].img);
        if (!base) continue;
        fprintf(f, "  %-24s %p .. %p  (%zu KiB)\n", g_imgs[i].soname, base,
                (const char *)base + kl_span(g_imgs[i].img),
                kl_span(g_imgs[i].img) / 1024);
    }
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
// libraries that do not exist here. On the host that stayed
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
// Exactly the same bug as the "Failed to load Il2CPP." stat (see kl_can_load),
// one library over and one cause deeper: that one is cured by asking
// kl_can_load instead of stat(), and this is what kl_can_load itself cannot
// see.
static int kl_core_shim_claims(const char *path);
int kl_can_dlopen(const char *path) {
    if (!path) return 0;
    return kl_egl_claims(path)  || kl_opensl_claims(path)   || kl_ovrp_claims(path) ||
           kl_ovrplat_claims(path) || kl_mediandk_claims(path) ||
           kl_vulkan_claims(path) || kl_aaudio_claims(path) ||
           kl_ndk_claims(path)    ||
           kl_openxr_claims(path) || kl_core_shim_claims(path) || kl_can_load(path);
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
// Per-target default dlopen refusals: guest plugins that trip visionOS AMFI
// (an uncatchable SIGKILL the instant the dylib's code is executed — a capture
// or telemetry SDK mapping/patching executable pages) and are not essential to
// running the app. Baked in per target so no KL_DLOPEN_REFUSE env is needed;
// KL_DLOPEN_REFUSE still adds to these. Each was identified by the app dying
// with a bare signal 9 immediately after the named dylib loaded.
static int kl_target_default_refused(const char *path) {
    const char *t = kl_driver_target_name();
    if (!path) return 0;
    // GLOBAL AMFI-trippers: Meta SDK native plugins that map/patch executable
    // pages and take an uncatchable SIGKILL the instant their init runs, on
    // EVERY guest that ships them — not gameplay (spatial audio, telemetry,
    // metrics). Refused by name regardless of target, because "which guest"
    // was never the discriminator; the library is. ZIX (Unity 6) died in
    // kl_run_init on libMetaXRAudioUnity exactly as missioniss did, and the
    // same set rides in atf/intoblack/etc., so this stops chasing it per
    // target. A missing optional audio/telemetry plugin is a load the guest
    // already tolerates.
    if (t) {
        static const char *const global[] = {
            "MetaXRAudioUnity", "SDKTelemetry", "OVRMetricsTool",
            "ConstellusUnityPlugin", "plugin_hmd_capture",
        };
        for (unsigned i = 0; i < sizeof global / sizeof global[0]; i++)
            if (strstr(path, global[i])) {
                static const char *gsaid[8]; static unsigned gn;
                int seen = 0;
                for (unsigned k = 0; k < gn; k++) if (gsaid[k] == global[i]) { seen = 1; break; }
                if (!seen && gn < 8) {
                    gsaid[gn++] = global[i];
                    fprintf(stderr, "  [klepton] guest dlopen(\"%s\") REFUSED (global "
                                    "default: '%s' is a Meta SDK plugin that trips "
                                    "visionOS AMFI) — the guest sees a library that is "
                                    "not installed\n", path, global[i]);
                }
                return 1;
            }
    }
    // KL_REFUSE_EOS=1 (A/B, default off): refuse Epic Online Services. zix
    // stalls forever entering its first map after "EOSSDKComponent inited",
    // polling Horizon messages with "[EOS SDK] ... local user being null" — the
    // shape of a login/session async that can never complete here. Refusing the
    // load makes the component fail its init and the game take its offline
    // path; whether zix tolerates that is exactly what the A/B answers. Not a
    // per-target default until it proves out.
    if (kl_env_on("KL_REFUSE_EOS", 0) && strstr(path, "libEOSSDK")) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [klepton] guest dlopen(\"%s\") REFUSED "
                            "(KL_REFUSE_EOS=1) — the guest sees a library that "
                            "is not installed\n", path);
        return 1;
    }
    if (!t) return 0;
    struct { const char *target, *sub; } d[] = {
        // (plugin_hmd_capture, ConstellusUnityPlugin, MetaXRAudioUnity,
        // SDKTelemetry, OVRMetricsTool are refused GLOBALLY above — they trip
        // AMFI on every guest, not just the one they were first seen on.)
        //
        // Not AMFI here but the same treatment for the same reason: Meta's FBNS
        // push-notification client (folly-based) throws an UNCAUGHT exception on
        // any network failure — online it chokes parsing a resolved Facebook
        // IPv6, offline it throws std::system_error on the failed DNS — and takes
        // the whole process down (SIGABRT). It is push notifications, not
        // gameplay, and Asgard's Wrath 2 only starts it once it believes a user
        // is signed in (KL_PLAT_USER). Refusing the load lets the game skip it.
        { "wrath2",     "libFbnsPlugin"      },   // Meta push (FBNS) — aborts offline and on
    };
    for (unsigned i = 0; i < sizeof d / sizeof d[0]; i++)
        if (strcmp(t, d[i].target) == 0 && strstr(path, d[i].sub)) {
            static const char *said[16]; static unsigned nsaid;
            int seen = 0;
            for (unsigned k = 0; k < nsaid; k++) if (said[k] == d[i].sub) { seen = 1; break; }
            if (!seen && nsaid < 16) {
                said[nsaid++] = d[i].sub;
                fprintf(stderr, "  [klepton] guest dlopen(\"%s\") REFUSED (per-target "
                                "default for '%s': '%s' trips visionOS AMFI) — the guest "
                                "sees a library that is not installed\n", path, t, d[i].sub);
            }
            return 1;
        }
    return 0;
}

static int kl_dlopen_refused(const char *path) {
    if (!path) return 0;
    // Baked per-target defaults are checked FIRST and unconditionally — they
    // must fire even when no KL_DLOPEN_REFUSE env var is set, which is the whole
    // point of baking them in.
    if (kl_target_default_refused(path)) return 1;
    static const char *list;
    static int inited;
    if (!inited) { inited = 1; list = kl_env_str("KL_DLOPEN_REFUSE", NULL); }
    if (!list || !*list) return 0;
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

// The Android system libraries that are ALWAYS resident on a device. Their
// DT_NEEDED edges are already "shim-served (skip)" (kl_load_recursive), but an
// explicit runtime dlopen() by name is a different door: there is no file to
// open, so it fell through to kl_load_recursive, failed, and returned NULL.
//
// That NULL is never what a device gives. On Android these are loaded before the
// first guest instruction runs, so dlopen("libc.so") returns a live handle and
// dlsym() off it resolves through libc — which here is exactly the shim table.
// Missioniss booted into libunity's pthread_once lazy-init, which dlopen()s
// "libc.so" to cache a libc entry point; the NULL both broke that init AND drove
// the failure-diagnostic frame walk into libunity's chainless .text (the 0x7
// fault). Serving the handle removes both: klb_dlsym routes it to the shim
// lookup + global image search, the same resolution a NULL/RTLD_DEFAULT handle
// gets, so a name the shim serves comes back and one it does not returns a clean
// dlerror instead of a crash.
//
// Matched by basename so a full path into the guest lib dir claims too. Not
// gated: returning a handle for a system library that is definitionally present
// is correct on every target, and nothing here changes a symbol's answer.
static const char *const kl_core_shim_libs[] = {
    "libc.so", "libdl.so", "libm.so", "liblog.so", "libandroid.so",
    "libz.so", "libc++.so", "libc++_shared.so", "libstdc++.so",
};
static const int g_core_shim_handle = 0;             // address is the sentinel

static int kl_core_shim_claims(const char *path) {
    if (!path) return 0;
    // Unity ONLY. libunity explicitly dlopen()s "libc.so" and NULL crashes it
    // (see above). UE4/UE5 guests (olar, wrath2, hl2) also dlopen these system
    // libs but were FINE with the old dlopen->NULL — serving them a handle made
    // their dlsym-through-it resolve libc/pthread differently and DEADLOCK them
    // (olar/wrath2 hung in cond_wait). Restrict the handle to the guest kind that
    // needs it so UE titles keep their proven-working NULL behavior.
    extern kl_guest_kind kl_driver_kind(void);
    if (kl_driver_kind() != KL_GUEST_UNITY) return 0;
    const char *b = basename_of(path);
    for (size_t i = 0; i < sizeof kl_core_shim_libs / sizeof kl_core_shim_libs[0]; i++)
        if (strcmp(b, kl_core_shim_libs[i]) == 0) return 1;
    return 0;
}

static int kl_core_shim_is_handle(void *h) { return h == (void *)&g_core_shim_handle; }

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
        const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
        snprintf(g_dlerr, sizeof g_dlerr, "dlopen failed: library \"%s\" not found", base);
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
    // libandroid.so, the same shape as the AAudio line above: FMOD's Android
    // file layer dlopens it by name rather than importing it, and there is no
    // libandroid.so on disk to fall through to. See kl_ndk_dlopen.
    void *ndk = kl_ndk_dlopen(path);
    if (ndk) return ndk;
    // The OpenXR loader. This one is NOT like the lines above it: the file
    // really is in the guest tree, so this must come first to keep the real
    // Khronos loader from loading successfully and then failing at the Android
    // runtime broker. See kl_openxr.c.
    void *xrl = kl_openxr_dlopen(path);
    if (xrl) return xrl;
    // The always-resident Android system libraries (libc/libdl/libm/...). No file
    // to open; served with a handle whose dlsym routes to the shim table. Must
    // come before the file-load fallthrough, which would fail and hand back NULL.
    if (kl_core_shim_claims(path)) {
        fprintf(stderr, "  [klepton] guest dlopen(\"%s\") -> shim-served handle\n", path);
        return (void *)&g_core_shim_handle;
    }
    pthread_mutex_lock(&g_lock);
    kl_image *found = kl_find_image(path);           // already loaded? refcount is coarse
    pthread_mutex_unlock(&g_lock);
    if (found) return found;

    char full[1024];
    if (strchr(path, '/')) snprintf(full, sizeof full, "%s", path);
    else                   snprintf(full, sizeof full, "%s/%s", g_libdir, path);

    kl_image *img = kl_load_recursive(full);
    if (!img) {
        // Keep dlerror() SHORT and Android-shaped: a guest's own load-failure
        // path copies this into a fixed buffer sized for Android's terse
        // "dlopen failed: library "libX.so" not found" — Half-Life's lambda1vr
        // (trying an absent libvgui_support.so) does, and our old full-container-
        // path spelling (~150 chars) overflowed its 1024-byte buffer into a
        // __strlen_chk abort. The full detail still goes to the log below.
        const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
        snprintf(g_dlerr, sizeof g_dlerr, "dlopen failed: library \"%s\" not found", base);
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

// KL_TRACE_DLSYM=1: name every dlsym that comes back NULL, with the guest image
// that asked. A NULL returned here and then CALLED is the jump-to-0x0 AMFI kills
// as an "Invalid Page" (OLAR) — uncatchable, so the crash report cannot name the
// culprit, but the last line this prints before the kill does.
// Default the dlsym-NULL trace ON for titles stuck in the jump-to-0x0 SIGKILL
// with nothing else naming the culprit — olar dies right after libmrutilitykit
// shared loads, and on device the user cannot set env, so it has to be the
// default there. KL_TRACE_DLSYM still overrides either way.
static int kl_dlsym_trace_default(void) {
    static const char *on[] = { "olar" };
    const char *t = kl_driver_target_name();
    if (t) for (unsigned i = 0; i < sizeof on / sizeof on[0]; i++)
        if (strcmp(t, on[i]) == 0) return 1;
    return 0;
}
static void *klb_dlsym_null(const char *name) {
    if (kl_env_on("KL_TRACE_DLSYM", kl_dlsym_trace_default())) {
        size_t off = 0;
        const char *who = kl_addr_image(__builtin_return_address(0), &off);
        fprintf(stderr, "  [dlsym] '%s' -> NULL (undefined) — asked from %s+0x%zx; "
                        "calling this is a jump-to-0x0\n",
                name ? name : "(null)", who ? who : "(host)", off);
    }
    return NULL;
}

void *klb_dlsym(void *handle, const char *name) {
    if (kl_egl_is_handle(handle)) return kl_egl_sym(name);
    if (kl_opensl_is_handle(handle)) return kl_opensl_sym(name);
    if (kl_ovrp_is_handle(handle)) return kl_ovrp_sym(name);
    if (kl_ovrplat_is_handle(handle)) return kl_ovrplat_sym(name);
    if (kl_mediandk_is_handle(handle)) return kl_mediandk_sym(name);
    if (kl_vulkan_is_handle(handle)) return kl_vulkan_sym(name);
    if (kl_aaudio_is_handle(handle)) return kl_aaudio_sym(name);
    if (kl_ndk_is_handle(handle)) return kl_ndk_sym(name);
    if (kl_openxr_is_handle(handle)) return kl_openxr_sym(name);
    // A core system-library handle (libc.so &c.) resolves like RTLD_DEFAULT: the
    // shim serves libc, and the global image search covers anything a guest .so
    // exported. A name neither has is a clean undefined-symbol dlerror, not a
    // fault — which is the whole point of serving the handle rather than NULL.
    if (kl_core_shim_is_handle(handle)) {
        void *s = kl_shim_lookup(name);
        if (s) return s;
        s = kl_guest_sym_global(name);
        if (!s) snprintf(g_dlerr, sizeof g_dlerr, "klepton: undefined symbol: %s", name);
        return s;
    }
    if (handle == NULL || handle == (void *)-1) {    // RTLD_DEFAULT / RTLD_NEXT
        void *s = kl_shim_lookup(name);
        if (s) return s;
        for (int i = 0; i < g_nimgs; i++) {
            void *v = kl_sym(g_imgs[i].img, name);
            if (v) return v;
        }
        snprintf(g_dlerr, sizeof g_dlerr, "klepton: undefined symbol: %s", name);
        return klb_dlsym_null(name);
    }
    // Anything left is meant to be a real loaded image. Validate it before
    // dereferencing: a guest's linker-hook init probes dlsym(handle=1,
    // "__loader_android_*") for bionic dynamic-linker internals that do not
    // exist here, and casting an invented handle to kl_image* faults inside
    // kl_sym. A NULL return is the honest "no such symbol" the caller handles.
    int known = 0;
    for (int i = 0; i < g_nimgs; i++)
        if ((void *)g_imgs[i].img == handle) { known = 1; break; }
    if (!known) {
        snprintf(g_dlerr, sizeof g_dlerr,
                 "klepton: dlsym on unknown handle %p (symbol %s)", handle, name);
        return NULL;
    }
    void *v = kl_sym((kl_image *)handle, name);
    if (!v) { snprintf(g_dlerr, sizeof g_dlerr, "klepton: undefined symbol: %s", name);
              return klb_dlsym_null(name); }
    { void *w = kl_dl_interpose(name, v); if (w) return w; }
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
