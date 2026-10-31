// libvulkan.so over MoltenVK — see kl_vulkan.h for what this is and why.
//
// The real Vulkan headers are used rather than transcribed, which is the
// opposite of kl_openxr's choice next door, and the reason is that the headers
// are already a build dependency here: MoltenVK ships them in the same tarball
// as the dylib, and there is no Vulkan path at all without that dylib. So
// transcribing would add a whole class of silent layout bug (a 182-entry-point
// API, structs up to ~100 fields) to buy exactly nothing.
//
// The __has_include guard keeps `make check` honest on a bare checkout: with no
// MoltenVK vendored this file still compiles, claims nothing, and says so BY
// NAME the moment a guest asks for Vulkan. That is the same shape as ANGLE not
// being built — a missing optional dependency should read as a missing
// dependency, not as a guest that mysteriously fails to render.
#include <dlfcn.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "kl_vulkan.h"
#include "kl_env.h"
#include "kl_driver.h"   // kl_driver_target_name() — per-target GPU identity spoof
#include "kl_fove.h"     // fixed foveation (the eye-pass recognition below)

// Matched on with a prefix test rather than equality: the guest asks by plain
// soname, but a path can arrive too (kl_dl.c hands through whatever the guest
// wrote, and BONELAB's plugin loader builds absolute paths).
static int name_is_vulkan(const char *p) {
    if (!p) return 0;
    const char *slash = strrchr(p, '/');
    const char *base = slash ? slash + 1 : p;
    return !strcmp(base, "libvulkan.so") || !strcmp(base, "libvulkan.so.1") ||
           !strcmp(base, "vulkan");
}

#if !__has_include(<vulkan/vulkan.h>)

// ---------------------------------------------------------------------------
// No MoltenVK vendored. Refuse by name, once, and let the guest take whatever
// path it takes when Vulkan is absent — which for BONELAB is the hardware
// requirements warning, i.e. exactly the pre-existing behaviour.
// ---------------------------------------------------------------------------
static void complain_once(void) {
    static int said;
    if (said++) return;
    fprintf(stderr,
            "  [vk] libvulkan.so requested, but MoltenVK is not vendored.\n"
            "  [vk] Run 'make mvk' (see BUILDING.md); this build has no Vulkan.\n");
}
int   kl_vulkan_claims(const char *n) { if (name_is_vulkan(n)) complain_once(); return 0; }
void *kl_vulkan_dlopen(const char *n) { if (name_is_vulkan(n)) complain_once(); return NULL; }
int   kl_vulkan_is_handle(const void *h) { (void)h; return 0; }
void *kl_vulkan_sym(const char *n) { (void)n; return NULL; }
void *kl_vulkan_lookup(const char *n) { (void)n; return NULL; }
void kl_vulkan_xr_image_capture(unsigned long long i, unsigned w, unsigned h, long long f,
                                unsigned a, unsigned fr, int e, int l) {
    (void)i; (void)w; (void)h; (void)f; (void)a; (void)fr; (void)e; (void)l;
}
void kl_vulkan_xr_image_capture_raw(unsigned long long i, unsigned w, unsigned h, unsigned bpp,
                                    int depth, unsigned a, const char *path) {
    (void)i; (void)w; (void)h; (void)bpp; (void)depth; (void)a; (void)path;
}
int   kl_vulkan_available(void) { return 0; }
void  kl_vulkan_stats(unsigned *p, unsigned *c) { if (p) *p = 0; if (c) *c = 0; }
int   kl_vulkan_guest_active(void) { return 0; }
unsigned long long kl_vulkan_eye_image(int s, int e, unsigned w, unsigned h, int srgb) {
    (void)s; (void)e; (void)w; (void)h; (void)srgb; return 0;
}
// Was missing, and it is what kl_ovrp.c actually calls — so a checkout that has
// never run `make mvk` did not fail to *compile*, it failed to LINK, which is
// the one failure this whole `#if` exists to prevent. Every entry point in the
// header belongs here.
unsigned long long kl_vulkan_eye_image_layers(int s, int e, unsigned w, unsigned h,
                                             int srgb, int layers) {
    (void)s; (void)e; (void)w; (void)h; (void)srgb; (void)layers; return 0;
}
unsigned long long kl_vulkan_layer_image(int k, int s, int e, unsigned w, unsigned h,
                                         int fmt, int layers) {
    (void)k; (void)s; (void)e; (void)w; (void)h; (void)fmt; (void)layers; return 0;
}
unsigned long long kl_vulkan_spacewarp_image(int depth, int s, unsigned w, unsigned h,
                                             int layers) {
    (void)depth; (void)s; (void)w; (void)h; (void)layers; return 0;
}
void *kl_vulkan_layer_mtl_texture(int k, int s, int e, int *w, int *h) {
    (void)k; (void)s; (void)e; if (w) *w = 0; if (h) *h = 0; return NULL;
}
void *kl_vulkan_spacewarp_mtl(int depth, int s, int *w, int *h, int *layers) {
    (void)depth; (void)s; if (w) *w = 0; if (h) *h = 0; if (layers) *layers = 0; return NULL;
}
// These two are referenced only by the visionOS compositor, so the host gates
// never missed them — the device link did.
void *kl_vulkan_mtl_for_handle(unsigned long long h) { (void)h; return NULL; }
void *kl_vulkan_wild_scratch_mtl(int *w, int *h) { if (w) *w = 0; if (h) *h = 0; return NULL; }
void  kl_vulkan_layer_rekey(int old_key, int new_key) { (void)old_key; (void)new_key; }
void  kl_vulkan_capture_eyes(unsigned f, int s) { (void)f; (void)s; }
void  kl_vulkan_capture_layers(void) {}
// No Vulkan instance, so no LUID: out stays untouched, as for a GLES guest.
int   kl_vulkan_display_luid(uint8_t out[8]) { (void)out; return 0; }
int   kl_vulkan_gpu_ids(unsigned *v, unsigned *d) { (void)v; (void)d; return 0; }
void *kl_vulkan_display_phys(void) { return NULL; }
void  kl_vulkan_frame_done(int s) { (void)s; }
int   kl_vulkan_frame_mark(void) { return 0; }
int   kl_vulkan_frame_wait(unsigned ms) { (void)ms; return 0; }
void  kl_vulkan_frame_publish(void) { }
unsigned long long kl_vulkan_frame_serial(void) { return 0; }
// The OpenXR seam. Answering "not supported" here is what keeps
// XR_KHR_vulkan_enable OUT of the advertised extension list on a checkout with
// no MoltenVK, so a Vulkan OpenXR guest is refused at xrCreateInstance — where
// the guest's own log names the extension — instead of somewhere further in.
int   kl_vulkan_xr_supported(void) { return 0; }
const char *kl_vulkan_xr_instance_extensions(void) { return ""; }
const char *kl_vulkan_xr_device_extensions(void) { return ""; }
void *kl_vulkan_xr_physical_device(void *vi) { (void)vi; return NULL; }
void  kl_vulkan_xr_api_range(unsigned *mmaj, unsigned *mmin,
                             unsigned *xmaj, unsigned *xmin) {
    if (mmaj) *mmaj = 0; if (mmin) *mmin = 0;
    if (xmaj) *xmaj = 0; if (xmin) *xmin = 0;
}
unsigned long long kl_vulkan_xr_image(unsigned w, unsigned h, unsigned l,
                                      unsigned m, long long f, int depth) {
    (void)w; (void)h; (void)l; (void)m; (void)f; (void)depth; return 0;
}
void *kl_vulkan_xr_image_mtl(unsigned long long img) { (void)img; return NULL; }
void  kl_vulkan_xr_image_not_eye(unsigned long long img) { (void)img; }
int   kl_vulkan_fove_indirect_selftest(void) { return -1; }
// Not an entry point but a variable, read by kl_mtl_foveate.m (part of every
// runtime link) and declared there, not in the header — so "every entry point
// in the header" above does not cover it, and its absence
// here was again a LINK failure on a checkout with no MoltenVK. Zero: no Vulkan
// pass ever runs, so there is never a drawn area to centre a fovea in.
uint32_t g_klvk_scene_view_w, g_klvk_scene_view_h;

#else  /* MoltenVK headers present */

#define VK_NO_PROTOTYPES
// The Android WSI half of the header, which is what `vkCreateAndroidSurfaceKHR`
// and its create-info live in. Safe to switch on with no Android SDK anywhere:
// vulkan_android.h forward-declares `struct ANativeWindow` and `struct
// AHardwareBuffer` itself and includes nothing. Transcribing the struct instead
// would have been four fields of avoidable risk.
#define VK_USE_PLATFORM_ANDROID_KHR 1
// ...and the Metal half, for `VK_EXT_metal_objects` — how the MTLTexture behind
// an eye VkImage is reached and handed to the compositor. Equally safe in plain
// C: vulkan_metal.h typedefs every Metal handle to `void *` unless __OBJC__ is
// defined, which it is not here.
#define VK_USE_PLATFORM_METAL_EXT 1
#include <vulkan/vulkan.h>
#include "kl_glfb.h"

#include <zlib.h>

#define KLVK_MAX_IMAGES 8

static int vk_trace(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_TRACE", 0);
    return on;
}
#define VKT(...) do { if (vk_trace()) fprintf(stderr, "  [vk] " __VA_ARGS__); } while (0)
#define VKI(...) fprintf(stderr, "  [vk] " __VA_ARGS__)

// ---------------------------------------------------------------------------
// Loading MoltenVK
// ---------------------------------------------------------------------------
//
// Two shapes, for the same reason ANGLE has two (kl_glfb.c's angle_dlopen): a
// bare dylib is what the host stages, and an app may only load code from inside
// its own bundle on visionOS, where it is a .framework. Try both rather than
// making the caller know which.
#define MVK_VENDORED_DIR "vendor-moltenvk/out/macos"

static void *mvk_try(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) VKT("dlopen(%s): %s\n", path, dlerror());
    return h;
}

static void *mvk_open(void) {
    const char *explicit = kl_env_str("KL_MVK_DYLIB", NULL);
    if (explicit) return mvk_try(explicit);

    const char *dir = kl_env_str("KL_MVK_DIR", MVK_VENDORED_DIR);
    char path[1024];
    snprintf(path, sizeof path, "%s/libMoltenVK.dylib", dir);
    void *h = mvk_try(path);
    if (h) return h;
    snprintf(path, sizeof path, "%s/MoltenVK.framework/MoltenVK", dir);
    if ((h = mvk_try(path))) return h;
    // The bundle case: @rpath resolves to <exec dir>/Frameworks, which is where
    // an embedded framework lands and the ONLY place AMFI will accept code from.
    if ((h = mvk_try("@rpath/MoltenVK.framework/MoltenVK"))) return h;
    return mvk_try("MoltenVK.framework/MoltenVK");
}

static void *g_mvk;
static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkGetDeviceProcAddr   real_gdpa;
static int klvk_logic_op_on(void);   // KL_VK_LOGIC_OP, defined with klvk_feature_surgery

// Pipeline-object hooks (kl_vk_pipe_hooks.h): an add-on records every sampler,
// layout, shader module, render pass and pipeline the guest creates, and
// replays recorded ones before the guest asks for them (kl_fossilize.cpp).
#include "kl_vk_pipe_hooks.h"
#include "kl_vk_eyefix.h"
static const klvk_pipe_hooks *g_pipe_hooks;
static __thread int t_pipe_replay;   // this thread belongs to the replayer
static __thread int t_ap_worker;     // ...to the compiles behind the frame (KL_VK_ASYNC_PIPES)
void klvk_set_pipe_hooks(const klvk_pipe_hooks *h) { g_pipe_hooks = h; }
void klvk_set_replay_thread(int on) { t_pipe_replay = on; }
#define PIPE_HOOK(fn, ...) do { if (g_pipe_hooks && g_pipe_hooks->fn && !t_pipe_replay) g_pipe_hooks->fn(__VA_ARGS__); } while (0)
static PFN_vkVoidFunction VKAPI_CALL klvk_GetDeviceProcAddr(VkDevice dev, const char *name);
// kl_fossilize.cpp, which is C++ and so is not in every binary this file is:
// the mains that are one cc command link without it, and get this. Where it
// is linked, its own definition is the one that counts.
__attribute__((weak)) void kl_fossilize_install(void) {}
static int g_init_done, g_avail;

static void *mvk_sym(const char *n) { return g_mvk ? dlsym(g_mvk, n) : NULL; }

static void vk_init(void) {
    if (g_init_done) return;
    g_init_done = 1;
    kl_fossilize_install();   // KL_VK_FOZ_RECORD / _REPLAY

    // MoltenVK logs its whole device census at info level, which buries
    // everything this port prints. Same choice as tests/t_mvk.c, same escape.
    if (!kl_env_on("KL_MVK_VERBOSE", 0)) setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);

    // UE Vulkan titles (wanderer, wrath2) pay a long boot PSO-precompile wall:
    // every pipeline goes through MoltenVK's SPIRV->MSL compiler (~300 ms each,
    // hundreds of them, SERIALIZED) before the guest submits its first XR layer —
    // the headset shows black until warmup completes, and with no on-disk cache it
    // recompiles from scratch every boot. Let MoltenVK spread the compiles across
    // its concurrent-compilation pool (MVKConfiguration.shouldMaximizeConcurrent
    // Compilation). Render correctness is unchanged; global + env-overridable.
    if (kl_env_on("KL_MVK_CONCURRENT_COMPILE", 1))
        setenv("MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION", "1", 0);

    // wrath2: exactly one UE4 warmup pipeline (~#5251, a tessellation PSO) wedges
    // MoltenVK's Metal compiler forever — MVKMetalCompiler::compile blocks in
    // std::condition_variable::wait_until on a newRenderPipelineState completion
    // handler that never fires, so RenderThread never returns from vkCreateGraphics
    // Pipelines and the whole UE4 TaskGraph freezes (frozen conds, compositor
    // layers=0). metalCompileTimeout defaults to infinite; a finite bound makes the
    // doomed compile FAIL (NULL handle) instead of hanging, so boot proceeds with at
    // most one PSO missing rather than a dead loading screen. Nanoseconds; wrath2-
    // gated (env-overridable for other titles).
    {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        int def = (t && strcmp(t, "wrath2") == 0) ? 1 : 0;
        if (kl_env_on("KL_MVK_COMPILE_TIMEOUT", def))
            setenv("MVK_CONFIG_METAL_COMPILE_TIMEOUT", "10000000000", 0); // 10 s
    }


    // wanderer black world: a MoltenVK/SPIRV-Cross argument-buffer codegen bug. A
    // fragment material referencing TWO Material Parameter Collections (two dynamic
    // uniform buffers) makes SPIRV-Cross emit `spvDescriptorSetBuffer0` with only
    // MaterialCollection0 declared, then a dynamic-offset accessor for a sibling
    // MaterialCollection1 that is never in the struct — the MSL fails to compile
    // (VK_ERROR_INITIALIZATION_FAILED, "no member named 'MaterialCollection1'"),
    // the PSO is dropped, that master environment material never draws, and the
    // world stays black (the eye resolve itself is fine — the armed magenta probe
    // came back BLACK, not magenta). Forcing MoltenVK OFF the argument-buffer path
    // makes SPIRV-Cross emit discrete `buffer(N)` bindings, dodging the bug. The
    // texel-buffer emu already moves manual-fetch buffers to textures, so discrete
    // binding pressure should still fit Metal's per-stage limit. wanderer-gated,
    // env-overridable (KL_MVK_NO_ARGBUF).
    {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        int def = (t && strcmp(t, "wanderer") == 0) ? 1 : 0;
        if (kl_env_on("KL_MVK_NO_ARGBUF", def))
            setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 0);  // 0 = NEVER
    }

    // LEGO Bricktales' see-through build preview flickered in stripes: it draws
    // the same bricks in two passes that must reach the same depth, and Metal's
    // fast math lets two vertex functions round the same position differently
    // (on a Quest the two agree). Fast math off made it clean on the
    // headset (2026-10-18, MVK_CONFIG_FAST_MATH_ENABLED=0 at launch) with the
    // GPU wait per frame unchanged (7-8 ms mean either way). LEGO-gated,
    // env-overridable (KL_MVK_NO_FAST_MATH).
    {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        int def = (t && strcmp(t, "x6521909757843713") == 0) ? 1 : 0;
        if (kl_env_on("KL_MVK_NO_FAST_MATH", def)) {
            setenv("MVK_CONFIG_FAST_MATH_ENABLED", "0", 0);   // 0 = NEVER
            VKI("Metal shaders without fast math (KL_MVK_NO_FAST_MATH)\n");
        }
    }

    // (The former MVK_CONFIG_USE_MTLHEAP=2 forcing lived here. It existed only to
    // make the dead fork-B alias land at MTLBuffer offset 0 via MTLHeap placement.
    // The alias is gone, replaced by the 2D-image mirror below, which is entirely
    // independent of MTLHeap mode: it reads the guest pool's mapped CPU bytes and
    // writes the mirror image's mapped CPU bytes, both host-side, so nothing here
    // needs to perturb MoltenVK's memory backing. Left on the driver default so
    // the mirror is exercised against stock MoltenVK; a launcher may still set
    // MVK_CONFIG_USE_MTLHEAP explicitly if wanted.)

    g_mvk = mvk_open();
    if (!g_mvk) {
        VKI("MoltenVK could not be loaded — run 'make mvk' (BUILDING.md).\n");
        return;
    }
    real_gipa = (PFN_vkGetInstanceProcAddr)mvk_sym("vkGetInstanceProcAddr");
    real_gdpa = (PFN_vkGetDeviceProcAddr)mvk_sym("vkGetDeviceProcAddr");
    if (!real_gipa) { VKI("MoltenVK has no vkGetInstanceProcAddr\n"); return; }
    g_avail = 1;

    PFN_vkEnumerateInstanceVersion eiv =
        (PFN_vkEnumerateInstanceVersion)real_gipa(NULL, "vkEnumerateInstanceVersion");
    uint32_t ver = VK_API_VERSION_1_0;
    if (eiv) eiv(&ver);
    VKI("MoltenVK loaded — instance API %u.%u.%u\n", VK_VERSION_MAJOR(ver),
        VK_VERSION_MINOR(ver), VK_VERSION_PATCH(ver));
}

int kl_vulkan_available(void) { vk_init(); return g_avail; }

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t magic;
    uint32_t w, h;
} klvk_surface;
#define KLVK_SURF_MAGIC 0x4b565355u   /* 'KVSU' */

// Everything a capture needs, resolved once per device. Held rather than
// re-resolved because vkQueuePresentKHR is on the frame path.
typedef struct {
    VkDevice          dev;
    VkPhysicalDevice  phys;
    uint32_t          queue_family;
    VkQueue           queue;          // for the acquire signal + the capture submit
    uint32_t          spread;         // queues of family 0 spread over families 0..n-1 (klvk_queue_spread)
    PFN_vkGetDeviceQueue              GetDeviceQueue;
    PFN_vkCreateImage                 CreateImage;
    PFN_vkDestroyImage                DestroyImage;
    PFN_vkGetImageMemoryRequirements  GetImageMemoryRequirements;
    PFN_vkAllocateMemory              AllocateMemory;
    PFN_vkFreeMemory                  FreeMemory;
    PFN_vkBindImageMemory             BindImageMemory;
    PFN_vkCreateBuffer                CreateBuffer;
    PFN_vkDestroyBuffer               DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkBindBufferMemory            BindBufferMemory;
    PFN_vkMapMemory                   MapMemory;
    PFN_vkUnmapMemory                 UnmapMemory;
    PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges;
    PFN_vkCreateCommandPool           CreateCommandPool;
    PFN_vkDestroyCommandPool          DestroyCommandPool;
    PFN_vkAllocateCommandBuffers      AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer          BeginCommandBuffer;
    PFN_vkEndCommandBuffer            EndCommandBuffer;
    PFN_vkResetCommandBuffer          ResetCommandBuffer;
    PFN_vkCmdPipelineBarrier          CmdPipelineBarrier;
    PFN_vkCmdCopyImageToBuffer        CmdCopyImageToBuffer;
    PFN_vkCmdClearColorImage          CmdClearColorImage;
    PFN_vkQueueSubmit                 QueueSubmit;
    PFN_vkCreateFence                 CreateFence;
    PFN_vkDestroyFence                DestroyFence;
    PFN_vkWaitForFences               WaitForFences;
    PFN_vkResetFences                 ResetFences;
    PFN_vkDeviceWaitIdle              DeviceWaitIdle;
    PFN_vkQueueWaitIdle               QueueWaitIdle;
    // Readback scratch, created on first use and shared by every capture path
    // (swapchain present and the OVRPlugin eye layer). Sized to the largest
    // image captured so far and grown as needed.
    VkBuffer        cap_buf;
    VkDeviceMemory  cap_mem;
    VkDeviceSize    cap_size;
    VkCommandPool   cap_pool;
    VkCommandBuffer cap_cmd;
    VkFence         cap_fence;
    // A few bytes of zeroes, for vertex inputs a pipeline leaves unfed
    // (KL_VK_VIN_FILL). Created on first use.
    VkBuffer        vin_buf;
    VkDeviceMemory  vin_mem;
} klvk_device;

typedef struct {
    uint32_t       magic;
    klvk_device   *d;
    VkFormat       format;
    VkExtent2D     extent;
    uint32_t       n;
    VkImage        images[KLVK_MAX_IMAGES];
    VkDeviceMemory mem[KLVK_MAX_IMAGES];
    uint32_t       next;
} klvk_swapchain;
#define KLVK_SWAP_MAGIC 0x4b565343u   /* 'KVSC' */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static klvk_device *g_devs[8];
// KL_VK_FRAME_EVENTS=1: what the guest did in each frame, counted, one line a
// frame (printed where a frame's picture is captured, so the frame numbers are
// the capture's). For finding what an every-Nth-frame glitch coincides with.
enum { KFE_IMG_NEW, KFE_IMG_DEL, KFE_COPY_B2I, KFE_MEM_NEW, KFE_MEM_DEL, KFE_PIPE, KFE_PASS, KFE_DRAW,
       KFE_BUF_NEW, KFE_SUBMIT, KFE_TMPL, KFE_COPY_IMG, KFE_BLIT, KFE_N };
static unsigned g_fe[KFE_N];
#define KFE(i) __atomic_add_fetch(&g_fe[i], 1, __ATOMIC_RELAXED)
static int          g_ndev;
static VkInstance   g_instance;
static unsigned     g_presented, g_captured;
static void cache_display_luid(VkInstance inst); // GPU LUID snapshot for ovrp_GetDisplayAdapterId2
static VkPhysicalDevice klvk_sub_phys(const char *where); // live device for a NULL-handle guest
// Substitute the live physical device wherever the guest passes NULL; see live_phys0.
#define KLVK_PHYS_OR_SUB(pd, where) ((pd) ? (pd) : klvk_sub_phys(where))

// Fixed foveation (kl_fove.m): the eye-pass recognition, defined with its
// tables further down. Every one returns at once while foveation is off.
static void fove_note_image(VkImage img, const VkImageCreateInfo *ci);
static void fove_note_view(VkImageView v, const VkImageViewCreateInfo *ci);
static void fove_note_fb(VkFramebuffer fb, const VkFramebufferCreateInfo *ci);
static void fove_image_gone(VkImage img);
static void fove_xr_image(VkImage img, uint32_t layers, uint32_t w, uint32_t h);
static void fove_rp_note1(const VkRenderPassCreateInfo *ci, VkRenderPass rp);
static void fove_rp_note2(const VkRenderPassCreateInfo2 *ci, VkRenderPass rp);
static const VkRenderPassBeginInfo *fove_begin(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi,
                                               VkRenderPassBeginInfo *tmp);
static void fove_sec_begin(VkCommandBuffer cb);
static void fove_xfer_note(const char *what, const char *via, VkImage src, VkImage dst, uint32_t n,
                           const VkImageSubresourceLayers *ss, const VkImageSubresourceLayers *ds,
                           int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w, uint32_t h);
static void fove_install(klvk_device *d);
static PFN_vkVoidFunction fove_entry(const char *name);

void kl_vulkan_stats(unsigned *p, unsigned *c) {
    if (p) *p = g_presented;
    if (c) *c = g_captured;
}

// Every live instance, newest last. g_instance is "the" instance for the many
// places that have only ever met one; it has to move when that one is destroyed
// and another is still alive, which is what an OpenVR runtime does to us: it
// brings up a throwaway instance and device to open its session, and replaces
// them with the game's own at the first submitted frame.
static VkInstance g_insts[4];
static int        g_ninst;

static klvk_device *dev_find(VkDevice d) {
    for (int i = 0; i < g_ndev; i++) if (g_devs[i]->dev == d) return g_devs[i];
    return NULL;
}
// Queue spread, for HITMAN 3 VR: Reloaded (x8012387035448196). Its engine
// (SVulkan::selectVulkanPhysicalDevice) takes only a queue family with graphics
// and compute AND more than two queues, and asks for three of them. MoltenVK
// offers four identical families of ONE queue each, so the engine found "No
// capable Vulkan physical device", went on with the OpenXR device anyway and
// fetched queue 1 of family 0, which does not exist: SIGSEGV in MoltenVK's
// vkGetDeviceQueue (Mac run 2026-10-20). With the spread on, family 0 reports
// as many queues as there are families like it, a request for k queues of
// family 0 becomes one queue in each of families 0..k-1, and queue i of family
// 0 is family i's queue. MoltenVK's families are all the same Metal device and
// command buffers are bound to a queue only at submit, so the work lands where
// it would have. KL_VK_QUEUE_SPREAD overrides.
static int klvk_queue_spread_on(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_VK_QUEUE_SPREAD", t && !strcmp(t, "x8012387035448196"));
    }
    return on;
}

static klvk_device *dev_of_queue(VkQueue q) {
    // Queues are opaque; the guest may use one we never fetched. With a single
    // device — which is every case here — the answer is unambiguous.
    (void)q;
    return g_ndev ? g_devs[0] : NULL;
}

// ---------------------------------------------------------------------------
// PNG (top-down; Vulkan images already are). kl_glfb.c has its own writer and
// it is NOT shared, deliberately: that one flips GL's bottom-up rows and is
// entangled with the exposure/gamma tonemap the GL capture applies. Sharing
// would mean either a flag that is wrong half the time or surgery on a
// 5000-line file on the critical rendering path.
// ---------------------------------------------------------------------------
static void be32w(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void png_chunk(FILE *f, const char *tag, const uint8_t *d, uint32_t n) {
    uint8_t h[4];
    be32w(h, n); fwrite(h, 1, 4, f); fwrite(tag, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    uLong c = crc32(0, (const Bytef *)tag, 4);
    if (n) c = crc32(c, (const Bytef *)d, n);
    be32w(h, (uint32_t)c); fwrite(h, 1, 4, f);
}

static int png_write_rgba(const char *path, const uint8_t *px, int w, int h) {
    size_t stride = (size_t)w * 4, raw_n = (stride + 1) * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    uLongf cn = compressBound((uLong)raw_n);
    uint8_t *cb = malloc(cn);
    if (!raw || !cb) { free(raw); free(cb); return 0; }
    for (int y = 0; y < h; y++) {
        raw[(stride + 1) * (size_t)y] = 0;
        memcpy(raw + (stride + 1) * (size_t)y + 1, px + stride * (size_t)y, stride);
    }
    int ok = compress(cb, &cn, raw, (uLong)raw_n) == Z_OK;
    FILE *f = ok ? fopen(path, "wb") : NULL;
    if (f) {
        static const uint8_t sig[8] = {0x89,'P','N','G',13,10,26,10};
        fwrite(sig, 1, 8, f);
        uint8_t ih[13];
        be32w(ih, (uint32_t)w); be32w(ih + 4, (uint32_t)h);
        ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0;
        png_chunk(f, "IHDR", ih, 13);
        png_chunk(f, "IDAT", cb, (uint32_t)cn);
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
    } else ok = 0;
    free(raw); free(cb);
    return ok;
}

// Is this format's byte order BGRA rather than RGBA? The swapchain format is
// whatever the guest picked from what we advertised, and a channel swap is
// invisible in a grey test frame and glaring in a real one.
static int fmt_is_bgra(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}

// ---------------------------------------------------------------------------
// Extension list surgery
// ---------------------------------------------------------------------------
//
// Every requested extension is checked against what MoltenVK actually
// advertises and silently-unsupported ones are DROPPED BY NAME rather than
// passed through. Passing them through fails the whole create call with
// VK_ERROR_EXTENSION_NOT_PRESENT, which tells the guest "no Vulkan" and tells
// us nothing about which of a dozen names was the problem.
//
// This is the same judgement the rest of the port makes about group answers:
// answering call-by-call would let the guest see a set that disagrees with
// itself.
typedef struct { const char *names[64]; uint32_t n; } extlist;

static void ext_add(extlist *l, const char *n) {
    if (l->n >= 64) return;
    for (uint32_t i = 0; i < l->n; i++) if (!strcmp(l->names[i], n)) return;
    l->names[l->n++] = n;
}

static int ext_supported(const VkExtensionProperties *have, uint32_t nhave,
                         const char *want) {
    for (uint32_t i = 0; i < nhave; i++)
        if (!strcmp(have[i].extensionName, want)) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------
// Window-system extensions we answer for ourselves on behalf of a Linux guest.
// None of them ever reaches MoltenVK: the surface behind each is the synthetic
// one, and so is the swapchain on top.
static const char *const k_synth_wsi[] = {
    "VK_EXT_headless_surface", "VK_KHR_xcb_surface", "VK_KHR_xlib_surface",
    "VK_KHR_wayland_surface",
};
static int ext_is_synth_wsi(const char *e) {
    for (unsigned i = 0; i < sizeof k_synth_wsi / sizeof *k_synth_wsi; i++)
        if (!strcmp(e, k_synth_wsi[i])) return 1;
    return 0;
}

static VkResult klvk_EnumerateInstanceExtensionProperties(
        const char *layer, uint32_t *count, VkExtensionProperties *props) {
    vk_init();
    if (!g_avail) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateInstanceExtensionProperties real =
        (PFN_vkEnumerateInstanceExtensionProperties)
            real_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t n = 0;
    real(layer, &n, NULL);
    VkExtensionProperties *tmp = calloc(n + 8, sizeof *tmp);
    if (!tmp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    real(layer, &n, tmp);

    // VK_KHR_android_surface is advertised because we implement it. Unity tests
    // for it before enabling it, and a Vulkan build that cannot find a surface
    // extension does not proceed.
    if (!layer && !ext_supported(tmp, n, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
        snprintf(tmp[n].extensionName, sizeof tmp[n].extensionName, "%s",
                 VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
        tmp[n].specVersion = 6;
        n++;
    }
    // ...and VK_EXT_headless_surface for the same reason, for a Linux guest: it
    // is the one window system an SDL offscreen video driver asks for, and the
    // surface it creates is the same synthetic one (see g_ours). MoltenVK has an
    // implementation of its own, which must not be the one that answers — the
    // swapchain on top is ours.
    // The X11 and Wayland ones are advertised alongside it: Source 2 will not
    // create an instance on Linux unless the window system it was built for is
    // on the list, whatever SDL then actually uses.
    for (unsigned i = 0; !layer && i < sizeof k_synth_wsi / sizeof *k_synth_wsi; i++)
        if (!ext_supported(tmp, n, k_synth_wsi[i])) {
            snprintf(tmp[n].extensionName, sizeof tmp[n].extensionName, "%s", k_synth_wsi[i]);
            tmp[n].specVersion = 6;
            n++;
        }

    VkResult r = VK_SUCCESS;
    if (!props) *count = n;
    else {
        uint32_t give = *count < n ? *count : n;
        memcpy(props, tmp, give * sizeof *props);
        if (give < n) r = VK_INCOMPLETE;
        *count = give;
    }
    free(tmp);
    VKT("vkEnumerateInstanceExtensionProperties(layer %s, %s) -> %d, %u extension(s)\n",
        layer ? layer : "none", props ? "fill" : "count", r, *count);
    return r;
}

// A VkPhysicalDevice belongs to its instance, and MoltenVK allocates a new one
// per vkCreateInstance. Unity 6 depends on it NOT changing: it creates a probe
// instance, destroys it, lets OculusXR ask ovrp_GetDisplayAdapterId2 for the VR
// device (a handle, on this plugin generation — see kl_ovrp.c), and only then
// creates the instance it keeps, where it looks that handle up. On a Quest the
// driver's physical device is one object for the process, so the handle from the
// dead instance is still the right answer; here it named freed memory and matched
// nothing.
//
// So a destroyed instance is PARKED instead, and a create that asks for the same
// thing gets it back — same instance, same physical device, which is the
// property the guest relies on. A create that differs is a real create, and the
// parked one is then really destroyed. KL_VK_PARK_INSTANCE=0/1 is the A/B.
static VkInstance g_parked_inst;
static char g_parked_key[1024], g_live_key[1024];

// Take a destroyed instance out of g_insts (see there), and move g_instance on
// to the newest one that is left.
static void klvk_forget_instance(VkInstance inst) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_ninst; i++)
        if (g_insts[i] == inst) {
            memmove(&g_insts[i], &g_insts[i + 1], (size_t)(g_ninst - i - 1) * sizeof *g_insts);
            g_ninst--;
            break;
        }
    if (g_instance == inst) g_instance = g_ninst ? g_insts[g_ninst - 1] : VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_lock);
}

static int klvk_park_instances(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        // batman (Unity 2022.3.45) probes the same way and needs the same
        // property: unparked, 2 host runs in 5 enumerated a different
        // VkPhysicalDevice on the instance it kept than the one the plugin had
        // been handed off the probe, matched nothing, and fell back to GLES2;
        // parked, 6 of 6 created their device.
        // Max Mustard (x5269080709783454) takes the handle answer too (see
        // klovrp_adapter_is_phys), so it needs the probe's device kept alive.
        // Demeo (x3634830803298285) likewise: with the handle answer alone,
        // three host runs created their device and the fourth enumerated
        // another VkPhysicalDevice and ended on "Unable to initialize the
        // Unity Engine Graphics API".
        // Arizona Sunshine 2 (x5245041552210029) takes the handle answer as
        // well, so the probe's device has to outlive the probe here too.
        // Ghost Town (x9531494193591052) takes the handle answer too
        // (klovrp_adapter_is_phys), so the probe's device has to still be the
        // one the instance it keeps enumerates.
        // Every Unity title under the profile for untested games takes the
        // handle answer (klovrp_adapter_is_phys), so it keeps the probe's
        // device too: LEGO Bricktales, unparked, created its device on three
        // host runs and on the fourth enumerated another VkPhysicalDevice and
        // ended on "Unable to initialize the Unity Engine Graphics API".
        extern int kl_untested_profile(void);
        extern kl_guest_kind kl_driver_kind(void);
        on = kl_env_on("KL_VK_PARK_INSTANCE",
                       (t && (strcmp(t, "walkabout") == 0 || strcmp(t, "batman") == 0 ||
                              strcmp(t, "x5269080709783454") == 0 ||
                              strcmp(t, "x3634830803298285") == 0 ||
                              strcmp(t, "x5245041552210029") == 0 ||
                              strcmp(t, "x9531494193591052") == 0)) ||
                       (kl_untested_profile() && kl_driver_kind() == KL_GUEST_UNITY));
    }
    return on;
}

// ---- The guest's own instance and device settings (KL_VK_DEVICE_INFO=<file>|auto) ----
// Pipelines compile to the same MSL only on a device made the same way, so a
// replay that runs before the game (the launcher warming the Metal cache with
// no guest at all: kl_foz_prewarm) needs these. Written as JSON at
// vkCreateDevice; "auto" is device-info.json in the pipeline recorder's folder
// (KL_VK_FOZ_DIR, else $HOME/Documents/klfz). Feature structs go out as their
// bytes after the sType/pNext header; one the table does not know is listed by
// sType with size 0. The format and the table are Glovebox's (MIT), so a file
// of either reads in the other.
//
// KL_VK_FOZ_APP=<name> names the application whose device this is about, by
// the application or the engine name it gives its instance: with an OpenVR
// runtime in the process the first device is the runtime's own.
static struct { uint32_t api, app_ver, eng_ver; char app[64], eng[64]; char *ext[64]; uint32_t n; } g_inst_info;
int klvk_instance_wanted(void) {
    const char *want = getenv("KL_VK_FOZ_APP");
    return !want || !*want || !strcmp(want, g_inst_info.app) || !strcmp(want, g_inst_info.eng);
}

static void devinfo_note_instance(const VkInstanceCreateInfo *ci) {
    if (!ci) return;
    for (uint32_t i = 0; i < g_inst_info.n; i++) free(g_inst_info.ext[i]);
    g_inst_info.n = 0;  // the newest instance's
    g_inst_info.api = g_inst_info.app_ver = g_inst_info.eng_ver = 0;
    g_inst_info.app[0] = g_inst_info.eng[0] = 0;
    const VkApplicationInfo *a = ci->pApplicationInfo;
    if (a) {
        g_inst_info.api = a->apiVersion; g_inst_info.app_ver = a->applicationVersion; g_inst_info.eng_ver = a->engineVersion;
        snprintf(g_inst_info.app, sizeof g_inst_info.app, "%s", a->pApplicationName ? a->pApplicationName : "");
        snprintf(g_inst_info.eng, sizeof g_inst_info.eng, "%s", a->pEngineName ? a->pEngineName : "");
    }
    for (uint32_t i = 0; i < ci->enabledExtensionCount && g_inst_info.n < 64; i++)
        g_inst_info.ext[g_inst_info.n++] = strdup(ci->ppEnabledExtensionNames[i]);
}

static const struct { VkStructureType t; size_t size; } k_feature_structs[] = {
#define F(T, S) { VK_STRUCTURE_TYPE_##T, sizeof(S) }
    F(PHYSICAL_DEVICE_FEATURES_2, VkPhysicalDeviceFeatures2),
    F(PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, VkPhysicalDeviceVulkan11Features),
    F(PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, VkPhysicalDeviceVulkan12Features),
    F(PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, VkPhysicalDeviceVulkan13Features),
    F(PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, VkPhysicalDevice16BitStorageFeatures),
    F(PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, VkPhysicalDevice8BitStorageFeatures),
    F(PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, VkPhysicalDeviceShaderFloat16Int8Features),
    F(PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, VkPhysicalDeviceDescriptorIndexingFeatures),
    F(PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, VkPhysicalDeviceTimelineSemaphoreFeatures),
    F(PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VkPhysicalDeviceBufferDeviceAddressFeatures),
    F(PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, VkPhysicalDeviceHostQueryResetFeatures),
    F(PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, VkPhysicalDeviceScalarBlockLayoutFeatures),
    F(PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES, VkPhysicalDeviceImagelessFramebufferFeatures),
    F(PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, VkPhysicalDeviceUniformBufferStandardLayoutFeatures),
    F(PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures),
    F(PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures),
    F(PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, VkPhysicalDeviceVulkanMemoryModelFeatures),
    F(PHYSICAL_DEVICE_MULTIVIEW_FEATURES, VkPhysicalDeviceMultiviewFeatures),
    F(PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, VkPhysicalDeviceSamplerYcbcrConversionFeatures),
    F(PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, VkPhysicalDeviceVariablePointersFeatures),
    F(PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, VkPhysicalDeviceShaderDrawParametersFeatures),
    F(PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, VkPhysicalDeviceProtectedMemoryFeatures),
    F(PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, VkPhysicalDeviceDynamicRenderingFeatures),
    F(PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, VkPhysicalDeviceSynchronization2Features),
    F(PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, VkPhysicalDeviceMaintenance4Features),
    F(PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, VkPhysicalDeviceImageRobustnessFeatures),
    F(PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES, VkPhysicalDeviceInlineUniformBlockFeatures),
    F(PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES, VkPhysicalDevicePipelineCreationCacheControlFeatures),
    F(PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, VkPhysicalDevicePrivateDataFeatures),
    F(PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures),
    F(PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, VkPhysicalDeviceShaderIntegerDotProductFeatures),
    F(PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES, VkPhysicalDeviceShaderTerminateInvocationFeatures),
    F(PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, VkPhysicalDeviceSubgroupSizeControlFeatures),
    F(PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES, VkPhysicalDeviceTextureCompressionASTCHDRFeatures),
    F(PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES, VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures),
    F(PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, VkPhysicalDeviceRobustness2FeaturesEXT),
    F(PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicStateFeaturesEXT),
    F(PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicState2FeaturesEXT),
#ifdef VK_EXT_extended_dynamic_state3
    // asked for by Source 2 where the layer offers it (Vision Pro M5, 2026-10-11)
    F(PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicState3FeaturesEXT),
#endif
    F(PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, VkPhysicalDeviceCustomBorderColorFeaturesEXT),
    F(PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, VkPhysicalDeviceDepthClipEnableFeaturesEXT),
    F(PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT, VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT),
    F(PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT, VkPhysicalDevice4444FormatsFeaturesEXT),
    F(PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT, VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT),
    F(PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT, VkPhysicalDeviceShaderAtomicFloatFeaturesEXT),
    // Source 2's own on MoltenVK: multiview, dynamic rendering, and these two
#ifdef VK_KHR_dynamic_rendering_local_read
    F(PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR, VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR),
#endif
#ifdef VK_EXT_host_image_copy
    F(PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT, VkPhysicalDeviceHostImageCopyFeaturesEXT),
#endif
#ifdef VK_VERSION_1_4
    F(PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, VkPhysicalDeviceVulkan14Features),
#endif
    // Unity 6 asks for these two (Walkabout Mini Golf 6.8, Vision Pro M5,
    // 2026-10-12). Without them in the table the description named two
    // structs without their contents, and the pre-start compile refused the
    // whole description ("a feature struct this build cannot rebuild").
#ifdef VK_KHR_present_wait
    F(PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, VkPhysicalDevicePresentWaitFeaturesKHR),
#endif
#ifdef VK_KHR_present_id
    F(PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, VkPhysicalDevicePresentIdFeaturesKHR),
#endif
#undef F
};

static void devinfo_hex(FILE *f, const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) fprintf(f, "%02x", ((const unsigned char *)p)[i]);
}

static void devinfo_write(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci) {
    const char *e = getenv("KL_VK_DEVICE_INFO");
    if (!e || !*e || !strcmp(e, "0") || !ci || !klvk_instance_wanted()) return;
    char path[1024];
    const char *dir = getenv("KL_VK_FOZ_DIR");  // the pipeline recorder's folder, as kl_fossilize.cpp finds it
    if (!strcmp(e, "auto") && dir && *dir) snprintf(path, sizeof path, "%s/device-info.json", dir);
    else if (!strcmp(e, "auto")) snprintf(path, sizeof path, "%s/Documents/klfz/device-info.json", getenv("HOME") ? getenv("HOME") : ".");
    else snprintf(path, sizeof path, "%s", e);
    FILE *f = fopen(path, "w");
    if (!f) { VKI("device info: cannot write %s\n", path); return; }
    VkPhysicalDeviceProperties pr = {0};
    PFN_vkGetPhysicalDeviceProperties gp = (PFN_vkGetPhysicalDeviceProperties)real_gipa(g_instance, "vkGetPhysicalDeviceProperties");
    if (gp && phys) gp(phys, &pr);
    fprintf(f, "{\"v\": 1, \"gpu\": \"%s\", \"driver\": %u, \"gpu_api\": %u,\n", pr.deviceName, pr.driverVersion, pr.apiVersion);
    fprintf(f, " \"instance\": {\"api\": %u, \"app\": \"%s\", \"app_version\": %u, \"engine\": \"%s\", \"engine_version\": %u, \"extensions\": [",
            g_inst_info.api, g_inst_info.app, g_inst_info.app_ver, g_inst_info.eng, g_inst_info.eng_ver);
    for (uint32_t i = 0; i < g_inst_info.n; i++) fprintf(f, "%s\"%s\"", i ? ", " : "", g_inst_info.ext[i]);
    fprintf(f, "]},\n \"device\": {\"extensions\": [");
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) fprintf(f, "%s\"%s\"", i ? ", " : "", ci->ppEnabledExtensionNames[i]);
    fprintf(f, "],\n  \"queues\": [");
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++)
        fprintf(f, "%s{\"family\": %u, \"count\": %u, \"flags\": %u}", i ? ", " : "", ci->pQueueCreateInfos[i].queueFamilyIndex,
                ci->pQueueCreateInfos[i].queueCount, ci->pQueueCreateInfos[i].flags);
    fprintf(f, "],\n  \"enabled_features\": ");
    if (ci->pEnabledFeatures) { fputc('"', f); devinfo_hex(f, ci->pEnabledFeatures, sizeof *ci->pEnabledFeatures); fputc('"', f); }
    else fprintf(f, "null");
    fprintf(f, ",\n  \"pnext\": [");
    unsigned n = 0, unknown = 0;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext, n++) {
        size_t size = 0;
        for (size_t k = 0; k < sizeof k_feature_structs / sizeof *k_feature_structs; k++)
            if (k_feature_structs[k].t == b->sType) { size = k_feature_structs[k].size; break; }
        fprintf(f, "%s\n   {\"sType\": %u, \"size\": %zu, \"data\": \"", n ? "," : "", (unsigned)b->sType, size);
        if (size > sizeof *b) devinfo_hex(f, (const char *)b + sizeof *b, size - sizeof *b);
        else unknown++;
        fputc('"', f); fputc('}', f);
    }
    fprintf(f, "]}}\n");
    fclose(f);
    VKI("device info: %u instance + %u device extension(s), %u pNext struct(s) (%u not in the table) -> %s\n",
        g_inst_info.n, ci->enabledExtensionCount, n, unknown, path);
}

static VkResult klvk_CreateInstance(const VkInstanceCreateInfo *ci,
                                    const VkAllocationCallbacks *alloc,
                                    VkInstance *out) {
    vk_init();
    if (!g_avail) return VK_ERROR_INITIALIZATION_FAILED;
    devinfo_note_instance(ci);
    if (ci && ci->pApplicationInfo)
        VKI("vkCreateInstance: application \"%s\", engine \"%s\"\n", g_inst_info.app, g_inst_info.eng);
    PFN_vkCreateInstance real = (PFN_vkCreateInstance)real_gipa(NULL, "vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties enum_ext =
        (PFN_vkEnumerateInstanceExtensionProperties)
            real_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!real || !enum_ext) return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t nhave = 0;
    enum_ext(NULL, &nhave, NULL);
    VkExtensionProperties *have = calloc(nhave ? nhave : 1, sizeof *have);
    if (have) enum_ext(NULL, &nhave, have);

    extlist keep = {{0}, 0};
    for (uint32_t i = 0; ci && i < ci->enabledExtensionCount; i++) {
        const char *e = ci->ppEnabledExtensionNames[i];
        if (!strcmp(e, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME) || ext_is_synth_wsi(e)) {
            VKT("instance ext %s -> ours (the surface is synthetic)\n", e);
            continue;
        }
        if (have && !ext_supported(have, nhave, e)) {
            VKI("instance ext %s NOT supported by MoltenVK — dropped\n", e);
            continue;
        }
        ext_add(&keep, e);
    }

    // MoltenVK is a portability driver: without this flag a conformant loader
    // enumerates ZERO physical devices and still returns VK_SUCCESS, which reads
    // exactly like "Metal is unavailable" and names nothing.
    VkInstanceCreateFlags flags = ci ? ci->flags : 0;
    if (!have || ext_supported(have, nhave, "VK_KHR_portability_enumeration")) {
        ext_add(&keep, "VK_KHR_portability_enumeration");
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VkInstanceCreateInfo mine = ci ? *ci : (VkInstanceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    mine.flags = flags;
    mine.enabledExtensionCount = keep.n;
    mine.ppEnabledExtensionNames = keep.names;
    // Validation layers do not exist here; asking for one fails the create.
    mine.enabledLayerCount = 0;
    mine.ppEnabledLayerNames = NULL;

    // MoltenVK clamps every physical device's REPORTED apiVersion down to the
    // instance's VkApplicationInfo.apiVersion. A UE4 guest asks for a 1.0
    // instance, but its device-selection gate then rejects any GPU that reports
    // < 1.1 — so the Apple GPU comes back "api 1.0.357", the guest decides the
    // device has no usable Vulkan and puts up "This device does not support
    // Vulkan…" and quits (Wrath2, SIGKILL, no device ever created). Raise the
    // requested version to at least 1.1 (MoltenVK backs 1.4 here) so the device
    // reports >= 1.1 and clears the gate. A guest that only uses 1.0 features is
    // unaffected by the higher ceiling; UE5 guests already ask for 1.1 and are
    // untouched. This is also why olar (UE5) got a 1.1 device and rendered.
    VkApplicationInfo appinfo;
    const VkApplicationInfo *src = ci ? ci->pApplicationInfo : NULL;
    appinfo = src ? *src
                  : (VkApplicationInfo){ .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO };
    if (appinfo.apiVersion < VK_API_VERSION_1_1) {
        VKI("instance apiVersion %u.%u -> raised to 1.1 (MoltenVK reports the "
            "device at the instance version; UE4's device gate needs >= 1.1)\n",
            VK_VERSION_MAJOR(appinfo.apiVersion), VK_VERSION_MINOR(appinfo.apiVersion));
        appinfo.apiVersion = VK_API_VERSION_1_1;
    }
    mine.pApplicationInfo = &appinfo;

    // What makes two creates "the same": the version MoltenVK clamps the device
    // to, the flags, and the extensions that survived the filter above.
    char key[sizeof g_live_key];
    int kn = snprintf(key, sizeof key, "%u/%u", appinfo.apiVersion, (unsigned)flags);
    for (uint32_t i = 0; i < keep.n && kn > 0 && (size_t)kn < sizeof key; i++)
        kn += snprintf(key + kn, sizeof key - (size_t)kn, "/%s", keep.names[i]);
    if (g_parked_inst && !strcmp(key, g_parked_key)) {
        *out = g_instance = g_parked_inst;
        g_parked_inst = VK_NULL_HANDLE;
        memcpy(g_live_key, key, sizeof key);
        cache_display_luid(*out);
        free(have);
        VKI("vkCreateInstance -> the parked instance %p again (same request, "
            "so the same VkPhysicalDevice)\n", (void *)*out);
        return VK_SUCCESS;
    }

    VkResult r = real(&mine, alloc, out);
    free(have);
    if (r == VK_SUCCESS) {
        memcpy(g_live_key, key, sizeof key);
        if (g_parked_inst) {
            PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)
                real_gipa(g_parked_inst, "vkDestroyInstance");
            klvk_forget_instance(g_parked_inst);
            if (di) di(g_parked_inst, NULL);
            g_parked_inst = VK_NULL_HANDLE;
        }
        pthread_mutex_lock(&g_lock);
        if (g_ninst < (int)(sizeof g_insts / sizeof *g_insts)) g_insts[g_ninst++] = *out;
        pthread_mutex_unlock(&g_lock);
        g_instance = *out;
        cache_display_luid(*out); // grab the GPU LUID now — the guest may destroy
                                  // this instance before ovrp_GetDisplayAdapterId2
        VKI("vkCreateInstance ok — %u extension(s)%s\n", keep.n,
            ci && ci->enabledLayerCount ? ", layers dropped" : "");
    } else {
        VKI("vkCreateInstance FAILED %d\n", r);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
static void dev_resolve(klvk_device *d) {
#define R(f) d->f = (PFN_vk##f)real_gdpa(d->dev, "vk" #f)
    R(GetDeviceQueue); R(CreateImage); R(DestroyImage); R(GetImageMemoryRequirements);
    R(AllocateMemory); R(FreeMemory); R(BindImageMemory);
    R(CreateBuffer); R(DestroyBuffer); R(GetBufferMemoryRequirements);
    R(BindBufferMemory); R(MapMemory); R(UnmapMemory);
    R(InvalidateMappedMemoryRanges);
    R(CreateCommandPool); R(DestroyCommandPool); R(AllocateCommandBuffers);
    R(BeginCommandBuffer); R(EndCommandBuffer); R(ResetCommandBuffer);
    R(CmdPipelineBarrier); R(CmdCopyImageToBuffer); R(CmdClearColorImage); R(QueueSubmit);
    R(CreateFence); R(DestroyFence); R(WaitForFences); R(ResetFences);
    R(DeviceWaitIdle); R(QueueWaitIdle);
#undef R
}

// Feature surgery, the same judgement as the extension lists above: a feature the
// guest enables that the device does not have fails the whole vkCreateDevice with
// VK_ERROR_FEATURE_NOT_PRESENT, which names a flag by ordinal and stops the guest
// dead. A guest written against one driver (Source 2's ARM build knows it will
// meet Turnip) enables what that driver has without asking. So every feature
// struct in the create chain is compared with what this device reports, and the
// ones it lacks are switched off BY NAME — sType and field index — so that what
// was refused is on record for whoever then has to emulate it.
//
// Every Vulkan feature struct is { sType, pNext, VkBool32... }, which is what
// lets this be one loop instead of a table of two hundred types: the device is
// asked about a scratch struct of the same sType pre-filled with a sentinel, and
// however many leading fields it overwrites is how many flags the struct has.
#define KLVK_FEAT_SENTINEL 0xA5A5A5A5u
#define KLVK_FEAT_WORDS    256
// KL_VK_LOGIC_OP (default on for HITMAN 3 VR: Reloaded): its light culling
// (lightculling.frag) ORs each light's bit into a tile mask with VK_LOGIC_OP_OR
// on a uint target. Without logic ops every light overwrote the tile, tiles kept
// only their last light, and the level showed dark regions with stepped edges
// (Mac, 2026-10-20). MoltenVK reports no logicOp (it is built without private
// API), so the operation is set on the Metal pipeline underneath it
// (kl_mtl_logic_op_next, kl_mtl_busy.m) while the Vulkan pipeline is created.
static int klvk_logic_op_on(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_VK_LOGIC_OP", t && !strcmp(t, "x8012387035448196"));
    }
    return on;
}
static void klvk_feature_surgery(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci) {
    PFN_vkGetPhysicalDeviceFeatures2 get = (PFN_vkGetPhysicalDeviceFeatures2)
        real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2");
    if (!get) get = (PFN_vkGetPhysicalDeviceFeatures2)
        real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2KHR");
    if (!get || !ci) return;
    struct probe { VkStructureType sType; void *pNext; uint32_t flag[KLVK_FEAT_WORDS]; };
    unsigned cleared = 0;
    for (VkBaseOutStructure *want = (VkBaseOutStructure *)ci->pNext; want; want = want->pNext) {
        // The classic struct is the payload of Features2 and is probed through it.
        struct probe *p = malloc(sizeof *p);
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        for (unsigned i = 0; i < KLVK_FEAT_WORDS; i++) p->flag[i] = KLVK_FEAT_SENTINEL;
        p->sType = want->sType;
        p->pNext = NULL;
        if (want->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            get(phys, (VkPhysicalDeviceFeatures2 *)p);
        } else {
            f2.pNext = p;
            get(phys, &f2);
        }
        uint32_t *asked = (uint32_t *)(want + 1);
        for (unsigned i = 0; i < KLVK_FEAT_WORDS && p->flag[i] != KLVK_FEAT_SENTINEL; i++)
            if (asked[i] && !p->flag[i]) {
                VKI("device feature sType %d flag #%u is NOT available on this device — "
                    "switched off\n", (int)want->sType, i + 1);
                asked[i] = VK_FALSE;
                cleared++;
            }
        free(p);
    }
    if (ci->pEnabledFeatures) {
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        get(phys, &f2);
        uint32_t *asked = (uint32_t *)ci->pEnabledFeatures, *have = (uint32_t *)&f2.features;
        for (unsigned i = 0; i < sizeof(VkPhysicalDeviceFeatures) / 4; i++)
            if (asked[i] && !have[i]) {
                VKI("device feature #%u of VkPhysicalDeviceFeatures is NOT available on "
                    "this device — switched off\n", i + 1);
                asked[i] = VK_FALSE;
                cleared++;
            }
    }
    if (cleared) VKI("vkCreateDevice: %u requested feature(s) the device lacks were dropped\n", cleared);
}

static VkResult klvk_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                                  const VkAllocationCallbacks *alloc, VkDevice *out) {
    VKT("call CreateDevice\n");   // KL_VK_TRACE: the order of a guest's capability queries
    PFN_vkCreateDevice real = (PFN_vkCreateDevice)real_gipa(g_instance, "vkCreateDevice");
    PFN_vkEnumerateDeviceExtensionProperties enum_ext =
        (PFN_vkEnumerateDeviceExtensionProperties)
            real_gipa(g_instance, "vkEnumerateDeviceExtensionProperties");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    // The whole point of the null-substitution family: Unity reaches here with a
    // NULL physicalDevice (its GPU select left it null), and MoltenVK would make
    // a device on nothing. Give it the real GPU so it gets a working VkDevice.
    phys = KLVK_PHYS_OR_SUB(phys, "CreateDevice");
    // as the guest asked, before anything below edits it
    devinfo_write(phys, ci);
    if (ci && g_pipe_hooks && g_pipe_hooks->device_info && !t_pipe_replay && klvk_instance_wanted())
        g_pipe_hooks->device_info(ci);

    uint32_t nhave = 0;
    VkExtensionProperties *have = NULL;
    if (enum_ext) {
        enum_ext(phys, NULL, &nhave, NULL);
        have = calloc(nhave ? nhave : 1, sizeof *have);
        if (have) enum_ext(phys, NULL, &nhave, have);
    }

    extlist keep = {{0}, 0};
    for (uint32_t i = 0; ci && i < ci->enabledExtensionCount; i++) {
        const char *e = ci->ppEnabledExtensionNames[i];
        if (have && !ext_supported(have, nhave, e)) {
            VKI("device ext %s NOT supported by MoltenVK — dropped\n", e);
            continue;
        }
        VKT("device ext %s\n", e);
        ext_add(&keep, e);
    }
    klvk_feature_surgery(phys, ci);
    // The spec REQUIRES this one to be enabled when the device advertises it,
    // and MoltenVK always does. Omitting it is undefined behaviour, not a
    // missing nicety.
    if (have && ext_supported(have, nhave, "VK_KHR_portability_subset"))
        ext_add(&keep, "VK_KHR_portability_subset");
    // ...and one the guest never asks for and we need: `vkExportMetalObjectsEXT`
    // is how the MTLTexture behind an eye VkImage reaches the compositor, and
    // calling it on a device that did not enable this is undefined. It adds no
    // behaviour of its own — nothing else in the device's operation changes —
    // and eye_mtl_texture falls back to MoltenVK's own deprecated entry point if
    // it is absent, so this is an upgrade rather than a requirement.
    if (have && ext_supported(have, nhave, "VK_EXT_metal_objects"))
        ext_add(&keep, "VK_EXT_metal_objects");

    VkDeviceCreateInfo mine = ci ? *ci : (VkDeviceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    // Queue spread (klvk_queue_spread_on): k queues of family 0 become one
    // queue in each of families 0..k-1, unless the guest names one of those
    // families itself.
    VkDeviceQueueCreateInfo spread_qci[16];
    uint32_t spread = 0;
    if (ci && klvk_queue_spread_on()) {
        for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
            const VkDeviceQueueCreateInfo *q = &ci->pQueueCreateInfos[i];
            if (q->queueFamilyIndex == 0 && q->queueCount > 1) spread = q->queueCount;
        }
        for (uint32_t i = 0; spread && i < ci->queueCreateInfoCount; i++) {
            uint32_t f = ci->pQueueCreateInfos[i].queueFamilyIndex;
            if (f != 0 && f < spread) spread = 0;
        }
        if (spread && spread + ci->queueCreateInfoCount <= 16) {
            uint32_t n = 0;
            for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
                const VkDeviceQueueCreateInfo *q = &ci->pQueueCreateInfos[i];
                if (q->queueFamilyIndex != 0) { spread_qci[n++] = *q; continue; }
                for (uint32_t k = 0; k < spread; k++) {
                    spread_qci[n] = *q;
                    spread_qci[n].queueFamilyIndex = k;
                    spread_qci[n].queueCount = 1;
                    spread_qci[n].pQueuePriorities = q->pQueuePriorities ? &q->pQueuePriorities[k] : NULL;
                    n++;
                }
            }
            mine.queueCreateInfoCount = n;
            mine.pQueueCreateInfos = spread_qci;
            VKI("queue spread: %u queues of family 0 -> one in each of families 0..%u\n", spread, spread - 1);
        } else spread = 0;
    }
    mine.enabledExtensionCount = keep.n;
    mine.ppEnabledExtensionNames = keep.names;
    mine.enabledLayerCount = 0;
    mine.ppEnabledLayerNames = NULL;

    VkResult r = real(phys, &mine, alloc, out);
    free(have);
    if (r != VK_SUCCESS) { VKI("vkCreateDevice FAILED %d\n", r); return r; }

    klvk_device *d = calloc(1, sizeof *d);
    if (!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
    d->dev = *out;
    d->phys = phys;
    d->queue_family = ci && ci->queueCreateInfoCount ? ci->pQueueCreateInfos[0].queueFamilyIndex : 0;
    d->spread = spread;
    dev_resolve(d);
    // Foveation of the guest's scene pass, underneath MoltenVK (KL_VK_FOVEATE;
    // kl_mtl_foveate.m). Nothing happens unless it is asked for.
    { extern void kl_mtl_foveate_install(void); kl_mtl_foveate_install(); }
    // A queue of our own, for the acquire signal and the capture submit. Taken
    // from the guest's own first queue family, which is the one it will present
    // on — see the note in klvk_AcquireNextImageKHR about the sharing.
    if (d->GetDeviceQueue) d->GetDeviceQueue(d->dev, d->queue_family, 0, &d->queue);

    pthread_mutex_lock(&g_lock);
    if (g_ndev < (int)(sizeof g_devs / sizeof *g_devs)) g_devs[g_ndev++] = d;
    pthread_mutex_unlock(&g_lock);
    fove_install(d);   // KL_FOVEATION (default off: returns at once)

    VKI("vkCreateDevice ok — %u extension(s), queue family %u\n", keep.n, d->queue_family);
    if (g_pipe_hooks && g_pipe_hooks->device_created)
        g_pipe_hooks->device_created(d->dev, klvk_GetDeviceProcAddr);
    return VK_SUCCESS;
}

// Destruction, which no Android guest ever did before exiting and a Linux one
// does in the middle of start-up (see g_insts). The records have to go with the
// objects: everything that says g_devs[0] or g_instance means "the device the
// guest is rendering with", and a destroyed one left in the first slot is a
// freed MoltenVK object handed back to MoltenVK.
void klvk_ap_drain_all(void);
static void klvk_frame_mark_forget(klvk_device *d);
static void klvk_pcache_wait(VkDevice dev);
static void klvk_DestroyDevice(VkDevice dev, const VkAllocationCallbacks *alloc) {
    PFN_vkDestroyDevice real = real_gdpa ? (PFN_vkDestroyDevice)real_gdpa(dev, "vkDestroyDevice") : NULL;
    klvk_pcache_wait(dev);
    klvk_ap_drain_all();
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_ndev; i++)
        if (g_devs[i]->dev == dev) {
            klvk_frame_mark_forget(g_devs[i]);
            if (g_devs[i]->vin_buf) g_devs[i]->DestroyBuffer(dev, g_devs[i]->vin_buf, NULL);
            if (g_devs[i]->vin_mem) g_devs[i]->FreeMemory(dev, g_devs[i]->vin_mem, NULL);
            free(g_devs[i]);
            memmove(&g_devs[i], &g_devs[i + 1], (size_t)(g_ndev - i - 1) * sizeof *g_devs);
            g_devs[--g_ndev] = NULL;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    VKI("vkDestroyDevice %p — %d device(s) remain\n", (void *)dev, g_ndev);
    if (real) real(dev, alloc);
}

static void VKAPI_CALL klvk_DestroyInstance(VkInstance inst, const VkAllocationCallbacks *alloc) {
    if (!inst || !real_gipa) return;
    PFN_vkDestroyInstance real = (PFN_vkDestroyInstance)real_gipa(inst, "vkDestroyInstance");
    // See klvk_park_instances. Only the live instance is parked, and only one at
    // a time. A parked instance is still alive, so its record stays where it is.
    if (klvk_park_instances() && inst == g_instance && !g_parked_inst) {
        g_parked_inst = inst;
        memcpy(g_parked_key, g_live_key, sizeof g_parked_key);
        VKI("vkDestroyInstance(%p) -> parked, not destroyed\n", (void *)inst);
        return;
    }
    klvk_forget_instance(inst);
    VKI("vkDestroyInstance %p — %d instance(s) remain\n", (void *)inst, g_ninst);
    if (real) real(inst, alloc);
}

// ---------------------------------------------------------------------------
// Surface — synthesized whole. There is no Android window here and nothing that
// has to reach a screen: this arc ends in a file on disk.
// ---------------------------------------------------------------------------
static VkResult klvk_CreateAndroidSurfaceKHR(VkInstance inst,
        const VkAndroidSurfaceCreateInfoKHR *ci, const VkAllocationCallbacks *alloc,
        VkSurfaceKHR *out) {
    (void)inst; (void)ci; (void)alloc;
    klvk_surface *s = calloc(1, sizeof *s);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->magic = KLVK_SURF_MAGIC;
    // The eye size the rest of the port already agreed on, so a Vulkan frame is
    // the same shape as the GLES one and the compositor seam does not have to
    // learn a second size later.
    s->w = (uint32_t)kl_env_int("KL_VK_WIDTH", 2064);
    s->h = (uint32_t)kl_env_int("KL_VK_HEIGHT", 2208);
    *out = (VkSurfaceKHR)s;
    VKI("vkCreateAndroidSurfaceKHR -> synthetic surface %ux%u\n", s->w, s->h);
    return VK_SUCCESS;
}

static klvk_surface *surf_of(VkSurfaceKHR h) {
    klvk_surface *s = (klvk_surface *)h;
    return (s && s->magic == KLVK_SURF_MAGIC) ? s : NULL;
}

static void klvk_DestroySurfaceKHR(VkInstance inst, VkSurfaceKHR surf,
                                   const VkAllocationCallbacks *alloc) {
    (void)inst; (void)alloc;
    klvk_surface *s = surf_of(surf);
    if (s) { s->magic = 0; free(s); }
}

static VkResult klvk_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice p, uint32_t fam,
        VkSurfaceKHR surf, VkBool32 *out) {
    (void)p; (void)fam; (void)surf;
    *out = VK_TRUE;
    return VK_SUCCESS;
}

static VkResult klvk_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice p,
        VkSurfaceKHR surf, VkSurfaceCapabilitiesKHR *c) {
    (void)p;
    klvk_surface *s = surf_of(surf);
    uint32_t w = s ? s->w : 1024, h = s ? s->h : 1024;
    memset(c, 0, sizeof *c);
    c->minImageCount = 2;
    c->maxImageCount = KLVK_MAX_IMAGES;
    c->currentExtent.width = w;   c->currentExtent.height = h;
    c->minImageExtent = c->currentExtent;
    c->maxImageExtent = c->currentExtent;
    c->maxImageArrayLayers = 1;
    c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR |
                                 VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    // TRANSFER_SRC is not decoration: it is what lets the capture read the image
    // the guest presented. A guest that trims usage to what the surface reports
    // would otherwise hand us an image that cannot legally be copied from.
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_STORAGE_BIT;
    return VK_SUCCESS;
}

static const VkFormat k_surf_formats[] = {
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
    VK_FORMAT_R8G8B8A8_SRGB,  VK_FORMAT_B8G8R8A8_SRGB,
};

static VkResult klvk_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice p,
        VkSurfaceKHR surf, uint32_t *count, VkSurfaceFormatKHR *out) {
    (void)p; (void)surf;
    uint32_t n = sizeof k_surf_formats / sizeof *k_surf_formats;
    if (!out) { *count = n; return VK_SUCCESS; }
    uint32_t give = *count < n ? *count : n;
    for (uint32_t i = 0; i < give; i++) {
        out[i].format = k_surf_formats[i];
        out[i].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }
    *count = give;
    return give < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult klvk_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice p,
        VkSurfaceKHR surf, uint32_t *count, VkPresentModeKHR *out) {
    (void)p; (void)surf;
    static const VkPresentModeKHR modes[] = {
        VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
    };
    uint32_t n = sizeof modes / sizeof *modes;
    if (!out) { *count = n; return VK_SUCCESS; }
    uint32_t give = *count < n ? *count : n;
    memcpy(out, modes, give * sizeof *out);
    *count = give;
    return give < n ? VK_INCOMPLETE : VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Swapchain — ours end to end
// ---------------------------------------------------------------------------
static uint32_t mem_type(klvk_device *d, uint32_t bits, VkMemoryPropertyFlags want) {
    PFN_vkGetPhysicalDeviceMemoryProperties gmp =
        (PFN_vkGetPhysicalDeviceMemoryProperties)
            real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof mp);
    if (gmp) gmp(d->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

// Forward decls: the swapchain path below registers its images, but the registry
// itself is defined further down beside klvk_image_alloc (which needs VkImage).
static void klvk_img_reg_add(VkImage img);
static int  klvk_img_reg_has(VkImage img);

static VkResult klvk_CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR *ci,
        const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
    (void)alloc;
    klvk_device *d = dev_find(dev);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;

    klvk_swapchain *sc = calloc(1, sizeof *sc);
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->magic = KLVK_SWAP_MAGIC;
    sc->d = d;
    sc->format = ci->imageFormat;
    sc->extent = ci->imageExtent;
    sc->n = ci->minImageCount < 2 ? 2 : ci->minImageCount;
    if (sc->n > KLVK_MAX_IMAGES) sc->n = KLVK_MAX_IMAGES;

    for (uint32_t i = 0; i < sc->n; i++) {
        VkImageCreateInfo ic = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = sc->format,
            .extent = { sc->extent.width, sc->extent.height, 1 },
            .mipLevels = 1,
            .arrayLayers = ci->imageArrayLayers ? ci->imageArrayLayers : 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            // TRANSFER_SRC on top of whatever the guest asked for: the capture
            // copies out of exactly these images.
            .usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = ci->imageSharingMode,
            .queueFamilyIndexCount = ci->queueFamilyIndexCount,
            .pQueueFamilyIndices = ci->pQueueFamilyIndices,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VkResult r = d->CreateImage(dev, &ic, NULL, &sc->images[i]);
        if (r != VK_SUCCESS) { VKI("swapchain image %u: %d\n", i, r); goto fail; }
        klvk_img_reg_add(sc->images[i]);

        VkMemoryRequirements mr;
        d->GetImageMemoryRequirements(dev, sc->images[i], &mr);
        uint32_t mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == UINT32_MAX) mt = mem_type(d, mr.memoryTypeBits, 0);
        VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = mt,
        };
        r = d->AllocateMemory(dev, &ai, NULL, &sc->mem[i]);
        if (r != VK_SUCCESS) { VKI("swapchain memory %u: %d\n", i, r); goto fail; }
        r = d->BindImageMemory(dev, sc->images[i], sc->mem[i], 0);
        if (r != VK_SUCCESS) { VKI("swapchain bind %u: %d\n", i, r); goto fail; }
    }

    *out = (VkSwapchainKHR)sc;
    VKI("vkCreateSwapchainKHR -> %u image(s) %ux%u fmt %d (ours, no presentation engine)\n",
        sc->n, sc->extent.width, sc->extent.height, (int)sc->format);
    return VK_SUCCESS;
fail:
    free(sc);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static klvk_swapchain *swap_of(VkSwapchainKHR h) {
    klvk_swapchain *s = (klvk_swapchain *)h;
    return (s && s->magic == KLVK_SWAP_MAGIC) ? s : NULL;
}

static void klvk_DestroySwapchainKHR(VkDevice dev, VkSwapchainKHR h,
                                     const VkAllocationCallbacks *alloc) {
    (void)alloc;
    klvk_swapchain *sc = swap_of(h);
    if (!sc) return;
    klvk_device *d = sc->d;
    if (d->DeviceWaitIdle) d->DeviceWaitIdle(dev);
    for (uint32_t i = 0; i < sc->n; i++) {
        if (sc->images[i]) d->DestroyImage(dev, sc->images[i], NULL);
        if (sc->mem[i]) d->FreeMemory(dev, sc->mem[i], NULL);
    }
    // The readback scratch is the DEVICE's, not this swapchain's — it outlives
    // any one swapchain and is shared with the eye path.
    sc->magic = 0;
    free(sc);
}

static VkResult klvk_GetSwapchainImagesKHR(VkDevice dev, VkSwapchainKHR h,
        uint32_t *count, VkImage *out) {
    (void)dev;
    klvk_swapchain *sc = swap_of(h);
    if (!sc) return VK_ERROR_INITIALIZATION_FAILED;
    if (!out) { *count = sc->n; return VK_SUCCESS; }
    uint32_t give = *count < sc->n ? *count : sc->n;
    memcpy(out, sc->images, give * sizeof *out);
    *count = give;
    return give < sc->n ? VK_INCOMPLETE : VK_SUCCESS;
}

// Signal whatever the guest asked to be signalled, with no work attached.
//
// This is the one place a real presentation engine is genuinely being stood in
// for: an acquire hands back an image AND a signal that it is safe to use. With
// no engine there is nothing to wait for, so the signal is immediate — an empty
// submit is the only legal way to signal a binary semaphore from the host side.
//
// It goes on the guest's own queue, which is a shared resource: this is safe
// because acquire and submit are called from the same thread in every renderer
// that exists (Vulkan queues are externally synchronized and Unity's render
// thread owns this one). Worth naming, because a second presenting thread would
// make it a race with no error surface.
static VkResult klvk_AcquireNextImageKHR(VkDevice dev, VkSwapchainKHR h,
        uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *index) {
    (void)timeout;
    klvk_swapchain *sc = swap_of(h);
    if (!sc) return VK_ERROR_OUT_OF_DATE_KHR;
    klvk_device *d = sc->d;

    *index = sc->next;
    sc->next = (sc->next + 1) % sc->n;

    if (sem || fence) {
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .signalSemaphoreCount = sem ? 1u : 0u,
            .pSignalSemaphores = sem ? &sem : NULL,
        };
        VkResult r = d->QueueSubmit(d->queue, 1, &si, fence);
        if (r != VK_SUCCESS) VKT("acquire signal submit: %d\n", r);
    }
    (void)dev;
    return VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Present — and the capture
// ---------------------------------------------------------------------------
// Readback scratch big enough for `need` bytes, on the device rather than on a
// swapchain: the OVRPlugin eye path has no swapchain at all and needs the same
// machinery, and the two never capture at once.
static int cap_prepare(klvk_device *d, VkDeviceSize need) {
    if (d->cap_buf && d->cap_size >= need) return 1;
    if (d->cap_buf) {                         // grew — tear the old one down
        if (d->DeviceWaitIdle) d->DeviceWaitIdle(d->dev);
        d->DestroyBuffer(d->dev, d->cap_buf, NULL);
        d->FreeMemory(d->dev, d->cap_mem, NULL);
        d->cap_buf = VK_NULL_HANDLE; d->cap_mem = VK_NULL_HANDLE;
    }

    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = need,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (d->CreateBuffer(d->dev, &bi, NULL, &d->cap_buf) != VK_SUCCESS) return 0;
    d->cap_size = need;

    VkMemoryRequirements mr;
    d->GetBufferMemoryRequirements(d->dev, d->cap_buf, &mr);
    uint32_t mt = mem_type(d, mr.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX)
        mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (mt == UINT32_MAX) return 0;
    VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = mt,
    };
    if (d->AllocateMemory(d->dev, &ai, NULL, &d->cap_mem) != VK_SUCCESS) return 0;
    if (d->BindBufferMemory(d->dev, d->cap_buf, d->cap_mem, 0) != VK_SUCCESS) return 0;

    if (d->cap_pool) return 1;                // pool/cmd/fence are size-independent
    VkCommandPoolCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = d->queue_family,
    };
    if (d->CreateCommandPool(d->dev, &pi, NULL, &d->cap_pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = d->cap_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (d->AllocateCommandBuffers(d->dev, &cbi, &d->cap_cmd) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (d->CreateFence(d->dev, &fi, NULL, &d->cap_fence) != VK_SUCCESS) return 0;
    return 1;
}

// "~/..." is taken from $HOME: on a headset the app's container, whose path
// changes with every install, so a launch can name its Documents folder
// without knowing it (KL_VK_OUT=~/Documents/vkout).
static const char *cap_dir(void) {
    static char buf[1024];
    static int done;
    static const char *dir;
    if (!done) {
        done = 1;
        dir = kl_env_str("KL_VK_OUT", NULL);
        const char *home = getenv("HOME");
        if (dir && dir[0] == '~' && dir[1] == '/' && home && *home) {
            snprintf(buf, sizeof buf, "%s%s", home, dir + 1);
            dir = buf;
        }
    }
    return dir;
}

// Copy any image out and write it as a PNG.
//
// `cur` is the layout the guest left the image in, and getting it wrong is
// cheap here in a way it would not be on a real driver: **MoltenVK does not
// implement image layouts** — Metal has no such concept — so the barrier is in
// practice a memory barrier and the transition is bookkeeping. It is still
// written correctly, because this file is also the model for the device path.
// `layer` is the array slice to read. It is 0 for every 2D image here and 0/1
// for the two eyes of an Array-layout eye image, and it has to travel all the
// way into the copy region: a barrier and a copy that both say slice 0 read the
// LEFT eye twice and produce two identical PNGs, which reads exactly like a
// guest that renders one eye — the failure this capture exists to distinguish.
static int cap_image_layer(klvk_device *d, VkQueue q, VkImage img, uint32_t w, uint32_t h,
                           VkFormat fmt, VkImageLayout cur, uint32_t layer,
                           const char *path,
                           const VkSemaphore *waits, uint32_t nwait) {
    VkDeviceSize need = (VkDeviceSize)w * h * 4;
    if (!cap_prepare(d, need)) { VKI("capture setup failed\n"); return 0; }

    d->ResetCommandBuffer(d->cap_cmd, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    d->BeginCommandBuffer(d->cap_cmd, &bi);

    VkImageMemoryBarrier to_src = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = cur,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1 },
    };
    d->CmdPipelineBarrier(d->cap_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);

    VkBufferImageCopy region = {
        .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { w, h, 1 },
    };
    d->CmdCopyImageToBuffer(d->cap_cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            d->cap_buf, 1, &region);

    // Put it back where the guest left it, or its next render pass starts from
    // a layout it did not expect.
    VkImageMemoryBarrier back = to_src;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = cur;
    d->CmdPipelineBarrier(d->cap_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &back);
    d->EndCommandBuffer(d->cap_cmd);

    // Waiting on the caller's semaphores does double duty: it orders the copy
    // after the guest's rendering, and it CONSUMES signals that would otherwise
    // stay high forever — a binary semaphore nobody waits on is a validation
    // error and, on a real driver, a hang two frames later.
    VkPipelineStageFlags *stages = NULL;
    if (nwait) {
        stages = calloc(nwait, sizeof *stages);
        for (uint32_t i = 0; i < nwait; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = nwait,
        .pWaitSemaphores = nwait ? waits : NULL,
        .pWaitDstStageMask = stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &d->cap_cmd,
    };
    d->ResetFences(d->dev, 1, &d->cap_fence);
    VkResult r = d->QueueSubmit(q, 1, &si, d->cap_fence);
    free(stages);
    if (r != VK_SUCCESS) { VKI("capture submit: %d\n", r); return 0; }
    d->WaitForFences(d->dev, 1, &d->cap_fence, VK_TRUE, 2000000000ull);

    void *mapped = NULL;
    if (d->MapMemory(d->dev, d->cap_mem, 0, need, 0, &mapped) != VK_SUCCESS) return 0;
    VkMappedMemoryRange mmr = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = d->cap_mem, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    if (d->InvalidateMappedMemoryRanges) d->InvalidateMappedMemoryRanges(d->dev, 1, &mmr);

    uint8_t *px = mapped;
    size_t n = (size_t)w * h;
    if (fmt_is_bgra(fmt))
        for (size_t i = 0; i < n; i++) {
            uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t;
        }
    // Alpha is not a coverage value here. A guest that never authors alpha
    // leaves it 0, and a correct RGB frame then
    // writes out as a fully TRANSPARENT PNG, which is visually identical to
    // black and has no error surface at all.
    for (size_t i = 0; i < n; i++) px[i * 4 + 3] = 255;

    // Is there anything in it? A dark frame and a never-written frame look the
    // same in a file browser, and this is the whole question the arc is asking.
    uint64_t sum = 0;
    size_t lit = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned v = px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2];
        sum += v;
        if (v) lit++;
    }

    // No path: the caller wants only the answer to "is there anything in it".
    if (!path) {
        d->UnmapMemory(d->dev, d->cap_mem);
        VKI("eye picture check: %ux%u layer %u — %.1f%% of the pixels are lit, "
            "mean %.1f / 255\n", w, h, layer, n ? 100.0 * (double)lit / (double)n : 0.0,
            n ? (double)sum / (double)(n * 3) : 0.0);
        return 1;
    }
    int ok = png_write_rgba(path, px, (int)w, (int)h);
    d->UnmapMemory(d->dev, d->cap_mem);
    if (ok) {
        g_captured++;
        VKI("-> %s (%zu lit / %zu, mean %.1f)\n", path, lit, n,
            n ? (double)sum / (double)(n * 3) : 0.0);
    } else {
        VKI("PNG write failed (%s)\n", path);
    }
    return ok;
}

static int cap_image(klvk_device *d, VkQueue q, VkImage img, uint32_t w, uint32_t h,
                     VkFormat fmt, VkImageLayout cur, const char *path,
                     const VkSemaphore *waits, uint32_t nwait) {
    return cap_image_layer(d, q, img, w, h, fmt, cur, 0, path, waits, nwait);
}

static void cap_frame(klvk_swapchain *sc, VkQueue q, uint32_t idx,
                      const VkSemaphore *waits, uint32_t nwait) {
    char path[1024];
    snprintf(path, sizeof path, "%s/vk_%05u.png", cap_dir(), g_presented);
    cap_image(sc->d, q, sc->images[idx], sc->extent.width, sc->extent.height,
              sc->format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, path, waits, nwait);
}

// ---------------------------------------------------------------------------
// The OVRPlugin eye layer — the path this guest actually takes
// ---------------------------------------------------------------------------
//
// BONELAB never creates a WSI swapchain. Unity's Oculus XR path asks OVRPlugin
// for its eye textures instead (`ovrp_GetLayerTexture2`) and submits them to the
// Oculus compositor, so `VK_KHR_swapchain` above is exercised by nothing on this
// title — it stays because it is the general answer for a flat Vulkan guest and
// because the two share every line of the capture below.
//
// kl_ovrp.c owns that seam and has always answered with GL texture names. On the
// Vulkan path those names go straight into `vkCreateImageView` as VkImage
// handles, which is a segfault inside MoltenVK with a GL name for an address.
// So the eye textures have to be real VkImages, and this is where they are made.
// Two eye LAYOUTS reach here, and they differ in how many images exist rather
// than in anything about one image:
//
//   Stereo (ovrpLayout 0) — one 2D image per (stage, eye), array layer 0.
//   Array  (ovrpLayout 3) — ONE image per stage with two array layers, handed
//                           back for both eyes; the eye IS the layer index.
//
// The second is what Unity calls Single Pass Instanced / Multiview, and it is
// how this title ships on a Quest. `layers` is therefore a property of the
// stage, not of the eye: kl_ovrp.c passes 2 for Array and 1 for Stereo, and the
// slot for eye 1 is left empty under Array so nothing can hand out a second
// image that does not exist.
#define KLVK_EYE_STAGES 4
typedef struct {
    VkImage        img;
    VkDeviceMemory mem;
    uint32_t       w, h;
    uint32_t       layers;
    VkFormat       fmt;
} klvk_img_slot;

static klvk_img_slot g_eye[KLVK_EYE_STAGES][2];

// ...and the OTHER layers, which until now shared the table above.
//
// `ovrp_GetLayerTexture2` names a LAYER, and the storage was keyed on (stage,
// eye) alone — so every layer the guest set up was handed the eye layer's
// images. For a Unity guest that was invisible, because its only other layer is
// a 1x1 dummy nothing renders into. An Unreal guest has real ones: OculusHMD's
// FSplash and its IStereoLayers quads are how RE4 draws its intro logos, and
// they were being rendered straight into the corner of the eye image — a small
// logo in the top-left of an otherwise black eye, which reads as a broken
// projection rather than as a layer with no storage of its own.
//
// Small and fixed: a guest that runs out says so by name rather than silently
// aliasing again, which is the failure this replaces.
#define KLVK_MAX_LAYERS 64
static struct {
    int           key;                       // the guest's layer id; 0 = free
    unsigned      seq;                        // claim order, for recycling oldest
    klvk_img_slot s[KLVK_EYE_STAGES][2];
} g_layer_img[KLVK_MAX_LAYERS];
static VkImage g_wild_scratch;   // wild-handle stand-in (see klvk_wild_scratch); defined lazily
static VkImage g_content_rt; static uint32_t g_content_rt_w, g_content_rt_h, g_content_rt_area; // KL_RT_DEBUG: largest UI render target
#ifndef KLVK_SCRATCH_MIPS
#ifndef KLVK_SCRATCH_MIPS
#define KLVK_SCRATCH_MIPS   12u
#define KLVK_SCRATCH_LAYERS 2u
#endif
#endif
static unsigned g_wild_copy_redir, g_wild_blit_redir; // commands rerouted onto the scratch
static unsigned g_layer_img_seq;

int kl_vulkan_guest_active(void) { return g_avail && g_ndev > 0; }

// The LUID of the physical device the guest's instance sees, for ovrp_GetDisplay
// AdapterId2. Unity (OculusXR) asks OVRPlugin which GPU is the VR one, then walks
// vkEnumeratePhysicalDevices matching each device's VkPhysicalDeviceIDProperties.
// deviceLUID against the answer — and hands the MATCH to vkGetPhysicalDeviceQueue
// FamilyProperties. Answering a zero LUID (the old GetDeviceId2 stub) matched
// nothing once MoltenVK 1.4.2 began reporting a real, non-zero deviceLUID, so
// Unity selected a NULL VkPhysicalDevice and crashed inside MVKPhysicalDevice::
// getQueueFamilies. Report the real one.
//
// It must be CACHED at instance-create time, not queried on demand: Unity creates
// a throwaway instance to enumerate, DESTROYS it, and only then calls
// GetDisplayAdapterId2 — so by the time the id is asked for, g_instance points at
// a torn-down handle. The LUID is a property of the GPU (constant for the run), so
// caching the first live instance's answer is exactly right.
static uint8_t g_display_luid[8];
static int     g_display_luid_valid;

// The same device as a HANDLE, for the OVRPlugin generation whose
// ovrp_GetDisplayAdapterId2 answer IS the VkPhysicalDevice (see kl_ovrp.c).
// Unlike the LUID this is a property of the instance, so it is refreshed on
// every create rather than cached once.
static VkPhysicalDevice g_display_phys;

void *kl_vulkan_display_phys(void) { return (void *)g_display_phys; }

static void cache_display_luid(VkInstance inst) {
    if (!inst || !real_gipa) return;
    PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)
        real_gipa(inst, "vkEnumeratePhysicalDevices");
    if (en) {
        uint32_t n1 = 1;
        VkPhysicalDevice pd1 = VK_NULL_HANDLE;
        if (en(inst, &n1, &pd1) >= 0 && pd1) g_display_phys = pd1;
    }
    if (g_display_luid_valid) return;
    PFN_vkGetPhysicalDeviceProperties2 gp2 = (PFN_vkGetPhysicalDeviceProperties2)
        real_gipa(inst, "vkGetPhysicalDeviceProperties2");
    if (!en || !gp2) return;
    uint32_t n = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (en(inst, &n, &pd) < 0 || !pd) return;
    VkPhysicalDeviceIDProperties idp;
    memset(&idp, 0, sizeof idp);
    idp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 p2;
    memset(&p2, 0, sizeof p2);
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &idp;
    gp2(pd, &p2);
    if (!idp.deviceLUIDValid) return;
    memcpy(g_display_luid, idp.deviceLUID, 8 /* VK_LUID_SIZE */);
    g_display_luid_valid = 1;
    VKI("cached display adapter LUID %02x%02x%02x%02x%02x%02x%02x%02x\n",
        g_display_luid[0], g_display_luid[1], g_display_luid[2], g_display_luid[3],
        g_display_luid[4], g_display_luid[5], g_display_luid[6], g_display_luid[7]);
}

// Returns 1 and fills out[8] once any Vulkan instance has existed and reported a
// valid LUID; 0 (out untouched) for a guest that never made a Vulkan instance
// (i.e. GLES), where the adapter id must stay empty.
int kl_vulkan_display_luid(uint8_t out[8]) {
    if (!out) return 0;
    if (!g_display_luid_valid) cache_display_luid(g_instance); // fallback if still live
    if (!g_display_luid_valid) return 0;
    memcpy(out, g_display_luid, 8);
    return 1;
}

// The GPU's Vulkan vendor and device ids, read before the guest has an instance
// (a throwaway one of our own): for Half-Life: Alyx's video settings file, which
// the game throws away for its automatic setup unless the VendorID/DeviceID in it
// are the GPU's (kl_app.c). MoltenVK's device id for an Apple GPU carries the OS
// version beside the GPU family — 0x1B02000A on an M5 Vision Pro at visionOS 27.2
// (the game's own file, 2026-10-13), 0x1B00000A at 27.0 (Glovebox's) — so it
// changes with a system update as well as between devices. Returns 1 and fills
// both, or 0 without MoltenVK. The ids are MoltenVK's own, not the spoofed ones
// some guests are shown (klvk_spoof_props); Alyx is shown the real ones.
//
// Not before the MoltenVK settings are in the environment (kl_driver_init):
// MoltenVK reads them once, at the first instance, and this makes it.
// (After Glovebox's kl_vulkan_gpu_ids, github.com/glovebox-vr/glovebox, MIT.)
int kl_vulkan_gpu_ids(unsigned *vendor, unsigned *device) {
    if (!vendor || !device) return 0;
    vk_init();
    if (!g_avail) return 0;
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)real_gipa(NULL, "vkCreateInstance");
    if (!ci) return 0;
    VkApplicationInfo app;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (ci(&info, NULL, &inst) != VK_SUCCESS || !inst) return 0;
    PFN_vkEnumeratePhysicalDevices en =
        (PFN_vkEnumeratePhysicalDevices)real_gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties gp =
        (PFN_vkGetPhysicalDeviceProperties)real_gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)real_gipa(inst, "vkDestroyInstance");
    uint32_t n = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    int ok = 0;
    if (en && gp && en(inst, &n, &pd) >= 0 && pd) {
        VkPhysicalDeviceProperties pr;
        gp(pd, &pr);
        *vendor = pr.vendorID;
        *device = pr.deviceID;
        ok = 1;
    }
    if (di) di(inst, NULL);
    return ok;
}

// The live instance's first physical device — the substitute for a guest that
// reaches a vkGetPhysicalDevice*/vkCreateDevice call with a NULL handle. AC Nexus
// (Unity) does exactly that: its GfxDeviceVulkan runs a whole battery of device
// queries (QueueFamilyProperties, Features2, MemoryProperties, ...) and then
// vkCreateDevice on a physicalDevice that came out NULL, and MoltenVK dereferences
// null in each. There is exactly one GPU, so a NULL handle is unambiguous —
// substitute it everywhere rather than crash. Enumerated fresh off g_instance each
// time so it is always a handle valid for the LIVE instance (the guest may have
// destroyed the one it enumerated). Cheap: MoltenVK caches its physical devices.
static VkPhysicalDevice live_phys0(void) {
    if (!g_instance || !real_gipa) return VK_NULL_HANDLE;
    PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)
        real_gipa(g_instance, "vkEnumeratePhysicalDevices");
    if (!en) return VK_NULL_HANDLE;
    uint32_t n = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    en(g_instance, &n, &pd);
    return pd;
}
// Substitute + one-time-per-callsite note. Evaluates to the usable handle.
static VkPhysicalDevice klvk_sub_phys(const char *where) {
    VkPhysicalDevice sub = live_phys0();
    static const char *last;
    if (last != where) { last = where;
        VKI("%s: guest passed NULL physicalDevice — substituting %p\n",
            where, (void *)sub); }
    return sub;
}

// ---------------------------------------------------------------------------
// The compositor seam — the MTLTexture behind an eye VkImage
// ---------------------------------------------------------------------------
//
// Both compositors (KleptonCompositor.swift on device, kl_view_mtl.m on the
// host) sample an `id<MTLTexture>` the guest rendered into, found through
// `kl_glfb_eye_mtl_texture`. On the GL path the host ALLOCATES that texture and
// ANGLE is told to use it as the eye's storage. On the Vulkan path the
// direction is the other way round and it is simpler: MoltenVK has already
// backed our VkImage with an MTLTexture, so there is nothing to import,
// negotiate a format for, or keep in step — we ask for the one that exists.
//
// **That asymmetry is the whole reason there is no provider call here.** An
// import (`VkImportMetalTextureInfoEXT`, or the deprecated `vkSetMTLTextureMVK`)
// would let the host choose the storage, and buys nothing: this eye image is
// never a drawable, the compositor only ever reads it, and a host-chosen
// texture would have to agree with the VkFormat exactly — the same
// "Incompatible format" class kl_glfb.h records for ANGLE, in an API where the
// failure is silent instead.
//
// Two entry points can answer, and they are tried in that order:
//
//   vkExportMetalObjectsEXT   VK_EXT_metal_objects, the standard one. Needs the
//                             extension ENABLED on the device, which is why
//                             klvk_CreateDevice adds it.
//   vkGetMTLTextureMVK        MoltenVK's own, deprecated but a plain dylib
//                             export that needs no extension — the fallback for
//                             a device created before this existed.
//
// Failure is not fatal anywhere: the capture path reads the VkImage directly
// and does not need this at all. A run with no compositor (every host run today)
// simply publishes a texture nobody samples.
static void *eye_mtl_texture(klvk_device *d, VkImage img) {
    if (!d || !img) return NULL;

    // vkGetMTLTextureMVK's own prototype lives in MoltenVK/mvk_deprecated_api.h,
    // which is an Objective-C header (`id<MTLTexture> *`) and cannot be included
    // from this plain-C file. The ABI is a pointer either way, so it is declared
    // here rather than dragging the translation unit into ObjC for one symbol.
    typedef void (*pfn_get_mtl_texture_mvk)(VkImage, void **);

    static PFN_vkExportMetalObjectsEXT export_ext;
    static pfn_get_mtl_texture_mvk     get_mvk;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        export_ext = (PFN_vkExportMetalObjectsEXT)
            (real_gdpa ? real_gdpa(d->dev, "vkExportMetalObjectsEXT") : NULL);
        get_mvk = (pfn_get_mtl_texture_mvk)mvk_sym("vkGetMTLTextureMVK");
        if (!export_ext && !get_mvk)
            VKI("no way to reach the MTLTexture behind an eye image — neither "
                "vkExportMetalObjectsEXT nor vkGetMTLTextureMVK resolves; the "
                "compositor will have nothing to sample (the capture is "
                "unaffected)\n");
    }

    if (export_ext) {
        VkExportMetalTextureInfoEXT tex = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
        .image = img,
            .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
        };
        VkExportMetalObjectsInfoEXT info = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
            .pNext = &tex,
        };
        export_ext(d->dev, &info);
        if (tex.mtlTexture) return (void *)tex.mtlTexture;
    }
    if (get_mvk) {
        void *t = NULL;
        get_mvk(img, &t);
        return t;
    }
    return NULL;
}

// Tell kl_glfb which MTLTexture is which eye, so every compositor and readback
// finds the Vulkan eyes through the same accessor it already uses for the GL
// ones. Under the Array layout one texture serves both eyes and the eye is the
// SLICE, which is exactly what that table's `slice` field was added for.
static void eye_publish_mtl(int stage, int eye, int layers) {
    klvk_device *d = g_devs[0];
    VkImage img = g_eye[stage][eye].img;
    void *tex = eye_mtl_texture(d, img);
    if (!tex) return;
    int w = (int)g_eye[stage][eye].w, h = (int)g_eye[stage][eye].h;
    if (layers > 1) {
        kl_glfb_note_eye_mtl_texture(0, stage, tex, 0, w, h);
        kl_glfb_note_eye_mtl_texture(1, stage, tex, 1, w, h);
    } else {
        kl_glfb_note_eye_mtl_texture(eye, stage, tex, 0, w, h);
    }
}

// A registry of every VkImage this process actually created - the guest's via
// the vkCreateImage wrapper, ours via klvk_image_alloc, and the WSI swapchain's.
// The image-view wrapper consults it to refuse a handle no create ever produced:
// AC Nexus feeds vkCreateImageView a deterministic wild pointer (it lands in
// MoltenVK's own read-only data), and MoltenVK's MVKImageView constructor
// dereferences it and takes SIGBUS instead of returning an error. Refusing turns
// that into a clean failure. Locked - guest worker threads create both at once -
// and FAIL-OPEN if it ever fills, so a legitimate view is never wrongly refused.
#define KLVK_IMG_REG_MAX 16384
static VkImage g_img_reg[KLVK_IMG_REG_MAX];
static int g_img_reg_n;
static pthread_mutex_t g_img_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static void klvk_img_reg_add(VkImage img) {
    if (!img) return;
    pthread_mutex_lock(&g_img_reg_lock);
    if (g_img_reg_n < KLVK_IMG_REG_MAX) g_img_reg[g_img_reg_n++] = img;
    pthread_mutex_unlock(&g_img_reg_lock);
}
static void klvk_img_reg_del(VkImage img) {
    if (!img) return;
    pthread_mutex_lock(&g_img_reg_lock);
    for (int i = 0; i < g_img_reg_n; i++)
        if (g_img_reg[i] == img) { g_img_reg[i] = g_img_reg[--g_img_reg_n]; break; }
    pthread_mutex_unlock(&g_img_reg_lock);
}

// ---------------------------------------------------------------------------
// KL_TRACE: one-build render dataflow tracer. Gated so it costs nothing off.
// ---------------------------------------------------------------------------
static int klvk_trace(void) { static int v=-1; if (v<0) v=kl_env_on("KL_TRACE",0); return v; }
static unsigned klvk_tid(void) { uint64_t t=0; pthread_threadid_np(NULL,&t); return (unsigned)t; }
#define klvk_trace_log(...) fprintf(stderr, "[TRACE] " __VA_ARGS__)
// Return 1 the first time a given (a,b) pair is seen, so per-frame-repeated
// render passes and copies each log ONCE instead of flooding the trace.
static int klvk_trace_once(unsigned long long a, unsigned long long b) {
    static struct { unsigned long long a, b; } seen[2048]; static int n;
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    int fresh = 1;
    pthread_mutex_lock(&lk);
    for (int i = 0; i < n; i++) if (seen[i].a==a && seen[i].b==b) { fresh = 0; break; }
    if (fresh && n < 2048) { seen[n].a=a; seen[n].b=b; n++; }
    pthread_mutex_unlock(&lk);
    return fresh;
}
// Label an image: is it an eye layer, a menu overlay panel (returns its key), or
// a plain tracked/wild image.
static int klvk_is_eye_image(VkImage img) {
    if (!img) return 0;
    for (int st=0; st<KLVK_EYE_STAGES; st++) for (int e=0;e<2;e++)
        if (g_eye[st][e].img==img) return 1;
    return 0;
}
static int klvk_layer_image_key(VkImage img) {
    if (!img) return 0;
    for (int i=0;i<KLVK_MAX_LAYERS;i++) { if (!g_layer_img[i].key) continue;
        for (int st=0; st<KLVK_EYE_STAGES; st++) for (int e=0;e<2;e++)
            if (g_layer_img[i].s[st][e].img==img) return g_layer_img[i].key; }
    return 0;
}
// view -> image, so a framebuffer's attachment views resolve to their images.
#define KLVK_VIEW_MAP_MAX 8192
static struct { VkImageView view; VkImage img; } g_view_map[KLVK_VIEW_MAP_MAX];
static int g_view_map_n;
static pthread_mutex_t g_view_map_lock = PTHREAD_MUTEX_INITIALIZER;
static void klvk_view_map_add(VkImageView v, VkImage img) {
    if (!v) return;
    pthread_mutex_lock(&g_view_map_lock);
    if (g_view_map_n<KLVK_VIEW_MAP_MAX){ g_view_map[g_view_map_n].view=v; g_view_map[g_view_map_n].img=img; g_view_map_n++; }
    pthread_mutex_unlock(&g_view_map_lock);
}
static VkImage klvk_view_image(VkImageView v) {
    VkImage img=VK_NULL_HANDLE;
    pthread_mutex_lock(&g_view_map_lock);
    for (int i=g_view_map_n-1;i>=0;i--) if (g_view_map[i].view==v){ img=g_view_map[i].img; break; }
    pthread_mutex_unlock(&g_view_map_lock);
    return img;
}
// compact label for a log line
static const char *klvk_img_tag(VkImage img) {
    if (!img) return "nil";
    if (klvk_is_eye_image(img)) return "EYE";
    if (klvk_layer_image_key(img)) return "panel";
    if (!klvk_img_reg_has(img)) return "WILD";
    return "trk";
}
static int klvk_img_reg_has(VkImage img) {
    int found = 0;
    pthread_mutex_lock(&g_img_reg_lock);
    if (g_img_reg_n >= KLVK_IMG_REG_MAX) found = 1;   /* full -> fail open */
    else for (int i = 0; i < g_img_reg_n; i++)
        if (g_img_reg[i] == img) { found = 1; break; }
    pthread_mutex_unlock(&g_img_reg_lock);
    return found;
}

// Adopt-on-use. A handle the guest presents that no create of ours logged is,
// in AC Nexus, a real Unity render target created through a Vulkan entry our
// dispatch did not intercept - NOT the wild overlay pointer the strict guard
// was built for (that source was removed upstream by controlling
// EnqueueSetupLayer2). Refusing such a handle makes MoltenVK reject Unity's
// render target, and Unity then renders nothing at all: it logs "temporary
// render texture not found" and every eye capture comes back 0-lit black. So
// The default is STRICT REFUSE. Adopting was tried and PROVEN unsafe: AC Nexus
// hands vkCreateImageView a handle (e.g. 0x..59d0, format 37 mips 0+2) right
// after it sets up its 2x2 Equirect placeholder layer - Unity's bogus native
// texture pointer for the loading skybox - and it points into a read-only
// mapping, so MVKImageView's constructor takes SIGBUS (signal 10) dereferencing
// it. Refusing turns that crash into a clean failure. KL_VK_IMG_ADOPT=1 opts
// back into adopting for a deliberate experiment, but it is known to crash on
// this title - do not enable it by default.
static int klvk_img_adopt_mode(void) {
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("KL_VK_IMG_ADOPT");
        m = (e && e[0] == '1') ? 1 : 0;   /* default OFF - adopting SIGBUSes */
    }
    return m;
}
static int klvk_img_reg_ok(VkImage img) {
    if (!img) return 1;
    if (klvk_img_reg_has(img)) return 1;
    if (!klvk_img_adopt_mode()) return 0;
    klvk_img_reg_add(img);
    static int adopted;
    if (adopted < 64) {
        adopted++;
        VKI("vkImage %#llx ADOPTED - no create of ours logged it, but the guest "
            "holds it as a real resource; letting it through "
            "(KL_VK_IMG_ADOPT=0 refuses instead)\n",
            (unsigned long long)(uintptr_t)img);
    }
    return 1;
}

// Create an image and back it with device-local memory.
//
// Shared by the two callers that need one — the OVRPlugin eye images below and
// the OpenXR swapchain images (kl_vulkan_xr_image) — because the sequence is
// identical and only the description differs. Written out twice it would be two
// copies of the memory-type fallback, which is exactly the kind of thing that
// drifts and then differs on the path nobody ran.
static VkImage klvk_image_alloc(klvk_device *d, VkFormat fmt,
                                unsigned w, unsigned h, uint32_t layers,
                                uint32_t mips, VkImageUsageFlags usage,
                                VkDeviceMemory *mem_out) {
    if (mem_out) *mem_out = VK_NULL_HANDLE;
    VkImageCreateInfo ic = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = { w, h, 1 },
        .mipLevels = mips ? mips : 1,
        .arrayLayers = layers ? layers : 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    // Unity's Vulkan renderer creates an alternate-format VIEW of a colour eye
    // texture (the sRGB<->UNORM pair), which is undefined unless the image was
    // told its format may vary. Without this AC Nexus faults inside MoltenVK's
    // MVKImageView constructor building that view; a same-format view (BONELAB)
    // is unaffected, so this only ever ADDS a capability.
    if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        ic.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VkImage img = VK_NULL_HANDLE;
    if (d->CreateImage(d->dev, &ic, NULL, &img) != VK_SUCCESS) {
        VKI("image %ux%u (%u layer(s)): vkCreateImage failed\n", w, h, layers);
        return VK_NULL_HANDLE;
    }
    VkMemoryRequirements mr;
    d->GetImageMemoryRequirements(d->dev, img, &mr);
    uint32_t mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) mt = mem_type(d, mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = mt,
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (d->AllocateMemory(d->dev, &ai, NULL, &mem) != VK_SUCCESS ||
        d->BindImageMemory(d->dev, img, mem, 0) != VK_SUCCESS) {
        VKI("image %ux%u: memory failed\n", w, h);
        d->DestroyImage(d->dev, img, NULL);
        return VK_NULL_HANDLE;
    }
    if (mem_out) *mem_out = mem;
    klvk_img_reg_add(img);
    return img;
}

// A non-eye layer's slot table, found or claimed by the guest's layer id.
// NULL when the table is full, which is named by the caller.
static klvk_img_slot *layer_slots_for(int key) {
    for (int i = 0; i < KLVK_MAX_LAYERS; i++)
        if (g_layer_img[i].key == key) return &g_layer_img[i].s[0][0];
    for (int i = 0; i < KLVK_MAX_LAYERS; i++)
        if (!g_layer_img[i].key) {
            g_layer_img[i].key = key;
            g_layer_img[i].seq = ++g_layer_img_seq;
            return &g_layer_img[i].s[0][0];
        }
    // Full: recycle the OLDEST-claimed slot rather than refusing. AC Nexus
    // re-creates its overlays across loading phases with a fresh layer id each
    // time and never frees the old ones, so a fixed table fills and
    // GetLayerTexture2 then failed for the rest - which is what kept the render
    // loop from ever finishing a frame. Evicting the earliest-claimed slot
    // keeps the newest (currently-visible) layers. The old slot's images are
    // left allocated (a compositor may sample them this instant) and are
    // resized/reused by kl_vulkan_layer_image for the new key.
    int oldest = 0;
    for (int i = 1; i < KLVK_MAX_LAYERS; i++)
        if (g_layer_img[i].seq < g_layer_img[oldest].seq) oldest = i;
    static unsigned evicted;
    if (evicted++ % 32 == 0)
        VKI("layer image table full, recycling oldest slot (key %d -> %d, %u so far)\n",
            g_layer_img[oldest].key, key, evicted);
    g_layer_img[oldest].key = key;
    g_layer_img[oldest].seq = ++g_layer_img_seq;
    return &g_layer_img[oldest].s[0][0];
}

// See kl_vulkan.h. The rename, not a copy: the table entry keeps its images
// and its age, only the key changes — so a compositor already sampling the
// MTLTexture behind it never sees the storage move.
void kl_vulkan_layer_rekey(int old_key, int new_key) {
    if (old_key == new_key || old_key == KLVK_EYE_LAYER || new_key == KLVK_EYE_LAYER)
        return;
    for (int i = 0; i < KLVK_MAX_LAYERS; i++) {
        if (g_layer_img[i].key != old_key) continue;
        g_layer_img[i].key = new_key;
        g_layer_img[i].seq = ++g_layer_img_seq;    // it is the NEWEST layer now
        static unsigned n;
        if (n++ % 32 == 0)
            VKI("layer %d re-keyed to %d — the recreated layer keeps its images "
                "(%u rekeys so far)\n", old_key, new_key, n);
        return;
    }
}

unsigned long long kl_vulkan_layer_image(int layer_key, int stage, int eye,
                                         unsigned w, unsigned h, int fmt_kind, int layers) {
    if (!kl_vulkan_guest_active()) return 0;
    if (stage < 0 || stage >= KLVK_EYE_STAGES || eye < 0 || eye > 1) return 0;
    if (!w || !h) return 0;
    if (layers < 1) layers = 1;
    if (layers > 2) layers = 2;
    // Under the Array layout there is one image for the stage and both eyes get
    // it; slot 0 owns it, so the eye argument selects a LAYER later and not a
    // second allocation here.
    if (layers > 1) eye = 0;
    klvk_device *d = g_devs[0];

    int is_eye = layer_key == KLVK_EYE_LAYER;
    klvk_img_slot *slots = is_eye ? &g_eye[0][0] : layer_slots_for(layer_key);
    if (!slots) {
        VKI("layer %d: all %d layer image slots are taken — refusing rather than "
            "handing back another layer's storage\n", layer_key, KLVK_MAX_LAYERS);
        return 0;
    }
    klvk_img_slot *slot = is_eye ? &g_eye[stage][eye] : &slots[stage * 2 + eye];

    // A half-float layer gets half-float storage. Ghostbusters: Rise of the
    // Ghost Lord (x4746232908818706) sets up a 1024x280 UI quad as
    // R16G16B16A16_FP and makes an RGBA16F view of the image it is handed; on
    // RGBA8 storage Metal's validation aborts in newTextureViewWithPixelFormat
    // ("not compatible with texture view pixelFormat", Mac host, 2026-10-18),
    // and without validation the view reads the bytes as the wrong format.
    // Overlays are sampled by the compositor, which takes either. The eye
    // layer stays RGBA8: its table and every readback assume it.
    int srgb = fmt_kind == KLVK_LAYER_SRGB;
    int fp16 = fmt_kind == KLVK_LAYER_FP16 && !is_eye;
    VkFormat fmt = fp16 ? VK_FORMAT_R16G16B16A16_SFLOAT
                 : srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    // A cached image is only the right answer if it is the image that was
    // ASKED for. RE4 sets up its eye layer twice — 2290x2400, then 2748x2880
    // once UE4 applies its own pixel density — and the size was not consulted,
    // so the second layer was handed the first one's storage and the engine
    // rendered a 2748x2880 frame into a 2290x2400 image.
    //
    // The old image is NOT destroyed. It is the GL path's rule for the GL path's
    // reason: a compositor or a capture may be sampling it on another thread this
    // instant, and freeing storage under them is a worse failure than holding a
    // few images that no longer describe anything. This happens at startup, once
    // per stage.
    if (slot->img && (slot->w != w || slot->h != h ||
                      slot->layers != (uint32_t)layers || slot->fmt != fmt)) {
        VKI("layer %d stage %d %s: was %ux%u (%u layer(s)), now %ux%u (%d) — "
            "allocating fresh; the old image is kept, not freed\n",
            layer_key, stage, eye ? "eye 1" : "eye 0",
            slot->w, slot->h, slot->layers, w, h, layers);
        slot->img = VK_NULL_HANDLE;
        slot->mem = VK_NULL_HANDLE;
    }
    if (slot->img) return (uint64_t)(uintptr_t)slot->img;

    // The guest renders into it, samples it for its own post chain, and we copy
    // out of it. TRANSFER_DST is there because Unity clears some targets with
    // vkCmdClearColorImage rather than a load op.
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImage img = klvk_image_alloc(d, fmt, w, h, (uint32_t)layers, 1,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT, &mem);
    if (img == VK_NULL_HANDLE) return 0;
    // KL_VK_EYE_TINT=1 pre-clears the image to a colour nothing in a rendered
    // scene produces, which is the A/B that separates the two ways an eye
    // texture can come back wrong. They are indistinguishable in the capture
    // otherwise: if the tint survives to the PNG, the guest never drew into
    // this image at all; if it does not, the guest drew and the content is
    // genuinely what it rendered. Off by default — it costs a submit per image
    // and it is a diagnostic, not a fix.
    //
    // ...and every fresh eye image is cleared to transparent black by default
    // (KL_VK_EYE_CLEAR=0 leaves it as it comes). Metal does not zero a new
    // texture: until the guest first draws into it, it shows whatever that
    // memory held before — old frames, a different one in each eye's slice.
    // Medal of Honor drops its eye to 3600x2888 while a level loads, the new
    // images were shown before the engine drew into them, and the owner saw
    // "strange frames, something different in each eye" (2026-10-18).
    int tint = kl_env_on("KL_VK_EYE_TINT", 0);
    int clear = tint || (is_eye && kl_env_on("KL_VK_EYE_CLEAR", 1));
    if (clear && d->CmdClearColorImage &&
        cap_prepare(d, 4)) {
        VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        d->ResetCommandBuffer(d->cap_cmd, 0);
        d->BeginCommandBuffer(d->cap_cmd, &bi);
        VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
            // EVERY layer: under the Array layout the two eyes are layers of
            // this one image, and tinting only layer 0 would leave the right
            // eye's "did the guest draw?" question unanswered.
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, (uint32_t)layers },
        };
        d->CmdPipelineBarrier(d->cap_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        // A DIFFERENT colour per array layer, and that is the second question
        // this diagnostic answers. Under the Array layout the two eyes are two
        // slices of one image, and "the guest drew" and "the capture reads the
        // right slice" are separate failures that look identical in the files:
        // a capture that reads layer 0 twice produces two byte-identical PNGs,
        // which is exactly what a guest rendering only the left eye produces
        // too. Green for eye 0, blue for eye 1 — so a run says which.
        static const VkClearColorValue TINT[2] = {
            { .float32 = { 0.0f, 1.0f, 0.0f, 1.0f } },   // eye 0: green
            { .float32 = { 0.0f, 0.0f, 1.0f, 1.0f } },   // eye 1: blue
        };
        static const VkClearColorValue BLACK = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
        for (int ly = 0; ly < layers; ly++) {
            VkImageSubresourceRange rng =
                { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, (uint32_t)ly, 1 };
            d->CmdClearColorImage(d->cap_cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  tint ? &TINT[layers > 1 ? ly : (eye & 1)] : &BLACK, 1, &rng);
        }
        d->EndCommandBuffer(d->cap_cmd);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1, .pCommandBuffers = &d->cap_cmd };
        d->ResetFences(d->dev, 1, &d->cap_fence);
        if (d->QueueSubmit(d->queue, 1, &si, d->cap_fence) == VK_SUCCESS)
            d->WaitForFences(d->dev, 1, &d->cap_fence, VK_TRUE, 2000000000ull);
        if (tint)
            VKI("eye image stage %d pre-cleared: %s (KL_VK_EYE_TINT)\n", stage,
                layers > 1 ? "layer 0 GREEN, layer 1 BLUE"
                           : (eye ? "eye 1 BLUE" : "eye 0 GREEN"));
        else
            VKI("eye image stage %d (%ux%u) cleared before the guest's first frame in it\n", stage, w, h);
    }

    slot->img = img;
    slot->mem = mem;
    slot->w = w;
    slot->h = h;
    slot->layers = (uint32_t)layers;
    slot->fmt = fmt;
    // Only the EYE layer is published into kl_glfb's eye table. That table is
    // what every compositor and every readback samples, so a splash quad
    // announcing itself there would replace the eye on screen with a logo.
    if (is_eye) eye_publish_mtl(stage, eye, layers);
    VKI("%s stage %d %s = VkImage %p (%ux%u %s)\n",
        is_eye ? "eye image" : "layer image",
        stage,
        layers > 1 ? "both eyes (2 array layers)" :
                     (eye ? "eye 1" : "eye 0"),
        (void *)img, w, h, fp16 ? "R16G16B16A16_SFLOAT" : srgb ? "R8G8B8A8_SRGB" : "R8G8B8A8_UNORM");
    return (uint64_t)(uintptr_t)img;
}

// See kl_vulkan.h. The guest renders its motion-vector pass into these; the
// compositor's frame generation reads the motion vectors of the stage it shows
// (kl_vulkan_spacewarp_mtl).
static klvk_img_slot g_sw[2][KLVK_EYE_STAGES];
unsigned long long kl_vulkan_spacewarp_image(int depth, int stage,
                                             unsigned w, unsigned h, int layers) {
    if (!kl_vulkan_guest_active()) return 0;
    if (stage < 0 || stage >= KLVK_EYE_STAGES || !w || !h) return 0;
    if (layers < 1) layers = 1;
    if (layers > 2) layers = 2;
    klvk_img_slot *slot = &g_sw[depth ? 1 : 0][stage];
    // D24_S8 is what the guest names, and it is not a format this GPU has; the
    // 32-bit float depth is what its own "24 bits or more" targets already use.
    VkFormat fmt = depth ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_R16G16B16A16_SFLOAT;
    if (slot->img && (slot->w != w || slot->h != h ||
                      slot->layers != (uint32_t)layers || slot->fmt != fmt)) {
        slot->img = VK_NULL_HANDLE;      // kept, not freed — as for the eye images
        slot->mem = VK_NULL_HANDLE;
    }
    if (slot->img) return (uint64_t)(uintptr_t)slot->img;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImage img = klvk_image_alloc(g_devs[0], fmt, w, h, (uint32_t)layers, 1,
                                   (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                          : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT) |
                                   VK_IMAGE_USAGE_SAMPLED_BIT, &mem);
    if (img == VK_NULL_HANDLE) return 0;
    slot->img = img; slot->mem = mem;
    slot->w = w; slot->h = h; slot->layers = (uint32_t)layers; slot->fmt = fmt;
    VKI("spacewarp %s image stage %d = VkImage %p (%ux%u, %d layer(s))\n",
        depth ? "depth" : "motion-vector", stage, (void *)img, w, h, layers);
    return (uint64_t)(uintptr_t)img;
}

void *kl_vulkan_spacewarp_mtl(int depth, int stage, int *w, int *h, int *layers) {
    if (w) *w = 0; if (h) *h = 0; if (layers) *layers = 0;
    if (!kl_vulkan_guest_active() || stage < 0 || stage >= KLVK_EYE_STAGES) return NULL;
    klvk_img_slot *slot = &g_sw[depth ? 1 : 0][stage];
    if (!slot->img) return NULL;
    void *t = eye_mtl_texture(g_devs[0], slot->img);
    if (t) { if (w) *w = (int)slot->w; if (h) *h = (int)slot->h; if (layers) *layers = (int)slot->layers; }
    return t;
}

unsigned long long kl_vulkan_eye_image_layers(int stage, int eye, unsigned w, unsigned h,
                                              int srgb, int layers) {
    return kl_vulkan_layer_image(KLVK_EYE_LAYER, stage, eye, w, h, srgb, layers);
}

void *kl_vulkan_layer_mtl_texture(int layer_key, int stage, int eye,
                                  int *w, int *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (!kl_vulkan_guest_active()) return NULL;
    if (stage < 0 || stage >= KLVK_EYE_STAGES || eye < 0 || eye > 1) return NULL;
    // Never the eye table: those go to a compositor through kl_glfb's eye table,
    // which is where every consumer already looks. This is only the OTHER
    // layers, which have no such table because nothing sampled them before.
    if (layer_key == KLVK_EYE_LAYER) return NULL;
    klvk_img_slot *slots = NULL;
    for (int i = 0; i < KLVK_MAX_LAYERS; i++)
        if (g_layer_img[i].key == layer_key) { slots = &g_layer_img[i].s[0][0]; break; }
    if (!slots) return NULL;
    klvk_img_slot *slot = &slots[stage * 2 + eye];
    // A layer allocated for eye 0 only (the common case: one texture serving
    // both eyes, with the per-eye ViewportRect selecting the part) answers eye 1
    // from slot 0 rather than with nothing.
    if (!slot->img && eye == 1) slot = &slots[stage * 2];
    if (!slot->img) return NULL;
    if (w) *w = (int)slot->w;
    if (h) *h = (int)slot->h;
    return eye_mtl_texture(g_devs[0], slot->img);
}

// Diagnostic/bridge: the MTLTexture behind the wild-handle scratch — the single
// image AC Nexus actually composites its frame into (its real render target
// allocation fails, so it falls back to a wild pointer we substitute with this
// scratch, and the copies/blits/draws all land here). The compositor can draw
// this to SEE what the guest produced, and ultimately to present it.
static void *klvk_overlay_mtl_texture(VkImage img);
void *kl_vulkan_wild_scratch_mtl(int *w, int *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (!kl_vulkan_guest_active()) return NULL;
    // KL_RT_DEBUG=1: show the guest's latest large UI render target instead of the
    // (empty) composite scratch — the menu pieces the guest actually drew.
    if (kl_env_on("KL_RT_DEBUG", 0) && g_content_rt && klvk_img_reg_has(g_content_rt)
        && !klvk_is_eye_image(g_content_rt)) {
        void *t = klvk_overlay_mtl_texture(g_content_rt);
        if (t) { if (w) *w = (int)g_content_rt_w; if (h) *h = (int)g_content_rt_h; return t; }
    }
    if (!g_wild_scratch) return NULL;
    if (w) *w = 4096;
    if (h) *h = 4096;
    return eye_mtl_texture(g_devs[0], g_wild_scratch);
}

// Resolve the MTLTexture behind a live guest VkImage using ONLY vkGetMTLTextureMVK.
// The eye path (eye_mtl_texture) may use vkExportMetalObjectsEXT first, but that
// faults on any image the guest never marked exportable - which is every one of
// these overlay textures - so it is deliberately not used here (that was the first
// texL-resolver crash).
static void *klvk_overlay_mtl_texture(VkImage img) {
    typedef void (*pfn_get_mtl_texture_mvk)(VkImage, void **);
    static pfn_get_mtl_texture_mvk get_mvk;
    static int resolved;
    if (!resolved) { resolved = 1; get_mvk = (pfn_get_mtl_texture_mvk)mvk_sym("vkGetMTLTextureMVK"); }
    if (!get_mvk || !img) return NULL;
    void *t = NULL;
    get_mvk(img, &t);
    return t;
}

// ---------------------------------------------------------------------------
// Overlay SHADOW copies — the fix for "the panel textures are stale by the time
// the compositor samples them".
//
// The guest renders every menu panel and submits on ONE thread (the [thr]
// census: all createRT and 37/38 QueueSubmits share a tid), while the
// compositor samples those same images from ITS thread with no ordering — so a
// panel that had content right after its render reads black by sample time
// (recycled, re-rendered, or mid-write). KL_SYNC_GUEST would serialize the two
// but stalls presentation and the visionOS watchdog SIGKILLs the app.
//
// So synchronize the CONTENT instead of the threads: for each live image the
// overlay resolver samples, keep a same-size shadow image, and after every
// guest vkQueueSubmit append one command buffer (same queue, so Metal's hazard
// tracking orders it after the guest's renders) that copies live -> shadow.
// The compositor then samples the shadow, which always holds the last COMPLETE
// frame and is never written by anyone else. KL_OVERLAY_SHADOW=0 disables.
//
// Image dimensions/format come from a side table filled by klvk_CreateImage —
// the registry alone holds bare handles.
#define KLVK_IMG_INFO_MAX 4096
static struct { VkImage img; VkFormat fmt; uint32_t w, h; } g_img_info[KLVK_IMG_INFO_MAX];
static pthread_mutex_t g_img_info_lock = PTHREAD_MUTEX_INITIALIZER;
static void klvk_img_info_add(VkImage img, VkFormat fmt, uint32_t w, uint32_t h) {
    pthread_mutex_lock(&g_img_info_lock);
    for (int i = 0; i < KLVK_IMG_INFO_MAX; i++)
        if (!g_img_info[i].img || g_img_info[i].img == img) {
            g_img_info[i].img = img; g_img_info[i].fmt = fmt;
            g_img_info[i].w = w; g_img_info[i].h = h; break;
        }
    pthread_mutex_unlock(&g_img_info_lock);
}
static int klvk_img_info_get(VkImage img, VkFormat *fmt, uint32_t *w, uint32_t *h) {
    int ok = 0;
    pthread_mutex_lock(&g_img_info_lock);
    for (int i = 0; i < KLVK_IMG_INFO_MAX; i++)
        if (g_img_info[i].img == img) {
            if (fmt) *fmt = g_img_info[i].fmt;
            if (w) *w = g_img_info[i].w;
            if (h) *h = g_img_info[i].h;
            ok = 1; break;
        }
    pthread_mutex_unlock(&g_img_info_lock);
    return ok;
}
static void klvk_img_info_del(VkImage img) {
    pthread_mutex_lock(&g_img_info_lock);
    for (int i = 0; i < KLVK_IMG_INFO_MAX; i++)
        if (g_img_info[i].img == img) { g_img_info[i].img = VK_NULL_HANDLE; break; }
    pthread_mutex_unlock(&g_img_info_lock);
}

#define KLVK_SHADOW_MAX 64
static struct {
    VkImage  live;      // the guest's image (may die; checked each submit)
    VkImage  shadow;    // ours, same size+format, persistent
    void    *mtl;       // shadow's MTLTexture, cached
    uint32_t w, h;
    VkFormat fmt;
    int      copied;    // a copy has landed at least once -> safe to sample
    int      fromcopy;  // owned by capture-on-copy: the pump must not clobber it
} g_shadow[KLVK_SHADOW_MAX];
static int g_shadow_n;
static pthread_mutex_t g_shadow_lock = PTHREAD_MUTEX_INITIALIZER;

// Called from the overlay resolver (compositor thread): make sure `img` has a
// shadow slot. Creation of the Vulkan objects happens on the SUBMIT thread.
static void klvk_shadow_want(VkImage img) {
    if (!img) return;
    pthread_mutex_lock(&g_shadow_lock);
    for (int i = 0; i < g_shadow_n; i++)
        if (g_shadow[i].live == img) { pthread_mutex_unlock(&g_shadow_lock); return; }
    if (g_shadow_n < KLVK_SHADOW_MAX) {
        g_shadow[g_shadow_n].live = img;
        g_shadow[g_shadow_n].shadow = VK_NULL_HANDLE;
        g_shadow[g_shadow_n].mtl = NULL;
        g_shadow[g_shadow_n].copied = 0;
        g_shadow_n++;
    }
    pthread_mutex_unlock(&g_shadow_lock);
}

// The resolver's read side: the shadow's MTLTexture once a copy has landed.
static void *klvk_shadow_mtl(VkImage live) {
    void *t = NULL;
    pthread_mutex_lock(&g_shadow_lock);
    for (int i = 0; i < g_shadow_n; i++)
        if (g_shadow[i].live == live && g_shadow[i].copied && g_shadow[i].shadow) {
            if (!g_shadow[i].mtl)
                g_shadow[i].mtl = klvk_overlay_mtl_texture(g_shadow[i].shadow);
            t = g_shadow[i].mtl;
            break;
        }
    pthread_mutex_unlock(&g_shadow_lock);
    return t;
}

// A live image died: drop its slot so the submit path never copies from a
// stale handle (the crash the texL resolver already learned to avoid).
static void klvk_shadow_forget(VkImage live) {
    pthread_mutex_lock(&g_shadow_lock);
    for (int i = 0; i < g_shadow_n; i++)
        if (g_shadow[i].live == live) { g_shadow[i].live = VK_NULL_HANDLE; break; }
    pthread_mutex_unlock(&g_shadow_lock);
}

// Find-or-create a shadow for `live` ON THE GUEST THREAD (safe: this thread owns
// the queue). Returns the shadow image, or NULL when dims are unknown or the
// table is full. Used by the capture-on-copy path in klvk_CmdCopyImage.
static VkImage klvk_shadow_for_copy(VkImage live) {
    if (!live) return VK_NULL_HANDLE;
    klvk_device *d = g_devs[0];
    if (!d) return VK_NULL_HANDLE;
    VkImage sh = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_shadow_lock);
    int slot = -1;
    for (int i = 0; i < g_shadow_n; i++)
        if (g_shadow[i].live == live) { slot = i; break; }
    if (slot < 0 && g_shadow_n < KLVK_SHADOW_MAX) {
        slot = g_shadow_n++;
        g_shadow[slot].live = live; g_shadow[slot].shadow = VK_NULL_HANDLE;
        g_shadow[slot].mtl = NULL; g_shadow[slot].copied = 0;
    }
    if (slot >= 0) {
        if (!g_shadow[slot].shadow) {
            VkFormat fmt; uint32_t w, h;
            if (klvk_img_info_get(live, &fmt, &w, &h) && w && h) {
                g_shadow[slot].w = w; g_shadow[slot].h = h; g_shadow[slot].fmt = fmt;
                g_shadow[slot].shadow = klvk_image_alloc(d, fmt, w, h, 1, 1,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, NULL);
            }
        }
        sh = g_shadow[slot].shadow;
        if (sh) { g_shadow[slot].copied = 1; g_shadow[slot].fromcopy = 1; }
    }
    pthread_mutex_unlock(&g_shadow_lock);
    return sh;
}

// The submit-side pump, called right after the guest's own vkQueueSubmit on the
// guest's thread. Creates shadow images on demand and records+submits ONE
// command buffer copying every wanted live image into its shadow. Layouts are
// GENERAL both sides: MoltenVK does not act on Vulkan layouts (Metal has none),
// and Metal's hazard tracking orders these copies after the guest's renders on
// the same queue. A private pool+cmdbuf+fence, reused; if the previous copy has
// not retired yet this round is skipped rather than blocked on.
static void klvk_shadow_pump(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_OVERLAY_SHADOW", 1);
    if (!on) return;
    klvk_device *d = g_devs[0];
    if (!d || !g_shadow_n) return;

    static VkCommandPool   pool;
    static VkCommandBuffer cmd;
    static VkFence         fence;
    static int             pending;
    if (!pool) {
        VkCommandPoolCreateInfo pi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = d->queue_family,
        };
        if (d->CreateCommandPool(d->dev, &pi, NULL, &pool) != VK_SUCCESS) return;
        VkCommandBufferAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (d->AllocateCommandBuffers(d->dev, &ai, &cmd) != VK_SUCCESS) return;
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (d->CreateFence(d->dev, &fi, NULL, &fence) != VK_SUCCESS) return;
    }
    if (pending) {
        if (d->WaitForFences(d->dev, 1, &fence, VK_TRUE, 0) != VK_SUCCESS)
            return;                       // previous copy still in flight: skip
        d->ResetFences(d->dev, 1, &fence);
        pending = 0;
    }

    static PFN_vkCmdCopyImage real_copy;
    if (!real_copy) real_copy = (PFN_vkCmdCopyImage)mvk_sym("vkCmdCopyImage");
    if (!real_copy) return;

    // Snapshot the work under the lock; do Vulkan work outside it.
    struct { VkImage live, shadow; uint32_t w, h; int idx; } work[KLVK_SHADOW_MAX];
    int nwork = 0;
    pthread_mutex_lock(&g_shadow_lock);
    for (int i = 0; i < g_shadow_n; i++) {
        VkImage live = g_shadow[i].live;
        if (!live || !klvk_img_reg_has(live)) continue;
        // Capture-on-copy owns this shadow: it updates at the guest's own copy,
        // when content is provably present. A pump copy AFTER submit could land
        // on a recycled (black) live image and clobber the good capture.
        if (g_shadow[i].fromcopy) continue;
        if (!g_shadow[i].shadow) {
            VkFormat fmt; uint32_t w, h;
            if (!klvk_img_info_get(live, &fmt, &w, &h) || !w || !h) continue;
            g_shadow[i].w = w; g_shadow[i].h = h; g_shadow[i].fmt = fmt;
            g_shadow[i].shadow = klvk_image_alloc(d, fmt, w, h, 1, 1,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, NULL);
            if (!g_shadow[i].shadow) { g_shadow[i].live = VK_NULL_HANDLE; continue; }
            static int said; if (said < 12) { said++;
                VKI("shadow: %ux%u fmt %d for live %#llx -> %#llx\n", w, h, (int)fmt,
                    (unsigned long long)(uintptr_t)live,
                    (unsigned long long)(uintptr_t)g_shadow[i].shadow); }
        }
        work[nwork].live = live; work[nwork].shadow = g_shadow[i].shadow;
        work[nwork].w = g_shadow[i].w; work[nwork].h = g_shadow[i].h;
        work[nwork].idx = i; nwork++;
    }
    pthread_mutex_unlock(&g_shadow_lock);
    if (!nwork) return;

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (d->BeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return;
    for (int i = 0; i < nwork; i++) {
        VkImageCopy rc = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .extent = { work[i].w, work[i].h, 1 },
        };
        real_copy(cmd, work[i].live, VK_IMAGE_LAYOUT_GENERAL,
                  work[i].shadow, VK_IMAGE_LAYOUT_GENERAL, 1, &rc);
    }
    if (d->EndCommandBuffer(cmd) != VK_SUCCESS) return;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd,
    };
    if (d->QueueSubmit(d->queue, 1, &si, fence) == VK_SUCCESS) {
        pending = 1;
        pthread_mutex_lock(&g_shadow_lock);
        for (int i = 0; i < nwork; i++)
            if (g_shadow[work[i].idx].live == work[i].live)
                g_shadow[work[i].idx].copied = 1;
        pthread_mutex_unlock(&g_shadow_lock);
    }
}

// Overlay texL decoder + resolver. The compositor hands us the guest's per-layer
// texL pointer (stored from EnqueueSubmitLayer2). The tracer established the layout:
// the pointer's FIRST word is the guest VkImage this layer's UI was rendered into
// (it matches the per-frame copy sources every time; [+8]/[+16] carry its format).
// Those images are real, MoltenVK-backed, and tracked here - the menu pixels are
// literally in them - so we hand the compositor that image's MTLTexture instead of
// the empty layer swapchain image it was sampling. KL_OVERLAY_TEXL=0 restores the
// old NULL-return (falls back to the layer image) if this ever misbehaves.
void *kl_vulkan_mtl_for_handle(unsigned long long h) {
    if (!h) return NULL;
    // A handle is only a POINTER to a texture struct on the Vulkan path. On a
    // GLES guest the same compositor ladder hands this the layer's GL texture
    // NAME — a small integer — and dereferencing that read unmapped low memory
    // (ieytd: SIGSEGV at 0x74, i.e. GL tex 116 used as an address). No Vulkan
    // guest, or a value no mapped pointer can have, answers NULL and lets the
    // ladder fall through to kl_glfb_layer_mtl_texture, which owns GL names.
    if (!kl_vulkan_guest_active() || h < 0x10000ULL) return NULL;
    const unsigned long long *w = (const unsigned long long *)(uintptr_t)h;
    VkImage img = (VkImage)(uintptr_t)w[0];

    // Diagnostic struct dump, deduped to once per distinct handle (KL_TRACE only).
    if (klvk_trace()) {
        static unsigned long long seen[128]; static int seen_n;
        static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
        int fresh = 1;
        pthread_mutex_lock(&lk);
        for (int i = 0; i < seen_n; i++) if (seen[i] == h) { fresh = 0; break; }
        if (fresh && seen_n < 128) seen[seen_n++] = h;
        pthread_mutex_unlock(&lk);
        if (fresh) {
            klvk_trace_log("[texL] handle %#llx struct dump:\n", h);
            for (int i = 0; i < 12; i++) {
                unsigned long long v = w[i];
                const char *tag = klvk_img_tag((VkImage)(uintptr_t)v);
                int key = klvk_layer_image_key((VkImage)(uintptr_t)v);
                klvk_trace_log("[texL]   [+%2d] %#018llx  %s%s k=%d\n",
                    i * 8, v, tag, (v && strcmp(tag,"nil") && strcmp(tag,"WILD")) ? " <-- IMAGE" : "", key);
            }
        }
    }

    // Resolve on EVERY call (not just the first), so every frame samples fresh UI.
    //
    // Default OFF for the store build of AC Nexus. This detour exists because
    // the layer's own swapchain image used to be empty there — and it was empty
    // because ovrp_GetLayerTexturePtr handed Unity the wrong thing (see
    // klovrp_GetLayerTexturePtr). With that answered properly Unity copies each
    // panel into its layer image, which is the picture OVRPlugin means to be
    // shown; a capture of the layer image holds the calibration panel intact
    // while this path put fragments of unrelated textures on screen in the same
    // frame (the submitted handle is a pooled render texture that Unity has
    // already handed to something else).
    static int texl = -1;
    if (texl < 0) {
        const char *t = kl_driver_target_name();
        // Ghost Town (x9531494193591052) takes the indirect texture pointer
        // (klovrp_texptr_indirect), so the picture is in the layer image.
        // Sampling texL there put AC Nexus's panels on a pooled texture Unity
        // had already reused.
        texl = kl_env_on("KL_OVERLAY_TEXL",
                         !(t && (!strcmp(t, "x5812519008825194") ||
                                 !strcmp(t, "x9531494193591052"))));
    }
    if (!texl) return NULL;
    // Only a live, tracked, non-eye image. A wild or already-destroyed handle here
    // is exactly the stale-MVKImage crash that destroy-tracking now prevents:
    // klvk_img_reg_has() is true only for an image still alive in our registry.
    int live = img && klvk_img_reg_has(img);
    int eye  = img && klvk_is_eye_image(img);
    // Register for shadow copies (klvk_shadow_pump) and sample the SHADOW once
    // one has landed: it holds the last complete frame, where the live image is
    // racing the guest's own re-render/recycle on another thread.
    void *mtl = NULL;
    if (live && !eye) {
        klvk_shadow_want(img);
        mtl = klvk_shadow_mtl(img);
        if (!mtl) mtl = klvk_overlay_mtl_texture(img);
    }
    // Bounded outcome log so we can tell a real HIT from a silent fall-through to
    // the empty layer image, and cross-reference the sampled image vs the render
    // targets. KL_TEXL_DIAG=1 (or KL_TRACE) turns it on; first 60 then every 600th.
    if (kl_env_on("KL_TEXL_DIAG", 0) || klvk_trace()) {
        static unsigned c;
        if (c < 60 || (c % 600) == 0)
            fprintf(stderr, "[TEXL] h=%#llx img=%#llx live=%d eye=%d -> mtl=%p %s\n",
                    h, (unsigned long long)(uintptr_t)img, live, eye, mtl,
                    mtl ? "HIT" : "miss(fallback to layer image)");
        c++;
    }
    return mtl;
}

unsigned long long kl_vulkan_eye_image(int stage, int eye, unsigned w, unsigned h,
                                       int srgb) {
    return kl_vulkan_eye_image_layers(stage, eye, w, h, srgb, 1);
}

// ---------------------------------------------------------------------------
// The OpenXR seam — XR_KHR_vulkan_enable. See kl_vulkan.h for why it exists.
// ---------------------------------------------------------------------------

// Whether the extension may be advertised. Deliberately NOT kl_vulkan_guest_active():
// that asks whether a device exists, and at xrCreateInstance time the guest has
// not created one yet — it cannot, because the whole point of the extension is
// that it asks US which physical device to create it on. So the question here
// is only whether MoltenVK is reachable at all.
int kl_vulkan_xr_supported(void) { return kl_vulkan_available(); }

// Empty, and true rather than lazy — klvk_CreateDevice already adds
// VK_EXT_metal_objects to every device the guest creates, so there is nothing
// left to require of the app. If something is ever genuinely required here,
// note that the spec wants a SPACE-delimited list in one buffer, not an array.
const char *kl_vulkan_xr_instance_extensions(void) { return ""; }
const char *kl_vulkan_xr_device_extensions(void)   { return ""; }

// The physical device the app must render with.
//
// The VkInstance is the APP's — it created one before asking, which is the
// ordering XR_KHR_vulkan_enable prescribes — so this enumerates on the handle it
// passes rather than on g_instance. Those are usually the same object here (the
// app's vkCreateInstance came through klvk_CreateInstance), but "usually" is not
// a thing to encode: the spec names the instance as a parameter precisely
// because the runtime is not entitled to assume it owns one.
//
// The FIRST device is the answer because MoltenVK exposes exactly one per Metal
// device, and picking by index rather than by score is honest about that. A
// machine that ever reports two would want a rule here, and would say so by
// this log line naming a count above 1.
void *kl_vulkan_xr_physical_device(void *vk_instance) {
    if (!kl_vulkan_available() || !vk_instance) return NULL;
    PFN_vkEnumeratePhysicalDevices enum_pd = (PFN_vkEnumeratePhysicalDevices)
        real_gipa((VkInstance)vk_instance, "vkEnumeratePhysicalDevices");
    if (!enum_pd) { VKI("xr: no vkEnumeratePhysicalDevices on the app's instance\n"); return NULL; }
    uint32_t n = 0;
    if (enum_pd((VkInstance)vk_instance, &n, NULL) != VK_SUCCESS || n == 0) {
        // The trap BONELAB paid for, in the one place it can recur: without
        // VK_KHR_portability_enumeration this answers zero devices AND
        // VK_SUCCESS, so a bare count of 0 is not necessarily an empty machine.
        VKI("xr: vkEnumeratePhysicalDevices reports 0 devices — if the app's "
            "instance omitted VK_KHR_portability_enumeration, that is why\n");
        return NULL;
    }
    VkPhysicalDevice pd[8];
    if (n > 8) n = 8;
    if (enum_pd((VkInstance)vk_instance, &n, pd) != VK_SUCCESS || n == 0) return NULL;
    if (n > 1) VKI("xr: %u physical devices; taking the first\n", n);
    return (void *)pd[0];
}

// One OpenXR swapchain image.
//
// Deliberately NOT keyed on (stage, eye) the way the OVRPlugin eye images are:
// in OpenXR the runtime allocates a swapchain's images at xrCreateSwapchain,
// which knows a size and a format and nothing else — WHICH swapchain is an eye
// is only asserted later, at xrEndFrame, from the projection layer. That is a
// property of the API, not of a guest.
//
// So this hands back a bare image and keeps no table of its own; the eye
// association, and with it the MTLTexture publication, belongs at the assertion.
unsigned long long kl_vulkan_xr_image(unsigned w, unsigned h, unsigned layers,
                                      unsigned mips, long long vk_format, int depth) {
    if (!kl_vulkan_guest_active()) return 0;
    klvk_device *d = g_devs[0];
    if (!d || !w || !h) return 0;
    // A depth image is not a colour image with a different format: it needs the
    // depth/stencil attachment usage, and asking for COLOR_ATTACHMENT on a depth
    // format is a validation error rather than a harmless extra bit.
    VkImageUsageFlags usage = depth
        ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
        : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImage img = klvk_image_alloc(d, (VkFormat)vk_format, w, h,
                                   layers ? layers : 1, mips ? mips : 1, usage, &mem);
    if (img == VK_NULL_HANDLE) return 0;
    if (!depth) fove_xr_image(img, layers ? layers : 1, w, h);   // a runtime eye image (foveation)
    return (uint64_t)(uintptr_t)img;
}

// The MTLTexture behind an image this file created, for the compositor seam.
// NULL when the export is unavailable — the caller reports it, because only the
// caller knows which eye it was about.
void *kl_vulkan_xr_image_mtl(unsigned long long image) {
    if (!kl_vulkan_guest_active() || !image) return NULL;
    return eye_mtl_texture(g_devs[0], (VkImage)(uintptr_t)image);
}

// The Vulkan version range a session may be created against.
//
// Measured off the physical device, not asserted: MoltenVK's instance-level and
// device-level versions differ, and the one that governs the app's device is the
// device's. The floor is 1.0 because nothing here needs more, and overstating a
// floor is the dangerous direction — the same reasoning, and the same trap, as
// the GLES range next door (an app told it needs 1.2 asks for 1.2).
void kl_vulkan_xr_api_range(unsigned *min_major, unsigned *min_minor,
                            unsigned *max_major, unsigned *max_minor) {
    if (min_major) *min_major = 1;
    if (min_minor) *min_minor = 0;
    // A conservative ceiling until a device answers, so a failure to measure
    // cannot present as "this runtime supports nothing".
    if (max_major) *max_major = 1;
    if (max_minor) *max_minor = 0;
    if (!kl_vulkan_available()) return;

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    if (g_ndev > 0 && g_devs[0]) phys = g_devs[0]->phys;
    if (phys == VK_NULL_HANDLE && g_instance) {
        PFN_vkEnumeratePhysicalDevices enum_pd = (PFN_vkEnumeratePhysicalDevices)
            real_gipa(g_instance, "vkEnumeratePhysicalDevices");
        uint32_t n = 1;
        VkPhysicalDevice one = VK_NULL_HANDLE;
        if (enum_pd && enum_pd(g_instance, &n, &one) >= 0 && n) phys = one;
    }
    if (phys == VK_NULL_HANDLE) return;

    PFN_vkGetPhysicalDeviceProperties props_fn = (PFN_vkGetPhysicalDeviceProperties)
        real_gipa(g_instance, "vkGetPhysicalDeviceProperties");
    if (!props_fn) return;
    VkPhysicalDeviceProperties p;
    memset(&p, 0, sizeof p);
    props_fn(phys, &p);
    if (max_major) *max_major = VK_VERSION_MAJOR(p.apiVersion);
    if (max_minor) *max_minor = VK_VERSION_MINOR(p.apiVersion);
}

// Diagnostic: dump each live non-eye layer image's lit-pixel count, so we can
// tell whether the guest actually RENDERED its menu UI into the overlay
// textures (lit > 0) or is submitting empty panels (0 lit). Gated on KL_VK_OUT
// like the eye capture; the PNGs go to the device but the '(N lit / M)' log
// line is the signal.
void kl_vulkan_capture_layers(void) {
    const char *dir = cap_dir();
    if (!dir || !kl_vulkan_guest_active()) return;
    klvk_device *d = g_devs[0];
    if (!d) return;
    for (int i = 0; i < KLVK_MAX_LAYERS; i++) {
        if (!g_layer_img[i].key) continue;
        klvk_img_slot *sl = &g_layer_img[i].s[0][0];   // stage 0, eye 0
        if (!sl->img) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/vk_layer%02d.png", dir, g_layer_img[i].key);
        cap_image_layer(d, d->queue, sl->img, sl->w, sl->h, sl->fmt,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, path, NULL, 0);
    }
    VKI("wild-handle reroute census: %u copies + %u blits redirected, scratch=%#llx\n",
        g_wild_copy_redir, g_wild_blit_redir,
        (unsigned long long)(uintptr_t)g_wild_scratch);
    if (g_wild_scratch) {
        char path[1024];
        snprintf(path, sizeof path, "%s/vk_scratch.png", dir);
        cap_image_layer(d, d->queue, g_wild_scratch, 4096, 4096,
                        VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, path, NULL, 0);
    }
}

// One stage per call — the one the association NAMED — unless KL_VK_OUT_ALL=1
// captures every stage that has an image. The distinction is the whole tool:
// on the Vulkan path the stage association runs on the frame-counter fallback
// ("eye stage NOT observed" — both wrath2 and AC Nexus), so the named stage is
// a GUESS, and a capture of only the guess cannot show a wrong one. With ALL,
// one frame yields every stage and eye side by side, and the named stage
// carries a "_named" suffix — so a stale or misassociated stage is visible as
// "the fresh picture is in s1 but s0 is _named".
void kl_vulkan_xr_image_capture(unsigned long long image, unsigned w, unsigned h,
                                long long vk_format, unsigned array_layer,
                                unsigned frame, int eye, int layer) {
    const char *dir = cap_dir();
    if (!dir || !image || !kl_vulkan_guest_active()) return;
    if (eye == 0 && layer == 0 && kl_env_on("KL_VK_FRAME_EVENTS", 0)) {
        unsigned v[KFE_N];
        for (int i = 0; i < KFE_N; i++) v[i] = __atomic_exchange_n(&g_fe[i], 0, __ATOMIC_ACQ_REL);
        fprintf(stderr, "  [fe] %u: passes %u draws %u tmpl %u submits %u | images +%u -%u, memory +%u -%u, "
                        "buffers +%u, uploads %u, pipelines %u\n", frame, v[KFE_PASS], v[KFE_DRAW], v[KFE_TMPL],
                v[KFE_SUBMIT], v[KFE_IMG_NEW], v[KFE_IMG_DEL], v[KFE_MEM_NEW], v[KFE_MEM_DEL], v[KFE_BUF_NEW],
                v[KFE_COPY_B2I], v[KFE_PIPE]);
    }
    int every = kl_env_int("KL_VK_OUT_EVERY", 1);
    if (every < 1) every = 1;
    // KL_VK_OUT_RUN=n: n frames in a row each time, for a pair the motion between them is measured on.
    int run = kl_env_int("KL_VK_OUT_RUN", 1);
    if (frame % (unsigned)every >= (unsigned)(run < 1 ? 1 : run)) return;
    klvk_device *d = g_devs[0];
    char path[1024];
    snprintf(path, sizeof path, "%s/xr_f%05u_layer%d_eye%d.png", dir, frame, layer, eye);
    // An OpenXR swapchain image is released in COLOR_ATTACHMENT_OPTIMAL.
    if (cap_image_layer(d, d->queue, (VkImage)(uintptr_t)image, w, h, (VkFormat)vk_format,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, array_layer, path, NULL, 0))
        g_presented++;
}

// See kl_vulkan.h. cap_image_layer's copy without its RGBA8 assumptions: the
// texels go to the file as they are, `bpp` bytes each, depth through the depth
// aspect. For the space-warp images (RGBA16F motion vectors, D32 depth), which
// a PNG cannot hold.
void kl_vulkan_xr_image_capture_raw(unsigned long long image, unsigned w, unsigned h,
                                    unsigned bpp, int depth, unsigned layer, const char *path) {
    if (!image || !path || !kl_vulkan_guest_active() || !w || !h || !bpp) return;
    klvk_device *d = g_devs[0];
    VkDeviceSize need = (VkDeviceSize)w * h * bpp;
    if (!d || !cap_prepare(d, need)) { VKI("raw capture setup failed\n"); return; }
    VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout cur = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                              : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    d->ResetCommandBuffer(d->cap_cmd, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    d->BeginCommandBuffer(d->cap_cmd, &bi);
    VkImageMemoryBarrier to_src = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = cur,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = (VkImage)(uintptr_t)image,
        .subresourceRange = { depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                    : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1 },
    };
    d->CmdPipelineBarrier(d->cap_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);
    VkBufferImageCopy region = {
        .imageSubresource = { aspect, 0, layer, 1 },
        .imageExtent = { w, h, 1 },
    };
    d->CmdCopyImageToBuffer(d->cap_cmd, (VkImage)(uintptr_t)image,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->cap_buf, 1, &region);
    VkImageMemoryBarrier back = to_src;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = cur;
    d->CmdPipelineBarrier(d->cap_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &back);
    d->EndCommandBuffer(d->cap_cmd);
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &d->cap_cmd,
    };
    d->ResetFences(d->dev, 1, &d->cap_fence);
    if (d->QueueSubmit(d->queue, 1, &si, d->cap_fence) != VK_SUCCESS) return;
    d->WaitForFences(d->dev, 1, &d->cap_fence, VK_TRUE, 2000000000ull);
    void *mapped = NULL;
    if (d->MapMemory(d->dev, d->cap_mem, 0, need, 0, &mapped) != VK_SUCCESS) return;
    VkMappedMemoryRange mmr = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = d->cap_mem, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    if (d->InvalidateMappedMemoryRanges) d->InvalidateMappedMemoryRanges(d->dev, 1, &mmr);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(mapped, 1, (size_t)need, f);
        fclose(f);
        VKI("-> %s (%ux%u, %u bytes a texel)\n", path, w, h, bpp);
    }
    d->UnmapMemory(d->dev, d->cap_mem);
}

void kl_vulkan_capture_eyes(unsigned frame, int stage) {
    const char *dir = cap_dir();
    // Without a capture directory — every device run — three frames are still
    // read back, for their brightness alone. "It is black" on a headset has two
    // opposite causes, a guest that drew nothing and a picture that is there
    // but not where the eyes are looking, and nothing else in a device log can
    // tell them apart. Three reads of one image is the whole cost;
    // KL_VK_EYE_CHECK=0 removes it.
    if (!dir && kl_vulkan_guest_active() && stage >= 0 && stage < KLVK_EYE_STAGES &&
        (frame == 300 || frame == 1200 || frame == 2400)) {
        static int on = -1;
        if (on < 0) on = kl_env_on("KL_VK_EYE_CHECK", 1);
        klvk_device *d = g_devs[0];
        klvk_img_slot *sl = &g_eye[stage][0];
        if (on && d && sl->img) {
            VKI("eye picture check at frame %u, stage %d:\n", frame, stage);
            for (uint32_t eye = 0; eye < (sl->layers > 1 ? 2u : 1u); eye++)
                cap_image_layer(d, d->queue, sl->img, sl->w, sl->h, sl->fmt,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, eye,
                                NULL, NULL, 0);
        }
    }
    if (!dir || !kl_vulkan_guest_active()) return;
    int every = kl_env_int("KL_VK_OUT_EVERY", 1);
    if (every < 1) every = 1;
    int run = kl_env_int("KL_VK_OUT_RUN", 1);
    if (frame % (unsigned)every >= (unsigned)(run < 1 ? 1 : run)) return;
    if (stage < 0 || stage >= KLVK_EYE_STAGES) return;
    // KL_XR_SPACEWARP_OUT: the motion vectors the guest drew for this stage (Application SpaceWarp),
    // raw, beside the eye pictures – the OVRPlugin counterpart of the OpenXR dump in kl_openxr.c.
    {
        static const char *swdir = (const char *)-1;
        if (swdir == (const char *)-1) swdir = kl_env_str("KL_XR_SPACEWARP_OUT", NULL);
        klvk_img_slot *mv = &g_sw[0][stage];
        if (swdir && mv->img)
            for (uint32_t eye = 0; eye < (mv->layers > 1 ? 2u : 1u); eye++) {
                char path[1024];
                snprintf(path, sizeof path, "%s/sw_f%05u_s%d_eye%u_mv_%ux%u.rgba16f", swdir, frame, stage,
                         eye, mv->w, mv->h);
                kl_vulkan_xr_image_capture_raw((unsigned long long)(uintptr_t)mv->img, mv->w, mv->h, 8, 0, eye, path);
            }
    }
    static int all = -1;
    if (all < 0) all = kl_env_on("KL_VK_OUT_ALL", 0);

    klvk_device *d = g_devs[0];
    for (int st = 0; st < KLVK_EYE_STAGES; st++) {
        if (!all && st != stage) continue;
        // Under the Array layout slot 0 holds the only image and the two eyes
        // are its layers, so the eye index selects a layer there and an image
        // here. The filename stays `eyeN` either way — it names what was
        // drawn, not where it was stored.
        int layered = g_eye[st][0].img && g_eye[st][0].layers > 1;
        for (int eye = 0; eye < 2; eye++) {
            int slot = layered ? 0 : eye;
            if (!g_eye[st][slot].img) continue;
            char path[1024];
            snprintf(path, sizeof path, "%s/vk_f%05u_s%d_eye%d%s.png", dir, frame,
                     st, eye, (all && st == stage) ? "_named" : "");
            // The guest submitted this to a compositor, so it left it readable
            // by one — SHADER_READ_ONLY_OPTIMAL. See cap_image on why a wrong
            // guess here is cheap on MoltenVK specifically.
            if (cap_image_layer(d, d->queue, g_eye[st][slot].img, g_eye[st][slot].w,
                                g_eye[st][slot].h, g_eye[st][slot].fmt,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                layered ? (uint32_t)eye : 0u, path, NULL, 0))
                g_presented++;
        }
    }
}

// ---------------------------------------------------------------------------
// The frame seam — "this eye texture is finished, a compositor may sample it"
// ---------------------------------------------------------------------------
//
// The GL path answers this with an MTLSharedEvent queued into ANGLE's own
// command stream (kl_glfb's klfb_gpu_frame_now): the compositor's command
// buffer waits on a value the guest's queue signals, and nothing ever stalls on
// the CPU. There is no equivalent here, and the reason is not an oversight —
// **the guest owns the VkQueue and we do not add work to its submissions.** A
// cross-queue GPU wait needs a shared event signalled *after* the guest's
// rendering, and the only ways to get one are to signal a timeline semaphore on
// the guest's queue (which needs `VK_KHR_timeline_semaphore` enabled on a device
// the GUEST creates) or to export the MTLCommandQueue and encode the signal
// ourselves in Objective-C, which this plain-C file cannot do.
//
// So this waits. `vkQueueWaitIdle` on the guest's own queue, at the moment the
// guest asserts the eye textures are drawn (ovrp_EndFrame4), and then a serial
// the compositor reads: past it, the picture is not merely submitted but
// COMPLETE, so no compositor-side ordering is needed at all.
//
// It is a real stall on the guest's frame thread and it is named as one. The
// capture path (KL_VK_OUT) already pays exactly this cost on every captured
// frame, and no device run has happened, so it buys correctness at a price
// nothing has yet measured. `KL_VK_FRAME_SYNC=0` removes the wait and keeps the
// serial — the A/B for "is the compositor showing a torn frame?", and the shape
// the timeline-semaphore version would have.
static uint64_t g_frame_serial;

unsigned long long kl_vulkan_frame_serial(void) {
    return __atomic_load_n(&g_frame_serial, __ATOMIC_ACQUIRE);
}

// ---------------------------------------------------------------------------
// EYE SHADOW (KL_EYE_SHADOW) — candidate fix for a black EYE, mirroring the
// overlay SHADOW above but for the big eye array image.
//
// The overlay resolver samples a SHADOW copy (klvk_shadow_*) rather than the
// live guest image, because a Vulkan image the guest renders on its own thread
// and the compositor samples from ITS thread reads black by sample time. The
// eye image is EXCLUDED from that path (see `live && !eye` in
// kl_vulkan_mtl_for_handle) and reaches the compositor as the LIVE MTLTexture
// through kl_glfb's eye table. Its only ordering is the vkQueueWaitIdle below.
//
// This is the same treatment for the eye: at frame end (the guest's own
// assertion that the stage is drawn), copy the live eye image into a persistent
// per-stage shadow ON THE GUEST'S OWN QUEUE — so Metal's hazard tracking orders
// the copy after the guest's render — and republish the SHADOW's MTLTexture
// into kl_glfb for that stage. The compositor then samples a stable image that
// holds the last complete frame and is written by nobody else.
//
// OFF by default so it is a clean A/B against the live-sample path. The copy is
// the same cost the KL_VK_OUT capture already pays, so it is measured.
static VkImage        g_eye_shadow[KLVK_EYE_STAGES];
static VkDeviceMemory g_eye_shadow_mem[KLVK_EYE_STAGES];
static void          *g_eye_shadow_mtl[KLVK_EYE_STAGES];

static void klvk_eye_shadow_copy(klvk_device *d, int stage) {
    if (!d || stage < 0 || stage >= KLVK_EYE_STAGES) return;
    static PFN_vkCmdCopyImage real_copy;
    if (!real_copy) real_copy = (PFN_vkCmdCopyImage)mvk_sym("vkCmdCopyImage");
    if (!real_copy) return;

    static VkCommandPool   pool;
    static VkCommandBuffer cmd;
    static VkFence         fence;
    if (!pool) {
        VkCommandPoolCreateInfo pi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = d->queue_family,
        };
        if (d->CreateCommandPool(d->dev, &pi, NULL, &pool) != VK_SUCCESS) return;
        VkCommandBufferAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (d->AllocateCommandBuffers(d->dev, &ai, &cmd) != VK_SUCCESS) return;
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (d->CreateFence(d->dev, &fi, NULL, &fence) != VK_SUCCESS) return;
    }

    // wanderer (and every Array-layout guest) keeps ONE image with N array
    // layers in slot 0, handed out for both eyes. That is the case this fix
    // targets, so it shadows slot 0's image with its full layer count. A Stereo
    // guest (one 1-layer image per eye) is deliberately left on the live path
    // for now — it does not exhibit the black this A/B is chasing.
    klvk_img_slot *slot = &g_eye[stage][0];
    if (!slot->img || !slot->w || !slot->h) return;
    uint32_t layers = slot->layers ? slot->layers : 1;

    if (!g_eye_shadow[stage]) {
        g_eye_shadow[stage] = klvk_image_alloc(d, slot->fmt, slot->w, slot->h,
            layers, 1, VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT, &g_eye_shadow_mem[stage]);
        if (!g_eye_shadow[stage]) return;
        VKI("eye shadow: stage %d %ux%u fmt %d %u layer(s) live %#llx -> %#llx "
            "(KL_EYE_SHADOW)\n", stage, slot->w, slot->h, (int)slot->fmt, layers,
            (unsigned long long)(uintptr_t)slot->img,
            (unsigned long long)(uintptr_t)g_eye_shadow[stage]);
    }

    d->ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (d->BeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return;
    VkImageCopy rc = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers },
        .extent = { slot->w, slot->h, 1 },
    };
    real_copy(cmd, slot->img, VK_IMAGE_LAYOUT_GENERAL,
              g_eye_shadow[stage], VK_IMAGE_LAYOUT_GENERAL, 1, &rc);
    if (d->EndCommandBuffer(cmd) != VK_SUCCESS) return;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd,
    };
    d->ResetFences(d->dev, 1, &fence);
    if (d->QueueSubmit(d->queue, 1, &si, fence) != VK_SUCCESS) return;
    d->WaitForFences(d->dev, 1, &fence, VK_TRUE, 2000000000ull);

    if (!g_eye_shadow_mtl[stage])
        g_eye_shadow_mtl[stage] = eye_mtl_texture(d, g_eye_shadow[stage]);
    void *tex = g_eye_shadow_mtl[stage];
    if (!tex) return;
    int w = (int)slot->w, h = (int)slot->h;
    if (layers > 1) {
        kl_glfb_note_eye_mtl_texture(0, stage, tex, 0, w, h);
        kl_glfb_note_eye_mtl_texture(1, stage, tex, 1, w, h);
    } else {
        kl_glfb_note_eye_mtl_texture(0, stage, tex, 0, w, h);
    }
}

// KL_VK_PROFILE, defined with the render-pass hooks further down.
#define KLVK_PROF_IMG 4096
static struct { VkImage img; uint16_t samples, layers; } g_prof_img[KLVK_PROF_IMG];
static unsigned g_prof_nimg;
static void klvk_prof_img_note(VkImage img, unsigned samples, unsigned layers) {
    unsigned i = __atomic_fetch_add(&g_prof_nimg, 1, __ATOMIC_RELAXED) % KLVK_PROF_IMG;
    g_prof_img[i].img = img;
    g_prof_img[i].samples = (uint16_t)samples;
    g_prof_img[i].layers = (uint16_t)layers;
}
static unsigned klvk_prof_img_samples(VkImage img) {
    for (unsigned i = 0; i < KLVK_PROF_IMG; i++)
        if (g_prof_img[i].img == img) return g_prof_img[i].samples;
    return 0;
}
// The guest's multisampled images and their sizes — a handful in any frame, so
// a short table. What klvk_widen_area() needs to know, always, not only when
// profiling.
static struct { VkImage img; uint32_t w, h; } g_msaa_img[32];
static void klvk_msaa_img_add(VkImage img, uint32_t w, uint32_t h) {
    for (unsigned i = 0; i < sizeof g_msaa_img / sizeof *g_msaa_img; i++) {
        VkImage none = VK_NULL_HANDLE;
        if (__atomic_compare_exchange_n(&g_msaa_img[i].img, &none, img, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            g_msaa_img[i].w = w; g_msaa_img[i].h = h;
            return;
        }
    }
}
static VkImageView g_scene_colour_view;
static void klvk_msaa_img_del(VkImage img) {
    if (!img) return;
    // Whatever view of it was remembered for klvk_input_att_fix() dies with it.
    uint32_t dw, dh;
    for (unsigned i = 0; i < sizeof g_msaa_img / sizeof *g_msaa_img; i++)
        if (g_msaa_img[i].img == img && g_msaa_img[i].w >= 1024) {
            (void)dw; (void)dh;
            __atomic_store_n(&g_scene_colour_view, VK_NULL_HANDLE, __ATOMIC_RELEASE);
        }
    for (unsigned i = 0; i < sizeof g_msaa_img / sizeof *g_msaa_img; i++)
        if (g_msaa_img[i].img == img) { g_msaa_img[i].w = g_msaa_img[i].h = 0; g_msaa_img[i].img = VK_NULL_HANDLE; }
}
static int klvk_msaa_img_size(VkImage img, uint32_t *w, uint32_t *h) {
    if (!img) return 0;
    for (unsigned i = 0; i < sizeof g_msaa_img / sizeof *g_msaa_img; i++)
        if (g_msaa_img[i].img == img && g_msaa_img[i].w) { *w = g_msaa_img[i].w; *h = g_msaa_img[i].h; return 1; }
    return 0;
}

// ...and the views of them, which is what a pass names. klvk_view_image() is
// not asked: its table is append-only and full after 8192 views, which a level's
// textures reach long before its render targets are (re)made.
static struct { VkImageView view; uint32_t w, h; } g_msaa_view[64];
static void klvk_msaa_view_note(VkImageView view, VkImage img) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    uint32_t w = 0, h = 0;
    int ms = klvk_msaa_img_size(img, &w, &h);
    int slot = -1;
    pthread_mutex_lock(&mu);
    for (unsigned i = 0; i < sizeof g_msaa_view / sizeof *g_msaa_view; i++) {
        if (g_msaa_view[i].view == view) { slot = (int)i; break; }     // a handle used again
        if (slot < 0 && !g_msaa_view[i].view) slot = (int)i;
    }
    if (slot < 0) { static unsigned rr; slot = (int)(rr++ % (sizeof g_msaa_view / sizeof *g_msaa_view)); }
    if (ms) { g_msaa_view[slot].w = w; g_msaa_view[slot].h = h; g_msaa_view[slot].view = view; }
    else if (g_msaa_view[slot].view == view) { g_msaa_view[slot].view = VK_NULL_HANDLE; g_msaa_view[slot].w = 0; }
    pthread_mutex_unlock(&mu);
}
static int klvk_msaa_view_size(VkImageView view, uint32_t *w, uint32_t *h) {
    if (!view) return 0;
    for (unsigned i = 0; i < sizeof g_msaa_view / sizeof *g_msaa_view; i++)
        if (g_msaa_view[i].view == view && g_msaa_view[i].w) { *w = g_msaa_view[i].w; *h = g_msaa_view[i].h; return 1; }
    return 0;
}

// The census of what the guest's images hold, by kind — the answer to "where
// does the GPU memory go". Estimated from the create infos (a format's bytes a
// texel, a third more for a mip chain), so it is the guest's request rather than
// what Metal charges; the two are close enough to rank the kinds.
enum { KLVK_CEN_MSAA, KLVK_CEN_TARGET, KLVK_CEN_BLOCK, KLVK_CEN_PLAIN, KLVK_CEN_N };
#define KLVK_CEN_SLOTS 32768u
static struct { VkImage img; uint32_t kb; uint8_t kind; } g_cen[KLVK_CEN_SLOTS];
static uint64_t g_cen_kb[KLVK_CEN_N];
static uint32_t g_cen_count[KLVK_CEN_N];
static pthread_mutex_t g_cen_mu = PTHREAD_MUTEX_INITIALIZER;
// Bits a texel: the formats an engine actually asks for; anything else is 32.
static unsigned klvk_cen_bits(VkFormat f, int *block) {
    *block = 0;
    switch ((int)f) {
    case 131: case 132: case 133: case 134:            // BC1
    case 139: case 140:                                // BC4
        *block = 1; return 4;
    case 135: case 136: case 137: case 138:            // BC2, BC3
    case 141: case 142: case 143: case 144: case 145: case 146:   // BC5, BC6H, BC7
        *block = 1; return 8;
    case 9: case 10: case 13: case 14:                 // R8
        return 8;
    case 16: case 17: case 20: case 21:                // R8G8
    case 70: case 74: case 76:                         // R16
    case 124:                                          // D16
        return 16;
    case 97:                                           // R16G16B16A16_SFLOAT
    case 91: case 95:                                  // R16G16B16A16
    case 103:                                          // R32G32_SFLOAT
    case 130:                                          // D32_SFLOAT_S8_UINT
        return 64;
    case 109:                                          // R32G32B32A32_SFLOAT
        return 128;
    default:
        if ((int)f >= 147 && (int)f <= 184) { *block = 1; return 8; }   // ETC2, EAC, ASTC: a guess
        return 32;
    }
}
static void klvk_cen_add(VkImage img, const VkImageCreateInfo *ci) {
    int block;
    unsigned bits = klvk_cen_bits(ci->format, &block);
    double b = (double)ci->extent.width * ci->extent.height * (ci->extent.depth ? ci->extent.depth : 1)
             * ci->arrayLayers * (ci->samples ? ci->samples : 1) * bits / 8.0;
    if (ci->mipLevels > 1) b *= 4.0 / 3.0;
    int kind = (unsigned)ci->samples > 1 ? KLVK_CEN_MSAA
             : (ci->usage & 0x30u)       ? KLVK_CEN_TARGET
             : block                     ? KLVK_CEN_BLOCK : KLVK_CEN_PLAIN;
    uint32_t kb = (uint32_t)(b / 1024.0) + 1;
    // The large ones by name: a handful of images is most of any such total.
    static int said;
    if (kb >= 24u << 10 && said < 300) {
        said++;
        fprintf(stderr, "  [vk] large image: %ux%u x%u layer(s), %u mip(s), %dx samples, format %d, "
                        "usage %#x — about %u MB\n", ci->extent.width, ci->extent.height,
                ci->arrayLayers, ci->mipLevels, (int)ci->samples, (int)ci->format,
                (unsigned)ci->usage, kb >> 10);
    }
    pthread_mutex_lock(&g_cen_mu);
    uint32_t h = (uint32_t)(((uintptr_t)img >> 4) * 2654435761u) % KLVK_CEN_SLOTS;
    for (uint32_t n = 0; n < KLVK_CEN_SLOTS; n++, h = (h + 1) % KLVK_CEN_SLOTS) {
        if (g_cen[h].img) continue;
        g_cen[h].img = img; g_cen[h].kb = kb; g_cen[h].kind = (uint8_t)kind;
        g_cen_kb[kind] += kb; g_cen_count[kind]++;
        break;
    }
    pthread_mutex_unlock(&g_cen_mu);
}
static void klvk_cen_del(VkImage img) {
    if (!img) return;
    pthread_mutex_lock(&g_cen_mu);
    uint32_t h = (uint32_t)(((uintptr_t)img >> 4) * 2654435761u) % KLVK_CEN_SLOTS;
    for (uint32_t n = 0; n < KLVK_CEN_SLOTS; n++, h = (h + 1) % KLVK_CEN_SLOTS) {
        if (g_cen[h].img != img) continue;
        // A tombstone that is not NULL, so a probe for a later entry still
        // walks past it; the slot is not reused, which 32768 of them can afford.
        g_cen_kb[g_cen[h].kind] -= g_cen[h].kb; g_cen_count[g_cen[h].kind]--;
        g_cen[h].img = (VkImage)(uintptr_t)1; g_cen[h].kb = 0;
        break;
    }
    pthread_mutex_unlock(&g_cen_mu);
}
// ...and of the guest's VkDeviceMemory, by memory type: how many allocations,
// how large, and how much of that was ever mapped. Under MoltenVK a device
// memory that is mapped, or has a buffer bound, is a real MTLBuffer of its whole
// size IN ADDITION to the textures bound to it, so "mapped image memory" is
// memory held twice.
#define KLVK_MEMC_SLOTS 16384u
static struct { VkDeviceMemory mem; uint32_t kb; uint8_t type, mapped; } g_memc[KLVK_MEMC_SLOTS];
static struct { uint64_t kb, mapped_kb; uint32_t n; } g_memc_type[32];
static uint32_t klvk_memc_slot(VkDeviceMemory mem, int insert) {
    uint32_t h = (uint32_t)(((uintptr_t)mem >> 4) * 2654435761u) % KLVK_MEMC_SLOTS;
    for (uint32_t n = 0; n < KLVK_MEMC_SLOTS; n++, h = (h + 1) % KLVK_MEMC_SLOTS) {
        if (g_memc[h].mem == mem) return h;
        if (!g_memc[h].mem) return insert ? h : KLVK_MEMC_SLOTS;
    }
    return KLVK_MEMC_SLOTS;
}
static void klvk_memc_add(VkDeviceMemory mem, VkDeviceSize size, uint32_t type) {
    pthread_mutex_lock(&g_cen_mu);
    uint32_t h = klvk_memc_slot(mem, 1);
    if (h < KLVK_MEMC_SLOTS && !g_memc[h].mem) {
        g_memc[h].mem = mem; g_memc[h].kb = (uint32_t)(size >> 10) + 1;
        g_memc[h].type = (uint8_t)(type & 31); g_memc[h].mapped = 0;
        g_memc_type[type & 31].kb += g_memc[h].kb; g_memc_type[type & 31].n++;
    }
    pthread_mutex_unlock(&g_cen_mu);
}
static void klvk_memc_map(VkDeviceMemory mem) {
    pthread_mutex_lock(&g_cen_mu);
    uint32_t h = klvk_memc_slot(mem, 0);
    if (h < KLVK_MEMC_SLOTS && !g_memc[h].mapped) {
        g_memc[h].mapped = 1; g_memc_type[g_memc[h].type].mapped_kb += g_memc[h].kb;
    }
    pthread_mutex_unlock(&g_cen_mu);
}
static void klvk_memc_del(VkDeviceMemory mem) {
    if (!mem) return;
    pthread_mutex_lock(&g_cen_mu);
    uint32_t h = klvk_memc_slot(mem, 0);
    if (h < KLVK_MEMC_SLOTS) {
        g_memc_type[g_memc[h].type].kb -= g_memc[h].kb; g_memc_type[g_memc[h].type].n--;
        if (g_memc[h].mapped) g_memc_type[g_memc[h].type].mapped_kb -= g_memc[h].kb;
        // Kept as a tombstone of its own key (a handle is reused only after a
        // free, and a re-add of the same key finds this slot first).
        g_memc[h].kb = 0; g_memc[h].mapped = 0; g_memc[h].mem = (VkDeviceMemory)(uintptr_t)1;
    }
    pthread_mutex_unlock(&g_cen_mu);
}
static unsigned g_prof_draws;
static unsigned g_prof_indirect, g_prof_nonindexed, g_prof_clear_att, g_prof_queries;
static int    klvk_profile(void);
static double klvk_prof_now(void);
static void   klvk_prof_frame(double wait_s);

static void klvk_fs_tick(void);
void kl_vulkan_frame_done(int stage) {
    if (!kl_vulkan_guest_active()) return;
    klvk_fs_tick();
    klvk_device *d = g_devs[0];
    static int eye_shadow = -1;
    // Default OFF. Tested ON for olar: the trailing (rain + menu) and grayscale
    // were UNCHANGED, so the eye-layer sampling race is NOT olar's problem — its
    // trailing is compositor/reprojection-side (it affects the overlay menu too)
    // and the grayscale is world-material/content. Not worth the per-frame copy
    // by default; KL_EYE_SHADOW=1 re-enables for the wanderer black-eye A/B it
    // was originally built for.
    if (eye_shadow < 0) eye_shadow = kl_env_on("KL_EYE_SHADOW", 0);
    if (eye_shadow) klvk_eye_shadow_copy(d, stage);
    double prof_t0 = klvk_profile() ? klvk_prof_now() : 0;
    if (kl_env_on("KL_VK_FRAME_SYNC", 1) && d && d->queue) {
        VkResult r = d->QueueWaitIdle ? d->QueueWaitIdle(d->queue)
                   : d->DeviceWaitIdle ? d->DeviceWaitIdle(d->dev)
                                       : VK_SUCCESS;
        if (r != VK_SUCCESS) {
            static int said;
            if (!said++)
                VKI("waiting for the guest's queue at frame end answered %d — a "
                    "compositor may sample an eye the GPU is still writing\n", (int)r);
        }
    }
    if (klvk_profile()) klvk_prof_frame(klvk_prof_now() - prof_t0);
    __atomic_add_fetch(&g_frame_serial, 1, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// The same completion, without stopping the guest for it (KL_VK_FRAME_ASYNC).
//
// kl_vulkan_frame_done waits on the guest's frame thread until its queue is
// idle, so the guest's next frame cannot start while the GPU draws this one:
// a frame costs CPU time PLUS GPU time rather than the larger of the two.
// Glovebox measured that wait for Half-Life: Alyx on a Vision Pro at 28 ms of a
// 33 ms frame (its klvr_compositor.cpp, MIT) and moved it to a worker; this is
// that idea for the OpenXR path.
//
// "We do not add work to the guest's submissions" still holds. A submission
// with NO batches and a fence is the one thing Vulkan lets a third party say on
// a queue: "the fence is signaled when all work previously submitted to the
// queue has completed" (vkQueueSubmit, submitCount 0). It is what MoltenVK's own
// vkQueueWaitIdle is made of. The mark is taken on the guest's thread, where the
// queue is the guest's to use — the same moment the wait used to run — and
// waited for on any thread, since a fence needs no queue.
//
// One mark at a time: the caller does not take the next before it has waited
// for this one, which is also what keeps a guest from drawing into a picture a
// compositor is still showing.
static pthread_mutex_t g_fmark_mu = PTHREAD_MUTEX_INITIALIZER;
static klvk_device    *g_fmark_dev;
static VkFence         g_fmark_fence;

static void klvk_frame_mark_forget(klvk_device *d) {
    pthread_mutex_lock(&g_fmark_mu);            // a wait in flight finishes first
    if (g_fmark_dev == d && d) {
        if (g_fmark_fence && d->DestroyFence) d->DestroyFence(d->dev, g_fmark_fence, NULL);
        g_fmark_fence = VK_NULL_HANDLE;
        g_fmark_dev = NULL;
    }
    pthread_mutex_unlock(&g_fmark_mu);
}

int kl_vulkan_frame_mark(void) {
    if (!kl_vulkan_guest_active()) return 0;
    klvk_device *d = g_devs[0];
    if (!d || !d->queue || !d->QueueSubmit || !d->CreateFence || !d->WaitForFences || !d->ResetFences)
        return 0;
    int ok = 0;
    pthread_mutex_lock(&g_fmark_mu);
    if (g_fmark_dev != d) {
        g_fmark_fence = VK_NULL_HANDLE;         // the old device took its fence with it
        g_fmark_dev = d;
    }
    if (!g_fmark_fence) {
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (d->CreateFence(d->dev, &fi, NULL, &g_fmark_fence) != VK_SUCCESS) g_fmark_fence = VK_NULL_HANDLE;
    } else {
        d->ResetFences(d->dev, 1, &g_fmark_fence);
    }
    if (g_fmark_fence) {
        VkResult r = d->QueueSubmit(d->queue, 0, NULL, g_fmark_fence);
        ok = r == VK_SUCCESS;
        if (!ok) {
            static int said;
            if (!said++)
                VKI("a fence-only submission on the guest's queue answered %d — frames "
                    "end the waiting way instead\n", (int)r);
        }
    }
    pthread_mutex_unlock(&g_fmark_mu);
    return ok;
}

// 1 when the GPU has finished everything up to the mark, 0 when `ms` ran out.
static double g_fmark_wait_s;          // how long the last wait took, for KL_VK_PROFILE
int kl_vulkan_frame_wait(unsigned ms) {
    int done = 0;
    double t0 = klvk_profile() ? klvk_prof_now() : 0;
    pthread_mutex_lock(&g_fmark_mu);
    klvk_device *d = g_fmark_dev;
    if (d && g_fmark_fence && d->WaitForFences)
        done = d->WaitForFences(d->dev, 1, &g_fmark_fence, VK_TRUE,
                                (uint64_t)ms * 1000000ull) == VK_SUCCESS;
    pthread_mutex_unlock(&g_fmark_mu);
    if (klvk_profile()) g_fmark_wait_s = klvk_prof_now() - t0;
    return done;
}

// The serial a compositor reads as "there is a new picture". The profile line
// keeps coming on this path; its "waiting for the GPU" is then time the WORKER
// spent waiting, which the guest no longer does.
void kl_vulkan_frame_publish(void) {
    klvk_fs_tick();
    if (klvk_profile()) klvk_prof_frame(g_fmark_wait_s);
    __atomic_add_fetch(&g_frame_serial, 1, __ATOMIC_RELEASE);
}

static VkResult klvk_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) {
    klvk_device *d = dev_of_queue(q);
    unsigned this_frame = g_presented++;

    int every = kl_env_int("KL_VK_OUT_EVERY", 1);
    if (every < 1) every = 1;
    const char *dir = cap_dir();
    int want = dir && (this_frame % (unsigned)every) == 0;

    for (uint32_t i = 0; pi && i < pi->swapchainCount; i++) {
        klvk_swapchain *sc = swap_of(pi->pSwapchains[i]);
        if (!sc) continue;
        if (want && i == 0) {
            cap_frame(sc, q, pi->pImageIndices[i], pi->pWaitSemaphores,
                      pi->waitSemaphoreCount);
        } else if (pi->waitSemaphoreCount && d) {
            // Not capturing, but the wait semaphores still have to be consumed.
            VkPipelineStageFlags *stages =
                calloc(pi->waitSemaphoreCount, sizeof *stages);
            for (uint32_t k = 0; k < pi->waitSemaphoreCount; k++)
                stages[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .waitSemaphoreCount = pi->waitSemaphoreCount,
                .pWaitSemaphores = pi->pWaitSemaphores,
        .pWaitDstStageMask = stages,
            };
            d->QueueSubmit(q, 1, &si, VK_NULL_HANDLE);
            free(stages);
        }
        if (pi->pResults) pi->pResults[i] = VK_SUCCESS;
    }
    if (this_frame == 0) VKI("first vkQueuePresentKHR — the guest is rendering\n");
    return VK_SUCCESS;
}

// Forward decls: the wild-handle -> scratch substitution used by the copy/blit
// wrappers below, defined further down beside klvk_CreateImageView.
static VkImage klvk_wild_scratch(void);
static VkImage klvk_sub_wild(VkImage img);

// vkCmdCopyImage, guarded exactly like the image-view path: AC Nexus's broken
// overlay layers feed a wild VkImage handle here too (seen as x4 in the SIGSEGV
// inside MVKCmdCopyImage::setContent). If either image was never created, drop
// the copy rather than let MoltenVK dereference garbage. Resolved from MoltenVK
// by name (a command function needs no device handle to look up).
static void VKAPI_CALL klvk_CmdCopyImage(VkCommandBuffer cb, VkImage src,
        VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
        const VkImageCopy *regions) {
    static PFN_vkCmdCopyImage real;
    if (!real) real = (PFN_vkCmdCopyImage)mvk_sym("vkCmdCopyImage");
    if (klvk_trace() && klvk_trace_once((unsigned long long)(uintptr_t)src,(unsigned long long)(uintptr_t)dst)) {
        int sk=klvk_layer_image_key(src), dk=klvk_layer_image_key(dst);
        klvk_trace_log("[copy] src %#llx(%s k=%d) -> dst %#llx(%s k=%d) n=%u\n",
            (unsigned long long)(uintptr_t)src, klvk_img_tag(src), sk,
            (unsigned long long)(uintptr_t)dst, klvk_img_tag(dst), dk, n);
    }
    if ((src && !klvk_img_reg_has(src)) || (dst && !klvk_img_reg_has(dst))) {
        // DEFAULT: DROP the copy. Redirecting it onto the scratch was tried
        // (KL_VK_WILD_REDIRECT=1 restores it) and correlates with the render
        // loop stalling at pipeline depth ~8: a copy whose regions were sized
        // for the guest's own texture lands out of bounds on the 4096x4096
        // scratch, the command buffer dies, its fence never signals, and Unity
        // waits on frame 1 forever. A dropped copy costs a blank overlay; a
        // dead fence costs every frame after the eighth.
        if (kl_env_on("KL_VK_WILD_REDIRECT", 0)) {
            g_wild_copy_redir++;
            static int red; if (red < 8) { red++;
                VKI("vkCmdCopyImage: REDIRECTING wild handle to scratch "
                    "(src %#llx dst %#llx) so the guest's composite can land\n",
                    (unsigned long long)(uintptr_t)src, (unsigned long long)(uintptr_t)dst); }
            VkImage nsrc = klvk_sub_wild(src);
            VkImage ndst = klvk_sub_wild(dst);
            // The stall fix. The guest sized these regions for ITS OWN composite
            // texture; landed unchanged on the 4096x4096 scratch they run out of
            // bounds, MoltenVK kills the command buffer, its fence never signals,
            // and Unity waits on frame 1 forever. CLAMP every region to the
            // scratch's per-mip bounds and drop any that fall entirely outside,
            // so the composite lands as a live, safe copy instead of a dead fence.
            if (ndst == g_wild_scratch && n && regions) {
                // Region census: log where the guest wants each panel to land in
                // its composite, so we can see the composite's true coordinate
                // space (and whether it exceeds the 4096 scratch). KL_TRACE gates.
                if (kl_env_on("KL_TRACE", 0)) {
                    static int rl;
                    for (uint32_t i = 0; i < n && rl < 40; i++, rl++)
                        fprintf(stderr, "[TRACE] [region] src %#llx dstOff(%d,%d) "
                            "ext(%ux%u) mip %u layer %u\n",
                            (unsigned long long)(uintptr_t)nsrc,
                            regions[i].dstOffset.x, regions[i].dstOffset.y,
                            regions[i].extent.width, regions[i].extent.height,
                            regions[i].dstSubresource.mipLevel,
                            regions[i].dstSubresource.baseArrayLayer);
                }
                VkImageCopy *cr = calloc(n, sizeof *cr);
                uint32_t cn = 0;
                for (uint32_t i = 0; i < n && cr; i++) {
                    VkImageCopy rc = regions[i];
                    if (rc.dstSubresource.mipLevel >= KLVK_SCRATCH_MIPS) continue;
                    uint32_t md = 4096u >> rc.dstSubresource.mipLevel; if (!md) md = 1u;
                    if (rc.dstOffset.x < 0) rc.dstOffset.x = 0;
                    if (rc.dstOffset.y < 0) rc.dstOffset.y = 0;
                    if ((uint32_t)rc.dstOffset.x >= md || (uint32_t)rc.dstOffset.y >= md) continue;
                    uint32_t maxw = md - (uint32_t)rc.dstOffset.x;
                    uint32_t maxh = md - (uint32_t)rc.dstOffset.y;
                    if (rc.extent.width  > maxw) rc.extent.width  = maxw;
                    if (rc.extent.height > maxh) rc.extent.height = maxh;
                    if (rc.extent.width == 0u || rc.extent.height == 0u) continue;
                    if (rc.dstSubresource.baseArrayLayer >= KLVK_SCRATCH_LAYERS)
                        rc.dstSubresource.baseArrayLayer = 0;
                    cr[cn++] = rc;
                }
                if (cn && real) real(cb, nsrc, sl, ndst, dl, cn, cr);
                free(cr);
                return;
            }
            src = nsrc;
            dst = ndst;
        } else {
            // CAPTURE-ON-COPY — the deterministic replacement for the timing-
            // sensitive shadow pump. This copy is the guest reading a menu
            // panel into its composite, i.e. the ONE moment its content is
            // guaranteed present, on the guest's own thread and in its own
            // command order. The wild composite destination is unusable, so
            // retarget the copy into the panel's persistent SHADOW instead:
            // recorded into the SAME command buffer, so it executes exactly
            // when the guest's read would have. The compositor samples the
            // shadow, and what it shows no longer depends on when it looks.
            // KL_SHADOW_FROMCOPY=0 disables.
            static int fc = -1;
            if (fc < 0) fc = kl_env_on("KL_SHADOW_FROMCOPY", 1);
            if (fc && real && src && klvk_img_reg_has(src)
                && dst && !klvk_img_reg_has(dst) && n && regions) {
                VkImage sh = klvk_shadow_for_copy(src);
                uint32_t shw = 0, shh = 0; VkFormat shf;
                if (sh && klvk_img_info_get(src, &shf, &shw, &shh)) {
                    VkImageCopy *rew = calloc(n, sizeof *rew);
                    uint32_t rn = 0;
                    for (uint32_t i = 0; i < n && rew; i++) {
                        VkImageCopy rc = regions[i];
                        if (rc.srcSubresource.mipLevel != 0) continue;
                        rc.dstSubresource = rc.srcSubresource;
                        rc.dstSubresource.baseArrayLayer = 0;
                        rc.dstOffset = rc.srcOffset;   // same rect, panel-space
                        if ((uint32_t)rc.dstOffset.x >= shw ||
                            (uint32_t)rc.dstOffset.y >= shh) continue;
                        uint32_t mw = shw - (uint32_t)rc.dstOffset.x;
                        uint32_t mh = shh - (uint32_t)rc.dstOffset.y;
                        if (rc.extent.width  > mw) rc.extent.width  = mw;
                        if (rc.extent.height > mh) rc.extent.height = mh;
                        if (rc.extent.width && rc.extent.height) rew[rn++] = rc;
                    }
                    if (rn) real(cb, src, sl, sh, VK_IMAGE_LAYOUT_GENERAL, rn, rew);
                    free(rew);
                    static int said; if (said < 8) { said++;
                        VKI("vkCmdCopyImage: CAPTURED panel %#llx into shadow %#llx "
                            "(composite copy retargeted)\n",
                            (unsigned long long)(uintptr_t)src,
                            (unsigned long long)(uintptr_t)sh); }
                    return;
                }
            }
            static int dropped; if (dropped < 4) { dropped++;
                VKI("vkCmdCopyImage: DROPPING copy on wild handle "
                    "(src %#llx dst %#llx) - KL_VK_WILD_REDIRECT=1 redirects instead\n",
                    (unsigned long long)(uintptr_t)src, (unsigned long long)(uintptr_t)dst); }
            return;
        }
    }
    if (n && regions)
        fove_xfer_note("vkCmdCopyImage", "copy", src, dst, n, &regions->srcSubresource, &regions->dstSubresource,
                       regions->srcOffset.x, regions->srcOffset.y, regions->dstOffset.x, regions->dstOffset.y,
                       regions->extent.width, regions->extent.height);
    if (real) real(cb, src, sl, dst, dl, n, regions);
}

// The rest of the image-consuming commands, guarded the same way: if a handle
// was never created here (AC Nexus's unbacked video-overlay garbage), drop the
// command instead of letting MoltenVK dereference the wild pointer. All resolved
// from MoltenVK by name; a command function needs no device handle.
#define KLVK_IMG_BAD(img) ((img) && !klvk_img_reg_ok(img))
static void VKAPI_CALL klvk_CmdBlitImage(VkCommandBuffer cb, VkImage src,
        VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
        const VkImageBlit *r, VkFilter f) {
    static PFN_vkCmdBlitImage real;
    if (!real) real = (PFN_vkCmdBlitImage)mvk_sym("vkCmdBlitImage");
    if (KLVK_IMG_BAD(src) || KLVK_IMG_BAD(dst)) {
        if (!kl_env_on("KL_VK_WILD_REDIRECT", 0)) return;   // drop, as the copy above
        g_wild_blit_redir++;
        src = klvk_sub_wild(src);
        dst = klvk_sub_wild(dst);
    }
    if (n && r)
        fove_xfer_note("vkCmdBlitImage", "blit", src, dst, n, &r->srcSubresource, &r->dstSubresource,
                       r->srcOffsets[0].x, r->srcOffsets[0].y, r->dstOffsets[0].x, r->dstOffsets[0].y,
                       (uint32_t)(r->dstOffsets[1].x - r->dstOffsets[0].x),
                       (uint32_t)(r->dstOffsets[1].y - r->dstOffsets[0].y));
    if (real) real(cb, src, sl, dst, dl, n, r, f);
}
static void VKAPI_CALL klvk_CmdResolveImage(VkCommandBuffer cb, VkImage src,
        VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
        const VkImageResolve *r) {
    static PFN_vkCmdResolveImage real;
    if (!real) real = (PFN_vkCmdResolveImage)mvk_sym("vkCmdResolveImage");
    if (KLVK_IMG_BAD(src) || KLVK_IMG_BAD(dst)) {
        VKI("vkCmdResolveImage: REFUSING (src %#llx dst %#llx unknown)\n",
            (unsigned long long)(uintptr_t)src, (unsigned long long)(uintptr_t)dst);
        return;
    }
    if (n && r)
        fove_xfer_note("vkCmdResolveImage", "resolve", src, dst, n, &r->srcSubresource, &r->dstSubresource,
                       r->srcOffset.x, r->srcOffset.y, r->dstOffset.x, r->dstOffset.y, r->extent.width,
                       r->extent.height);
    if (real) real(cb, src, sl, dst, dl, n, r);
}
static void VKAPI_CALL klvk_CmdClearColorImage(VkCommandBuffer cb, VkImage img,
        VkImageLayout l, const VkClearColorValue *c, uint32_t n,
        const VkImageSubresourceRange *r) {
    static PFN_vkCmdClearColorImage real;
    if (!real) real = (PFN_vkCmdClearColorImage)mvk_sym("vkCmdClearColorImage");
    if (KLVK_IMG_BAD(img)) { VKI("vkCmdClearColorImage: REFUSING (img %#llx unknown)\n",
        (unsigned long long)(uintptr_t)img); return; }
    if (real) real(cb, img, l, c, n, r);
}
static int emu_on(void);    // defined below (target/env gate for the mirror + probes)
static int probe_on(void);
static int klvk_astc_sub_has(VkImage img);   // KL_VK_ASTC_RGBA8 test; defined below
static void VKAPI_CALL klvk_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer buf,
        VkImage img, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) {
    KFE(KFE_COPY_B2I);
    static PFN_vkCmdCopyBufferToImage real;
    if (!real) real = (PFN_vkCmdCopyBufferToImage)mvk_sym("vkCmdCopyBufferToImage");
    if (KLVK_IMG_BAD(img)) { VKI("vkCmdCopyBufferToImage: REFUSING (img %#llx unknown)\n",
        (unsigned long long)(uintptr_t)img); return; }
    // KL_VK_ASTC_RGBA8 test: this image was created RGBA8 in place of ASTC. The
    // guest's ASTC-block upload would overrun the RGBA8 store (4x the bytes), so SKIP
    // it and clear to solid GREEN. World turns green => the ASTC albedo reaches
    // surfaces with valid UVs => the gray was the ASTC content/decode. Stays gray =>
    // ASTC is not the visible albedo. The image is in TRANSFER_DST (l) here.
    if (klvk_astc_sub_has(img)) {
        static PFN_vkCmdClearColorImage clr;
        if (!clr) clr = (PFN_vkCmdClearColorImage)mvk_sym("vkCmdClearColorImage");
        if (clr) {
            VkClearColorValue green; green.float32[0] = 0.0f; green.float32[1] = 1.0f;
            green.float32[2] = 0.0f; green.float32[3] = 1.0f;
            VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                            VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
            clr(cb, img, l, &green, 1, &rng);
        }
        return;   // skip the real ASTC->RGBA8 copy (would overrun)
    }
    // PROBE (olar gray hunt): count texture uploads by target-image FORMAT. The gray
    // world materials are the open question — if the ASTC world textures (fmt
    // 165/171/172 = ASTC 6x6/8x8) never appear here, they are created but never
    // filled -> sampled empty -> flat gray. A per-format tally (logged once per new
    // format, then a running total) tells us whether the albedo textures get data.
    if (emu_on() || probe_on()) {
        static struct { int fmt, count; } hist[32];
        static int nhist, total;
        VkFormat f = 0; uint32_t w = 0, h = 0;
        klvk_img_info_get(img, &f, &w, &h);
        pthread_mutex_lock(&g_img_info_lock);
        total++;
        int seen = 0;
        for (int i = 0; i < nhist; i++) if (hist[i].fmt == (int)f) { hist[i].count++; seen = 1; break; }
        if (!seen && nhist < 32) {
            hist[nhist].fmt = (int)f; hist[nhist].count = 1; nhist++;
            VKI("[probe] CopyBufferToImage: first upload to format %d (%ux%u) — "
                "texture data IS being delivered to this format (total uploads %d)\n",
                (int)f, w, h, total);
        }
        if ((total & (total - 1)) == 0)   // powers of two: 1,2,4,8,... a cheap heartbeat
            VKI("[probe] CopyBufferToImage: %d uploads so far across %d distinct "
                "formats\n", total, nhist);
        pthread_mutex_unlock(&g_img_info_lock);
    }
    if (real) real(cb, buf, img, l, n, r);
    // DECISIVE DIAGNOSTIC (KL_VK_TINT_DEFAULTS=1): tint every 1x1 texture magenta
    // right after its upload. If the gray WORLD turns magenta, materials are
    // sampling 1x1 default placeholders (binding/upload-sync bug); if it stays gray,
    // the world samples its real textures and the bug is in sampling (UVs / ASTC).
    // The image is still in TRANSFER_DST here (l), and 1x1 defaults are copy targets,
    // so a clear needs no extra barrier. Diagnostic only; off by default.
    if (real && kl_env_on("KL_VK_TINT_DEFAULTS", 0)) {
        VkFormat f = 0; uint32_t w = 0, h = 0;
        // Only plain 8-bit color formats (37..50 = R8G8B8A8 / B8G8R8A8 variants) are
        // safe to color-clear — that is exactly what UE's default placeholder
        // textures are. Depth (124=D16_UNORM..130), compressed (>=131: BC/ETC/ASTC),
        // and float formats CANNOT be color-cleared (MoltenVK errors and the process
        // dies), so skip them.
        if (klvk_img_info_get(img, &f, &w, &h) && w <= 1 && h <= 1 &&
            (int)f >= 37 && (int)f <= 50) {
            static PFN_vkCmdClearColorImage clr;
            if (!clr) clr = (PFN_vkCmdClearColorImage)mvk_sym("vkCmdClearColorImage");
            if (clr) {
                VkClearColorValue magenta;
                magenta.float32[0] = 1.0f; magenta.float32[1] = 0.0f;
                magenta.float32[2] = 1.0f; magenta.float32[3] = 1.0f;
                VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                clr(cb, img, l, &magenta, 1, &rng);
            }
        }
    }
}
static void VKAPI_CALL klvk_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage img,
        VkImageLayout l, VkBuffer buf, uint32_t n, const VkBufferImageCopy *r) {
    static PFN_vkCmdCopyImageToBuffer real;
    if (!real) real = (PFN_vkCmdCopyImageToBuffer)mvk_sym("vkCmdCopyImageToBuffer");
    if (KLVK_IMG_BAD(img)) { VKI("vkCmdCopyImageToBuffer: REFUSING (img %#llx unknown)\n",
        (unsigned long long)(uintptr_t)img); return; }
    if (real) real(cb, img, l, buf, n, r);
}
// Pipeline barrier is special: it carries an ARRAY of image barriers, so rather
// than drop the whole call we filter out the ones on unknown images and forward
// the rest - the eye layer's own transitions must still go through.
// The command buffers that are inside a vkCmdBeginRendering right now. A few
// recording threads at most, so a short table; a slot is claimed and released
// by the thread that records that buffer.
static VkCommandBuffer g_in_rendering[16];
static void klvk_in_rendering_set(VkCommandBuffer cb, int on) {
    for (unsigned i = 0; i < sizeof g_in_rendering / sizeof *g_in_rendering; i++) {
        VkCommandBuffer want = on ? VK_NULL_HANDLE : cb;
        if (__atomic_compare_exchange_n(&g_in_rendering[i], &want, on ? cb : VK_NULL_HANDLE, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;
    }
}
static int klvk_in_rendering(VkCommandBuffer cb) {
    for (unsigned i = 0; i < sizeof g_in_rendering / sizeof *g_in_rendering; i++)
        if (__atomic_load_n(&g_in_rendering[i], __ATOMIC_ACQUIRE) == cb) return 1;
    return 0;
}

// KL_VK_PASS_BARRIERS — what to do with a vkCmdPipelineBarrier recorded INSIDE a
// dynamic-rendering pass. Default (for the targets that set it): 0, drop it.
//
// Metal has no barrier that can be issued in the middle of a render pass on
// Apple's own GPUs, so MoltenVK does the only thing it can: it ENDS the Metal
// pass and opens another (MVKCmdPipelineBarrier::encode -> beginMetalRenderPass,
// seen in a backtrace taken from kl_mtl_foveate.m's trace). Ending a pass
// stores every attachment and re-opening it loads them again, whatever the
// Vulkan pass said about load and store — for a multisampled eye target that is
// hundreds of megabytes written and read back each frame, plus a second resolve.
// Half-Life: Alyx records one such barrier per frame in its scene pass, and with
// it gone the pass is one Metal pass (Mac, first level, 2x: 17.1 -> 14.9 ms of
// GPU a frame).
//
// That barrier makes the multisampled COLOUR attachment readable as an input
// attachment (layout RENDERING_LOCAL_READ, access INPUT_ATTACHMENT_READ): the
// engine's own per-sample resolve reads what the pass has drawn so far. On a
// tile-based GPU that read is a framebuffer fetch, served from the tile, and
// needs nothing stored — with the barrier dropped the picture is the same to
// within the scene's own animation (menu, Mac: mean difference 0.44 of 255
// against a second run of the unchanged path). Barriers outside a pass are
// untouched, and so is any in-pass barrier that is not exactly this shape.
// 0 drop, 1 keep, 2 defer — see the correction below.
static int klvk_pass_barriers(void) {
    static int v = -1;
    if (v < 0) { v = kl_env_int("KL_VK_PASS_BARRIERS", 1); if (v < 0 || v > 2) v = 1; }
    return v;
}
static unsigned g_prof_pass_barriers, g_prof_pass_barriers_dropped;

// CORRECTION (2026-10-06, the same day). Dropping that barrier and doing nothing
// else was wrong, and the menu comparison above could not have shown it. The
// reader is not the engine's resolve: it is one small full-screen shader that
// reads sample 0 of the scene as an input attachment, takes its luminance,
// discards outside a range and writes nothing — an exposure meter, counted by
// the one occlusion query a frame. MoltenVK serves an input attachment as an
// ordinary texture read, which returns what was last STORED; with the pass no
// longer split nothing was ever stored, so the meter read stale memory. The
// picture of a scene without characters did not change. What a player saw was
// the eyes of characters flaring, far too bright and flickering.
//
// Two answers are here. Mode 0 keeps the barrier dropped and has the pass store
// its colour as it ends (klvk_widen_area), so the meter reads the previous
// frame. Mode 2 DEFERS the barrier instead: it is held back and issued just
// before the first pipeline that reads a subpass input is bound, and dropped if
// the pass ends first — the right shape for a guest whose reader is occasional.
// For this one it never drops: the meter is drawn every frame. Which pipelines
// read a subpass input is known from their fragment shader (an OpTypeImage of
// dimension SubpassData).
// KL_VK_FS_STATS=<file> / KL_VK_SKIP_FS=<hash>,<hash>,... — which fragment
// shader draws what. Every graphics pipeline is known by a hash of its fragment
// shader's SPIR-V, which is the same from run to run (a handle or a creation
// order is not). The first knob counts draws per hash and writes the table to a
// file; the second drops every draw whose pipeline has one of the listed hashes.
// Halving the list until a thing on screen disappears names the shader that
// draws it — how the shader of a character's eyes is found in an engine whose
// shaders carry no names.
#define KLVK_FS_SLOTS 8192u
static struct { uint64_t key; uint32_t hash; } g_fs_mod[KLVK_FS_SLOTS], g_fs_pipe[KLVK_FS_SLOTS];
static struct { uint32_t hash; uint32_t draws; } g_fs_count[2048];
static uint32_t g_fs_skip[2048];
static int g_fs_nskip = -1, g_fs_stats = -1;
static __thread uint32_t t_fs_hash;
static __thread int t_fs_skip;
static uint32_t klvk_fnv(const void *p, size_t n) {
    const uint8_t *b = p; uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h ? h : 1;
}
static void klvk_fs_put(void *tab_, uint64_t key, uint32_t hash) {
    struct { uint64_t key; uint32_t hash; } *tab = tab_;
    if (!key) return;
    uint32_t i = (uint32_t)((key >> 4) * 2654435761u) % KLVK_FS_SLOTS;
    for (uint32_t n = 0; n < KLVK_FS_SLOTS; n++, i = (i + 1) % KLVK_FS_SLOTS)
        if (!tab[i].key || tab[i].key == key) { tab[i].hash = hash; tab[i].key = key; return; }
}
static uint32_t klvk_fs_get(const void *tab_, uint64_t key) {
    const struct { uint64_t key; uint32_t hash; } *tab = tab_;
    if (!key) return 0;
    uint32_t i = (uint32_t)((key >> 4) * 2654435761u) % KLVK_FS_SLOTS;
    for (uint32_t n = 0; n < KLVK_FS_SLOTS; n++, i = (i + 1) % KLVK_FS_SLOTS) {
        if (tab[i].key == key) return tab[i].hash;
        if (!tab[i].key) return 0;
    }
    return 0;
}
static int klvk_fs_on(void) {
    if (g_fs_nskip < 0) {
        int n = 0;
        const char *e = kl_env_str("KL_VK_SKIP_FS", NULL);
        while (e && *e && n < (int)(sizeof g_fs_skip / sizeof *g_fs_skip)) {
            char *end = NULL;
            unsigned long v = strtoul(e, &end, 16);
            if (end == e) break;
            g_fs_skip[n++] = (uint32_t)v;
            e = *end == ',' ? end + 1 : end;
        }
        const char *f = kl_env_str("KL_VK_FS_STATS", NULL);
        const char *rec = kl_env_str("KL_VK_PIPE_REC", NULL);
        g_fs_stats = (f && *f) || (rec && *rec);
        g_fs_nskip = n;
        if (n) fprintf(stderr, "  [vk] KL_VK_SKIP_FS: draws of %d fragment shader(s) are dropped\n", n);
    }
    static int sct = -1;
    if (sct < 0) sct = kl_env_str("KL_VK_SCISSOR_TRACE", NULL) != NULL ||
                       kl_env_str("KL_VK_NO_STENCIL_FS", NULL) != NULL ||
                       kl_env_str("KL_VK_VI_TRACE_FS", NULL) != NULL;
    return g_fs_nskip > 0 || g_fs_stats > 0 || sct;
}
// KL_VK_DESC_TRACE=1: for a few frames well into a run, the order in which a
// recording thread updates descriptor sets, binds them and draws, from the
// subpass-reading draw on. MoltenVK reads a set's contents when the command
// buffer is ENCODED, not when the bind is recorded, so a set that is written
// again after a draw that used it shows that draw the later contents.
static __thread char t_ev[2600];
static __thread int t_ev_n = -1;
static unsigned g_ev_frames;
static void klvk_ev(const char *fmt, unsigned long long a, unsigned long long b) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_DESC_TRACE", 0);
    if (!on || t_ev_n < 0 || t_ev_n > 2500) return;
    t_ev_n += snprintf(t_ev + t_ev_n, sizeof t_ev - (size_t)t_ev_n, fmt, a, b);
}
static void klvk_ev_start(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_DESC_TRACE", 0);
    if (!on) return;
    if (t_ev_n > 0) {
        unsigned f = __atomic_add_fetch(&g_ev_frames, 1, __ATOMIC_RELAXED);
        if (f > 900 && f <= 903) fprintf(stderr, "  [vk] from the subpass reader on:%s\n", t_ev);
    }
    t_ev_n = 0; t_ev[0] = 0;
}
// KL_VK_AFTER_SUBPASS_TRACE=1: the fragment shaders of the draws that follow a
// subpass-reading draw inside its pass, for a handful of frames well into a run
// — what is drawn right after the engine's exposure meter.
static __thread int t_after_n = -1;
static __thread uint32_t t_after_seq[24];
static unsigned g_after_frames;
static void klvk_after_note(uint32_t hash, int start) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_AFTER_SUBPASS_TRACE", 0);
    if (!on) return;
    if (start) {
        if (t_after_n > 0) {
            unsigned f = __atomic_add_fetch(&g_after_frames, 1, __ATOMIC_RELAXED);
            if (f > 900 && f <= 906) {
                fprintf(stderr, "  [vk] after the subpass reader:");
                for (int i = 0; i < t_after_n; i++) fprintf(stderr, " %08x", t_after_seq[i]);
                fprintf(stderr, "\n");
            }
        }
        t_after_n = 0;
        return;
    }
    if (t_after_n >= 0 && t_after_n < 24) t_after_seq[t_after_n++] = hash;
}
// KL_VK_SCISSOR_TRACE=<hash>,...: for draws whose fragment shader has one of
// these hashes (KL_VK_FS_STATS's), the scissor and viewport set last on the
// recording thread, for the first 80 such draws (after KL_VK_FS_FROM). Written
// for HITMAN 3 VR: Reloaded's Scaleform board, which a headset showed cut into
// screen-aligned steps (2026-10-21).
static __thread VkRect2D t_sc[4];
static __thread uint32_t t_sc_first, t_sc_n;
static __thread VkViewport t_vp[4];
static __thread uint32_t t_vp_first, t_vp_n;
static void VKAPI_CALL klvk_CmdSetScissor(void *cb, uint32_t first, uint32_t n, const VkRect2D *r) {
    static void (*real)(void *, uint32_t, uint32_t, const VkRect2D *);
    if (!real && real_gdpa) real = (void (*)(void *, uint32_t, uint32_t, const VkRect2D *))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdSetScissor");
    t_sc_first = first; t_sc_n = n < 4 ? n : 4;
    for (uint32_t i = 0; r && i < t_sc_n; i++) t_sc[i] = r[i];
    if (real) real(cb, first, n, r);
}
static void VKAPI_CALL klvk_CmdSetViewport(void *cb, uint32_t first, uint32_t n, const VkViewport *v) {
    static void (*real)(void *, uint32_t, uint32_t, const VkViewport *);
    if (!real && real_gdpa) real = (void (*)(void *, uint32_t, uint32_t, const VkViewport *))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdSetViewport");
    t_vp_first = first; t_vp_n = n < 4 ? n : 4;
    for (uint32_t i = 0; v && i < t_vp_n; i++) t_vp[i] = v[i];
    if (real) real(cb, first, n, v);
}
static void klvk_scissor_trace(uint32_t hash) {
    static uint32_t list[32];
    static int n = -1;
    static unsigned said;
    if (n < 0) {
        n = 0;
        const char *e = kl_env_str("KL_VK_SCISSOR_TRACE", NULL);
        while (e && *e && n < 32) {
            char *end = NULL;
            unsigned long v = strtoul(e, &end, 16);
            if (end == e) break;
            list[n++] = (uint32_t)v;
            e = *end == ',' ? end + 1 : end;
        }
    }
    if (!n || said >= 80) return;
    int hit = 0;
    for (int i = 0; i < n; i++) if (list[i] == hash) hit = 1;
    if (!hit) return;
    // Only scissors that cut something: a full-frame one says nothing.
    int cut = 0;
    for (uint32_t i = 0; i < t_sc_n && i < t_vp_n; i++) {
        int vw = (int)(t_vp[i].width < 0 ? -t_vp[i].width : t_vp[i].width);
        int vh = (int)(t_vp[i].height < 0 ? -t_vp[i].height : t_vp[i].height);
        if ((int)t_sc[i].extent.width < vw || (int)t_sc[i].extent.height < vh || t_sc[i].offset.x || t_sc[i].offset.y) cut = 1;
    }
    if (!cut) return;
    said++;
    char sc[256] = "", vp[256] = "";
    size_t o = 0;
    for (uint32_t i = 0; i < t_sc_n && o + 48 < sizeof sc; i++)
        o += (size_t)snprintf(sc + o, sizeof sc - o, " [%u] %dx%d+%d+%d", t_sc_first + i, (int)t_sc[i].extent.width,
                              (int)t_sc[i].extent.height, t_sc[i].offset.x, t_sc[i].offset.y);
    o = 0;
    for (uint32_t i = 0; i < t_vp_n && o + 48 < sizeof vp; i++)
        o += (size_t)snprintf(vp + o, sizeof vp - o, " [%u] %.0fx%.0f+%.0f+%.0f", t_vp_first + i, (double)t_vp[i].width,
                              (double)t_vp[i].height, (double)t_vp[i].x, (double)t_vp[i].y);
    VKI("scissor trace %08x: scissor%s | viewport%s\n", hash, sc[0] ? sc : " (none set)", vp[0] ? vp : " (none set)");
}

// KL_VK_FS_FROM=<seconds>: both of the knobs above start that long after the
// first draw — so a player can walk to the place in question with every shader
// there, and the count is of that place alone. HITMAN 3 VR: Reloaded's window
// in its tutorial's third room, found on the headset (2026-10-21).
static int klvk_fs_armed(void) {
    static int armed = -1;
    static double from, t0;
    if (armed > 0) return 1;
    if (armed < 0) { from = kl_env_float("KL_VK_FS_FROM", 0.0f); armed = from > 0 ? 0 : 1; if (armed) return 1; }
    double now = klvk_prof_now();
    if (t0 == 0) t0 = now;
    if (now - t0 < from) return 0;
    armed = 1;
    VKI("KL_VK_FS_FROM: counting and dropping fragment shaders from %.0f s on\n", from);
    return 1;
}
// At a draw: 1 if it is to be dropped.
static int klvk_fs_draw(void) {
    KFE(KFE_DRAW);
    klvk_after_note(t_fs_hash, 0);
    klvk_ev(" D%08llx", t_fs_hash, 0);
    if (!klvk_fs_on()) return 0;
    if (!klvk_fs_armed()) return 0;
    klvk_scissor_trace(t_fs_hash);
    if (g_fs_stats > 0) {
        if (!t_fs_hash) t_fs_hash = 0xffffffffu;        // a pipeline whose fragment shader is not known
        for (unsigned i = 0; i < sizeof g_fs_count / sizeof *g_fs_count; i++) {
            if (g_fs_count[i].hash == t_fs_hash) { __atomic_add_fetch(&g_fs_count[i].draws, 1, __ATOMIC_RELAXED); break; }
            uint32_t none = 0;
            if (!g_fs_count[i].hash &&
                __atomic_compare_exchange_n(&g_fs_count[i].hash, &none, t_fs_hash, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                __atomic_add_fetch(&g_fs_count[i].draws, 1, __ATOMIC_RELAXED); break;
            }
            if (g_fs_count[i].hash == t_fs_hash) { __atomic_add_fetch(&g_fs_count[i].draws, 1, __ATOMIC_RELAXED); break; }
        }
    }
    return t_fs_skip;
}
static void klvk_fs_dump(void) {
    const char *f = kl_env_str("KL_VK_FS_STATS", NULL);
    if (!f || !*f) return;
    FILE *o = fopen(f, "w");
    if (!o) return;
    for (unsigned i = 0; i < sizeof g_fs_count / sizeof *g_fs_count && g_fs_count[i].hash; i++)
        fprintf(o, "%08x %u\n", g_fs_count[i].hash, g_fs_count[i].draws);
    fclose(o);
}
static void klvk_fs_tick(void) {
    static unsigned n;
    if (g_fs_stats > 0 && !(++n % 200)) klvk_fs_dump();
}

#define KLVK_SUBPASS_SLOTS 4096u
static uint64_t g_subpass_mod[KLVK_SUBPASS_SLOTS], g_subpass_pipe[KLVK_SUBPASS_SLOTS];
// Vertex shader modules of Scaleform (their uniform block is named
// SCALEFORM_CONSTANTS_BLOCK): see klvk_vi_norm.
static uint64_t g_sf_mod[KLVK_SUBPASS_SLOTS];
// 1x1 images and the views of them: what a guest binds where it has to bind
// SOMETHING (klvk_input_att_fix). A set of their own, because the general image
// and view tables of this file are full after a few thousand entries — which a
// level's textures are — and then answer "unknown" for everything created later.
static uint64_t g_tiny_img[KLVK_SUBPASS_SLOTS], g_tiny_view[KLVK_SUBPASS_SLOTS];
static void klvk_handle_set_add(uint64_t *set, uint64_t h) {
    if (!h) return;
    uint32_t i = (uint32_t)((h >> 4) * 2654435761u) % KLVK_SUBPASS_SLOTS;
    for (uint32_t n = 0; n < KLVK_SUBPASS_SLOTS; n++, i = (i + 1) % KLVK_SUBPASS_SLOTS) {
        uint64_t none = 0;
        if (__atomic_load_n(&set[i], __ATOMIC_ACQUIRE) == h) return;
        if (__atomic_compare_exchange_n(&set[i], &none, h, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
        if (none == h) return;
    }
}
static int klvk_handle_set_has(const uint64_t *set, uint64_t h) {
    if (!h) return 0;
    uint32_t i = (uint32_t)((h >> 4) * 2654435761u) % KLVK_SUBPASS_SLOTS;
    for (uint32_t n = 0; n < KLVK_SUBPASS_SLOTS; n++, i = (i + 1) % KLVK_SUBPASS_SLOTS) {
        uint64_t v = __atomic_load_n(&set[i], __ATOMIC_ACQUIRE);
        if (v == h) return 1;
        if (!v) return 0;
    }
    return 0;
}
static int klvk_spirv_reads_subpass(const uint32_t *code, size_t nwords) {
    if (!code || nwords < 6 || code[0] != 0x07230203u) return 0;
    for (size_t i = 5; i < nwords; ) {
        uint32_t len = code[i] >> 16, op = code[i] & 0xffffu;
        if (!len) return 0;
        if (op == 25 /* OpTypeImage */ && len >= 4 && i + 3 < nwords && code[i + 3] == 6 /* SubpassData */)
            return 1;
        i += len;
    }
    return 0;
}
// One recording thread, one command buffer at a time: what it last bound, and
// the barrier it is holding back.
static __thread VkCommandBuffer t_bind_cb;
static __thread int             t_bind_reads;
static __thread struct {
    VkCommandBuffer cb;
    VkPipelineStageFlags ss, ds;
    VkDependencyFlags df;
    uint32_t n;
    VkImageMemoryBarrier ib[4];
} t_held;
static void klvk_held_barrier_flush(VkCommandBuffer cb) {
    if (!t_held.cb || t_held.cb != cb) return;
    PFN_vkCmdPipelineBarrier real = (PFN_vkCmdPipelineBarrier)mvk_sym("vkCmdPipelineBarrier");
    t_held.cb = VK_NULL_HANDLE;
    if (real) real(cb, t_held.ss, t_held.ds, t_held.df, 0, NULL, 0, NULL, t_held.n, t_held.ib);
}

static void VKAPI_CALL klvk_CmdPipelineBarrier(VkCommandBuffer cb,
        VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags df,
        uint32_t mbc, const VkMemoryBarrier *mb,
        uint32_t bbc, const VkBufferMemoryBarrier *bb,
        uint32_t ibc, const VkImageMemoryBarrier *ib) {
    static PFN_vkCmdPipelineBarrier real;
    if (!real) real = (PFN_vkCmdPipelineBarrier)mvk_sym("vkCmdPipelineBarrier");
    if (!real) return;
    if (klvk_in_rendering(cb)) {
        __atomic_add_fetch(&g_prof_pass_barriers, 1, __ATOMIC_RELAXED);
        static int said;
        if (said < 4) {
            said++;
            fprintf(stderr, "  [vk] a barrier INSIDE a render pass: stages %#x -> %#x, flags %#x, "
                            "%u memory (%#x -> %#x), %u buffer, %u image (layout %d -> %d)%s\n",
                    ss, ds, df, mbc, mbc ? mb[0].srcAccessMask : 0, mbc ? mb[0].dstAccessMask : 0,
                    bbc, ibc, ibc ? (int)ib[0].oldLayout : -1, ibc ? (int)ib[0].newLayout : -1,
                    klvk_pass_barriers() == 0 ? " — dropped (KL_VK_PASS_BARRIERS=0)" :
                    klvk_pass_barriers() == 2 ? " — held back until a pipeline reads it (KL_VK_PASS_BARRIERS=2)" : "");
            for (uint32_t i = 0; i < ibc; i++) {
                VkFormat f = 0; uint32_t w = 0, h = 0, mw = 0, mh = 0;
                klvk_img_info_get(ib[i].image, &f, &w, &h);
                fprintf(stderr, "  [vk]   image %u: format %d %ux%u%s, aspect %#x, access %#x -> %#x\n", i,
                        (int)f, w, h, klvk_msaa_img_size(ib[i].image, &mw, &mh) ? " multisampled" : "",
                        ib[i].subresourceRange.aspectMask, ib[i].srcAccessMask, ib[i].dstAccessMask);
            }
        }
        // Only the case that is understood is dropped: nothing but colour images
        // being made readable as input attachments. A depth attachment read back
        // cannot be fetched from the tile and does need the pass split.
        int fetchable = !mbc && !bbc && ibc;
        for (uint32_t i = 0; i < ibc && fetchable; i++)
            if (ib[i].subresourceRange.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
                !(ib[i].dstAccessMask & VK_ACCESS_INPUT_ATTACHMENT_READ_BIT)) fetchable = 0;
        int mode = klvk_pass_barriers();
        if (mode == 0 && fetchable) {
            __atomic_add_fetch(&g_prof_pass_barriers_dropped, 1, __ATOMIC_RELAXED);
            return;
        }
        // Held back only when this thread KNOWS the bound pipeline reads no
        // subpass input; anything else is the case the barrier exists for.
        if (mode == 2 && fetchable && ibc <= 4 && t_bind_cb == cb && !t_bind_reads) {
            klvk_held_barrier_flush(cb);
            t_held.cb = cb; t_held.ss = ss; t_held.ds = ds; t_held.df = df; t_held.n = ibc;
            memcpy(t_held.ib, ib, ibc * sizeof *ib);
            return;
        }
    }
    int bad = 0;
    for (uint32_t i = 0; i < ibc; i++) if (KLVK_IMG_BAD(ib[i].image)) { bad = 1; break; }
    if (!bad) { real(cb, ss, ds, df, mbc, mb, bbc, bb, ibc, ib); return; }
    // A wild handle here is AC Nexus's menu/composite render target, which also
    // reaches vkCreateImageView and vkCmdCopyImage - both now SUBSTITUTE the
    // scratch for it. Its layout transitions must land on that SAME scratch or
    // the substituted image is used as a colour attachment while still in
    // UNDEFINED layout: MoltenVK then renders nothing and the guest's own
    // CommandBuffer reports "temporary render texture not found", which is the
    // menu never appearing. So REWRITE the barrier onto the scratch rather than
    // DROP it - the whole lifecycle of the wild handle stays on one real image.
    VkImageMemoryBarrier *rew = calloc(ibc ? ibc : 1, sizeof *rew);
    uint32_t redir = 0;
    if (rew) for (uint32_t i = 0; i < ibc; i++) {
        rew[i] = ib[i];
        if (KLVK_IMG_BAD(ib[i].image)) { rew[i].image = klvk_sub_wild(ib[i].image); redir++; }
    }
    static int said; if (said < 8) { said++;
        VKI("vkCmdPipelineBarrier: REROUTED %u of %u image barriers onto the "
            "scratch so the substituted render target transitions with it\n",
            redir, ibc); }
    real(cb, ss, ds, df, mbc, mb, bbc, bb, ibc, rew ? rew : ib);
    free(rew);
}

// A thin pass-through over MoltenVK's vkCreateImage that LOGS the handle it
// hands back and the RESULT. A guest that ignores a failed create keeps an
// uninitialised VkImage variable and later feeds it to vkCreateImageView, where
// MoltenVK dereferences the garbage and faults - so the question "was this image
// ever really created, and did the call succeed" is answered here. Our own
// klvk_image_alloc uses d->CreateImage (the real entry) and never reaches this,
// so this only ever sees the GUEST's images.
static int klvk_is_astc(VkFormat f);   // defined below (near format-properties)
// KL_VK_ASTC_RGBA8 test: hash set of images substituted ASTC->RGBA8, so
// CmdCopyBufferToImage can skip the (overrunning) ASTC upload and clear to green
// instead. Open-addressed, VkImage keys; sized for olar's ~500-2000 ASTC images.
#define KLVK_ASTC_SUB_N 4096
static VkImage g_astc_sub[KLVK_ASTC_SUB_N];
static pthread_mutex_t g_astc_sub_lock = PTHREAD_MUTEX_INITIALIZER;
static void klvk_astc_sub_add(VkImage img) {
    if (!img) return;
    uint64_t h = ((uint64_t)(uintptr_t)img >> 6) & (KLVK_ASTC_SUB_N - 1);
    pthread_mutex_lock(&g_astc_sub_lock);
    for (int i = 0; i < KLVK_ASTC_SUB_N; i++) {
        uint64_t s = (h + i) & (KLVK_ASTC_SUB_N - 1);
        if (!g_astc_sub[s] || g_astc_sub[s] == img) { g_astc_sub[s] = img; break; }
    }
    pthread_mutex_unlock(&g_astc_sub_lock);
}
static int klvk_astc_sub_has(VkImage img) {
    if (!img) return 0;
    uint64_t h = ((uint64_t)(uintptr_t)img >> 6) & (KLVK_ASTC_SUB_N - 1);
    int found = 0;
    pthread_mutex_lock(&g_astc_sub_lock);
    for (int i = 0; i < KLVK_ASTC_SUB_N; i++) {
        uint64_t s = (h + i) & (KLVK_ASTC_SUB_N - 1);
        if (!g_astc_sub[s]) break;
        if (g_astc_sub[s] == img) { found = 1; break; }
    }
    pthread_mutex_unlock(&g_astc_sub_lock);
    return found;
}
// KL_VK_FORCE_SAMPLES=4: every multisampled thing the guest makes with fewer
// samples is made with this many — its images, the attachments of its render
// passes and its pipelines, which are the three places a sample count is
// stated and have to agree. A guest picks its count from its own settings (an
// Unreal title: r.MobileMSAA in a device profile inside its paks), and where
// those cannot be reached this is how the picture gets smoother edges anyway.
// Single-sampled things are never touched, so what is resolved into, sampled
// and presented stays as it was. Marvel's Deadpool VR renders 2x; its settings
// are in an encrypted pak, and an Engine.ini beside its saved game asking for
// 4x changed nothing (2026-10-12). Unset or 0: off. Only 4 is taken — it is
// the most this GPU offers.
//
// The setting in the start window is its default (kl_vulkan_set_force_samples_default),
// offered there for a game seen to multisample with fewer — which is what
// kl_vulkan_guest_samples is for: the count the guest itself asked for on its
// large targets, before anything here raised it.
static int g_force_samples_default;
void kl_vulkan_set_force_samples_default(int samples) {
    g_force_samples_default = samples == 4 ? 4 : 0;
}
//
// Never past KL_VK_MAX_SAMPLES: a game that is told the device offers two is
// told so because four did not fit (AC Nexus), and it is not offered the
// setting either — kl_vulkan_guest_samples answers 0 for it.
static uint32_t klvk_sample_mask(void);
static int g_guest_samples;
int kl_vulkan_guest_samples(void) {
    return klvk_sample_mask() == ~0u ? __atomic_load_n(&g_guest_samples, __ATOMIC_RELAXED) : 0;
}
static VkSampleCountFlagBits klvk_raise_samples(VkSampleCountFlagBits s) {
    static int v = -1;
    if (v < 0) {
        v = kl_env_int("KL_VK_FORCE_SAMPLES", g_force_samples_default);
        if (v != 4 || klvk_sample_mask() != ~0u) v = 0;
        if (v) VKI("KL_VK_FORCE_SAMPLES=%d: the guest's multisampled images, render passes "
                   "and pipelines are made with %d samples\n", v, v);
    }
    return v && (unsigned)s > 1 && (unsigned)s < (unsigned)v ? (VkSampleCountFlagBits)v : s;
}

// KL_VK_MSAA_MEMORYLESS=1 (a trial, Half-Life: Alyx) — the multisampled DEPTH
// target of the scene pass in Metal memoryless storage.
//
// The engine's scene pass clears its multisampled depth, resolves it as the
// pass ends and stores none of it: the samples are needed inside that one pass
// and nowhere else, and on a tile-based GPU they can live in tile memory for
// its length. As an ordinary image they are 2327x2218 x 2 eyes x 4 samples of
// 32-bit depth held for nothing — about 165 MB at "Balanced".
//
// The way is Glovebox's (github.com/glovebox-vr/glovebox, kl_vulkan.c,
// KL_VK_MSAA_MEMORYLESS, MIT): the image is created as a TRANSIENT attachment
// and nothing else, and the allocation the engine then makes for it — not a
// dedicated one: a block of exactly the size it was just told — is moved to
// MoltenVK's LAZILY_ALLOCATED memory type, which is MTLStorageModeMemoryless.
//
// Depth only, where Glovebox does colour as well. The colour samples of that
// pass are stored here on purpose (klvk_widen_area: what the exposure meter
// reads), and a memoryless texture cannot be stored. A memoryless attachment
// also must never be loaded or stored by any pass; this engine's frame has one
// multisampled pass, and kl_mtl_foveate.m makes any other legal and counts it.
static int klvk_ml_on(void) {
    static int v = -1;
    if (v < 0) v = kl_env_on("KL_VK_MSAA_MEMORYLESS", 0);
    return v;
}
#define KLVK_ML_MAX 16
static struct { VkImage img; VkDeviceSize size; uint32_t bits; int open; } g_ml[KLVK_ML_MAX];
static VkDeviceMemory g_ml_mem[KLVK_ML_MAX];
static int g_ml_n, g_ml_nmem;
static pthread_mutex_t g_ml_mu = PTHREAD_MUTEX_INITIALIZER;
static int klvk_ml_find(VkImage img) {          // under g_ml_mu
    for (int i = 0; i < g_ml_n; i++) if (g_ml[i].img == img) return i;
    return -1;
}
static int klvk_ml_lazy_type(VkDevice dev, uint32_t bits) {
    static PFN_vkGetPhysicalDeviceMemoryProperties gmp;
    if (!gmp && real_gipa)
        gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
    klvk_device *pd = dev_find(dev);
    if (!pd || !gmp) return -1;
    VkPhysicalDeviceMemoryProperties mp;
    gmp(pd->phys, &mp);
    static int said;
    if (!said++)
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            fprintf(stderr, "  [vk] memoryless: the device's memory type %u has properties %#x%s\n", i,
                    mp.memoryTypes[i].propertyFlags,
                    (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? " (lazily allocated)" : "");
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) && ((bits >> i) & 1))
            return (int)i;
    return -1;
}

static VkResult VKAPI_CALL klvk_CreateImage(VkDevice dev, const VkImageCreateInfo *ci,
        const VkAllocationCallbacks *alloc, VkImage *out) {
    KFE(KFE_IMG_NEW);
    static PFN_vkCreateImage real;
    if (!real && real_gdpa) real = (PFN_vkCreateImage)real_gdpa(dev, "vkCreateImage");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    int did_astc_sub = 0;
    VkImageCreateInfo mod;
    // KL_VK_MSAA_MEMORYLESS: a large multisampled depth target becomes a
    // transient attachment and nothing else (see klvk_ml_on).
    // A depth-stencil image can name a usage of its own for the stencil aspect
    // (VkImageStencilUsageCreateInfo in the chain), and MoltenVK decides by the
    // two together: an INPUT_ATTACHMENT bit in either keeps the image out of
    // memoryless storage (MVKImage::getMemoryRequirements, v1.4.2). The engine's
    // carries one — the first headset run with only `usage` changed was told
    // "memory types 0x1", real memory only (2026-10-13). So that struct is
    // given the same usage for the length of the call and put back after it.
    int ml_pending = 0;
    VkImageUsageFlags ml_was = 0, ml_stencil_was = 0;
    VkImageStencilUsageCreateInfo *ml_stencil = NULL;
    if (ci && (unsigned)ci->samples > 1 && (ci->usage & 0x20u) && !(ci->usage & 0x10u) &&
        ci->extent.width >= 1024 && ci->extent.height >= 1024 && klvk_ml_on()) {
        mod = *ci;
        ml_was = mod.usage;
        mod.usage = 0x20u | 0x40u;       // DEPTH_STENCIL_ATTACHMENT | TRANSIENT_ATTACHMENT
        for (const VkBaseInStructure *b = (const VkBaseInStructure *)mod.pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO) {
                ml_stencil = (VkImageStencilUsageCreateInfo *)(uintptr_t)b;
                ml_stencil_was = ml_stencil->stencilUsage;
                ml_stencil->stencilUsage = mod.usage;
            }
        ci = &mod;
        ml_pending = 1;
    }
    if (ci && (ci->usage & 0x80u) && kl_env_on("KL_STRIP_INPUT_ATT", 0)) {
        // No render pass in this guest uses an input attachment (census: inputAtts 0),
        // yet its temp render targets carry INPUT_ATTACHMENT usage — which makes
        // MoltenVK back them as memoryless/tile storage, so Unity's own temp-RT
        // bookkeeping cannot bind them ("temporary render texture not found") and
        // the menu compositing pass is skipped. Drop the unused bit so the RT is a
        // plain, fully-backed colour/depth target.
        mod = *ci;
        mod.usage &= ~0x80u;   // VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
        static int said; if (said < 8) { said++;
            VKI("vkCreateImage: stripped INPUT_ATTACHMENT usage %#x -> %#x (%ux%u fmt %d)\n",
                ci->usage, mod.usage, ci->extent.width, ci->extent.height, (int)ci->format); }
        ci = &mod;
    }
    // KL_VK_NO_TRANSIENT=0|1 — create TRANSIENT_ATTACHMENT images as ordinary
    // ones. A transient image may be bound to MoltenVK's lazily allocated memory
    // type, which is Metal memoryless storage: it lives in tile memory for one
    // Metal render pass and is gone when that pass ends. On a Quest the Vulkan
    // render pass is the unit, but MoltenVK ends its Metal pass and opens another
    // wherever it must (KL_VK_PASS_BARRIERS), and a memoryless attachment cannot
    // be stored across that seam — the second pass reads undefined tile memory.
    // Skydance's BEHEMOTH (UE5 mobile, 2x MSAA, a subpass-input reader) drew its
    // whole picture in 32x32 tiles, each one either all there or all missing:
    // depth, per Apple GPU tile, was whatever the tile held (2026-10-11, frame
    // captures). Without the bit MoltenVK offers only real memory for the image,
    // so what a split pass stores is there when it loads. The cost is the
    // memory a tiler did not need — the multisampled scene targets at eye size.
    // Into The Radius 2 (x6605328856182465, UE 5.5 mobile, 4x MSAA, a
    // subpass-input reader) the same way: from its main menu on, both eyes
    // showed the picture in blocks of garbage — other passes' contents, noise —
    // different per eye and per frame, read as flicker even holding still
    // (2026-10-20, eye captures). With the bit dropped (its 3840x2464 two-layer
    // colour and depth) the menu was clean on the headset, 4.7 GB still free.
    // Roboquest VR, Meta Store build (x9269393463181825, UE 5.6 mobile, 4x
    // MSAA, a subpass-input reader): the scene black but for glimpses of
    // models, the UI drawn over it (on device, 2026-10-23; the same picture as
    // its other build, x3151760, on 2026-10-22).
    //   default 1 for those three titles, else 0
    static int no_transient = -1;
    if (no_transient < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        no_transient = kl_env_on("KL_VK_NO_TRANSIENT",
                                 t && (!strcmp(t, "x5767635983306831") ||
                                       !strcmp(t, "x6605328856182465") ||
                                       !strcmp(t, "x9269393463181825")));
    }
    if (ci && (ci->usage & 0x40u) && no_transient) {
        mod = *ci;
        mod.usage &= ~0x40u;   // VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT
        static int said; if (said < 8) { said++;
            VKI("vkCreateImage: TRANSIENT_ATTACHMENT dropped (KL_VK_NO_TRANSIENT) usage "
                "%#x -> %#x (%ux%u fmt %d samples %d layers %u)\n",
                ci->usage, mod.usage, ci->extent.width, ci->extent.height,
                (int)ci->format, (int)ci->samples, ci->arrayLayers); }
        ci = &mod;
    }
    // DECISIVE TEST (KL_VK_ASTC_RGBA8=1): create every ASTC image as RGBA8 instead.
    // The guest's ASTC upload then lands in an RGBA8 store (garbled bytes reinterpreted
    // as colour). If the gray WORLD turns GARBLED/COLOURFUL, the ASTC albedo was
    // reaching surfaces with valid UVs -> the gray was ASTC DECODE. If it stays gray,
    // ASTC is not the visible albedo. The matching view-format rewrite is in
    // klvk_CreateImageView (klvk_astc_sub_has). Diagnostic; off by default.
    if (ci && klvk_is_astc(ci->format) && kl_env_on("KL_VK_ASTC_RGBA8", 0)) {
        mod = *ci;
        mod.format = VK_FORMAT_R8G8B8A8_UNORM;
        // KEEP MUTABLE_FORMAT (flags 0x8): UE creates format-reinterpreted views on
        // these textures, and Metal asserts on a view unless the parent carries
        // pixelFormatView usage (which MoltenVK adds for MUTABLE images). Only drop
        // the format LIST (pNext) — it enumerates ASTC formats incompatible with the
        // RGBA8 store; a mutable image with no list allows any same-class view.
        mod.flags |= 0x8u;    // ensure MUTABLE so view creation is legal
        mod.pNext = NULL;
        ci = &mod;
        did_astc_sub = 1;
        static int said; if (said < 8) { said++;
            VKI("[test] ASTC fmt->RGBA8 substitute (%ux%u) — ASTC bytes will land as "
                "garbled colour; world colourful => gray was ASTC decode\n",
                ci->extent.width, ci->extent.height); }
    }
    // The guest's own count on an eye-sized target, for the start window.
    if (ci && (unsigned)ci->samples > 1 && ci->extent.width >= 1024 && ci->extent.height >= 1024 &&
        (int)ci->samples > __atomic_load_n(&g_guest_samples, __ATOMIC_RELAXED))
        __atomic_store_n(&g_guest_samples, (int)ci->samples, __ATOMIC_RELAXED);
    if (ci && klvk_raise_samples(ci->samples) != ci->samples) {
        if (ci != &mod) mod = *ci;
        mod.samples = klvk_raise_samples(mod.samples);
        ci = &mod;
    }
    VkResult r = real(dev, ci, alloc, out);
    if (ml_stencil) ml_stencil->stencilUsage = ml_stencil_was;
    if (ml_pending && r == VK_SUCCESS && out && *out) {
        pthread_mutex_lock(&g_ml_mu);
        if (g_ml_n < KLVK_ML_MAX) { g_ml[g_ml_n].img = *out; g_ml[g_ml_n].size = 0; g_ml[g_ml_n].open = 0; g_ml_n++; }
        pthread_mutex_unlock(&g_ml_mu);
        fprintf(stderr, "  [vk] memoryless: multisampled depth %ux%u x%u layer(s), %ux samples, format %d — usage "
                        "%#x -> %#x (transient)", ci->extent.width, ci->extent.height, ci->arrayLayers,
                (unsigned)ci->samples, (int)ci->format, ml_was, ci->usage);
        if (ml_stencil) fprintf(stderr, ", its stencil usage %#x -> %#x for the call", ml_stencil_was, ci->usage);
        fprintf(stderr, "\n");
    }
    // MoltenVK rejects some formats outright (Wanderer: VK_FORMAT_ASTC_8x8_UNORM/
    // SRGB come back VK_ERROR_FEATURE_NOT_PRESENT) and leaves *out NULL. A null
    // image is not inert: the guest does not check, and the next call on it —
    // vkGetImageMemoryRequirements, vkBindImageMemory, vkCreateImageView — walks
    // a null `this` inside MoltenVK and crashes a render worker. Substitute a
    // real image of the same extent in a guaranteed format so the guest gets a
    // valid handle; its contents will be wrong (the ASTC upload lands in an RGBA8
    // store) but that is a wrong texture, not a dead process.
    if ((r != VK_SUCCESS || (out && !*out)) && out && ci) {
        VkImageCreateInfo fb = *ci;
        fb.pNext  = NULL;              // drop any format-list / external-memory chain
        fb.format = VK_FORMAT_R8G8B8A8_UNORM;
        fb.flags &= ~0x8u;            // MUTABLE_FORMAT: a format list would be needed
        if (fb.usage == 0) fb.usage = 0x4;                 // SAMPLED
        fb.usage &= ~0x80u;           // drop INPUT_ATTACHMENT (memoryless) just in case
        VkResult r2 = real(dev, &fb, alloc, out);
        if ((r2 != VK_SUCCESS || !*out)) {                 // last resort: single mip
            fb.mipLevels = 1;
            r2 = real(dev, &fb, alloc, out);
        }
        static int said;
        if (r2 == VK_SUCCESS && *out) {
            if (said < 16) { said++;
                VKI("vkCreateImage: format %d rejected (result %d) — substituted "
                    "RGBA8 %ux%u mips %u so the guest gets a real handle (texture "
                    "will be wrong, but no null-handle crash downstream)\n",
                    (int)ci->format, (int)r, ci->extent.width, ci->extent.height,
                    fb.mipLevels);
                fflush(stderr); }
            r = VK_SUCCESS;
        } else if (said < 16) { said++;
            VKI("vkCreateImage: format %d rejected (result %d) AND the RGBA8 "
                "substitute also failed (result %d, %ux%u) — the null-handle crash "
                "will follow; needs a closer look\n",
                (int)ci->format, (int)r, (int)r2, ci->extent.width, ci->extent.height);
            fflush(stderr);
        }
    }
    if (r == VK_SUCCESS && out) klvk_img_reg_add(*out);
    if (r == VK_SUCCESS && out) fove_note_image(*out, ci);
    if (r == VK_SUCCESS && out && ci)
        klvk_img_info_add(*out, ci->format, ci->extent.width, ci->extent.height);
    if (r == VK_SUCCESS && out && ci && ci->extent.width <= 1 && ci->extent.height <= 1)
        klvk_handle_set_add(g_tiny_img, (uint64_t)(uintptr_t)*out);
    if (r == VK_SUCCESS && out && ci && (unsigned)ci->samples > 1) {
        klvk_msaa_img_add(*out, ci->extent.width, ci->extent.height);
        if (kl_env_on("KL_VK_WIDEN_TRACE", 0))
            fprintf(stderr, "  [vk] multisampled image %p: %ux%u x%u, %dx\n", (void *)*out,
                    ci->extent.width, ci->extent.height, ci->arrayLayers, (int)ci->samples);
    }
    if (r == VK_SUCCESS && out && ci && klvk_profile()) {
        klvk_prof_img_note(*out, (unsigned)ci->samples, ci->arrayLayers);
        klvk_cen_add(*out, ci);
    }
    if (r == VK_SUCCESS && out && *out && did_astc_sub) klvk_astc_sub_add(*out);
    if (klvk_trace() && ci && (ci->extent.width >= 480 || ci->extent.height >= 480)
        && (ci->usage & 0x30u))   // colour or depth attachment == a render target
        fprintf(stderr, "[TRACE] [thr] createRT %#llx %ux%u fmt %d usage %#x tid=%u\n",
            (unsigned long long)(uintptr_t)(out?*out:0), ci->extent.width, ci->extent.height,
            (int)ci->format, ci->usage, klvk_tid());
    static int logged;
    if (logged < 4096 && (r != VK_SUCCESS || (ci && ci->mipLevels > 1) || logged < 64)) {
        logged++;
        VKI("vkCreateImage: -> %#llx result %d type %d format %d %ux%u mips %u layers %u "
            "usage %#x flags %#x\n",
            (unsigned long long)(uintptr_t)(out ? *out : 0), (int)r,
            ci ? (int)ci->imageType : -1, ci ? (int)ci->format : -1,
            ci ? ci->extent.width : 0, ci ? ci->extent.height : 0,
            ci ? ci->mipLevels : 0, ci ? ci->arrayLayers : 0,
            ci ? ci->usage : 0, ci ? ci->flags : 0);
    }
    return r;
}

// vkGetImageMemoryRequirements with a guard for VK_NULL_HANDLE. When a
// vkCreateImage fails (Wanderer: VK_FORMAT_ASTC_8x8_UNORM_BLOCK comes back
// VK_ERROR_FEATURE_NOT_PRESENT from MoltenVK), UE5 does not check the result and
// asks the null image for its requirements; MoltenVK then dereferences the null
// this (MVKImage::getMemoryRequirements, fault at +0x20). Answer a small dummy
// requirement instead of forwarding the null, so a rejected format degrades to a
// broken texture rather than a hard crash on a render worker.
static void VKAPI_CALL klvk_GetImageMemoryRequirements(VkDevice dev, VkImage img,
        VkMemoryRequirements *mr) {
    static PFN_vkGetImageMemoryRequirements real;
    if (!real && real_gdpa)
        real = (PFN_vkGetImageMemoryRequirements)real_gdpa(dev, "vkGetImageMemoryRequirements");
    if (!img) {
        if (mr) { mr->size = 256; mr->alignment = 256; mr->memoryTypeBits = ~0u; }
        static int said;
        if (said < 8) { said++;
            VKI("vkGetImageMemoryRequirements(NULL image) — a prior vkCreateImage "
                "failed (unsupported format?); returning a dummy requirement rather "
                "than letting MoltenVK dereference the null handle\n"); }
        return;
    }
    if (real) real(dev, img, mr);
    // KL_VK_MSAA_MEMORYLESS: what the image needs is noted, and the next
    // allocation of exactly that size is the one moved (klvk_AllocateMemory).
    if (mr && g_ml_n && klvk_ml_on()) {
        pthread_mutex_lock(&g_ml_mu);
        int k = klvk_ml_find(img);
        pthread_mutex_unlock(&g_ml_mu);
        if (k >= 0) {
            int lazy = klvk_ml_lazy_type(dev, mr->memoryTypeBits);
            pthread_mutex_lock(&g_ml_mu);
            g_ml[k].size = mr->size; g_ml[k].bits = mr->memoryTypeBits; g_ml[k].open = lazy >= 0;
            pthread_mutex_unlock(&g_ml_mu);
            fprintf(stderr, "  [vk] memoryless: that image needs %.1f MB, memory types %#x — %s\n",
                    (double)mr->size / 1048576.0, mr->memoryTypeBits,
                    lazy >= 0 ? "the next allocation of that size goes to the lazily allocated type"
                              : "none of them is lazily allocated; it stays in real memory");
        }
    }
}

// A thin pass-through over MoltenVK's vkCreateImageView that LOGS the guest's
// request first. The guest (Unity's Vulkan renderer) makes these on the eye
// images this file hands out through kl_ovrp; when MoltenVK faults building one,
// the only way to see WHICH view it asked for - format, type, subresource - is
// here, because nothing else on the path records it. It forwards verbatim to the
// real entry point resolved from MoltenVK and changes no behaviour of its own.
// A valid stand-in for the guest's WILD image handles - AC Nexus's loading
// video texture (an Android-surface import that cannot exist on visionOS). The
// guest builds a vkCreateImageView on it every frame; refusing returned an
// error that aborted the guest's whole frame, so it never reached EndFrame4 and
// the menu never rendered. Handing back a view on THIS image instead lets the
// frame finish - the video samples/renders black (the real video is shown by
// the host loading skybox), and the wild pointer never reaches MoltenVK. Big
// enough (4096, 12 mips, 2 layers, mutable format) that any view the guest asks
// for fits, and any framebuffer it sizes to its own texture is <= this.
#define KLVK_SCRATCH_MIPS   12u
#define KLVK_SCRATCH_LAYERS 2u
static VkImage klvk_wild_scratch(void) {
    if (!g_wild_scratch) {
        klvk_device *d = g_devs[0];
        if (!d) return VK_NULL_HANDLE;
        g_wild_scratch = klvk_image_alloc(d, VK_FORMAT_R8G8B8A8_UNORM,
            4096, 4096, KLVK_SCRATCH_LAYERS, KLVK_SCRATCH_MIPS,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            NULL);
        if (g_wild_scratch)
            VKI("created 4096x4096 scratch image %#llx for wild-handle view "
                "substitution\n", (unsigned long long)(uintptr_t)g_wild_scratch);
    }
    return g_wild_scratch;
}

// Map a WILD (unregistered) image handle to the scratch image so a command
// referencing it succeeds against a real resource instead of being dropped.
// Registered handles pass through unchanged.
static VkImage klvk_sub_wild(VkImage img) {
    if (!img || klvk_img_reg_has(img)) return img;
    VkImage sc = klvk_wild_scratch();
    return sc ? sc : img;
}

static void VKAPI_CALL klvk_DestroyImage(VkDevice dev, VkImage img,
        const VkAllocationCallbacks *alloc) {
    KFE(KFE_IMG_DEL);
    static PFN_vkDestroyImage real;
    if (!real && real_gdpa) real = (PFN_vkDestroyImage)real_gdpa(dev, "vkDestroyImage");
    klvk_img_reg_del(img);
    klvk_img_info_del(img);
    klvk_shadow_forget(img);
    if (klvk_profile()) klvk_cen_del(img);
    klvk_msaa_img_del(img);
    fove_image_gone(img);
    if (real) real(dev, img, alloc);
}

static VkResult VKAPI_CALL klvk_CreateImageView(VkDevice dev,
        const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkImageView *out) {
    static PFN_vkCreateImageView real;
    if (!real && real_gdpa)
        real = (PFN_vkCreateImageView)real_gdpa(dev, "vkCreateImageView");
    if (ci) {
        static int logged;
        if (logged < 4096) {   // high enough to capture the crashing view
            logged++;
            const VkImageSubresourceRange *r = &ci->subresourceRange;
            VKI("vkCreateImageView: image %#llx viewType %d format %d aspect %#x "
                "mips %u+%u layers %u+%u createFlags %#x\n",
                (unsigned long long)(uintptr_t)ci->image, (int)ci->viewType,
                (int)ci->format, r->aspectMask, r->baseMipLevel, r->levelCount,
                r->baseArrayLayer, r->layerCount, ci->flags);
        }
    }
    if (ci && ci->image && !klvk_img_reg_ok(ci->image)) {
        VkImage scratch = klvk_wild_scratch();
        if (scratch && real) {
            VkImageViewCreateInfo sub = *ci;
            sub.image = scratch;
            VkImageSubresourceRange *r = &sub.subresourceRange;
            if (r->baseMipLevel >= KLVK_SCRATCH_MIPS) r->baseMipLevel = 0;
            if (r->levelCount != VK_REMAINING_MIP_LEVELS &&
                r->baseMipLevel + r->levelCount > KLVK_SCRATCH_MIPS)
                r->levelCount = KLVK_SCRATCH_MIPS - r->baseMipLevel;
            if (r->baseArrayLayer >= KLVK_SCRATCH_LAYERS) r->baseArrayLayer = 0;
            if (r->layerCount != VK_REMAINING_ARRAY_LAYERS &&
                r->baseArrayLayer + r->layerCount > KLVK_SCRATCH_LAYERS)
                r->layerCount = KLVK_SCRATCH_LAYERS - r->baseArrayLayer;
            static int subbed;
            if (subbed < 16) {
                subbed++;
                VKI("vkCreateImageView: SUBSTITUTING scratch for wild handle "
                    "%#llx (viewType %d format %d) so the frame can complete\n",
                    (unsigned long long)(uintptr_t)ci->image, (int)ci->viewType,
                    (int)ci->format);
            }
            VkResult rr = real(dev, &sub, alloc, out);
            if (rr != VK_SUCCESS && out) *out = VK_NULL_HANDLE;
            else if (out) klvk_view_map_add(*out, ci->image);
            return rr;
        }
        VKI("vkCreateImageView: REFUSING a view on image %#llx - no create ever\n"
            "        produced that handle, and no scratch to substitute. "
            "viewType %d format %d mips %u+%u\n",
            (unsigned long long)(uintptr_t)ci->image, (int)ci->viewType,
            (int)ci->format, ci->subresourceRange.baseMipLevel,
            ci->subresourceRange.levelCount);
        if (out) *out = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    // KL_VK_ASTC_RGBA8 test: every ASTC image was created RGBA8, so ANY ASTC-format
    // view must be rewritten to match the store or Metal asserts. Keyed on the flag +
    // the requested format (not per-image tracking, which overflowed — olar has 500+
    // ASTC images).
    VkImageViewCreateInfo ivmod;
    if (ci && klvk_is_astc(ci->format) && kl_env_on("KL_VK_ASTC_RGBA8", 0)) {
        ivmod = *ci; ivmod.format = VK_FORMAT_R8G8B8A8_UNORM; ci = &ivmod;
    }
    VkResult klvk_ivr = real(dev, ci, alloc, out);
    if (klvk_ivr==VK_SUCCESS && out && ci) klvk_view_map_add(*out, ci->image);
    if (klvk_ivr==VK_SUCCESS && out && ci) klvk_msaa_view_note(*out, ci->image);
    if (klvk_ivr==VK_SUCCESS && out && ci && klvk_handle_set_has(g_tiny_img, (uint64_t)(uintptr_t)ci->image))
        klvk_handle_set_add(g_tiny_view, (uint64_t)(uintptr_t)*out);
    if (klvk_ivr==VK_SUCCESS && out && ci) fove_note_view(*out, ci);
    return klvk_ivr;
}

// ---------------------------------------------------------------------------
// Pipeline-compile instrumentation. The guest's ShaderWarmupManager compiles its
// whole variant collection here, and on this stack every one goes through
// MoltenVK's SPIR-V->Metal compiler, which is far slower than a native driver.
// From the outside a warmup that is grinding and one that is wedged look
// identical - a frozen loading screen - so this counts and TIMES the compiles
// and logs a running total. A steadily climbing count is "slow, be patient";
// a count that stops while the app is still alive is "wedged, look here".
// Gated on nothing: it is one fprintf per ~50 pipelines, negligible against a
// compile measured in tens of milliseconds.
static unsigned long long g_pipe_gfx, g_pipe_comp, g_pipe_ns;
static double kl_pipe_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static void kl_pipe_report(const char *kind, unsigned n, double dt_ms) {
    g_pipe_ns += (unsigned long long)(dt_ms * 1e6);
    unsigned long long tot = g_pipe_gfx + g_pipe_comp;
    static unsigned long long last;
    if (tot - last >= 50 || dt_ms > 250.0) {
        last = tot;
        VKI("pipeline warmup: %llu graphics + %llu compute = %llu compiled, "
            "%.1f s in the compiler so far (last %s batch of %u took %.0f ms)\n",
            g_pipe_gfx, g_pipe_comp, tot, (double)g_pipe_ns / 1e9, kind, n, dt_ms);
    }
}
static VkResult VKAPI_CALL klvk_CreateGraphicsPipelines0(VkDevice dev,
        VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
        const VkAllocationCallbacks *alloc, VkPipeline *out);
// The dynamic-vertex-input emulation, defined after the async-pipeline block it borrows from.
static int klvk_vi_on(void);
static int klvk_vi_dynamic(const VkGraphicsPipelineCreateInfo *ci);
static VkResult klvk_vi_defer(VkDevice dev, const VkGraphicsPipelineCreateInfo *ci, VkPipeline *out);
static unsigned g_subpass_pipes;
static VkResult klvk_ap_create(VkDevice dev, VkPipelineCache cache, uint32_t n,
        const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *alloc, VkPipeline *out);
static VkResult VKAPI_CALL klvk_CreateGraphicsPipelines(VkDevice dev,
        VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
        const VkAllocationCallbacks *alloc, VkPipeline *out) {
    if (t_pipe_replay) return klvk_CreateGraphicsPipelines0(dev, cache, n, ci, alloc, out);
    KFE(KFE_PIPE);
    double rec_t0 = kl_pipe_ms();
    VkResult r = klvk_ap_create(dev, cache, n, ci, alloc, out);
    // KL_VK_PIPE_REC=<file>: one line per graphics pipeline the guest asks for —
    // its shaders by the hash of their SPIR-V, what it renders into and how long
    // the compile took. What a shipped pipeline database (Fossilize) has to hold
    // for a replay of it to spare the guest these compiles.
    {
        static FILE *rec; static int tried;
        static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
        if (!tried) { tried = 1; const char *f = kl_env_str("KL_VK_PIPE_REC", NULL);
                      if (f && *f) rec = fopen(f, "w"); }
        if (rec && ci) {
            double ms = kl_pipe_ms() - rec_t0;
            klvk_fs_on();
            pthread_mutex_lock(&mu);
            for (uint32_t i = 0; i < n; i++) {
                fprintf(rec, "gfx ms=%.1f rp=%d", ms / n, ci[i].renderPass ? 1 : 0);
                for (uint32_t k = 0; k < ci[i].stageCount && ci[i].pStages; k++) {
                    const VkPipelineShaderStageCreateInfo *st = &ci[i].pStages[k];
                    uint32_t h = klvk_fs_get(g_fs_mod, (uint64_t)(uintptr_t)st->module);
                    for (const VkBaseInStructure *x = st->pNext; x && !h; x = x->pNext)
                        if (x->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                            const VkShaderModuleCreateInfo *m = (const VkShaderModuleCreateInfo *)x;
                            if (m->pCode) h = klvk_fnv(m->pCode, m->codeSize);
                        }
                    fprintf(rec, " s%x=%08x", (unsigned)st->stage, h);
                }
                if (ci[i].pMultisampleState) fprintf(rec, " samples=%d", (int)ci[i].pMultisampleState->rasterizationSamples);
                if (ci[i].pInputAssemblyState) fprintf(rec, " topo=%d", (int)ci[i].pInputAssemblyState->topology);
                for (const VkBaseInStructure *x = ci[i].pNext; x; x = x->pNext)
                    if (x->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
                        const VkPipelineRenderingCreateInfo *ri = (const VkPipelineRenderingCreateInfo *)x;
                        fprintf(rec, " views=%#x color=", ri->viewMask);
                        for (uint32_t c = 0; c < ri->colorAttachmentCount; c++)
                            fprintf(rec, "%s%d", c ? "," : "", (int)ri->pColorAttachmentFormats[c]);
                        fprintf(rec, " depth=%d stencil=%d", (int)ri->depthAttachmentFormat, (int)ri->stencilAttachmentFormat);
                    }
                fprintf(rec, "\n");
            }
            fflush(rec);
            pthread_mutex_unlock(&mu);
        }
    }
    // Which of them read a subpass input (see klvk_pass_barriers): by the
    // module, or by the SPIR-V chained to the stage where there is no module.
    for (uint32_t i = 0; ci && out && i < n && klvk_fs_on(); i++) {
    
        if (!out[i]) continue;
        for (uint32_t k = 0; k < ci[i].stageCount && ci[i].pStages; k++) {
            const VkPipelineShaderStageCreateInfo *st = &ci[i].pStages[k];
            if (st->stage != VK_SHADER_STAGE_FRAGMENT_BIT) continue;
            uint32_t h = klvk_fs_get(g_fs_mod, (uint64_t)(uintptr_t)st->module);
            for (const VkBaseInStructure *x = st->pNext; x && !h; x = x->pNext)
                if (x->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                    const VkShaderModuleCreateInfo *m = (const VkShaderModuleCreateInfo *)x;
                    if (m->pCode) h = klvk_fnv(m->pCode, m->codeSize);
                }
            if (h) klvk_fs_put(g_fs_pipe, (uint64_t)(uintptr_t)out[i], h);
        }
    }
    for (uint32_t i = 0; ci && out && i < n; i++) {
        if (!out[i]) continue;
        int reads = 0;
        for (uint32_t k = 0; k < ci[i].stageCount && ci[i].pStages && !reads; k++) {
            const VkPipelineShaderStageCreateInfo *st = &ci[i].pStages[k];
            if (klvk_handle_set_has(g_subpass_mod, (uint64_t)(uintptr_t)st->module)) reads = 1;
            for (const VkBaseInStructure *x = st->pNext; x && !reads; x = x->pNext)
                if (x->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                    const VkShaderModuleCreateInfo *m = (const VkShaderModuleCreateInfo *)x;
                    if (!(m->codeSize & 3u) && klvk_spirv_reads_subpass(m->pCode, m->codeSize / 4)) reads = 1;
                }
        }
        if (reads) {
            // What such a pipeline may WRITE: a full-screen draw that reads the
            // scene and is meant only to be counted must leave it alone.
            static int told;
            if (told++ < 3) {
                const VkPipelineColorBlendStateCreateInfo *cb = ci[i].pColorBlendState;
                const VkPipelineDepthStencilStateCreateInfo *ds = ci[i].pDepthStencilState;
                const VkPipelineDynamicStateCreateInfo *dy = ci[i].pDynamicState;
                const VkPipelineMultisampleStateCreateInfo *ms = ci[i].pMultisampleState;
                fprintf(stderr, "  [vk] a pipeline that reads a subpass input: %u colour attachment(s)",
                        cb ? cb->attachmentCount : 0);
                for (uint32_t a = 0; cb && a < cb->attachmentCount && a < 4; a++)
                    fprintf(stderr, " [write mask %#x, blend %d]", cb->pAttachments[a].colorWriteMask,
                            (int)cb->pAttachments[a].blendEnable);
                if (ds) fprintf(stderr, "; depth test %d write %d op %d; stencil test %d (front fail %d pass "
                                        "%d dfail %d cmp %d wmask %#x ref %u)", (int)ds->depthTestEnable,
                                (int)ds->depthWriteEnable, (int)ds->depthCompareOp, (int)ds->stencilTestEnable,
                                (int)ds->front.failOp, (int)ds->front.passOp, (int)ds->front.depthFailOp,
                                (int)ds->front.compareOp, ds->front.writeMask, ds->front.reference);
                if (ms) fprintf(stderr, "; samples %d, sample shading %d (min %.2f), alpha-to-coverage %d, mask %s",
                                (int)ms->rasterizationSamples, (int)ms->sampleShadingEnable,
                                (double)ms->minSampleShading, (int)ms->alphaToCoverageEnable,
                                ms->pSampleMask ? "given" : "none");
                fprintf(stderr, "; dynamic states:");
                for (uint32_t d = 0; dy && d < dy->dynamicStateCount; d++) fprintf(stderr, " %d", (int)dy->pDynamicStates[d]);
                fprintf(stderr, "\n");
            }
            klvk_handle_set_add(g_subpass_pipe, (uint64_t)(uintptr_t)out[i]);
            unsigned c = __atomic_add_fetch(&g_subpass_pipes, 1, __ATOMIC_RELAXED);
            if (c == 1 || c == 10 || c == 100)
                fprintf(stderr, "  [vk] %u pipeline(s) so far read a subpass input\n", c);
        }
    }
    return r;
}
// ---------------------------------------------------------------------------
// KL_VK_VIN_FILL — a vertex shader input the pipeline does not supply.
//
// Unreal builds a pipeline's vertex input state from what the mesh's vertex
// declaration has AND the shader reads, so a shader that reads an attribute the
// declaration lacks gets a pipeline without it. Vulkan calls the read undefined
// and the drivers these games shipped on take it; Metal does not compile such a
// pipeline at all — "Vertex attribute N is not defined in the vertex
// descriptor", or "Function requires stage_in attributes but no descriptor was
// set" where none is supplied — and MoltenVK hands back
// VK_ERROR_INITIALIZATION_FAILED and no pipeline. Alien: Rogue Incursion
// (UE 5.3) has 23 of these among its first hundred and asks to quit over each.
//
// So a pipeline MoltenVK has just refused is looked at once more: if its vertex
// shader has inputs at locations the state leaves out, it is created again with
// those locations fed from one more binding — stride 0, which MoltenVK makes a
// constant step — over a buffer of zeroes of ours, bound to that binding
// whenever the pipeline is. The shader reads 0 there, which "undefined" allows.
// A pipeline that compiled is never touched. Default on for that title only.
//
// The same retry takes a second refusal of the same kind: an input the state
// DOES supply, as 32-bit floats, that the shader reads as int or uint — "Cannot
// convert attribute from MTLAttributeFormatFloat2 to int4 or uint4". Vulkan
// calls that undefined too; a Quest's fetch hands the shader the stored bits.
// Here the attribute is declared with the integer format of the same size,
// which hands it exactly those bits. Arizona Sunshine 2 (Unity 2022.3,
// x5245041552210029) had some two thousand of these in a session, in bursts
// as new things came into view — each a Metal compile thrown away and a draw
// left out — and is the second title it is on for by default. This half has a
// switch of its own, KL_VK_VIN_INT (default on for that title only), so Alien's
// retry stays what was measured there: a pipeline refused for this reason is
// still left out for it rather than drawn with bits nobody has looked at.
#define KLVK_VIN_SLOTS 8192u
#define KLVK_VIN_MAXB  16u
// By shader module: the input locations it reads as float / int / uint.
// By pipeline: f is the binding its zeroes are on.
typedef struct { uint64_t key; uint32_t f, i, u; } klvk_vin_ent;
static klvk_vin_ent g_vin_mod[KLVK_VIN_SLOTS], g_vin_pipe[KLVK_VIN_SLOTS];
static pthread_mutex_t g_vin_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_vin_pipes;        // pipelines made this way; 0 keeps the bind hooks out of it
static int klvk_vin_on(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        // HITMAN 3 VR: Reloaded's title screen has one (in_pos at location 2 of a
        // two-attribute layout set with vkCmdSetVertexInputEXT, Mac 2026-10-20).
        // Roboquest VR, Meta Store build (UE 5.6, x9269393463181825): 70 of the
        // first ~3,700 pipelines of its start-up precompile, all "Vertex
        // attribute N is not defined in the vertex descriptor". Unreal asks to
        // quit over the first one from inside the lock its compile workers
        // share, and the "compiling shaders" screen then waits forever (on
        // device, 2026-10-23).
        on = kl_env_on("KL_VK_VIN_FILL", t && (!strcmp(t, "x24882487241367228") ||
                                              !strcmp(t, "x5245041552210029") ||
                                              !strcmp(t, "x8012387035448196") ||
                                              !strcmp(t, "x9269393463181825")));
    }
    return on;
}
// The float-read-as-integer half of the retry (only asked once KL_VK_VIN_FILL is on).
static int klvk_vin_int_on(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_VK_VIN_INT", t && !strcmp(t, "x5245041552210029"));
    }
    return on;
}
// Open addressing; a key of 1 is a slot whose entry was removed.
static void klvk_vin_put(klvk_vin_ent *tab, uint64_t key, uint32_t f, uint32_t i, uint32_t u) {
    if (key < 2) return;
    uint32_t s = (uint32_t)((key >> 4) * 2654435761u) % KLVK_VIN_SLOTS, at = UINT32_MAX;
    pthread_mutex_lock(&g_vin_mu);
    for (uint32_t n = 0; n < KLVK_VIN_SLOTS; n++, s = (s + 1) % KLVK_VIN_SLOTS) {
        if (tab[s].key == key) { at = s; break; }
        if (tab[s].key == 1 && at == UINT32_MAX) at = s;
        if (!tab[s].key) { if (at == UINT32_MAX) at = s; break; }
    }
    if (at != UINT32_MAX) tab[at] = (klvk_vin_ent){ key, f, i, u };
    pthread_mutex_unlock(&g_vin_mu);
}
static int klvk_vin_get(const klvk_vin_ent *tab, uint64_t key, klvk_vin_ent *out) {
    if (key < 2) return 0;
    int hit = 0;
    uint32_t s = (uint32_t)((key >> 4) * 2654435761u) % KLVK_VIN_SLOTS;
    pthread_mutex_lock(&g_vin_mu);
    for (uint32_t n = 0; n < KLVK_VIN_SLOTS; n++, s = (s + 1) % KLVK_VIN_SLOTS) {
        if (tab[s].key == key) { *out = tab[s]; hit = 1; break; }
        if (!tab[s].key) break;
    }
    pthread_mutex_unlock(&g_vin_mu);
    return hit;
}
static void klvk_vin_del(klvk_vin_ent *tab, uint64_t key) {
    if (key < 2) return;
    uint32_t s = (uint32_t)((key >> 4) * 2654435761u) % KLVK_VIN_SLOTS;
    pthread_mutex_lock(&g_vin_mu);
    for (uint32_t n = 0; n < KLVK_VIN_SLOTS; n++, s = (s + 1) % KLVK_VIN_SLOTS) {
        if (tab[s].key == key) { tab[s].key = 1; break; }
        if (!tab[s].key) break;
    }
    pthread_mutex_unlock(&g_vin_mu);
}
// The input locations of a vertex shader, by the base type it reads each as.
// 0 for a module that is not a vertex shader or has no such input. Arrays and
// structs are left out: a pipeline short of one of those stays refused.
static int klvk_spirv_vertex_inputs(const uint32_t *code, size_t nwords, klvk_vin_ent *out) {
    memset(out, 0, sizeof *out);
    if (!code || nwords < 6 || code[0] != 0x07230203u) return 0;
    uint32_t bound = code[3];
    if (!bound || bound > (1u << 22)) return 0;
    // Per id: the Location decorated on it (+1); for a type its base kind
    // (1 float, 2 int, 3 uint) and how many locations it takes; for a pointer
    // into the Input storage class, what it points at.
    struct { uint32_t loc, ptr; uint8_t kind, cols, builtin; } *id = calloc(bound, sizeof *id);
    if (!id) return 0;
    int vertex = 0;
#define KLVK_ID(x) ((x) < bound)
    for (size_t i = 5; i < nwords; ) {
        uint32_t len = code[i] >> 16, op = code[i] & 0xffffu;
        if (!len || i + len > nwords || op == 54 /* OpFunction */) break;
        const uint32_t *w = &code[i];
        if (op == 15 /* OpEntryPoint */ && len >= 3) {
            if (w[1] == 0 /* Vertex */) vertex = 1;
        } else if (op == 71 /* OpDecorate */ && len >= 3 && KLVK_ID(w[1])) {
            if (w[2] == 30 /* Location */ && len >= 4) id[w[1]].loc = w[3] + 1;
            if (w[2] == 11 /* BuiltIn */) id[w[1]].builtin = 1;
        } else if (op == 21 /* OpTypeInt */ && len >= 4 && KLVK_ID(w[1])) {
            id[w[1]].kind = w[3] ? 2 : 3; id[w[1]].cols = 1;
        } else if (op == 22 /* OpTypeFloat */ && len >= 3 && KLVK_ID(w[1])) {
            id[w[1]].kind = 1; id[w[1]].cols = 1;
        } else if (op == 23 /* OpTypeVector */ && len >= 4 && KLVK_ID(w[1]) && KLVK_ID(w[2])) {
            id[w[1]].kind = id[w[2]].kind; id[w[1]].cols = 1;
        } else if (op == 24 /* OpTypeMatrix */ && len >= 4 && KLVK_ID(w[1]) && KLVK_ID(w[2])) {
            id[w[1]].kind = id[w[2]].kind; id[w[1]].cols = w[3] <= 4 ? (uint8_t)w[3] : 0;
        } else if (op == 32 /* OpTypePointer */ && len >= 4 && KLVK_ID(w[1]) && KLVK_ID(w[3])) {
            if (w[2] == 1 /* Input */) id[w[1]].ptr = w[3];
        } else if (op == 59 /* OpVariable */ && len >= 4 && KLVK_ID(w[1]) && KLVK_ID(w[2]) && w[3] == 1 /* Input */) {
            uint32_t t = id[w[1]].ptr, loc = id[w[2]].loc;
            if (t && loc && !id[w[2]].builtin && id[t].kind)
                for (uint32_t c = 0; c < id[t].cols && loc - 1 + c < 32; c++) {
                    uint32_t bit = 1u << (loc - 1 + c);
                    if (id[t].kind == 1) out->f |= bit; else if (id[t].kind == 2) out->i |= bit; else out->u |= bit;
                }
        }
        i += len;
    }
#undef KLVK_ID
    free(id);
    return vertex && (out->f | out->i | out->u);
}
static VkBuffer klvk_vin_zero(klvk_device *d) {
    if (!d) return VK_NULL_HANDLE;
    pthread_mutex_lock(&g_vin_mu);
    if (!d->vin_buf && d->CreateBuffer && d->GetBufferMemoryRequirements && d->AllocateMemory &&
        d->BindBufferMemory && d->MapMemory && d->UnmapMemory && d->DestroyBuffer && d->FreeMemory) {
        VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64,
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE; void *p = NULL;
        if (d->CreateBuffer(d->dev, &bi, NULL, &b) == VK_SUCCESS && b) {
            VkMemoryRequirements mr; memset(&mr, 0, sizeof mr);
            d->GetBufferMemoryRequirements(d->dev, b, &mr);
            VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = mr.size,
                .memoryTypeIndex = mem_type(d, mr.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
            if (ai.memoryTypeIndex != UINT32_MAX &&
                d->AllocateMemory(d->dev, &ai, NULL, &m) == VK_SUCCESS &&
                d->BindBufferMemory(d->dev, b, m, 0) == VK_SUCCESS &&
                d->MapMemory(d->dev, m, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS && p) {
                memset(p, 0, (size_t)mr.size);
                d->UnmapMemory(d->dev, m);
                d->vin_buf = b; d->vin_mem = m;
            } else {
                d->DestroyBuffer(d->dev, b, NULL);
                if (m) d->FreeMemory(d->dev, m, NULL);
            }
        }
    }
    VkBuffer r = d->vin_buf;
    pthread_mutex_unlock(&g_vin_mu);
    return r;
}
// The integer format whose components are the same 32 bits as a float format's
// (VK_FORMAT_UNDEFINED for any other format): an attribute read as int/uint.
static VkFormat klvk_vin_int_format(VkFormat f, int sint) {
    switch (f) {
    case VK_FORMAT_R32_SFLOAT:          return sint ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
    case VK_FORMAT_R32G32_SFLOAT:       return sint ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT;
    case VK_FORMAT_R32G32B32_SFLOAT:    return sint ? VK_FORMAT_R32G32B32_SINT : VK_FORMAT_R32G32B32_UINT;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return sint ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
    default:                            return VK_FORMAT_UNDEFINED;
    }
}
// One pipeline MoltenVK refused, once more with the inputs it lacks.
static void klvk_vin_retry(PFN_vkCreateGraphicsPipelines real, VkDevice dev, VkPipelineCache cache,
        const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *alloc, VkPipeline *out) {
    static unsigned said;
    klvk_vin_ent in = {0};
    int known = 0;
    for (uint32_t k = 0; k < ci->stageCount && ci->pStages && !known; k++) {
        const VkPipelineShaderStageCreateInfo *st = &ci->pStages[k];
        if (st->stage != VK_SHADER_STAGE_VERTEX_BIT) continue;
        known = klvk_vin_get(g_vin_mod, (uint64_t)(uintptr_t)st->module, &in);
        for (const VkBaseInStructure *x = st->pNext; x && !known; x = x->pNext)
            if (x->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                const VkShaderModuleCreateInfo *m = (const VkShaderModuleCreateInfo *)x;
                if (m->pCode && !(m->codeSize & 3u))
                    known = klvk_spirv_vertex_inputs(m->pCode, m->codeSize / 4, &in);
            }
    }
    for (uint32_t k = 0; ci->pDynamicState && k < ci->pDynamicState->dynamicStateCount; k++)
        if (ci->pDynamicState->pDynamicStates[k] == VK_DYNAMIC_STATE_VERTEX_INPUT_EXT) known = 0;
    const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
    uint32_t nb = vi ? vi->vertexBindingDescriptionCount : 0, na = vi ? vi->vertexAttributeDescriptionCount : 0;
    uint32_t have = 0, slot = 0;
    for (uint32_t a = 0; a < na; a++)
        if (vi->pVertexAttributeDescriptions[a].location < 32) have |= 1u << vi->pVertexAttributeDescriptions[a].location;
    for (uint32_t b = 0; b < nb; b++)
        if (vi->pVertexBindingDescriptions[b].binding >= slot) slot = vi->pVertexBindingDescriptions[b].binding + 1;
    uint32_t reads = in.f | in.i | in.u, miss = reads & ~have, conv = 0;
    for (uint32_t a = 0; a < na && klvk_vin_int_on(); a++) {
        uint32_t loc = vi->pVertexAttributeDescriptions[a].location;
        if (loc < 32 && ((in.i | in.u) >> loc & 1) &&
            klvk_vin_int_format(vi->pVertexAttributeDescriptions[a].format, 1) != VK_FORMAT_UNDEFINED)
            conv |= 1u << loc;
    }
    if (!known || !(miss | conv) || (miss && slot >= KLVK_VIN_MAXB)) {
        if (__atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < 16)
            VKI("CreateGraphicsPipelines: a refused pipeline is not one KL_VK_VIN_FILL can help "
                "(vertex shader %s, reads locations %#x, pipeline supplies %#x over %u binding(s))\n",
                known ? "known" : "unknown", reads, have, nb);
        return;
    }
    VkBuffer zero = miss ? klvk_vin_zero(dev_find(dev)) : VK_NULL_HANDLE;
    VkVertexInputBindingDescription *bd = malloc((nb + 1) * sizeof *bd);
    VkVertexInputAttributeDescription *ad = malloc((na + 32) * sizeof *ad);
    if ((miss && !zero) || !bd || !ad) { free(bd); free(ad); return; }
    if (nb) memcpy(bd, vi->pVertexBindingDescriptions, nb * sizeof *bd);
    if (na) memcpy(ad, vi->pVertexAttributeDescriptions, na * sizeof *ad);
    for (uint32_t a = 0; a < na; a++)
        if (ad[a].location < 32 && (conv >> ad[a].location & 1))
            ad[a].format = klvk_vin_int_format(ad[a].format, (int)(in.i >> ad[a].location & 1));
    bd[nb] = (VkVertexInputBindingDescription){ .binding = slot, .stride = 0,
                                                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
    uint32_t n = na;
    for (uint32_t loc = 0; loc < 32; loc++)
        if (miss & (1u << loc))
            ad[n++] = (VkVertexInputAttributeDescription){ .location = loc, .binding = slot, .offset = 0,
                .format = (in.f >> loc & 1) ? VK_FORMAT_R32G32B32A32_SFLOAT
                        : (in.i >> loc & 1) ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT };
    VkPipelineVertexInputStateCreateInfo v2;
    if (vi) v2 = *vi;
    else { memset(&v2, 0, sizeof v2); v2.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO; }
    v2.vertexBindingDescriptionCount = nb + (miss ? 1 : 0);  v2.pVertexBindingDescriptions = bd;
    v2.vertexAttributeDescriptionCount = n;     v2.pVertexAttributeDescriptions = ad;
    VkGraphicsPipelineCreateInfo c = *ci;
    c.pVertexInputState = &v2;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = real(dev, cache, 1, &c, alloc, &p);
    free(bd); free(ad);
    if (r == VK_SUCCESS && p) {
        if (miss) {
            klvk_vin_put(g_vin_pipe, (uint64_t)(uintptr_t)p, slot, 0, 0);
            __atomic_add_fetch(&g_vin_pipes, 1, __ATOMIC_RELAXED);
        }
        *out = p;
    }
    unsigned s = __atomic_fetch_add(&said, 1, __ATOMIC_RELAXED);
    if (s < 8 || (!p && s < 64))
        VKI("CreateGraphicsPipelines: the vertex shader reads locations %#x and the pipeline "
            "supplies %#x — refused by Metal, created again with %#x reading zeroes on "
            "binding %u and %#x read as integers: %s (result %d)\n", reads, have, miss, slot,
            conv, p ? "ok" : "STILL REFUSED", (int)r);
}
// What the recording thread's command buffer has bound, as the guest knows it,
// and the binding the bound pipeline's zeroes are on (-1: none). The guest does
// not rebind a buffer it believes is still there, so one we displaced for a
// pipeline of the kind above is put back when another pipeline follows.
static __thread struct {
    void *cb; uint64_t buf[KLVK_VIN_MAXB], off[KLVK_VIN_MAXB]; uint32_t have; int k;
} t_vin = { .k = -1 };
static void klvk_vin_bindvb(void *cb, uint32_t binding, uint64_t buf, uint64_t off) {
    static void (*real)(void *, uint32_t, uint32_t, const uint64_t *, const uint64_t *);
    if (!real && real_gdpa) real = (void (*)(void *, uint32_t, uint32_t, const uint64_t *, const uint64_t *))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdBindVertexBuffers");
    if (real && buf) real(cb, binding, 1, &buf, &off);
}
static void klvk_vin_begin(void *cb) {
    if (t_vin.cb == cb) { t_vin.have = 0; t_vin.k = -1; }
}
static void klvk_vin_guest_bound(void *cb, uint32_t first, uint32_t count,
        const uint64_t *bufs, const uint64_t *offs) {
    if (!__atomic_load_n(&g_vin_pipes, __ATOMIC_RELAXED) || !bufs) return;
    if (t_vin.cb != cb) { t_vin.cb = cb; t_vin.have = 0; t_vin.k = -1; }
    for (uint32_t i = 0; i < count && first + i < KLVK_VIN_MAXB; i++) {
        t_vin.buf[first + i] = bufs[i];
        t_vin.off[first + i] = offs ? offs[i] : 0;
        t_vin.have |= 1u << (first + i);
    }
    if (t_vin.k >= 0 && (uint32_t)t_vin.k >= first && (uint32_t)t_vin.k < first + count && g_devs[0])
        klvk_vin_bindvb(cb, (uint32_t)t_vin.k, (uint64_t)(uintptr_t)g_devs[0]->vin_buf, 0);
}
static void klvk_vin_pipeline_bound(void *cb, uint64_t pipeline) {
    if (!__atomic_load_n(&g_vin_pipes, __ATOMIC_RELAXED)) return;
    klvk_vin_ent e;
    int k = klvk_vin_get(g_vin_pipe, pipeline, &e) ? (int)e.f : -1;
    if (t_vin.cb != cb) { t_vin.cb = cb; t_vin.have = 0; t_vin.k = -1; }
    if (t_vin.k >= 0 && t_vin.k != k && (t_vin.have & (1u << t_vin.k)))
        klvk_vin_bindvb(cb, (uint32_t)t_vin.k, t_vin.buf[t_vin.k], t_vin.off[t_vin.k]);
    t_vin.k = k;
    if (k >= 0 && g_devs[0])
        klvk_vin_bindvb(cb, (uint32_t)k, (uint64_t)(uintptr_t)g_devs[0]->vin_buf, 0);
}

static VkResult VKAPI_CALL klvk_CreateGraphicsPipelines0(VkDevice dev,
        VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
        const VkAllocationCallbacks *alloc, VkPipeline *out) {
    static PFN_vkCreateGraphicsPipelines real;
    if (!real && real_gdpa)
        real = (PFN_vkCreateGraphicsPipelines)real_gdpa(dev, "vkCreateGraphicsPipelines");

    // KL_VK_PIPE_DESCRIBE=1: which parts of each create info are absent, its
    // dynamic states and its pNext types — what to look at when MoltenVK faults
    // inside newMTLRenderPipelineDescriptor on a NULL member.
    if (ci && kl_env_on("KL_VK_PIPE_DESCRIBE", 0)) {
        for (uint32_t i = 0; i < n; i++) {
            const VkGraphicsPipelineCreateInfo *c = &ci[i];
            fprintf(stderr, "  [vk] pipeline %u/%u: stages %u vi %d ia %d tess %d vp %d rs %d ms %d ds %d cb %d dyn %d rp %p sub %u flags %#x",
                    i, n, c->stageCount, !!c->pVertexInputState, !!c->pInputAssemblyState,
                    !!c->pTessellationState, !!c->pViewportState, !!c->pRasterizationState,
                    !!c->pMultisampleState, !!c->pDepthStencilState, !!c->pColorBlendState,
                    !!c->pDynamicState, (void *)c->renderPass, c->subpass, (unsigned)c->flags);
            if (c->pDepthStencilState) {
                const VkPipelineDepthStencilStateCreateInfo *d = c->pDepthStencilState;
                fprintf(stderr, " ds.test %u write %u cmp %d bounds %u stencil %u f(%d/%d/%d cmp %d rm %#x wm %#x ref %u)",
                        d->depthTestEnable, d->depthWriteEnable, (int)d->depthCompareOp, d->depthBoundsTestEnable,
                        d->stencilTestEnable, (int)d->front.failOp, (int)d->front.passOp, (int)d->front.depthFailOp,
                        (int)d->front.compareOp, d->front.compareMask, d->front.writeMask, d->front.reference);
            }
            if (c->pRasterizationState)
                fprintf(stderr, " rs.bias %u %.3f/%.3f/%.3f cull %d",
                        c->pRasterizationState->depthBiasEnable, (double)c->pRasterizationState->depthBiasConstantFactor,
                        (double)c->pRasterizationState->depthBiasSlopeFactor, (double)c->pRasterizationState->depthBiasClamp,
                        (int)c->pRasterizationState->cullMode);
            if (c->pViewportState) fprintf(stderr, " vp.n %u sc.n %u", c->pViewportState->viewportCount, c->pViewportState->scissorCount);
            if (c->pColorBlendState) fprintf(stderr, " cb.att %u logicop %u/%d blend0 %d wmask0 %#x", c->pColorBlendState->attachmentCount,
                                             c->pColorBlendState->logicOpEnable, (int)c->pColorBlendState->logicOp,
                                             c->pColorBlendState->attachmentCount && c->pColorBlendState->pAttachments ? (int)c->pColorBlendState->pAttachments[0].blendEnable : -1,
                                             c->pColorBlendState->attachmentCount && c->pColorBlendState->pAttachments ? (unsigned)c->pColorBlendState->pAttachments[0].colorWriteMask : 0);
            for (uint32_t k = 0; c->pDynamicState && k < c->pDynamicState->dynamicStateCount; k++)
                fprintf(stderr, "%s%d", k ? "," : " dyn=", (int)c->pDynamicState->pDynamicStates[k]);
            for (const VkBaseInStructure *x = c->pNext; x; x = x->pNext) fprintf(stderr, " next=%d", (int)x->sType);
            for (uint32_t k = 0; k < c->stageCount && c->pStages; k++)
                if (c->pStages[k].stage == VK_SHADER_STAGE_FRAGMENT_BIT && klvk_fs_on())
                    fprintf(stderr, " fs %08x", klvk_fs_get(g_fs_mod, (uint64_t)(uintptr_t)c->pStages[k].module));
            fprintf(stderr, "\n");
        }
    }

    // A pipeline whose vertex input is dynamic (VK_EXT_vertex_input_dynamic_state,
    // which MoltenVK does not have) is not compiled here: it becomes a handle of
    // ours, compiled once per vertex layout when the guest sets one (klvk_vi_*).
    if (ci && klvk_vi_on()) {
        int any = 0;
        for (uint32_t i = 0; i < n; i++) any |= klvk_vi_dynamic(&ci[i]);
        if (any) {
            VkResult first = VK_SUCCESS;
            for (uint32_t i = 0; i < n; i++) {
                VkResult r = klvk_vi_dynamic(&ci[i]) ? klvk_vi_defer(dev, &ci[i], &out[i])
                                                     : klvk_CreateGraphicsPipelines0(dev, cache, 1, &ci[i], alloc, &out[i]);
                if (r != VK_SUCCESS && first == VK_SUCCESS) first = r;
            }
            return first;
        }
    }

    // KL_VK_FIX_PRIMRESTART (default ON): Metal cannot DISABLE primitive restart
    // for strip/fan topologies, so MoltenVK rejects a strip pipeline that sets
    // primitiveRestartEnable=FALSE with VK_ERROR_FEATURE_NOT_PRESENT and leaves a
    // NULL handle — and every draw that binds it then renders NOTHING (invisible
    // strip geometry: landscape, decals, ribbons, some particle/UI meshes). The
    // olar log shows exactly this ("[mvk-warn] ... Metal does not support
    // disabling primitive restart"). Metal always has restart ON, so forcing it
    // TRUE makes the pipeline valid; the only behavioural change is that a
    // max-value index (0xFFFF/0xFFFFFFFF) in strip data restarts the primitive —
    // which is precisely what that reserved index means. List topologies ignore
    // the flag and MoltenVK accepts them, so they are left untouched.
    VkGraphicsPipelineCreateInfo *mod = NULL;
    VkPipelineInputAssemblyStateCreateInfo *ias = NULL;
    int fixed = 0;
    static int fixon = -1;
    if (fixon < 0) fixon = kl_env_on("KL_VK_FIX_PRIMRESTART", 1);
    if (fixon && ci && n) {
        for (uint32_t i = 0; i < n; i++) {
            const VkPipelineInputAssemblyStateCreateInfo *a = ci[i].pInputAssemblyState;
            if (!a || a->primitiveRestartEnable) continue;
            VkPrimitiveTopology t = a->topology;
            if (t != VK_PRIMITIVE_TOPOLOGY_LINE_STRIP &&
                t != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP &&
                t != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN &&
                t != VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY &&
                t != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY)
                continue;
            if (!mod) {
                mod = malloc((size_t)n * sizeof *mod);
                ias = malloc((size_t)n * sizeof *ias);
                if (!mod || !ias) { free(mod); free(ias); mod = NULL; ias = NULL; break; }
                memcpy(mod, ci, (size_t)n * sizeof *mod);
            }
            ias[i] = *a;
            ias[i].primitiveRestartEnable = VK_TRUE;
            mod[i].pInputAssemblyState = &ias[i];
            fixed++;
        }
    }

    // rasterizationSamples == 0. Not a sample count — the valid values are bits,
    // and "no multisampling" is VK_SAMPLE_COUNT_1_BIT — but it is what a
    // zero-initialised VkPipelineMultisampleStateCreateInfo carries, Mesa reads
    // it as one sample, and code written on Linux therefore ships it (xrizer's
    // overlay pipeline does). Metal is handed the number as it stands and
    // refuses: "rasterSampleCount (0) is not supported by device". Read it the
    // way the drivers the guest was tested on do.
    VkPipelineMultisampleStateCreateInfo *mss = NULL;
    int zero_samples = 0;
    for (uint32_t i = 0; ci && i < n; i++) {
        const VkPipelineMultisampleStateCreateInfo *m = ci[i].pMultisampleState;
        if (!m || m->rasterizationSamples != 0) continue;
        if (!mod) {
            mod = malloc((size_t)n * sizeof *mod);
            if (!mod) break;
            memcpy(mod, ci, (size_t)n * sizeof *mod);
        }
        if (!mss && !(mss = malloc((size_t)n * sizeof *mss))) break;
        mss[i] = *m;
        mss[i].rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        mod[i].pMultisampleState = &mss[i];
        zero_samples++;
    }
    if (zero_samples) { static int said; if (!said++)
        VKI("CreateGraphicsPipelines: rasterizationSamples 0 read as 1 on %d "
            "pipeline(s) — Metal rejects the zero that Linux drivers accept\n", zero_samples); }

    // KL_VK_FORCE_SAMPLES (see klvk_raise_samples). A sample mask comes with
    // the count: one written for two samples leaves the added ones switched
    // off, which is four samples of memory for two of coverage — so its
    // pattern is repeated over them (all-on stays all-on).
    VkSampleMask *masks = NULL;
    for (uint32_t i = 0; ci && i < n; i++) {
        const VkPipelineMultisampleStateCreateInfo *m =
            mod && mod[i].pMultisampleState ? mod[i].pMultisampleState : ci[i].pMultisampleState;
        if (!m) continue;
        VkSampleCountFlagBits want = klvk_raise_samples(m->rasterizationSamples);
        if (want == m->rasterizationSamples) continue;
        if (!mod) {
            mod = malloc((size_t)n * sizeof *mod);
            if (!mod) break;
            memcpy(mod, ci, (size_t)n * sizeof *mod);
        }
        if (!mss && !(mss = malloc((size_t)n * sizeof *mss))) break;
        if (m != &mss[i]) mss[i] = *m;
        if (mss[i].pSampleMask) {
            if (!masks && !(masks = malloc((size_t)n * sizeof *masks))) break;
            unsigned had = (unsigned)mss[i].rasterizationSamples;
            VkSampleMask bits = mss[i].pSampleMask[0] & ((1u << had) - 1), wide = 0;
            for (unsigned at = 0; at < (unsigned)want; at += had) wide |= bits << at;
            masks[i] = wide;
            mss[i].pSampleMask = &masks[i];
        }
        mss[i].rasterizationSamples = want;
        mod[i].pMultisampleState = &mss[i];
    }

    // KL_VK_PIPE_TRACE (default ON for wrath2): one UE4 warmup pipeline wedges
    // MoltenVK's Metal compiler forever (MVKMetalCompiler::compile never returns),
    // and the existing warmup counter only logs on EXIT, so the hung pipeline is
    // invisible. Log an ENTER (flushed) before real() and a DONE after; the last
    // ENTER with no matching DONE names the wedging pipeline and its shape.
    static int pipe_trace = -1;
    if (pipe_trace < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        pipe_trace = kl_env_on("KL_VK_PIPE_TRACE", t && !strcmp(t, "wrath2"));
    }
    static unsigned long long g_pipe_enter;
    const VkGraphicsPipelineCreateInfo *use = mod ? mod : ci;
    if (pipe_trace && use) {
        for (uint32_t i = 0; i < n; i++) {
            unsigned long long idx = __atomic_add_fetch(&g_pipe_enter, 1, __ATOMIC_RELAXED);
            unsigned smask = 0;
            for (uint32_t s = 0; s < use[i].stageCount && use[i].pStages; s++)
                smask |= use[i].pStages[s].stage;
            VKI("pipe-trace: ENTER gfx #%llu stages=%u stagemask=0x%x tess=%d subpass=%u\n",
                idx, use[i].stageCount, smask, use[i].pTessellationState ? 1 : 0, use[i].subpass);
        }
        fflush(stderr);
    }
    double t0 = kl_pipe_ms();
    // A logic operation MoltenVK would drop (KL_VK_LOGIC_OP): set on the Metal
    // pipeline it is about to create, from underneath (kl_mtl_logic_op_next).
    int logic = -1;
    if (n == 1 && use && use[0].pColorBlendState && use[0].pColorBlendState->logicOpEnable && klvk_logic_op_on()) {
        // Vulkan's order -> Apple's (see kl_mtl_busy.m)
        static const int vk2mtl[16] = { 0 /*CLEAR*/, 6 /*AND*/, 12 /*AND_REVERSE*/, 2 /*COPY*/, 13 /*AND_INVERTED*/,
                                        4 /*NO_OP*/, 10 /*XOR*/, 8 /*OR*/, 9 /*NOR*/, 11 /*EQUIVALENT*/, 5 /*INVERT*/,
                                        14 /*OR_REVERSE*/, 3 /*COPY_INVERTED*/, 15 /*OR_INVERTED*/, 7 /*NAND*/, 1 /*SET*/ };
        unsigned vop = (unsigned)use[0].pColorBlendState->logicOp;
        if (vop < 16) {
            static int installed;
            if (!installed) { installed = 1; extern int kl_mtl_busy_install(void); kl_mtl_busy_install(); }
            logic = vk2mtl[vop];
            extern void kl_mtl_logic_op_next(int);
            kl_mtl_logic_op_next(logic);
            static int said;
            if (said++ < 3) VKI("logic op %u (Metal %d) for a pipeline being created (KL_VK_LOGIC_OP)\n", vop, logic);
        }
    }
    VkResult r = real ? real(dev, cache, n, use, alloc, out)
                      : VK_ERROR_INITIALIZATION_FAILED;
    if (r < 0 && real && use && out && klvk_vin_on()) {
        uint32_t left = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!out[i]) klvk_vin_retry(real, dev, cache, &use[i], alloc, &out[i]);
            if (!out[i]) left++;
        }
        if (!left) r = VK_SUCCESS;
    }
    if (logic >= 0) { extern void kl_mtl_logic_op_next(int); kl_mtl_logic_op_next(-1); }
    if (pipe_trace) { VKI("pipe-trace: DONE through #%llu (r=%d)\n", g_pipe_enter, (int)r); fflush(stderr); }
    free(mod); free(ias); free(mss); free(masks);
    if (fixed) { static int said; if (said < 8) { said++;
        VKI("CreateGraphicsPipelines: forced primitiveRestartEnable=TRUE on %d "
            "strip pipeline(s) MoltenVK would otherwise reject (invisible "
            "geometry)\n", fixed); } }
    if (!t_pipe_replay) {
        g_pipe_gfx += n;
        kl_pipe_report(t_ap_worker ? "background gfx" : "gfx", n, kl_pipe_ms() - t0);
    }
    return r;
}
// vkCreatePipelineCache with the cache a guest saved in an earlier session.
//
// MoltenVK's pipeline cache is the MSL source of every shader the session
// used, and reading it back COMPILES each of them, in this call, before it
// returns. Asgard's Wrath 2 hands over 30 MB at every start. While the
// system's own Metal cache still holds the results that is 0.2 s; when it does
// not — after the app was installed again, after an update of the system, or
// when the system cleared the app's caches to make room — it was 95 s on a
// Mac, and on a headset a black picture with no sign of life for as long
// ("it does not start any more", given up after 18 s, 2026-10-11).
//
// So the read gets a moment (KL_VK_CACHE_WAIT_MS, default 1500), on a thread
// of its own. Done in time: the guest gets that cache, as before. Not done:
// the guest gets an EMPTY cache and goes on at once — which is the start of a
// freshly installed game, and the game's own "compiling shaders" screen where
// it has one, instead of nothing at all. The read is left to finish behind it;
// what it compiles lands in Metal's cache, so the pipelines the game then asks
// for are quick, and so is the whole saved cache at the next start. It runs at
// background priority once the guest has gone on, so the two do not fight over
// the compiler ("you are doing it twice now", the same day). Its result
// is thrown away rather than merged into the guest's cache while the guest is
// using it (the guest writes its cache out again as it goes).
//
// A card of our own over the wait was tried first (c823cf0) and was the worse
// answer: the game has a screen for exactly this, and behind the card it then
// came as well — Wrath 2 walks its own list of 7,150 pipelines at every start
// (a second on a warm headset), so the saved cache was the same work once
// more, in front of it and in the dark.
typedef struct {
    VkDevice dev;
    PFN_vkCreatePipelineCache create;
    void *blob; size_t size;
    VkPipelineCacheCreateFlags flags;
    VkPipelineCache cache;
    VkResult res;
    int done, abandoned;
    double t0;
} klvk_pcache_job;
static pthread_mutex_t g_pcache_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_pcache_cv = PTHREAD_COND_INITIALIZER;
static klvk_pcache_job *g_pcache_behind;     // the one read still running behind the guest

static void *klvk_pcache_thread(void *arg) {
    klvk_pcache_job *j = arg;
    pthread_setname_np("kl-vk-saved-cache");
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .flags = j->flags,
                                     .initialDataSize = j->size, .pInitialData = j->blob };
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkResult r = j->create(j->dev, &ci, NULL, &cache);
    pthread_mutex_lock(&g_pcache_mu);
    j->cache = cache; j->res = r; j->done = 1;
    int abandoned = j->abandoned;
    pthread_cond_broadcast(&g_pcache_cv);
    pthread_mutex_unlock(&g_pcache_mu);
    if (!abandoned) return NULL;            // the caller is waiting and owns the job

    fprintf(stderr, "  [vk] saved pipeline cache: read behind the guest in %.1f s (result %d) — "
                    "its shaders are in Metal's cache for the next start\n", (kl_pipe_ms() - j->t0) / 1000.0, (int)r);
    if (r == VK_SUCCESS && cache && real_gdpa) {
        PFN_vkDestroyPipelineCache destroy = (PFN_vkDestroyPipelineCache)real_gdpa(j->dev, "vkDestroyPipelineCache");
        if (destroy) destroy(j->dev, cache, NULL);
    }
    pthread_mutex_lock(&g_pcache_mu);
    if (g_pcache_behind == j) g_pcache_behind = NULL;
    pthread_cond_broadcast(&g_pcache_cv);
    pthread_mutex_unlock(&g_pcache_mu);
    free(j->blob); free(j);
    return NULL;
}

// A device is about to go: a read still running on it has to end first.
static void klvk_pcache_wait(VkDevice dev) {
    pthread_mutex_lock(&g_pcache_mu);
    while (g_pcache_behind && g_pcache_behind->dev == dev) pthread_cond_wait(&g_pcache_cv, &g_pcache_mu);
    pthread_mutex_unlock(&g_pcache_mu);
}

static VkResult VKAPI_CALL klvk_CreatePipelineCache(VkDevice dev, const VkPipelineCacheCreateInfo *ci,
        const VkAllocationCallbacks *alloc, VkPipelineCache *out) {
    static PFN_vkCreatePipelineCache real;
    if (!real && real_gdpa)
        real = (PFN_vkCreatePipelineCache)real_gdpa(dev, "vkCreatePipelineCache");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    size_t saved = ci && ci->pInitialData ? ci->initialDataSize : 0;
    unsigned wait_ms = kl_env_uint("KL_VK_CACHE_WAIT_MS", 1500);
    klvk_pcache_job *j = NULL;
    pthread_mutex_lock(&g_pcache_mu);
    int busy = g_pcache_behind != NULL;
    pthread_mutex_unlock(&g_pcache_mu);
    // Small caches are read in place, as is everything with the switch at 0;
    // so is a second large one while a first is still being read.
    if (saved < (1u << 20) || !wait_ms || busy || !(j = calloc(1, sizeof *j)) || !(j->blob = malloc(saved))) {
        free(j);
        double t0 = kl_pipe_ms();
        VkResult r = real(dev, ci, alloc, out);
        if (saved >= (1u << 20))
            fprintf(stderr, "  [vk] vkCreatePipelineCache: %.1f MB of saved cache read in place, result %d after %.1f s\n",
                    (double)saved / 1048576.0, (int)r, (kl_pipe_ms() - t0) / 1000.0);
        return r;
    }
    memcpy(j->blob, ci->pInitialData, saved);
    j->dev = dev; j->create = real; j->size = saved; j->flags = ci->flags; j->t0 = kl_pipe_ms();
    // The thread is a background one that is lifted to the caller's urgency
    // for as long as the caller waits for it: once the guest has gone on with
    // an empty cache, the read must not compete with the compiling the guest
    // then does in front of the player — that would be the same wait twice.
    pthread_t th;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_set_qos_class_np(&at, QOS_CLASS_UTILITY, 0);
    int started = pthread_create(&th, &at, klvk_pcache_thread, j) == 0;
    pthread_attr_destroy(&at);
    if (!started) { free(j->blob); free(j); return real(dev, ci, alloc, out); }
    pthread_override_t lift = pthread_override_qos_class_start_np(th, QOS_CLASS_USER_INITIATED, 0);

    struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += wait_ms / 1000; until.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_pcache_mu);
    while (!j->done && pthread_cond_timedwait(&g_pcache_cv, &g_pcache_mu, &until) == 0) { }
    int done = j->done;
    if (!done) { j->abandoned = 1; g_pcache_behind = j; }
    pthread_mutex_unlock(&g_pcache_mu);
    if (lift) pthread_override_qos_class_end_np(lift);

    if (done) {
        pthread_join(th, NULL);
        VkResult r = j->res;
        if (r == VK_SUCCESS) *out = j->cache;
        fprintf(stderr, "  [vk] vkCreatePipelineCache: %.1f MB of saved cache, result %d after %.1f s\n",
                (double)saved / 1048576.0, (int)r, (kl_pipe_ms() - j->t0) / 1000.0);
        free(j->blob); free(j);
        return r;
    }
    fprintf(stderr, "  [vk] vkCreatePipelineCache: %.1f MB of saved cache not read within %u ms — Metal's cache is cold. "
                    "The guest starts with an empty cache, as after a fresh install; the read goes on behind it\n",
            (double)saved / 1048576.0, wait_ms);
    pthread_detach(th);
    VkPipelineCacheCreateInfo empty = *ci;
    empty.initialDataSize = 0; empty.pInitialData = NULL;
    return real(dev, &empty, alloc, out);
}

static VkResult VKAPI_CALL klvk_CreateComputePipelines(VkDevice dev,
        VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo *ci,
        const VkAllocationCallbacks *alloc, VkPipeline *out) {
    static PFN_vkCreateComputePipelines real;
    if (!real && real_gdpa)
        real = (PFN_vkCreateComputePipelines)real_gdpa(dev, "vkCreateComputePipelines");
    double t0 = kl_pipe_ms();
    VkResult r = real ? real(dev, cache, n, ci, alloc, out)
                      : VK_ERROR_INITIALIZATION_FAILED;
    if (!t_pipe_replay) {
        g_pipe_comp += n;
        kl_pipe_report("compute", n, kl_pipe_ms() - t0);
    }
    return r;
}

// ---------------------------------------------------------------------------
// KL_VK_ASYNC_PIPES — a pipeline is compiled BEHIND the frame, not in it.
//
// A guest that creates a pipeline at the draw that first needs it (Source 2
// does: "pre-warmed 0, draw-time compiled N" for every one of them) waits in
// that draw for the compiler. On a native driver that wait is short, or spared
// by a pipeline database replayed before the game starts; here every first use
// goes through SPIR-V -> MSL -> Metal's compiler, a quarter to half a second
// the first time a shader is seen on a device — and in a headset a frame that
// does not come is the whole world standing still. (Metal keeps what it has
// compiled, so it is once per pipeline per device; the database the game ships
// is of no use, it holds the desktop build's shaders and not one of these.)
//
// So the compile is handed to a worker and the guest waits for it only as long
// as a frame can spare. A compile that takes longer is answered with a handle
// of OUR making; binding that handle while the pipeline is not there yet binds
// nothing, and the draws that follow on that command buffer are dropped. What
// the guest wanted drawn appears a few frames late instead of every frame
// arriving late.
//
// Only for pipelines that draw with a depth buffer (mode 1) — the eyes, the
// views a level shows on its monitors, and the shadow maps: what is drawn there
// is drawn anew every frame (this engine clears its 4096x4096 shadow atlas each
// frame and redraws all of it), so a dropped draw costs those frames and
// nothing else. A panel of the interface (colour alone) may be rendered once
// and kept, and would keep the hole; a full-screen pass that is skipped is a
// black picture. On the headset the one stall left with shadows excluded was
// the vertex shader of a shadow pipeline. Mode 2 takes every graphics
// pipeline, to test with.
//   KL_VK_ASYNC_WAIT_MS   the most one creation waits (default 8); the waits
//                         of all creations together are held to a tenth of
//                         the time that passes
//   KL_VK_ASYNC_DELAY_MS  a test knob: every compile takes this much longer
#define KLVK_AP_MAX 16384u
typedef struct { void *p[128]; int n, fail; } klvk_ap_arena;
typedef struct klvk_apipe {
    uint64_t real;                      // the pipeline, once there is one
    int state;                          // 0 compiling, 1 there, 2 failed, 3 destroyed
    VkResult result;
    VkDevice dev;
    VkGraphicsPipelineCreateInfo ci;    // a copy that owns everything it points to
    klvk_ap_arena mem;
    struct klvk_apipe *next;
    double t0;
} klvk_apipe;
static klvk_apipe g_ap_pool[KLVK_AP_MAX];
static unsigned g_ap_used, g_ap_late, g_ap_intime;
static pthread_mutex_t g_ap_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ap_work = PTHREAD_COND_INITIALIZER, g_ap_done = PTHREAD_COND_INITIALIZER;
static klvk_apipe *g_ap_head, *g_ap_tail;
static int g_ap_pending, g_ap_threads;
static double g_ap_allow = -1, g_ap_last;

static klvk_apipe *klvk_ap_from(uint64_t h) {
    uintptr_t p = (uintptr_t)h, b = (uintptr_t)g_ap_pool;
    return (p >= b && p < b + sizeof g_ap_pool) ? (klvk_apipe *)p : NULL;
}
static void *klvk_ap_dup(klvk_ap_arena *a, const void *src, size_t size) {
    if (!src || !size) return NULL;
    void *d = a->n < (int)(sizeof a->p / sizeof *a->p) ? malloc(size) : NULL;
    if (!d) { a->fail = 1; return NULL; }
    memcpy(d, src, size);
    a->p[a->n++] = d;
    return d;
}
static void klvk_ap_free(klvk_ap_arena *a) {
    for (int i = 0; i < a->n; i++) free(a->p[i]);
    a->n = 0;
}
static void klvk_ap_unknown(const char *where, const void *chain) {
    static int said;
    if (said++ < 12)
        VKI("async pipelines: a %s carries a structure of type %d that is not copied here — "
            "that pipeline is compiled in place\n", where, (int)((const VkBaseInStructure *)chain)->sType);
}
// A copy of the create-info that outlives the call it came with. 0 if there is
// something in it this does not know how to copy.
static int klvk_ap_clone(klvk_ap_arena *a, const VkGraphicsPipelineCreateInfo *ci,
                         VkGraphicsPipelineCreateInfo *o) {
    *o = *ci;
    o->pNext = NULL;
    o->flags &= ~(VkPipelineCreateFlags)VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    o->basePipelineHandle = VK_NULL_HANDLE; o->basePipelineIndex = -1;
    const void **tail = &o->pNext;
    for (const VkBaseInStructure *x = ci->pNext; x; x = x->pNext) {
        if (x->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
            VkPipelineRenderingCreateInfo *r = klvk_ap_dup(a, x, sizeof *r);
            if (!r) return 0;
            r->pNext = NULL;
            r->pColorAttachmentFormats = klvk_ap_dup(a, r->pColorAttachmentFormats,
                                                     r->colorAttachmentCount * sizeof(VkFormat));
            *tail = r; tail = &r->pNext;
        } else if (x->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO) {
            // Answered into the guest's memory, at a time it no longer expects it: left out.
        } else if (x->sType == VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR) {
            // Plain values; Unity's Vulkan device chains it where the device names the extension.
            VkPipelineFragmentShadingRateStateCreateInfoKHR *f = klvk_ap_dup(a, x, sizeof *f);
            if (!f) return 0;
            f->pNext = NULL;
            *tail = f; tail = &f->pNext;
        } else { klvk_ap_unknown("pipeline", x); return 0; }
    }
    unsigned stages = 0;
    VkPipelineShaderStageCreateInfo *st = klvk_ap_dup(a, ci->pStages, ci->stageCount * sizeof *st);
    o->pStages = st;
    for (uint32_t i = 0; st && i < ci->stageCount; i++) {
        if (st[i].pNext) { klvk_ap_unknown("shader stage", st[i].pNext); return 0; }
        stages |= st[i].stage;
        if (st[i].pName) st[i].pName = klvk_ap_dup(a, st[i].pName, strlen(st[i].pName) + 1);
        if (st[i].pSpecializationInfo) {
            VkSpecializationInfo *sp = klvk_ap_dup(a, st[i].pSpecializationInfo, sizeof *sp);
            if (!sp) return 0;
            sp->pMapEntries = klvk_ap_dup(a, sp->pMapEntries, sp->mapEntryCount * sizeof *sp->pMapEntries);
            sp->pData = klvk_ap_dup(a, sp->pData, sp->dataSize);
            st[i].pSpecializationInfo = sp;
        }
    }
    int dyn_vp = 0, dyn_sc = 0;
    if (ci->pDynamicState) {
        VkPipelineDynamicStateCreateInfo *d = klvk_ap_dup(a, ci->pDynamicState, sizeof *d);
        if (!d) return 0;
        if (d->pNext) { klvk_ap_unknown("dynamic state", d->pNext); return 0; }
        d->pDynamicStates = klvk_ap_dup(a, d->pDynamicStates, d->dynamicStateCount * sizeof *d->pDynamicStates);
        for (uint32_t i = 0; d->pDynamicStates && i < d->dynamicStateCount; i++) {
            if (d->pDynamicStates[i] == VK_DYNAMIC_STATE_VIEWPORT) dyn_vp = 1;
            if (d->pDynamicStates[i] == VK_DYNAMIC_STATE_SCISSOR) dyn_sc = 1;
        }
        o->pDynamicState = d;
    }
    if (ci->pVertexInputState) {
        VkPipelineVertexInputStateCreateInfo *v = klvk_ap_dup(a, ci->pVertexInputState, sizeof *v);
        if (!v) return 0;
        if (v->pNext) { klvk_ap_unknown("vertex input state", v->pNext); return 0; }
        v->pVertexBindingDescriptions = klvk_ap_dup(a, v->pVertexBindingDescriptions,
            v->vertexBindingDescriptionCount * sizeof *v->pVertexBindingDescriptions);
        v->pVertexAttributeDescriptions = klvk_ap_dup(a, v->pVertexAttributeDescriptions,
            v->vertexAttributeDescriptionCount * sizeof *v->pVertexAttributeDescriptions);
        o->pVertexInputState = v;
    }
#define KLVK_AP_POD(field, name) \
    if (ci->field) { \
        if (((const VkBaseInStructure *)ci->field)->pNext) { \
            klvk_ap_unknown(name, ((const VkBaseInStructure *)ci->field)->pNext); return 0; } \
        o->field = klvk_ap_dup(a, ci->field, sizeof *ci->field); \
    }
    KLVK_AP_POD(pInputAssemblyState, "input assembly state")
    KLVK_AP_POD(pDepthStencilState, "depth/stencil state")
    if (ci->pRasterizationState) {
        // Its one known chain member: depth clip (VK_EXT_depth_clip_enable, plain
        // values) — HITMAN 3 VR: Reloaded chains it on every pipeline.
        VkPipelineRasterizationStateCreateInfo *rs = klvk_ap_dup(a, ci->pRasterizationState, sizeof *rs);
        if (!rs) return 0;
        rs->pNext = NULL;
        const void **rtail = &rs->pNext;
        for (const VkBaseInStructure *x = ci->pRasterizationState->pNext; x; x = x->pNext) {
            if (x->sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT) {
                klvk_ap_unknown("rasterization state", x); return 0; }
            VkPipelineRasterizationDepthClipStateCreateInfoEXT *dc = klvk_ap_dup(a, x, sizeof *dc);
            if (!dc) return 0;
            dc->pNext = NULL;
            *rtail = dc; rtail = &dc->pNext;
        }
        o->pRasterizationState = rs;
    }
    o->pTessellationState = NULL;
    if (stages & (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)) {
        KLVK_AP_POD(pTessellationState, "tessellation state")
    }
#undef KLVK_AP_POD
    if (ci->pViewportState) {
        VkPipelineViewportStateCreateInfo *v = klvk_ap_dup(a, ci->pViewportState, sizeof *v);
        if (!v) return 0;
        if (v->pNext) { klvk_ap_unknown("viewport state", v->pNext); return 0; }
        v->pViewports = dyn_vp ? NULL : klvk_ap_dup(a, v->pViewports, v->viewportCount * sizeof *v->pViewports);
        v->pScissors = dyn_sc ? NULL : klvk_ap_dup(a, v->pScissors, v->scissorCount * sizeof *v->pScissors);
        o->pViewportState = v;
    }
    if (ci->pMultisampleState) {
        VkPipelineMultisampleStateCreateInfo *m = klvk_ap_dup(a, ci->pMultisampleState, sizeof *m);
        if (!m) return 0;
        if (m->pNext) { klvk_ap_unknown("multisample state", m->pNext); return 0; }
        m->pSampleMask = klvk_ap_dup(a, m->pSampleMask,
                                     (((size_t)m->rasterizationSamples + 31) / 32) * sizeof(VkSampleMask));
        o->pMultisampleState = m;
    }
    if (ci->pColorBlendState) {
        VkPipelineColorBlendStateCreateInfo *c = klvk_ap_dup(a, ci->pColorBlendState, sizeof *c);
        if (!c) return 0;
        if (c->pNext) { klvk_ap_unknown("colour blend state", c->pNext); return 0; }
        c->pAttachments = klvk_ap_dup(a, c->pAttachments, c->attachmentCount * sizeof *c->pAttachments);
        o->pColorBlendState = c;
    }
    return !a->fail;
}
static void *klvk_ap_worker(void *arg) {
    (void)arg;
    pthread_setname_np("kl-pipeline-compile");
    t_ap_worker = 1;
    unsigned delay = kl_env_uint("KL_VK_ASYNC_DELAY_MS", 0);
    for (;;) {
        pthread_mutex_lock(&g_ap_mu);
        while (!g_ap_head) pthread_cond_wait(&g_ap_work, &g_ap_mu);
        klvk_apipe *j = g_ap_head;
        g_ap_head = j->next;
        if (!g_ap_head) g_ap_tail = NULL;
        pthread_mutex_unlock(&g_ap_mu);
        if (delay) usleep(delay * 1000u);
        VkPipeline p = VK_NULL_HANDLE;
        // No pipeline cache: the guest's is one per thread of ITS, and this is not one of them.
        VkResult r = klvk_CreateGraphicsPipelines0(j->dev, VK_NULL_HANDLE, 1, &j->ci, NULL, &p);
        klvk_ap_free(&j->mem);
        pthread_mutex_lock(&g_ap_mu);
        j->real = (uint64_t)(uintptr_t)p;
        j->result = r;
        j->state = (r == VK_SUCCESS && p) ? 1 : 2;
        g_ap_pending--;
        pthread_cond_broadcast(&g_ap_done);
        pthread_mutex_unlock(&g_ap_mu);
    }
    return NULL;
}
// Every compile that is under way has ended when this returns: before the guest
// destroys something a compile may be reading.
static void klvk_ap_drain(void) {
    if (!__atomic_load_n(&g_ap_used, __ATOMIC_RELAXED)) return;
    pthread_mutex_lock(&g_ap_mu);
    while (g_ap_pending > 0) pthread_cond_wait(&g_ap_done, &g_ap_mu);
    pthread_mutex_unlock(&g_ap_mu);
}
void klvk_ap_drain_all(void) { klvk_ap_drain(); }
static int klvk_ap_mode(void) {
    static int mode = -1;
    if (mode < 0) mode = (int)kl_env_uint("KL_VK_ASYNC_PIPES", 0);
    return mode;
}
// KL_VK_ASYNC_RP — the same for pipelines made for a VkRenderPass, which is
// how Unity 2022 makes all of its own (Arkham Shadow: 400 pipelines in twelve
// minutes of play, up to 1.1 s each on the headset, every one a frozen
// picture). Off unless set: Source 2 makes its pipelines with dynamic
// rendering, and what it was measured with stays as it was.
//
// Mode 1's test — does the pipeline draw with a depth buffer — is in the pass
// for these: which of its subpasses have a depth/stencil attachment, noted
// when the pass is made (one bit per subpass; a pass of more than 32 is not
// noted, and its pipelines are compiled in place). A pass the guest destroys
// first waits for the compiles under way, which may be reading it.
static int klvk_ap_rp_on(void) {
    static int on = -1;
    if (on < 0) on = klvk_ap_mode() && kl_env_on("KL_VK_ASYNC_RP", 0);
    return on;
}
#define KLVK_RP_SLOTS 8192u
typedef struct { uint64_t rp; uint32_t depth; int known; } klvk_rp_ent;
static klvk_rp_ent g_rp_tab[KLVK_RP_SLOTS];
static pthread_mutex_t g_rp_mu = PTHREAD_MUTEX_INITIALIZER;
// Under g_rp_mu. A slot keeps its handle once given one: MoltenVK hands the
// address of a destroyed pass out again, and that finds its old slot.
static klvk_rp_ent *klvk_rp_slot(uint64_t rp, int add) {
    uint32_t i = (uint32_t)((rp >> 4) * 2654435761u) % KLVK_RP_SLOTS;
    for (uint32_t k = 0; k < KLVK_RP_SLOTS; k++, i = (i + 1) % KLVK_RP_SLOTS) {
        if (g_rp_tab[i].rp == rp) return &g_rp_tab[i];
        if (!g_rp_tab[i].rp) {
            if (!add) return NULL;
            g_rp_tab[i].rp = rp;
            return &g_rp_tab[i];
        }
    }
    return NULL;
}
static void klvk_rp_note(uint64_t rp, uint32_t subpasses, uint32_t depth) {
    if (!rp || !klvk_ap_rp_on()) return;
    pthread_mutex_lock(&g_rp_mu);
    klvk_rp_ent *e = klvk_rp_slot(rp, 1);
    if (e) { e->depth = depth; e->known = subpasses <= 32; }
    pthread_mutex_unlock(&g_rp_mu);
    static int said;
    if (!e && !said++) VKI("async pipelines: the render-pass table is full — pipelines for passes made "
                           "from now on are compiled in place\n");
}
static int klvk_rp_depth(uint64_t rp, uint32_t subpass) {
    pthread_mutex_lock(&g_rp_mu);
    klvk_rp_ent *e = klvk_rp_slot(rp, 0);
    int d = e && e->known && subpass < 32 && ((e->depth >> subpass) & 1u);
    pthread_mutex_unlock(&g_rp_mu);
    return d;
}
// 1 if the pass was noted: compiles that may read it are possible.
static int klvk_rp_forget(uint64_t rp) {
    if (!rp || !klvk_ap_rp_on()) return 0;
    pthread_mutex_lock(&g_rp_mu);
    klvk_rp_ent *e = klvk_rp_slot(rp, 0);
    if (e) e->known = 0;
    pthread_mutex_unlock(&g_rp_mu);
    return e != NULL;
}
static VkResult klvk_ap_create(VkDevice dev, VkPipelineCache cache, uint32_t n,
        const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *alloc, VkPipeline *out) {
    int mode = klvk_ap_mode();
    if (!mode || n != 1 || !ci || !out)
        return klvk_CreateGraphicsPipelines0(dev, cache, n, ci, alloc, out);
    if (ci->renderPass) {
        if (!klvk_ap_rp_on() ||
            (mode == 1 && !klvk_rp_depth((uint64_t)(uintptr_t)ci->renderPass, ci->subpass)))
            return klvk_CreateGraphicsPipelines0(dev, cache, n, ci, alloc, out);
    } else if (mode == 1) {
        const VkPipelineRenderingCreateInfo *ri = NULL;
        for (const VkBaseInStructure *x = ci->pNext; x; x = x->pNext)
            if (x->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) ri = (const void *)x;
        if (!ri || ri->depthAttachmentFormat == VK_FORMAT_UNDEFINED)
            return klvk_CreateGraphicsPipelines0(dev, cache, n, ci, alloc, out);
    }
    unsigned idx = __atomic_fetch_add(&g_ap_used, 1, __ATOMIC_RELAXED);
    if (idx >= KLVK_AP_MAX) {
        __atomic_store_n(&g_ap_used, KLVK_AP_MAX, __ATOMIC_RELAXED);
        return klvk_CreateGraphicsPipelines0(dev, cache, n, ci, alloc, out);
    }
    klvk_apipe *j = &g_ap_pool[idx];
    if (!klvk_ap_clone(&j->mem, ci, &j->ci)) {
        klvk_ap_free(&j->mem);
        j->state = 3;
        return klvk_CreateGraphicsPipelines0(dev, cache, n, ci, alloc, out);
    }
    j->dev = dev; j->state = 0; j->real = 0; j->next = NULL; j->t0 = kl_pipe_ms();
    static double cap = -1;
    if (cap < 0) cap = (double)kl_env_uint("KL_VK_ASYNC_WAIT_MS", 8);

    pthread_mutex_lock(&g_ap_mu);
    while (g_ap_threads < 3) {
        pthread_t t;
        pthread_attr_t at; pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_set_qos_class_np(&at, QOS_CLASS_USER_INITIATED, 0);
        int e = pthread_create(&t, &at, klvk_ap_worker, NULL);
        pthread_attr_destroy(&at);
        if (e) break;
        g_ap_threads++;
    }
    if (!g_ap_threads) {
        pthread_mutex_unlock(&g_ap_mu);
        klvk_ap_free(&j->mem);
        j->state = 3;
        return klvk_CreateGraphicsPipelines0(dev, cache, n, ci, alloc, out);
    }
    if (g_ap_tail) g_ap_tail->next = j; else g_ap_head = j;
    g_ap_tail = j;
    g_ap_pending++;
    pthread_cond_signal(&g_ap_work);
    // What this creation may wait: what the frames since the last one have saved up.
    double now = j->t0;
    if (g_ap_allow < 0) g_ap_allow = cap; else g_ap_allow += (now - g_ap_last) * 0.1;
    if (g_ap_allow > cap) g_ap_allow = cap;
    g_ap_last = now;
    double until = now + g_ap_allow;
    while (!j->state) {
        double left = until - kl_pipe_ms();
        if (left <= 0.05) break;
        struct timespec ts = { (time_t)(left / 1000.0), (long)((left - (double)(long)(left / 1000.0) * 1000.0) * 1e6) };
        pthread_cond_timedwait_relative_np(&g_ap_done, &g_ap_mu, &ts);
    }
    double waited = kl_pipe_ms() - now;
    g_ap_allow -= waited; if (g_ap_allow < 0) g_ap_allow = 0;
    g_ap_last = now + waited;
    int state = j->state;
    VkResult r = VK_SUCCESS;
    if (state == 1) { *out = (VkPipeline)(uintptr_t)j->real; j->real = 0; j->state = 3; g_ap_intime++; }
    else if (state == 2) { *out = VK_NULL_HANDLE; r = j->result != VK_SUCCESS ? j->result : VK_ERROR_UNKNOWN; j->state = 3; }
    else { *out = (VkPipeline)(uintptr_t)j; g_ap_late++; }
    unsigned late = g_ap_late, intime = g_ap_intime; int pending = g_ap_pending;
    pthread_mutex_unlock(&g_ap_mu);
    if (!state && (late <= 5 || !(late % 25)))
        VKI("async pipelines: %u compiled behind the frame so far, %u were there within the wait "
            "(%d under way now)\n", late, intime, pending);
    return r;
}
// The command buffers on which the pipeline that is bound is not there yet.
#define KLVK_AP_CBS 4096u
static struct { uint64_t cb; int skip; } g_ap_cb[KLVK_AP_CBS];
static unsigned g_ap_cb_n, g_ap_dropped;
// 0 if it could not be noted (the table is full): then the caller has to wait.
static int klvk_ap_mark(void *cb, int skip) {
    if (!skip && !__atomic_load_n(&g_ap_cb_n, __ATOMIC_RELAXED)) return 1;
    uint64_t h = (uint64_t)(uintptr_t)cb;
    uint32_t i = (uint32_t)((h >> 4) * 2654435761u) % KLVK_AP_CBS;
    for (uint32_t n = 0; n < KLVK_AP_CBS; n++, i = (i + 1) % KLVK_AP_CBS) {
        uint64_t v = __atomic_load_n(&g_ap_cb[i].cb, __ATOMIC_ACQUIRE);
        if (!v) {
            if (!skip) return 1;
            uint64_t none = 0;
            if (__atomic_compare_exchange_n(&g_ap_cb[i].cb, &none, h, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                __atomic_add_fetch(&g_ap_cb_n, 1, __ATOMIC_RELAXED);
                v = h;
            } else v = none;
        }
        if (v == h) { __atomic_store_n(&g_ap_cb[i].skip, skip, __ATOMIC_RELEASE); return 1; }
    }
    return !skip;
}
static int klvk_ap_skip(void *cb) {
    if (!__atomic_load_n(&g_ap_cb_n, __ATOMIC_RELAXED)) return 0;
    uint64_t h = (uint64_t)(uintptr_t)cb;
    uint32_t i = (uint32_t)((h >> 4) * 2654435761u) % KLVK_AP_CBS;
    for (uint32_t n = 0; n < KLVK_AP_CBS; n++, i = (i + 1) % KLVK_AP_CBS) {
        uint64_t v = __atomic_load_n(&g_ap_cb[i].cb, __ATOMIC_ACQUIRE);
        if (v == h) {
            if (!__atomic_load_n(&g_ap_cb[i].skip, __ATOMIC_ACQUIRE)) return 0;
            __atomic_add_fetch(&g_ap_dropped, 1, __ATOMIC_RELAXED);
            return 1;
        }
        if (!v) return 0;
    }
    return 0;
}
// At a bind: what to hand MoltenVK for the guest's handle. 0 with *drop set if
// the pipeline is not there yet and the draws on this command buffer are to go.
static uint64_t klvk_ap_bind(void *cb, uint32_t bindPoint, uint64_t pipeline, int *drop) {
    *drop = 0;
    klvk_apipe *j = klvk_ap_from(pipeline);
    if (!j) { if (bindPoint == 0) klvk_ap_mark(cb, 0); return pipeline; }
    pthread_mutex_lock(&g_ap_mu);
    int state = j->state; uint64_t real = j->real;
    pthread_mutex_unlock(&g_ap_mu);
    if (state == 0 && !klvk_ap_mark(cb, 1)) {           // nowhere to note it: wait for it
        pthread_mutex_lock(&g_ap_mu);
        while (!j->state) pthread_cond_wait(&g_ap_done, &g_ap_mu);
        state = j->state; real = j->real;
        pthread_mutex_unlock(&g_ap_mu);
    }
    if (state == 1 && real) { klvk_ap_mark(cb, 0); return real; }
    if (state != 0) klvk_ap_mark(cb, 1);                // failed or destroyed: nothing to draw with, ever
    *drop = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// VK_EXT_vertex_input_dynamic_state, emulated (KL_VK_VI_EMU, default on; it only
// ever touches pipelines that name VK_DYNAMIC_STATE_VERTEX_INPUT_EXT).
//
// HITMAN 3 VR: Reloaded (x8012387035448196, IOI's Glacier on a Quest) creates
// every graphics pipeline WITHOUT a vertex input state and sets the layout per
// draw with vkCmdSetVertexInputEXT — without even enabling the extension, which
// the Quest's driver has and MoltenVK does not. MoltenVK built the Metal
// pipeline descriptor from the missing state and faulted
// (MVKGraphicsPipeline::initShaderConversionConfig, Mac run 2026-10-20).
//
// So such a pipeline is not compiled when it is created. The guest gets a
// handle of ours holding a copy of the create info (klvk_ap_clone, the async
// layer's deep copy) with the vertex-input dynamic state taken out; each layout
// the guest sets while it is bound is compiled once into a real pipeline (a
// variant) and bound in its place. A command buffer remembers the deferred
// pipeline and the last layout, so the variant is bound on whichever of the two
// calls comes second, and draws in between are left out (klvk_ap_mark), as for
// a pipeline still compiling. The fragment-shading-rate dynamic state goes out
// with it: MoltenVK has no shading rates either, and the guest's
// vkCmdSetFragmentShadingRateKHR is already a no-op here. Binding divisors
// other than 1 are not applied (said once). Shader modules a deferred pipeline
// uses outlive the guest's vkDestroyShaderModule until the pipeline goes, since
// a variant may be compiled long after the guest is done with its modules.
// Costs: a Metal compile on the recording thread the first time a layout meets
// a pipeline.
#define KLVK_VI_MAX   32768u
#define KLVK_VI_MAXB  16u
#define KLVK_VI_MAXA  32u
typedef struct { uint32_t nb, na, hash;
                 VkVertexInputBindingDescription b[KLVK_VI_MAXB];
                 VkVertexInputAttributeDescription a[KLVK_VI_MAXA]; } klvk_vi_layout;
typedef struct klvk_vi_var { klvk_vi_layout l; uint64_t real; struct klvk_vi_var *next; } klvk_vi_var;
typedef struct {
    VkDevice dev;
    VkGraphicsPipelineCreateInfo ci;   // owns what it points to (mem)
    klvk_ap_arena mem;
    klvk_vi_var *vars;
    unsigned nvars;
    pthread_mutex_t mu;
} klvk_vipipe;
static klvk_vipipe *g_vi_slot[KLVK_VI_MAX];
static unsigned g_vi_next, g_vi_live;
static pthread_mutex_t g_vi_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_vi_internal;     // klvk_CmdBindPipeline called by the emulation itself

static int klvk_vi_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_VI_EMU", 1);
    return on;
}
static int klvk_vi_dynamic(const VkGraphicsPipelineCreateInfo *ci) {
    if (!ci->pDynamicState) return 0;
    for (uint32_t k = 0; k < ci->pDynamicState->dynamicStateCount; k++)
        if (ci->pDynamicState->pDynamicStates[k] == VK_DYNAMIC_STATE_VERTEX_INPUT_EXT) return 1;
    return 0;
}
static klvk_vipipe *klvk_vi_from(uint64_t h) {
    uintptr_t p = (uintptr_t)h, b = (uintptr_t)g_vi_slot;
    if (p < b || p >= b + sizeof g_vi_slot || (p - b) % sizeof *g_vi_slot) return NULL;
    return *(klvk_vipipe **)p;
}

// Shader modules held for deferred pipelines: count, and whether the guest has
// already destroyed it (then it goes with its last pipeline).
#define KLVK_VI_MODS 8192u
static struct { uint64_t mod; unsigned refs; int dead; VkDevice dev; } g_vi_mod[KLVK_VI_MODS];
static void klvk_vi_mod_ref(VkDevice dev, uint64_t mod, int delta) {
    if (!mod) return;
    uint64_t gone = 0;
    pthread_mutex_lock(&g_vi_mu);
    uint32_t i = (uint32_t)((mod >> 4) * 2654435761u) % KLVK_VI_MODS, free_at = KLVK_VI_MODS;
    for (uint32_t n = 0; n < KLVK_VI_MODS; n++, i = (i + 1) % KLVK_VI_MODS) {
        if (g_vi_mod[i].mod == mod) { free_at = i; break; }
        if (!g_vi_mod[i].mod) { if (delta > 0) free_at = i; break; }
    }
    if (free_at < KLVK_VI_MODS) {
        if (!g_vi_mod[free_at].mod) { g_vi_mod[free_at].mod = mod; g_vi_mod[free_at].dev = dev; }
        if (delta > 0) g_vi_mod[free_at].refs++;
        else if (g_vi_mod[free_at].refs && !--g_vi_mod[free_at].refs && g_vi_mod[free_at].dead) {
            gone = mod; dev = g_vi_mod[free_at].dev;
            g_vi_mod[free_at].dead = 0;   // the slot stays (probe chains), refs 0 and not dead
        }
    }
    pthread_mutex_unlock(&g_vi_mu);
    if (gone && real_gdpa) {
        PFN_vkDestroyShaderModule d = (PFN_vkDestroyShaderModule)real_gdpa(dev, "vkDestroyShaderModule");
        if (d) d(dev, (VkShaderModule)(uintptr_t)gone, NULL);
    }
}
// 1 if the module is held: the guest's destroy is put off.
static int klvk_vi_mod_hold(uint64_t mod) {
    if (!mod || !__atomic_load_n(&g_vi_live, __ATOMIC_RELAXED)) return 0;
    int held = 0;
    pthread_mutex_lock(&g_vi_mu);
    uint32_t i = (uint32_t)((mod >> 4) * 2654435761u) % KLVK_VI_MODS;
    for (uint32_t n = 0; n < KLVK_VI_MODS && g_vi_mod[i].mod; n++, i = (i + 1) % KLVK_VI_MODS)
        if (g_vi_mod[i].mod == mod) { if (g_vi_mod[i].refs) { g_vi_mod[i].dead = 1; held = 1; } break; }
    pthread_mutex_unlock(&g_vi_mu);
    return held;
}

static VkResult klvk_vi_defer(VkDevice dev, const VkGraphicsPipelineCreateInfo *ci, VkPipeline *out) {
    klvk_vipipe *p = calloc(1, sizeof *p);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!klvk_ap_clone(&p->mem, ci, &p->ci)) {
        klvk_ap_free(&p->mem); free(p);
        static int said;
        if (said++ < 8) VKI("dynamic vertex input: a pipeline carries something the copy does not know — "
                            "it is compiled as it is and will likely fail\n");
        return klvk_CreateGraphicsPipelines0(dev, VK_NULL_HANDLE, 1, ci, NULL, out);
    }
    // Out with the two dynamic states nothing here can honour.
    VkPipelineDynamicStateCreateInfo *d = (VkPipelineDynamicStateCreateInfo *)p->ci.pDynamicState;
    if (d && d->pDynamicStates) {
        VkDynamicState *ds = (VkDynamicState *)d->pDynamicStates;
        uint32_t k = 0;
        for (uint32_t i = 0; i < d->dynamicStateCount; i++)
            if (ds[i] != VK_DYNAMIC_STATE_VERTEX_INPUT_EXT &&
                ds[i] != VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR) ds[k++] = ds[i];
        d->dynamicStateCount = k;
    }
    p->ci.pVertexInputState = NULL;
    p->dev = dev;
    pthread_mutex_init(&p->mu, NULL);
    pthread_mutex_lock(&g_vi_mu);
    unsigned i, n;
    for (n = 0, i = g_vi_next; n < KLVK_VI_MAX && g_vi_slot[i]; n++) i = (i + 1) % KLVK_VI_MAX;
    if (n < KLVK_VI_MAX) { g_vi_slot[i] = p; g_vi_next = (i + 1) % KLVK_VI_MAX; g_vi_live++; }
    unsigned live = g_vi_live;
    pthread_mutex_unlock(&g_vi_mu);
    if (n == KLVK_VI_MAX) {
        klvk_ap_free(&p->mem); free(p);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    for (uint32_t s = 0; s < p->ci.stageCount; s++)
        klvk_vi_mod_ref(dev, (uint64_t)(uintptr_t)p->ci.pStages[s].module, +1);
    *out = (VkPipeline)(uintptr_t)&g_vi_slot[i];
    if (live == 1 || !(live % 500))
        VKI("dynamic vertex input: %u pipeline(s) waiting for their vertex layout "
            "(VK_EXT_vertex_input_dynamic_state, emulated)\n", live);
    return VK_SUCCESS;
}

static uint32_t klvk_vi_hash(const klvk_vi_layout *l) {
    uint32_t h = klvk_fnv(l->b, l->nb * sizeof *l->b);
    return h ^ (klvk_fnv(l->a, l->na * sizeof *l->a) * 16777619u) ^ (l->nb << 8) ^ l->na;
}
static int klvk_vi_same(const klvk_vi_layout *x, const klvk_vi_layout *y) {
    return x->hash == y->hash && x->nb == y->nb && x->na == y->na &&
           !memcmp(x->b, y->b, x->nb * sizeof *x->b) && !memcmp(x->a, y->a, x->na * sizeof *x->a);
}
// An integer vertex format the vertex shader reads as float: its SCALED twin.
// SPIRV-Cross refuses the pair outright ("Vertex attribute type mismatch
// between host and shader"); Vulkan calls it undefined, and a GL-minded engine
// means the integer value as a number — which is what a SCALED format hands the
// shader (Metal converts uchar/ushort to float). 32-bit integers have no SCALED
// format and are left as they are. HITMAN 3 VR: Reloaded, headset 2026-10-20:
// nine pipelines of its world refused this way.
// Integer formats as the normalized ones of the same layout. Scaleform's vertex
// shaders read their per-vertex factor (R8G8B8A8_UINT in HITMAN 3 VR: Reloaded's
// batches) as a float, use .z * 255.01 as the index of the batch's matrix and .w
// as an alpha factor: 0..1, i.e. what the Quest's driver gave them. Made SCALED
// (0..255) the index ran far past the matrix table and the batch was not drawn —
// a board in the tutorial missing in blocks (headset, 2026-10-21).
static VkFormat klvk_vi_norm(VkFormat f) {
    switch ((int)f) {
    case VK_FORMAT_R8_UINT: return VK_FORMAT_R8_UNORM;              case VK_FORMAT_R8_SINT: return VK_FORMAT_R8_SNORM;
    case VK_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_UNORM;          case VK_FORMAT_R8G8_SINT: return VK_FORMAT_R8G8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8_UNORM;      case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8_SNORM;
    case VK_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_UNORM;  case VK_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_B8G8R8A8_UINT: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case VK_FORMAT_R16_UINT: return VK_FORMAT_R16_UNORM;            case VK_FORMAT_R16_SINT: return VK_FORMAT_R16_SNORM;
    case VK_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_UNORM;      case VK_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SNORM;
    case VK_FORMAT_R16G16B16A16_UINT: return VK_FORMAT_R16G16B16A16_UNORM; case VK_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}
static VkFormat klvk_vi_scaled(VkFormat f) {
    switch ((int)f) {
    case VK_FORMAT_R8_UINT: return VK_FORMAT_R8_USCALED;            case VK_FORMAT_R8_SINT: return VK_FORMAT_R8_SSCALED;
    case VK_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_USCALED;        case VK_FORMAT_R8G8_SINT: return VK_FORMAT_R8G8_SSCALED;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8_USCALED;    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8_SSCALED;
    case VK_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_USCALED; case VK_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SSCALED;
    case VK_FORMAT_B8G8R8A8_UINT: return VK_FORMAT_B8G8R8A8_USCALED; case VK_FORMAT_B8G8R8A8_SINT: return VK_FORMAT_B8G8R8A8_SSCALED;
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    case VK_FORMAT_A2B10G10R10_SINT_PACK32: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    case VK_FORMAT_R16_UINT: return VK_FORMAT_R16_USCALED;          case VK_FORMAT_R16_SINT: return VK_FORMAT_R16_SSCALED;
    case VK_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_USCALED;    case VK_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SSCALED;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16_USCALED; case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16_SSCALED;
    case VK_FORMAT_R16G16B16A16_UINT: return VK_FORMAT_R16G16B16A16_USCALED; case VK_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SSCALED;
    default: return VK_FORMAT_UNDEFINED;
    }
}
// ...and the other way round: a float, normalized or scaled format the shader
// reads as int or uint gets the integer format of the same size, which hands
// the shader the stored bits — what a Quest's vertex fetch does (the reasoning
// of KL_VK_VIN_INT). HITMAN 3 VR: Reloaded's skinned meshes read their bone
// indices (in_weightIndices, uint4) from R8G8B8A8_UNORM; Metal refused
// "uint4 cannot be read using MTLAttributeFormatUChar4Normalized" (Mac 2026-10-20).
static VkFormat klvk_vi_integer(VkFormat f, int sint) {
#define KLVK_VI_I(a, b, c, d, ui, si) case a: case b: case c: case d: return sint ? si : ui;
    switch ((int)f) {
    KLVK_VI_I(VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_USCALED, VK_FORMAT_R8_SSCALED, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT)
    KLVK_VI_I(VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT)
    KLVK_VI_I(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT)
    KLVK_VI_I(VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_USCALED, VK_FORMAT_R16_SSCALED, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT)
    KLVK_VI_I(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT)
    KLVK_VI_I(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT)
    case VK_FORMAT_R16_SFLOAT:          return sint ? VK_FORMAT_R16_SINT : VK_FORMAT_R16_UINT;
    case VK_FORMAT_R16G16_SFLOAT:       return sint ? VK_FORMAT_R16G16_SINT : VK_FORMAT_R16G16_UINT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return sint ? VK_FORMAT_R16G16B16A16_SINT : VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R32_SFLOAT:          return sint ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
    case VK_FORMAT_R32G32_SFLOAT:       return sint ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT;
    case VK_FORMAT_R32G32B32_SFLOAT:    return sint ? VK_FORMAT_R32G32B32_SINT : VK_FORMAT_R32G32B32_UINT;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return sint ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
    default: return VK_FORMAT_UNDEFINED;
    }
#undef KLVK_VI_I
}
// The vertex stage's input types (klvk_vin_*; recorded while KL_VK_VIN_FILL is on).
static int klvk_vi_inputs(const klvk_vipipe *p, klvk_vin_ent *in) {
    for (uint32_t k = 0; k < p->ci.stageCount; k++)
        if (p->ci.pStages[k].stage == VK_SHADER_STAGE_VERTEX_BIT)
            return klvk_vin_get(g_vin_mod, (uint64_t)(uintptr_t)p->ci.pStages[k].module, in);
    return 0;
}

// The variant of a deferred pipeline for one layout, compiled on first use.
static uint64_t klvk_vi_variant(klvk_vipipe *p, const klvk_vi_layout *l) {
    pthread_mutex_lock(&p->mu);
    for (klvk_vi_var *v = p->vars; v; v = v->next)
        if (klvk_vi_same(&v->l, l)) { uint64_t r = v->real; pthread_mutex_unlock(&p->mu); return r; }
    // The layout as compiled: integer formats the shader reads as float made SCALED.
    VkVertexInputAttributeDescription attrs[KLVK_VI_MAXA];
    memcpy(attrs, l->a, l->na * sizeof *attrs);
    klvk_vin_ent in = {0};
    int have_in = klvk_vi_inputs(p, &in);
    // KL_VK_VI_SF_NORM=0: Scaleform's integers read as floats are SCALED like any other.
    static int sf_on = -1;
    if (sf_on < 0) sf_on = kl_env_on("KL_VK_VI_SF_NORM", 1);
    int sf = 0;
    for (uint32_t k = 0; sf_on && k < p->ci.stageCount; k++)
        if (p->ci.pStages[k].stage == VK_SHADER_STAGE_VERTEX_BIT &&
            klvk_handle_set_has(g_sf_mod, (uint64_t)(uintptr_t)p->ci.pStages[k].module)) sf = 1;
    unsigned scaled = 0;
    for (uint32_t i = 0; have_in && i < l->na; i++) {
        if (attrs[i].location >= 32) continue;
        uint32_t bit = 1u << attrs[i].location;
        VkFormat sc = VK_FORMAT_UNDEFINED;
        if (in.f & bit) sc = sf ? klvk_vi_norm(attrs[i].format) : klvk_vi_scaled(attrs[i].format);
        else if (in.u & bit) sc = klvk_vi_integer(attrs[i].format, 0);
        else if (in.i & bit) sc = klvk_vi_integer(attrs[i].format, 1);
        if (sc != VK_FORMAT_UNDEFINED) { attrs[i].format = sc; scaled++; }
    }
    // KL_VK_VI_TRACE_FS=<hash>,...: every variant of a pipeline with one of these
    // fragment shaders says its layout — each attribute's location, binding,
    // offset, the format the guest gave and the one compiled — and what its
    // vertex shader reads as float/int/uint.
    {
        static uint32_t list[16];
        static int ln = -1;
        if (ln < 0) {
            ln = 0;
            const char *e = kl_env_str("KL_VK_VI_TRACE_FS", NULL);
            while (e && *e && ln < 16) {
                char *end = NULL;
                unsigned long v = strtoul(e, &end, 16);
                if (end == e) break;
                list[ln++] = (uint32_t)v;
                e = *end == ',' ? end + 1 : end;
            }
        }
        uint32_t fh = 0;
        for (uint32_t s = 0; ln && s < p->ci.stageCount; s++)
            if (p->ci.pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                fh = klvk_fs_get(g_fs_mod, (uint64_t)(uintptr_t)p->ci.pStages[s].module);
        for (int i = 0; fh && i < ln; i++)
            if (list[i] == fh) {
                char d[700]; size_t o = 0; d[0] = 0;
                for (uint32_t k = 0; k < l->na && o + 64 < sizeof d; k++)
                    o += (size_t)snprintf(d + o, sizeof d - o, " loc%u b%u+%u fmt %d->%d", l->a[k].location, l->a[k].binding,
                                          l->a[k].offset, (int)l->a[k].format, (int)attrs[k].format);
                char b[200]; size_t ob = 0; b[0] = 0;
                for (uint32_t k = 0; k < l->nb && ob + 40 < sizeof b; k++)
                    ob += (size_t)snprintf(b + ob, sizeof b - ob, " b%u stride %u rate %d", l->b[k].binding, l->b[k].stride, (int)l->b[k].inputRate);
                VKI("vi trace %08x:%s |%s | reads float %#x int %#x uint %#x\n", fh, d, b, in.f, in.i, in.u);
                break;
            }
    }
    if (scaled) {
        static unsigned said;
        if (said++ < 4 || !(said % 100))
            VKI("dynamic vertex input: %u attribute(s) given in the format the shader reads them as "
                "(integer read as float: SCALED; float/normalized read as integer: the integer format) "
                "(%u variant(s) so far)\n", scaled, said);
    }
    VkPipelineVertexInputStateCreateInfo vis = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = l->nb, .pVertexBindingDescriptions = l->b,
        .vertexAttributeDescriptionCount = l->na, .pVertexAttributeDescriptions = attrs };
    VkGraphicsPipelineCreateInfo c = p->ci;
    c.pVertexInputState = &vis;
    // KL_VK_NO_STENCIL_FS=<hash>,...: variants of pipelines whose fragment shader
    // has one of these hashes are compiled with the stencil test off. A test for
    // HITMAN 3 VR: Reloaded's Scaleform board, drawn through a stencil mask and
    // missing in screen-aligned blocks on the headset (2026-10-21).
    VkPipelineDepthStencilStateCreateInfo ds_nost;
    {
        static uint32_t list[16];
        static int ln = -1;
        if (ln < 0) {
            ln = 0;
            const char *e = kl_env_str("KL_VK_NO_STENCIL_FS", NULL);
            while (e && *e && ln < 16) {
                char *end = NULL;
                unsigned long v = strtoul(e, &end, 16);
                if (end == e) break;
                list[ln++] = (uint32_t)v;
                e = *end == ',' ? end + 1 : end;
            }
        }
        if (ln && c.pDepthStencilState && c.pDepthStencilState->stencilTestEnable) {
            uint32_t fh = 0;
            for (uint32_t s = 0; s < c.stageCount; s++)
                if (c.pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                    fh = klvk_fs_get(g_fs_mod, (uint64_t)(uintptr_t)c.pStages[s].module);
            for (int i = 0; i < ln; i++)
                if (fh && list[i] == fh) {
                    ds_nost = *c.pDepthStencilState;
                    ds_nost.stencilTestEnable = VK_FALSE;
                    c.pDepthStencilState = &ds_nost;
                    static int said;
                    if (said++ < 4) VKI("KL_VK_NO_STENCIL_FS: a variant of %08x without its stencil test\n", fh);
                    break;
                }
        }
    }
    VkPipeline real = VK_NULL_HANDLE;
    double t0 = kl_pipe_ms();
    // Behind the frame when KL_VK_ASYNC_PIPES asks for it: this is where such a
    // guest's compiles really happen — its vkCreateGraphicsPipelines only made
    // the handle above — so the switch never reached them, and on the headset
    // each new variant held the picture 0.3-0.9 s (HITMAN 3 VR: Reloaded,
    // 2026-10-20). klvk_ap_create answers with the pipeline, or with a handle of
    // its own that klvk_ap_bind resolves (dropping the draws until it is there),
    // and applies its own rule of which pipelines qualify. A pipeline with a
    // logic op stays in place: that op reaches Metal through one flag set around
    // the compile (kl_mtl_logic_op_next), which compiles on other threads would
    // share.
    int lop = c.pColorBlendState && c.pColorBlendState->logicOpEnable;
    VkResult r = klvk_ap_mode() && !lop
        ? klvk_ap_create(p->dev, VK_NULL_HANDLE, 1, &c, NULL, &real)
        : klvk_CreateGraphicsPipelines0(p->dev, VK_NULL_HANDLE, 1, &c, NULL, &real);
    double ms = kl_pipe_ms() - t0;
    klvk_vi_var *v = calloc(1, sizeof *v);
    if (v) { v->l = *l; v->real = r == VK_SUCCESS ? (uint64_t)(uintptr_t)real : 0; v->next = p->vars; p->vars = v; p->nvars++; }
    pthread_mutex_unlock(&p->mu);
    static unsigned made, failed;
    unsigned m = __atomic_add_fetch(&made, 1, __ATOMIC_RELAXED);
    if (r != VK_SUCCESS) {
        unsigned f = __atomic_add_fetch(&failed, 1, __ATOMIC_RELAXED);
        if (f <= 12) {
            char desc[512]; size_t o = 0; desc[0] = 0;
            for (uint32_t i = 0; i < l->na && o + 40 < sizeof desc; i++)
                o += (size_t)snprintf(desc + o, sizeof desc - o, " loc%u:fmt%d@b%u+%u",
                                      attrs[i].location, (int)attrs[i].format, attrs[i].binding, attrs[i].offset);
            VKI("dynamic vertex input: a variant (%u bindings, %u attributes) was refused (%d);%s; "
                "shader reads float %#x int %#x uint %#x%s\n", l->nb, l->na, (int)r, desc,
                in.f, in.i, in.u, have_in ? "" : " (types unknown)");
        }
    }
    if (m <= 3 || !(m % 200))
        VKI("dynamic vertex input: %u variant(s) compiled so far (this one %.1f ms, %u bindings, %u attributes)\n",
            m, ms, l->nb, l->na);
    return v ? v->real : 0;
}

// Per command buffer: the deferred pipeline bound and the last layout set.
#define KLVK_VI_CBS 4096u
typedef struct { uint64_t cb; klvk_vipipe *pipe; int have; klvk_vi_layout l; } klvk_vi_cb;
static klvk_vi_cb g_vi_cb[KLVK_VI_CBS];
static klvk_vi_cb *klvk_vi_cb_of(void *cb, int add) {
    uint64_t h = (uint64_t)(uintptr_t)cb;
    uint32_t i = (uint32_t)((h >> 4) * 2654435761u) % KLVK_VI_CBS;
    for (uint32_t n = 0; n < KLVK_VI_CBS; n++, i = (i + 1) % KLVK_VI_CBS) {
        uint64_t v = __atomic_load_n(&g_vi_cb[i].cb, __ATOMIC_ACQUIRE);
        if (v == h) return &g_vi_cb[i];
        if (!v) {
            if (!add) return NULL;
            uint64_t none = 0;
            if (__atomic_compare_exchange_n(&g_vi_cb[i].cb, &none, h, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return &g_vi_cb[i];
            if (none == h) return &g_vi_cb[i];
        }
    }
    static int said;
    if (!said++) VKI("dynamic vertex input: the command-buffer table is full — draws on further "
                     "command buffers are left out\n");
    return NULL;
}
static void klvk_vi_begin(void *cb) {
    klvk_vi_cb *e = klvk_vi_cb_of(cb, 0);
    if (e) { e->pipe = NULL; e->have = 0; }
}
static void VKAPI_CALL klvk_CmdBindPipeline(void *cb, uint32_t bindPoint, uint64_t pipeline);
// Bind the variant for what this command buffer has, if it has both.
static void klvk_vi_settle(void *cb, klvk_vi_cb *e) {
    if (!e || !e->pipe) return;
    if (!e->have) { klvk_ap_mark(cb, 1); return; }    // no layout yet: its draws are left out
    uint64_t real = klvk_vi_variant(e->pipe, &e->l);
    if (!real) { klvk_ap_mark(cb, 1); return; }
    t_vi_internal = 1;
    klvk_CmdBindPipeline(cb, 0 /* graphics */, real);
    t_vi_internal = 0;
}
static void VKAPI_CALL klvk_CmdSetVertexInputEXT(void *cb,
        uint32_t nb, const VkVertexInputBindingDescription2EXT *b,
        uint32_t na, const VkVertexInputAttributeDescription2EXT *a) {
    klvk_vi_cb *e = klvk_vi_cb_of(cb, 1);
    if (!e) return;
    if (nb > KLVK_VI_MAXB || na > KLVK_VI_MAXA) {
        static int said;
        if (!said++) VKI("dynamic vertex input: a layout of %u bindings / %u attributes is beyond %u / %u — "
                         "its draws are left out\n", nb, na, KLVK_VI_MAXB, KLVK_VI_MAXA);
        e->have = 0;
        klvk_ap_mark(cb, 1);
        return;
    }
    klvk_vi_layout l;
    memset(&l, 0, sizeof l);
    l.nb = nb; l.na = na;
    for (uint32_t i = 0; i < nb; i++) {
        l.b[i].binding = b[i].binding; l.b[i].stride = b[i].stride; l.b[i].inputRate = b[i].inputRate;
        if (b[i].divisor != 1) { static int said; if (!said++)
            VKI("dynamic vertex input: binding divisor %u is not applied\n", b[i].divisor); }
    }
    for (uint32_t i = 0; i < na; i++) {
        l.a[i].location = a[i].location; l.a[i].binding = a[i].binding;
        l.a[i].format = a[i].format; l.a[i].offset = a[i].offset;
    }
    l.hash = klvk_vi_hash(&l);
    if (e->have && klvk_vi_same(&e->l, &l)) return;    // the same again: nothing changes
    e->l = l; e->have = 1;
    klvk_vi_settle(cb, e);
}
// From klvk_CmdBindPipeline: 1 if the pipeline was one of ours (handled here).
static int klvk_vi_bind(void *cb, uint32_t bindPoint, uint64_t pipeline) {
    if (t_vi_internal || bindPoint != 0) return 0;
    klvk_vipipe *p = __atomic_load_n(&g_vi_live, __ATOMIC_RELAXED) ? klvk_vi_from(pipeline) : NULL;
    if (!p) {
        klvk_vi_cb *e = __atomic_load_n(&g_vi_live, __ATOMIC_RELAXED) ? klvk_vi_cb_of(cb, 0) : NULL;
        if (e) e->pipe = NULL;
        return 0;
    }
    klvk_vi_cb *e = klvk_vi_cb_of(cb, 1);
    if (!e) { klvk_ap_mark(cb, 1); return 1; }
    e->pipe = p;
    klvk_vi_settle(cb, e);
    return 1;
}
// From klvk_DestroyPipeline: 1 if it was one of ours (its variants are destroyed).
static void VKAPI_CALL klvk_DestroyPipeline(VkDevice dev, uint64_t pipeline, const VkAllocationCallbacks *alloc);
static int klvk_vi_destroy(VkDevice dev, uint64_t pipeline,
                           void (*real)(VkDevice, uint64_t, const VkAllocationCallbacks *)) {
    klvk_vipipe *p = klvk_vi_from(pipeline);
    if (!p) return 0;
    pthread_mutex_lock(&g_vi_mu);
    *(klvk_vipipe **)(uintptr_t)pipeline = NULL;
    g_vi_live--;
    pthread_mutex_unlock(&g_vi_mu);
    for (klvk_vi_var *v = p->vars, *nx; v; v = nx) {
        nx = v->next;
        if (v->real && klvk_ap_from(v->real)) klvk_DestroyPipeline(dev, v->real, NULL);   // compiled behind the frame
        else if (v->real && real) real(dev, v->real, NULL);
        free(v);
    }
    for (uint32_t s = 0; s < p->ci.stageCount; s++)
        klvk_vi_mod_ref(dev, (uint64_t)(uintptr_t)p->ci.pStages[s].module, -1);
    klvk_ap_free(&p->mem);
    pthread_mutex_destroy(&p->mu);
    free(p);
    return 1;
}

static void VKAPI_CALL klvk_DestroyPipeline(VkDevice dev, uint64_t pipeline, const VkAllocationCallbacks *alloc) {
    static void (*real)(VkDevice, uint64_t, const VkAllocationCallbacks *);
    if (!real && real_gdpa) real = (void (*)(VkDevice, uint64_t, const VkAllocationCallbacks *))
        real_gdpa(dev, "vkDestroyPipeline");
    klvk_apipe *j = klvk_ap_from(pipeline);
    if (j) {
        pthread_mutex_lock(&g_ap_mu);
        while (!j->state) pthread_cond_wait(&g_ap_done, &g_ap_mu);
        pipeline = j->real; j->real = 0; j->state = 3;
        pthread_mutex_unlock(&g_ap_mu);
        if (!pipeline) return;
    }
    if (klvk_vi_destroy(dev, pipeline, real)) return;
    if (__atomic_load_n(&g_vin_pipes, __ATOMIC_RELAXED)) klvk_vin_del(g_vin_pipe, pipeline);
    if (real) real(dev, pipeline, alloc);
}
static void VKAPI_CALL klvk_DestroyShaderModule(VkDevice dev, uint64_t mod, const VkAllocationCallbacks *alloc) {
    static void (*real)(VkDevice, uint64_t, const VkAllocationCallbacks *);
    if (!real && real_gdpa) real = (void (*)(VkDevice, uint64_t, const VkAllocationCallbacks *))
        real_gdpa(dev, "vkDestroyShaderModule");
    klvk_ap_drain();
    if (klvk_vi_mod_hold(mod)) return;   // a deferred pipeline still needs it, and its input types (klvk_vi_*)
    if (klvk_vin_on()) klvk_vin_del(g_vin_mod, mod);
    if (real) real(dev, mod, alloc);
}
static void VKAPI_CALL klvk_DestroyPipelineLayout(VkDevice dev, uint64_t layout, const VkAllocationCallbacks *alloc) {
    static void (*real)(VkDevice, uint64_t, const VkAllocationCallbacks *);
    if (!real && real_gdpa) real = (void (*)(VkDevice, uint64_t, const VkAllocationCallbacks *))
        real_gdpa(dev, "vkDestroyPipelineLayout");
    klvk_ap_drain();
    if (real) real(dev, layout, alloc);
}
// A name for one of our handles is a name for nothing MoltenVK knows.
static VkResult VKAPI_CALL klvk_SetDebugUtilsObjectNameEXT(VkDevice dev, const VkDebugUtilsObjectNameInfoEXT *info) {
    static VkResult (*real)(VkDevice, const VkDebugUtilsObjectNameInfoEXT *);
    if (!real && real_gdpa) real = (VkResult (*)(VkDevice, const VkDebugUtilsObjectNameInfoEXT *))
        real_gdpa(dev, "vkSetDebugUtilsObjectNameEXT");
    if (!real || (info && klvk_ap_from(info->objectHandle))) return VK_SUCCESS;
    return real(dev, info);
}
static VkResult VKAPI_CALL klvk_SetDebugUtilsObjectTagEXT(VkDevice dev, const VkDebugUtilsObjectTagInfoEXT *info) {
    static VkResult (*real)(VkDevice, const VkDebugUtilsObjectTagInfoEXT *);
    if (!real && real_gdpa) real = (VkResult (*)(VkDevice, const VkDebugUtilsObjectTagInfoEXT *))
        real_gdpa(dev, "vkSetDebugUtilsObjectTagEXT");
    if (!real || (info && klvk_ap_from(info->objectHandle))) return VK_SUCCESS;
    return real(dev, info);
}
static VkResult VKAPI_CALL klvk_DebugMarkerSetObjectNameEXT(VkDevice dev, const VkDebugMarkerObjectNameInfoEXT *info) {
    static VkResult (*real)(VkDevice, const VkDebugMarkerObjectNameInfoEXT *);
    if (!real && real_gdpa) real = (VkResult (*)(VkDevice, const VkDebugMarkerObjectNameInfoEXT *))
        real_gdpa(dev, "vkDebugMarkerSetObjectNameEXT");
    if (!real || (info && klvk_ap_from(info->object))) return VK_SUCCESS;
    return real(dev, info);
}
static VkResult VKAPI_CALL klvk_DebugMarkerSetObjectTagEXT(VkDevice dev, const VkDebugMarkerObjectTagInfoEXT *info) {
    static VkResult (*real)(VkDevice, const VkDebugMarkerObjectTagInfoEXT *);
    if (!real && real_gdpa) real = (VkResult (*)(VkDevice, const VkDebugMarkerObjectTagInfoEXT *))
        real_gdpa(dev, "vkDebugMarkerSetObjectTagEXT");
    if (!real || (info && klvk_ap_from(info->object))) return VK_SUCCESS;
    return real(dev, info);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
typedef struct { const char *name; PFN_vkVoidFunction fn; } entry;

// --- KL_TRACE: framebuffer + render-pass census (what image is rendered INTO) ---
#define KLVK_FB_MAP_MAX 4096
static struct { VkFramebuffer fb; VkImage img0; unsigned att; } g_fb_map[KLVK_FB_MAP_MAX];
static int g_fb_map_n;
static pthread_mutex_t g_fb_map_lock = PTHREAD_MUTEX_INITIALIZER;
static VkImage klvk_fb_image0(VkFramebuffer fb) {
    VkImage img = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_fb_map_lock);
    for (int i = g_fb_map_n - 1; i >= 0; i--) if (g_fb_map[i].fb == fb) { img = g_fb_map[i].img0; break; }
    pthread_mutex_unlock(&g_fb_map_lock);
    return img;
}
static VkResult VKAPI_CALL klvk_CreateFramebuffer(VkDevice dev,
        const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *a,
        VkFramebuffer *out) {
    static PFN_vkCreateFramebuffer real;
    if (!real) real = (PFN_vkCreateFramebuffer)mvk_sym("vkCreateFramebuffer");
    VkResult r = real ? real(dev, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && ci && out) {
        VkImage img0 = VK_NULL_HANDLE;
        if (ci->attachmentCount && ci->pAttachments) img0 = klvk_view_image(ci->pAttachments[0]);
        pthread_mutex_lock(&g_fb_map_lock);
        if (g_fb_map_n < KLVK_FB_MAP_MAX) {
            g_fb_map[g_fb_map_n].fb = *out; g_fb_map[g_fb_map_n].img0 = img0;
            g_fb_map[g_fb_map_n].att = ci->attachmentCount; g_fb_map_n++;
        }
        pthread_mutex_unlock(&g_fb_map_lock);
        fove_note_fb(*out, ci);
        // KL_RT_DEBUG: remember the latest substantial UI render target (the guest
        // renders its menu pieces into these; the compositing that would gather
        // them into the submitted panel fails, so this is where the pixels are).
        if (img0 && !klvk_is_eye_image(img0) && ci->width >= 480 && ci->height >= 240) {
            uint32_t area = ci->width * ci->height;
            if (area >= g_content_rt_area) {   // keep the biggest — the main menu panel is the most stable
                g_content_rt = img0; g_content_rt_w = ci->width; g_content_rt_h = ci->height; g_content_rt_area = area;
            }
        }
        if (klvk_trace()) {
            klvk_trace_log("[fb] create %#llx att=%u %ux%u\n",
                (unsigned long long)(uintptr_t)*out, ci->attachmentCount, ci->width, ci->height);
            for (uint32_t i = 0; i < ci->attachmentCount && ci->pAttachments; i++) {
                VkImage im = klvk_view_image(ci->pAttachments[i]);
                klvk_trace_log("[fb]   att%u view %#llx -> img %#llx (%s k=%d)\n",
                    i, (unsigned long long)(uintptr_t)ci->pAttachments[i],
                    (unsigned long long)(uintptr_t)im, klvk_img_tag(im), klvk_layer_image_key(im));
            }
        }
    }
    return r;
}
// --- wanderer eye-resolve discriminator + MSAA-store candidate fix ---
// The eye scene pass is a standard MSAA MULTIVIEW resolve (att0 MSAA colour ->
// att1 single-sample eye array layer, viewMask 0x3) yet the eye reads black; the
// resolve-target storeOp is already STORE, so that is not the lever. Two gates,
// both default-ON for wanderer, both env-overridable:
//  - KL_VK_EYE_RESOLVE_PROBE: bright-magenta CLEAR the eye resolve target so the
//    next run reads out "resolve never landed" (stays magenta) vs "scene empty"
//    (goes black) vs "fixed" (renders).
//  - KL_VK_MSAA_STORE: force the MSAA colour SOURCE (att0) storeOp DONT_CARE->STORE.
//    A memoryless DONT_CARE MSAA source resolves via a bare MultisampleResolve store
//    action; on affected MoltenVK the per-slice resolve into a non-zero multiview
//    array layer does not fire in that config — promoting to StoreAndMultisample
//    Resolve gives it real backing. Costs bandwidth only; safe.
// Wanderer begins its eye pass with vkCmdBeginRenderPass2KHR (NOT the v1 entry),
// so the clear injection MUST hook the v2 begin; the v1 hook is kept for symmetry.
static int klvk_is_wanderer(void) {
    extern const char *kl_driver_target_name(void);
    const char *t = kl_driver_target_name();
    return t && strcmp(t, "wanderer") == 0;
}
static int klvk_eye_probe_on(void) {
    static int v = -1;
    // Default OFF now: the probe already did its job — it came back BLACK (not
    // magenta) on wanderer, proving the multiview MSAA resolve DOES land and the
    // black was empty content (the MoltenVK argument-buffer material-compile bug,
    // fixed via KL_MVK_NO_ARGBUF in vk_init). Leaving it default-on would magenta-
    // clear the eye and mask the now-rendering world. Still armable via the env.
    if (v < 0) v = kl_env_on("KL_VK_EYE_RESOLVE_PROBE", 0);
    return v;
}
static int klvk_msaa_store_on(void) {
    static int v = -1;
    if (v < 0) v = kl_env_on("KL_VK_MSAA_STORE", klvk_is_wanderer());
    return v;
}
#define KLVK_CLEARPASS_MAX 128
static struct { VkRenderPass rp; uint32_t att; } g_clearpass[KLVK_CLEARPASS_MAX];
static int g_clearpass_n;
static pthread_mutex_t g_clearpass_lock = PTHREAD_MUTEX_INITIALIZER;
static void klvk_clearpass_add(VkRenderPass rp, uint32_t att) {
    if (!rp) return;
    pthread_mutex_lock(&g_clearpass_lock);
    if (g_clearpass_n < KLVK_CLEARPASS_MAX) { g_clearpass[g_clearpass_n].rp = rp;
        g_clearpass[g_clearpass_n].att = att; g_clearpass_n++; }
    pthread_mutex_unlock(&g_clearpass_lock);
}
static int klvk_clearpass_lookup(VkRenderPass rp, uint32_t *att) {
    int found = 0; if (!rp) return 0;
    pthread_mutex_lock(&g_clearpass_lock);
    for (int i = g_clearpass_n - 1; i >= 0; i--)
        if (g_clearpass[i].rp == rp) { if (att) *att = g_clearpass[i].att; found = 1; break; }
    pthread_mutex_unlock(&g_clearpass_lock);
    return found;
}
static VkClearValue *klvk_probe_inject_clears(const VkRenderPassBeginInfo *bi,
                                              VkRenderPassBeginInfo *tmp) {
    uint32_t att = 0;
    if (!bi || !klvk_clearpass_lookup(bi->renderPass, &att)) return NULL;
    uint32_t n = bi->clearValueCount > att + 1 ? bi->clearValueCount : att + 1;
    VkClearValue *cv = calloc(n, sizeof *cv);
    if (!cv) return NULL;
    if (bi->clearValueCount && bi->pClearValues)
        memcpy(cv, bi->pClearValues, bi->clearValueCount * sizeof *cv);
    cv[att].color.float32[0] = 1.0f; cv[att].color.float32[1] = 0.0f;   // bright magenta
    cv[att].color.float32[2] = 1.0f; cv[att].color.float32[3] = 1.0f;
    *tmp = *bi; tmp->clearValueCount = n; tmp->pClearValues = cv;
    return cv;
}
// ---- KL_VK_PROFILE=<frames> ------------------------------------------------
//
// Where a frame's pixels go, said every <frames> frames: how many render passes
// the guest opened, at what size and sample count, and how many of their
// attachments were LOADED at the start and STORED at the end.
//
// It exists for one question — why a pixel costs what it does on this GPU — and
// the answer is usually in exactly these numbers. An Apple GPU is a tile-based
// deferred renderer: a pass that begins with LOAD reads the whole attachment
// from memory into the tiles and one that ends with STORE writes it all back,
// every sample of it. A desktop renderer splits a frame into many passes over
// the same multisampled targets because on the hardware it was written for that
// is free; here each split is a full copy of the target in and out. "megasamples
// moved" below is that traffic, and it scales with the picture's size the way
// the frame time was measured to.
//
// Counted when a pass is RECORDED, which is per frame for an engine that
// rebuilds its command buffers each frame (Source 2 does).
static int klvk_profile(void) {
    static int v = -1;
    if (v < 0) v = kl_env_int("KL_VK_PROFILE", 0);
    return v;
}
#define KLVK_PROF_RP    1024
#define KLVK_PROF_SHAPE 48
typedef struct { VkRenderPass rp; uint32_t n; struct { int samples, load, store, fmt; } a[10]; } klvk_prof_rp;
typedef struct { uint32_t w, h; int samples, atts, loads, stores, dyn, views, resolves;
                 unsigned count; } klvk_prof_shape;
// `dyn` carries a dynamic-rendering pass's flags + 8: 1 = its draws are in
// secondary command buffers, 2 = SUSPENDING (the pass continues in the next
// command buffer), 4 = RESUMING (it continues the previous one's).
static klvk_prof_rp    g_prof_rp[KLVK_PROF_RP];
static int             g_prof_nrp;
static klvk_prof_shape g_prof_shape[KLVK_PROF_SHAPE];
static int             g_prof_nshape;
static unsigned        g_prof_passes, g_prof_frames;
static double          g_prof_wait_s, g_prof_span_s, g_prof_last;
static pthread_mutex_t g_prof_mu = PTHREAD_MUTEX_INITIALIZER;

static double klvk_prof_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static void klvk_prof_rp_note(VkRenderPass rp, uint32_t n, int idx, int fmt, int samples,
                              int load, int store) {
    pthread_mutex_lock(&g_prof_mu);
    klvk_prof_rp *e = NULL;
    for (int i = g_prof_nrp - 1; i >= 0 && !e; i--) if (g_prof_rp[i].rp == rp) e = &g_prof_rp[i];
    if (!e && g_prof_nrp < KLVK_PROF_RP) { e = &g_prof_rp[g_prof_nrp++]; e->rp = rp; e->n = 0; }
    if (e && idx < 10) {
        e->n = n < 10 ? n : 10;
        e->a[idx].fmt = fmt; e->a[idx].samples = samples;
        e->a[idx].load = load; e->a[idx].store = store;
    }
    pthread_mutex_unlock(&g_prof_mu);
}
static void klvk_prof_shape_add(uint32_t w, uint32_t h, int samples, int atts, int loads,
                                int stores, int dyn, int views, int resolves) {
    klvk_prof_shape *e = NULL;
    for (int i = 0; i < g_prof_nshape && !e; i++) {
        klvk_prof_shape *c = &g_prof_shape[i];
        if (c->w == w && c->h == h && c->samples == samples && c->atts == atts &&
            c->loads == loads && c->stores == stores && c->dyn == dyn &&
            c->views == views && c->resolves == resolves) e = c;
    }
    if (!e && g_prof_nshape < KLVK_PROF_SHAPE) {
        e = &g_prof_shape[g_prof_nshape++];
        *e = (klvk_prof_shape){ w, h, samples, atts, loads, stores, dyn, views, resolves, 0 };
    }
    if (e) e->count++;
    g_prof_passes++;
}
static void klvk_prof_pass(VkRenderPass rp, uint32_t w, uint32_t h) {
    if (!klvk_profile()) return;
    pthread_mutex_lock(&g_prof_mu);
    int samples = 0, loads = 0, stores = 0, atts = 0;
    for (int i = g_prof_nrp - 1; i >= 0; i--)
        if (g_prof_rp[i].rp == rp) {
            atts = (int)g_prof_rp[i].n;
            for (int k = 0; k < atts; k++) {
                if (g_prof_rp[i].a[k].samples > samples) samples = g_prof_rp[i].a[k].samples;
                if (g_prof_rp[i].a[k].load == 0) loads++;       // VK_ATTACHMENT_LOAD_OP_LOAD
                if (g_prof_rp[i].a[k].store == 0) stores++;     // VK_ATTACHMENT_STORE_OP_STORE
            }
            break;
        }
    klvk_prof_shape_add(w, h, samples, atts, loads, stores, 0, 1, 0);
    pthread_mutex_unlock(&g_prof_mu);
}
// What Metal has allocated for this process, in MB, as VK_EXT_memory_budget
// reports it (MoltenVK answers with MTLDevice.currentAllocatedSize). 0 if the
// question cannot be asked. On unified memory this is part of the footprint the
// system limits, so it is the split of that number into "GPU" and "the rest".
static unsigned klvk_prof_gpu_mb(void) {
    if (!real_gipa || g_ndev <= 0 || !g_devs[0]) return 0;
    PFN_vkGetPhysicalDeviceMemoryProperties2 q = (PFN_vkGetPhysicalDeviceMemoryProperties2)
        real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties2");
    if (!q) q = (PFN_vkGetPhysicalDeviceMemoryProperties2)
        real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties2KHR");
    if (!q) return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 mp;
    memset(&mp, 0, sizeof mp);
    mp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    mp.pNext = &b;
    q(g_devs[0]->phys, &mp);
    VkDeviceSize used = 0;
    for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
        used += b.heapUsage[i];
    return (unsigned)(used >> 20);
}
// Once per guest frame, around the wait for its GPU work.
static void klvk_prof_frame(double wait_s) {
    int every = klvk_profile();
    if (!every) return;
    pthread_mutex_lock(&g_prof_mu);
    double now = klvk_prof_now();
    if (g_prof_last > 0) g_prof_span_s += now - g_prof_last;
    g_prof_last = now;
    g_prof_wait_s += wait_s;
    if (++g_prof_frames >= (unsigned)every) {
        double f = (double)g_prof_frames;
        fprintf(stderr, "  [vk] PROFILE over %u frames: %.1f ms a frame, of which %.1f ms waiting "
                        "for the GPU at frame end; %.1f render passes a frame\n",
                g_prof_frames, g_prof_span_s * 1000.0 / f, g_prof_wait_s * 1000.0 / f,
                (double)g_prof_passes / f);
        double total = 0;
        for (int i = 0; i < g_prof_nshape; i++) {
            klvk_prof_shape *c = &g_prof_shape[i];
            double ms = (double)c->w * c->h * (c->samples ? c->samples : 1) / 1e6;
            total += ms * (c->loads + c->stores) * c->count / f;
        }
        for (int i = 0; i < g_prof_nshape; i++) {
            klvk_prof_shape *c = &g_prof_shape[i];
            double ms = (double)c->w * c->h * (c->samples ? c->samples : 1) / 1e6;
            double moved = ms * (c->loads + c->stores) * c->count / f;
            if (c->count / f < 0.05) continue;
            fprintf(stderr, "  [vk]   %5.1f x a frame: %4ux%-4u x%d view(s), %dx samples, "
                            "%d attachment(s), %d loaded, %d stored, %d resolved%s%s%s — "
                            "%.0f megasamples drawn, %.0f moved\n",
                    c->count / f, c->w, c->h, c->views, c->samples, c->atts, c->loads,
                    c->stores, c->resolves,
                    c->dyn >= 8 && (c->dyn & 4) ? ", RESUMING" : "",
                    c->dyn >= 8 && (c->dyn & 2) ? ", SUSPENDING" : "",
                    c->dyn >= 8 && (c->dyn & 1) ? ", secondary buffers" : "",
                    ms * c->views * c->count / f, moved);
        }
        fprintf(stderr, "  [vk]   total: %.0f megasamples loaded or stored a frame; "
                        "%.0f indexed draws a frame; %u MB of GPU memory allocated\n",
                total, (double)g_prof_draws / f, klvk_prof_gpu_mb());
        pthread_mutex_lock(&g_cen_mu);
        fprintf(stderr, "  [vk]   the guest's images, as asked for: %u multisampled targets %llu MB, "
                        "%u other targets %llu MB, %u compressed textures %llu MB, "
                        "%u other textures %llu MB\n",
                g_cen_count[KLVK_CEN_MSAA], (unsigned long long)(g_cen_kb[KLVK_CEN_MSAA] >> 10),
                g_cen_count[KLVK_CEN_TARGET], (unsigned long long)(g_cen_kb[KLVK_CEN_TARGET] >> 10),
                g_cen_count[KLVK_CEN_BLOCK], (unsigned long long)(g_cen_kb[KLVK_CEN_BLOCK] >> 10),
                g_cen_count[KLVK_CEN_PLAIN], (unsigned long long)(g_cen_kb[KLVK_CEN_PLAIN] >> 10));
        for (int i = 0; i < 32; i++)
            if (g_memc_type[i].n)
                fprintf(stderr, "  [vk]   the guest's device memory, type %d: %u allocation(s), %llu MB, "
                                "of which %llu MB mapped at some time\n", i, g_memc_type[i].n,
                        (unsigned long long)(g_memc_type[i].kb >> 10),
                        (unsigned long long)(g_memc_type[i].mapped_kb >> 10));
        pthread_mutex_unlock(&g_cen_mu);
        fprintf(stderr, "  [vk]   also a frame: %.1f non-indexed draws, %.1f indirect draws, "
                        "%.1f vkCmdClearAttachments, %.1f queries begun\n",
                (double)g_prof_nonindexed / f, (double)g_prof_indirect / f,
                (double)g_prof_clear_att / f, (double)g_prof_queries / f);
        fprintf(stderr, "  [vk]   and %.1f barriers inside a render pass, of which %.1f were not "
                        "issued (each one issued ends and re-opens the Metal pass)\n",
                (double)g_prof_pass_barriers / f, (double)g_prof_pass_barriers_dropped / f);
        g_prof_pass_barriers = g_prof_pass_barriers_dropped = 0;
        g_prof_nonindexed = g_prof_indirect = g_prof_clear_att = g_prof_queries = 0;
        g_prof_draws = 0;
        g_prof_frames = 0; g_prof_passes = 0; g_prof_nshape = 0;
        g_prof_wait_s = 0; g_prof_span_s = 0;
    }
    pthread_mutex_unlock(&g_prof_mu);
}

static void VKAPI_CALL klvk_CmdBeginRenderPass(VkCommandBuffer cb,
        const VkRenderPassBeginInfo *bi, VkSubpassContents c) {
    static PFN_vkCmdBeginRenderPass real;
    if (!real) real = (PFN_vkCmdBeginRenderPass)mvk_sym("vkCmdBeginRenderPass");
    if (klvk_trace() && bi && klvk_trace_once((unsigned long long)(uintptr_t)bi->framebuffer,(unsigned long long)(uintptr_t)klvk_fb_image0(bi->framebuffer))) {
        VkImage im = klvk_fb_image0(bi->framebuffer);
        klvk_trace_log("[rp] begin fb %#llx -> att0 img %#llx (%s k=%d) area %ux%u\n",
            (unsigned long long)(uintptr_t)bi->framebuffer,
            (unsigned long long)(uintptr_t)im, klvk_img_tag(im), klvk_layer_image_key(im),
            bi->renderArea.extent.width, bi->renderArea.extent.height);
    }
    if (bi) klvk_prof_pass(bi->renderPass, bi->renderArea.extent.width, bi->renderArea.extent.height);
    VkRenderPassBeginInfo snap;
    bi = fove_begin(cb, bi, &snap);   // KL_FOVEATION (off: returns bi)
    VkRenderPassBeginInfo tmp; VkClearValue *inj = klvk_probe_inject_clears(bi, &tmp);
    if (real) real(cb, inj ? &tmp : bi, c);
    free(inj);
}
static void VKAPI_CALL klvk_CmdBeginRenderPass2(VkCommandBuffer cb,
        const VkRenderPassBeginInfo *bi, const VkSubpassBeginInfo *sbi) {
    static PFN_vkCmdBeginRenderPass2 real;
    if (!real) { real = (PFN_vkCmdBeginRenderPass2)mvk_sym("vkCmdBeginRenderPass2");
        if (!real) real = (PFN_vkCmdBeginRenderPass2)mvk_sym("vkCmdBeginRenderPass2KHR"); }
    if (klvk_trace() && bi && klvk_trace_once((unsigned long long)(uintptr_t)bi->framebuffer,(unsigned long long)(uintptr_t)klvk_fb_image0(bi->framebuffer))) {
        VkImage im = klvk_fb_image0(bi->framebuffer);
        klvk_trace_log("[rp2] begin fb %#llx -> att0 img %#llx (%s k=%d) area %ux%u\n",
            (unsigned long long)(uintptr_t)bi->framebuffer,
            (unsigned long long)(uintptr_t)im, klvk_img_tag(im), klvk_layer_image_key(im),
            bi->renderArea.extent.width, bi->renderArea.extent.height);
    }
    if (bi) klvk_prof_pass(bi->renderPass, bi->renderArea.extent.width, bi->renderArea.extent.height);
    VkRenderPassBeginInfo snap;
    bi = fove_begin(cb, bi, &snap);   // KL_FOVEATION (off: returns bi)
    VkRenderPassBeginInfo tmp; VkClearValue *inj = klvk_probe_inject_clears(bi, &tmp);
    if (real) real(cb, inj ? &tmp : bi, sbi);
    free(inj);
}

// KL_TRACE: render-pass census. Logs each render pass's subpass structure and,
// always, any that MoltenVK REJECTS. AC Nexus's menu compositing target reaches
// vkCreateImage but never a framebuffer/render pass ("temporary render texture
// not found"); this shows whether the passes that DO get built use input
// attachments / multiple subpasses (the MoltenVK-thorny shapes) and whether any
// creation fails outright.
static VkResult VKAPI_CALL klvk_CreateRenderPass(VkDevice dev,
        const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a,
        VkRenderPass *out) {
    static PFN_vkCreateRenderPass real;
    if (!real && real_gdpa) real = (PFN_vkCreateRenderPass)real_gdpa(dev, "vkCreateRenderPass");
    // KL_VK_FORCE_SAMPLES, on a copy (see klvk_raise_samples).
    VkAttachmentDescription *raised = NULL;
    VkRenderPassCreateInfo rci;
    for (uint32_t i = 0; ci && i < ci->attachmentCount; i++) {
        if (klvk_raise_samples(ci->pAttachments[i].samples) == ci->pAttachments[i].samples) continue;
        if (!raised) {
            raised = malloc(ci->attachmentCount * sizeof *raised);
            if (!raised) break;
            memcpy(raised, ci->pAttachments, ci->attachmentCount * sizeof *raised);
        }
        raised[i].samples = klvk_raise_samples(raised[i].samples);
    }
    if (raised) { rci = *ci; rci.pAttachments = raised; ci = &rci; }
    VkResult r = real ? real(dev, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (ci && out && r == VK_SUCCESS && klvk_profile())
        for (uint32_t i = 0; i < ci->attachmentCount; i++)
            klvk_prof_rp_note(*out, ci->attachmentCount, (int)i, (int)ci->pAttachments[i].format,
                              (int)ci->pAttachments[i].samples, (int)ci->pAttachments[i].loadOp,
                              (int)ci->pAttachments[i].storeOp);
    if (ci && (r != VK_SUCCESS || klvk_trace())) {
        unsigned inputs = 0;
        for (uint32_t i = 0; i < ci->subpassCount; i++)
            inputs += ci->pSubpasses[i].inputAttachmentCount;
        fprintf(stderr, "[TRACE] [rpass] create -> %#llx result %d atts %u subpasses %u inputAtts %u\n",
            (unsigned long long)(uintptr_t)(out ? *out : 0), (int)r,
            ci->attachmentCount, ci->subpassCount, inputs);
        for (uint32_t i = 0; i < ci->attachmentCount; i++)
            fprintf(stderr, "[TRACE] [rpass]   att[%u] format %d samples %d loadOp %d storeOp %d\n",
                i, (int)ci->pAttachments[i].format, (int)ci->pAttachments[i].samples,
                (int)ci->pAttachments[i].loadOp, (int)ci->pAttachments[i].storeOp);
        for (uint32_t sp = 0; sp < ci->subpassCount; sp++) {
            const VkSubpassDescription *d = &ci->pSubpasses[sp];
            fprintf(stderr, "[TRACE] [rpass]   subpass %u: color=%u input=%u depth=%s\n",
                sp, d->colorAttachmentCount, d->inputAttachmentCount,
                d->pDepthStencilAttachment ? "yes" : "no");
            for (uint32_t k = 0; k < d->inputAttachmentCount; k++)
                fprintf(stderr, "[TRACE] [rpass]     input[%u] att=%u layout=%d\n",
                    k, d->pInputAttachments[k].attachment, (int)d->pInputAttachments[k].layout);
        }
    }
    if (r == VK_SUCCESS && out && ci) {                     // KL_VK_ASYNC_RP: which subpasses have depth
        uint32_t depth = 0;
        for (uint32_t sp = 0; sp < ci->subpassCount && sp < 32; sp++)
            if (ci->pSubpasses[sp].pDepthStencilAttachment &&
                ci->pSubpasses[sp].pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED)
                depth |= 1u << sp;
        klvk_rp_note((uint64_t)(uintptr_t)*out, ci->subpassCount, depth);
    }
    free(raised);
    if (r == VK_SUCCESS && out) fove_rp_note1(ci, *out);   // foveation: the eye-pass check reads it
    return r;
}
static VkResult VKAPI_CALL klvk_CreateRenderPass2(VkDevice dev,
        const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a,
        VkRenderPass *out) {
    const VkRenderPassCreateInfo2 *guest_ci = ci;   // foveation reads the guest's own, not our copy
    static PFN_vkCreateRenderPass2 real;
    if (!real && real_gdpa) {
        real = (PFN_vkCreateRenderPass2)real_gdpa(dev, "vkCreateRenderPass2");
        if (!real) real = (PFN_vkCreateRenderPass2)real_gdpa(dev, "vkCreateRenderPass2KHR");
    }
    // Force STORE on every MSAA resolve target. UE5 (wanderer) resolves its
    // multiview scene colour into the eye image but marks that resolve attachment
    // storeOp=DONT_CARE — legal on a Quest tiler where the resolve write survives,
    // but on Metal/MoltenVK a DONT_CARE resolve attachment maps to a store action
    // that DISCARDS the resolved pixels, leaving the eye image black. Rewriting the
    // resolve target's storeOp to STORE makes MoltenVK write it back. Safe for
    // other titles: a resolve target you keep is exactly one you meant to store, so
    // this only ever costs a writeback that was already intended. KL_VK_RESOLVE_STORE=0
    // opts out. Modifies a COPY; the guest's arrays stay untouched.
    VkAttachmentDescription2 *atts = NULL;
    VkRenderPassCreateInfo2 ci2;
    int probe = klvk_eye_probe_on();
    int msaastore = klvk_msaa_store_on();
    int resolve_store = kl_env_on("KL_VK_RESOLVE_STORE", 1);
    uint32_t probe_att = VK_ATTACHMENT_UNUSED;         // eye resolve target we CLEARed
    if (ci && ci->attachmentCount && (resolve_store || probe || msaastore)) {
        int forced = 0, msaa_forced = 0;
        atts = malloc(ci->attachmentCount * sizeof *atts);
        if (atts) {
            memcpy(atts, ci->pAttachments, ci->attachmentCount * sizeof *atts);
            for (uint32_t sp = 0; sp < ci->subpassCount; sp++) {
                const VkSubpassDescription2 *d = &ci->pSubpasses[sp];
                int mv = d->viewMask != 0;              // multiview = the eye scene pass
                for (uint32_t k = 0; d->pResolveAttachments &&
                                     k < d->colorAttachmentCount; k++) {
                    uint32_t ai  = d->pResolveAttachments[k].attachment;                 // resolve target = eye
                    uint32_t src = d->pColorAttachments ? d->pColorAttachments[k].attachment
                                                        : VK_ATTACHMENT_UNUSED;          // MSAA source
                    if (resolve_store && ai < ci->attachmentCount &&
                        atts[ai].storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE) {
                        atts[ai].storeOp = VK_ATTACHMENT_STORE_OP_STORE; forced++;
                    }
                    if (mv && probe && ai < ci->attachmentCount) {
                        atts[ai].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; probe_att = ai;   // discriminator
                    }
                    if (mv && msaastore && src < ci->attachmentCount &&
                        atts[src].samples != VK_SAMPLE_COUNT_1_BIT &&
                        atts[src].storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE) {
                        atts[src].storeOp = VK_ATTACHMENT_STORE_OP_STORE; msaa_forced++; // candidate fix
                    }
                }
            }
            if (forced || msaa_forced || probe_att != VK_ATTACHMENT_UNUSED) {
                ci2 = *ci; ci2.pAttachments = atts; ci = &ci2;
                if (forced)
                    VKI("CreateRenderPass2: forced STORE on %d resolve target(s) "
                        "(was DONT_CARE — Metal would discard the resolve)\n", forced);
                if (probe_att != VK_ATTACHMENT_UNUSED)
                    VKI("CreateRenderPass2: wanderer eye-resolve PROBE armed — att%u "
                        "loadOp->CLEAR (bright magenta); MSAA-source STORE forced on %d att(s)\n",
                        probe_att, msaa_forced);
            } else { free(atts); atts = NULL; }
        }
    }
    // KL_VK_FORCE_SAMPLES, on the same copy (see klvk_raise_samples).
    for (uint32_t i = 0; ci && i < ci->attachmentCount; i++) {
        if (klvk_raise_samples(ci->pAttachments[i].samples) == ci->pAttachments[i].samples) continue;
        if (!atts) {
            atts = malloc(ci->attachmentCount * sizeof *atts);
            if (!atts) break;
            memcpy(atts, ci->pAttachments, ci->attachmentCount * sizeof *atts);
            ci2 = *ci; ci2.pAttachments = atts; ci = &ci2;
        }
        atts[i].samples = klvk_raise_samples(atts[i].samples);
    }
    VkResult r = real ? real(dev, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (ci && out && r == VK_SUCCESS && klvk_profile())
        for (uint32_t i = 0; i < ci->attachmentCount; i++)
            klvk_prof_rp_note(*out, ci->attachmentCount, (int)i, (int)ci->pAttachments[i].format,
                              (int)ci->pAttachments[i].samples, (int)ci->pAttachments[i].loadOp,
                              (int)ci->pAttachments[i].storeOp);
    if (r == VK_SUCCESS && out && probe_att != VK_ATTACHMENT_UNUSED)
        klvk_clearpass_add(*out, probe_att);
    if (ci && (r != VK_SUCCESS || klvk_trace())) {
        unsigned inputs = 0;
        for (uint32_t i = 0; i < ci->subpassCount; i++)
            inputs += ci->pSubpasses[i].inputAttachmentCount;
        fprintf(stderr, "[TRACE] [rpass2] create -> %#llx result %d atts %u subpasses %u inputAtts %u\n",
            (unsigned long long)(uintptr_t)(out ? *out : 0), (int)r,
            ci->attachmentCount, ci->subpassCount, inputs);
        // Per-attachment sample count — an MSAA color (samples>1) paired with a
        // single-sample resolve target is the shape that leaves an eye image
        // black if the multiview resolve into its array layers never lands.
        for (uint32_t i = 0; i < ci->attachmentCount; i++)
            fprintf(stderr, "[TRACE] [rpass2]   att[%u] format %d samples %d loadOp %d storeOp %d\n",
                i, (int)ci->pAttachments[i].format, (int)ci->pAttachments[i].samples,
                (int)ci->pAttachments[i].loadOp, (int)ci->pAttachments[i].storeOp);
        for (uint32_t sp = 0; sp < ci->subpassCount; sp++) {
            const VkSubpassDescription2 *d = &ci->pSubpasses[sp];
            fprintf(stderr, "[TRACE] [rpass2]   subpass %u: color=%u input=%u depth=%s viewMask %#x\n",
                sp, d->colorAttachmentCount, d->inputAttachmentCount,
                d->pDepthStencilAttachment ? "yes" : "no", d->viewMask);
            // Which colour attachment resolves into which — names att1 as the
            // resolve target of the MSAA att0 (the suspected wanderer black).
            for (uint32_t k = 0; d->pResolveAttachments && k < d->colorAttachmentCount; k++)
                if (d->pResolveAttachments[k].attachment != VK_ATTACHMENT_UNUSED)
                    fprintf(stderr, "[TRACE] [rpass2]     color[%u] att=%u RESOLVES into att=%u\n",
                        k, d->pColorAttachments[k].attachment,
                        d->pResolveAttachments[k].attachment);
        }
    }
    if (r == VK_SUCCESS && out && ci) {                     // KL_VK_ASYNC_RP: which subpasses have depth
        uint32_t depth = 0;
        for (uint32_t sp = 0; sp < ci->subpassCount && sp < 32; sp++)
            if (ci->pSubpasses[sp].pDepthStencilAttachment &&
                ci->pSubpasses[sp].pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED)
                depth |= 1u << sp;
        klvk_rp_note((uint64_t)(uintptr_t)*out, ci->subpassCount, depth);
    }
    if (r == VK_SUCCESS && out) fove_rp_note2(guest_ci, *out);
    free(atts);
    return r;
}
static void VKAPI_CALL klvk_DestroyRenderPass(VkDevice dev, uint64_t rp, const VkAllocationCallbacks *alloc) {
    static void (*real)(VkDevice, uint64_t, const VkAllocationCallbacks *);
    if (!real && real_gdpa) real = (void (*)(VkDevice, uint64_t, const VkAllocationCallbacks *))
        real_gdpa(dev, "vkDestroyRenderPass");
    if (klvk_rp_forget(rp)) klvk_ap_drain();   // a compile behind the frame may be reading it
    if (real) real(dev, rp, alloc);
}
// KL_TRACE: dynamic-rendering census (vkCmdBeginRendering). If the guest renders
// its eye/menu with VK_KHR_dynamic_rendering there is NO framebuffer and NO
// render pass object, so this is the only place those renders are visible.
// KL_VK_WIDEN_AREA — a multisampled pass that neither loads nor (but for its
// resolve) keeps its attachments is told to cover ALL of them.
//
// A render area smaller than its attachments obliges the implementation to
// preserve what is outside it, and Metal has no partial load or store: MoltenVK
// therefore turns "clear" into LOAD plus a clearing draw and "don't care" into
// STORE (seen in the pass descriptors: load action 1 where the guest asked for
// none). On a tile-based GPU that is every sample of the attachment read from
// memory before the first triangle and written back after the last — for
// Half-Life: Alyx's scene pass, whose targets are a few percent larger than the
// picture it draws (3102x2956 against 3072x2880, because the picture is rounded
// to a multiple of 96), two layers of colour and depth at 2x: about 330 MB in
// and 330 MB out a frame, for the sake of a margin nothing ever reads.
//
// Widening the area to the attachment makes it a real clear and a real discard.
// The guest's viewport and scissor are untouched, so it draws what it drew. Only
// multisampled attachments qualify — a single-sample target may be an atlas
// whose other tiles are exactly what a render area is there to protect.
// The size of the picture inside such a pass, for kl_mtl_foveate.m: a rate map
// is laid out over the attachment, and the fovea belongs in the middle of what
// is DRAWN.
uint32_t g_klvk_scene_view_w, g_klvk_scene_view_h;
// (g_scene_colour_view, declared with the multisampled images: the view of that
// pass's colour, for klvk_input_att_fix().)

// With the barrier dropped (KL_VK_PASS_BARRIERS=0) the multisampled colour of
// such a pass is STORED as it ends, although the guest said it need not be. What
// reads it inside the pass — in Half-Life: Alyx the exposure metering, a
// full-screen draw that reads the scene as an input attachment, discards by
// luminance and is counted by an occlusion query — is served a texture read by
// MoltenVK, and a texture read returns what was last stored. Stored at the end
// of each frame, that is the previous frame's picture: a meter one frame late,
// which is what an exposure meter can be. Never stored, it was stale memory, and
// the exposure derived from it made the eyes of characters flare (2026-10-06).
// One store of the colour samples is a quarter of what the split cost.
#define KLVK_WIDEN_COLOURS 4
static const VkRenderingInfo *klvk_widen_area(const VkRenderingInfo *ri, VkRenderingInfo *mod,
                                              VkRenderingAttachmentInfo *colours) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_WIDEN_AREA", 0);
    if (!ri || ri->renderArea.offset.x || ri->renderArea.offset.y) return ri;
    if (kl_env_on("KL_VK_WIDEN_TRACE", 0) && ri->renderArea.extent.width >= 1024) {
        static int said;
        uint32_t tw = 0, th = 0;
        int ms = (ri->colorAttachmentCount &&
                  klvk_msaa_view_size(ri->pColorAttachments[0].imageView, &tw, &th)) ||
                 (ri->pDepthAttachment &&
                  klvk_msaa_view_size(ri->pDepthAttachment->imageView, &tw, &th));
        if (ms && said++ < 12) {
            const VkRenderingAttachmentInfo *c = ri->colorAttachmentCount ? &ri->pColorAttachments[0] : NULL;
            const VkRenderingAttachmentInfo *d = ri->pDepthAttachment, *st = ri->pStencilAttachment;
            uint32_t a = 0, b = 0;
            fprintf(stderr, "  [vk] widen? area %ux%u flags %#x colours %u c0 view %p load %d msaa %d | "
                            "depth view %p load %d msaa %d | stencil view %p load %d\n",
                    ri->renderArea.extent.width, ri->renderArea.extent.height, ri->flags,
                    ri->colorAttachmentCount, c ? (void *)c->imageView : NULL, c ? (int)c->loadOp : -1,
                    c ? klvk_msaa_view_size(c->imageView, &a, &b) : -1,
                    d ? (void *)d->imageView : NULL, d ? (int)d->loadOp : -1,
                    d ? klvk_msaa_view_size(d->imageView, &a, &b) : -1,
                    st ? (void *)st->imageView : NULL, st ? (int)st->loadOp : -1);
        }
    }
    uint32_t w = 0, h = 0, n = 0;
    for (uint32_t i = 0; i < ri->colorAttachmentCount && ri->pColorAttachments; i++) {
        const VkRenderingAttachmentInfo *a = &ri->pColorAttachments[i];
        if (!a->imageView) continue;
        uint32_t aw, ah;
        if (a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD ||
            !klvk_msaa_view_size(a->imageView, &aw, &ah)) return ri;
        if (n++ && (aw != w || ah != h)) return ri;
        w = aw; h = ah;
    }
    for (int k = 0; k < 2; k++) {
        const VkRenderingAttachmentInfo *a = k ? ri->pStencilAttachment : ri->pDepthAttachment;
        if (!a || !a->imageView) continue;
        uint32_t aw, ah;
        if (a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD ||
            !klvk_msaa_view_size(a->imageView, &aw, &ah)) return ri;
        if (n++ && (aw != w || ah != h)) return ri;
        w = aw; h = ah;
    }
    if (!n) return ri;
    if (w >= 1024 && h >= 1024) {
        g_klvk_scene_view_w = ri->renderArea.extent.width;
        g_klvk_scene_view_h = ri->renderArea.extent.height;
        if (ri->colorAttachmentCount && ri->pColorAttachments[0].imageView)
            __atomic_store_n(&g_scene_colour_view, ri->pColorAttachments[0].imageView, __ATOMIC_RELEASE);
    }
    int widen = on && (ri->renderArea.extent.width < w || ri->renderArea.extent.height < h);
    int keep = klvk_pass_barriers() == 0 && ri->colorAttachmentCount &&
               ri->colorAttachmentCount <= KLVK_WIDEN_COLOURS && w >= 1024 && h >= 1024;
    if (!widen && !keep) return ri;
    *mod = *ri;
    if (widen) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [vk] KL_VK_WIDEN_AREA: a %ux%u multisampled pass is given its whole "
                            "%ux%u attachments, so nothing of them is loaded\n",
                    ri->renderArea.extent.width, ri->renderArea.extent.height, w, h);
        mod->renderArea.extent.width = w;
        mod->renderArea.extent.height = h;
    }
    // KL_VK_METER_STORE_EVERY=<n> (a trial; 1 is every frame, as before): the
    // store above is every colour sample of both eyes written out of the tiles
    // — 2327x2218 x 2 x 4 samples of 8 bytes, about 330 MB — each frame, for a
    // meter that asks how bright the scene is. Stored every nth frame only, the
    // meter reads a picture up to n frames old, which an exposure that adapts
    // over seconds can live with; in between the pass ends as the engine asked
    // (nothing stored) and the texture keeps what was stored last.
    // KL_VK_METER_STORE_AB=<frames>: measure it. The two ways alternate every
    // <frames> frames inside one run — every frame, then every eighth — and the
    // GPU time of the scene command buffers is kept apart for each
    // (kl_mtl_foveate.m). After every four rounds a line says both means: the
    // same scene, the same minute, the same temperature, which two runs one
    // after the other never are (2026-10-13: runs of a minute scattered by more
    // than the effect).
    if (keep) {
        static int every = -1, ab = -1;
        static unsigned turn;
        if (every < 0) { every = kl_env_int("KL_VK_METER_STORE_EVERY", 1); if (every < 1) every = 1; }
        if (ab < 0) { ab = kl_env_int("KL_VK_METER_STORE_AB", 0); if (ab < 0) ab = 0; }
        unsigned t = __atomic_fetch_add(&turn, 1, __ATOMIC_RELAXED);
        int now_every = every;
        if (ab > 0) {
            extern void kl_mtl_gpu_phase(int phase);
            extern double kl_mtl_gpu_phase_take(int phase, unsigned long *n);
            unsigned round = t / (unsigned)ab;
            int phase = (int)(round & 1u);          // 0: every frame, 1: every eighth
            if (t % (unsigned)ab == 0) {
                kl_mtl_gpu_phase(phase);
                if (round && round % 8 == 0) {
                    unsigned long n0 = 0, n1 = 0;
                    double a = kl_mtl_gpu_phase_take(0, &n0), b = kl_mtl_gpu_phase_take(1, &n1);
                    if (a > 0 && b > 0)
                        fprintf(stderr, "  [vk] KL_VK_METER_STORE_AB: scene GPU time %.2f ms with the colour stored every "
                                        "frame (%lu frames), %.2f ms stored every 8th (%lu frames) — %.2f ms, %.0f%%\n",
                                a, n0, b, n1, a - b, (a - b) * 100.0 / a);
                }
            }
            now_every = phase ? 8 : 1;
        }
        if (now_every > 1 && (t % (unsigned)now_every) != 0) {
            static int said;
            if (!said++)
                fprintf(stderr, "  [vk] KL_VK_METER_STORE_EVERY=%d: the multisampled colour is stored on "
                                "every %d. frame only%s\n", now_every, now_every,
                        ab > 0 ? " (alternating with every frame, KL_VK_METER_STORE_AB)" : "");
            keep = 0;
            if (!widen) return ri;
        }
    }
    if (keep) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [vk] KL_VK_PASS_BARRIERS=0: the multisampled colour of the %ux%u pass is "
                            "stored as it ends, for what reads it in the next frame\n", w, h);
        memcpy(colours, ri->pColorAttachments, ri->colorAttachmentCount * sizeof *colours);
        for (uint32_t i = 0; i < ri->colorAttachmentCount; i++)
            if (colours[i].imageView) colours[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        mod->pColorAttachments = colours;
    }
    return mod;
}

static void VKAPI_CALL klvk_CmdBeginRendering(VkCommandBuffer cb,
        const VkRenderingInfo *ri) {
    KFE(KFE_PASS);
    static PFN_vkCmdBeginRendering real;
    if (!real) {
        real = (PFN_vkCmdBeginRendering)mvk_sym("vkCmdBeginRendering");
        if (!real) real = (PFN_vkCmdBeginRendering)mvk_sym("vkCmdBeginRenderingKHR");
    }
    VkRenderingInfo widened;
    VkRenderingAttachmentInfo widened_colours[KLVK_WIDEN_COLOURS];
    if (klvk_trace() && ri) {
        klvk_trace_log("[dynr] begin area %ux%u layers %u colorAtts %u depth %s\n",
            ri->renderArea.extent.width, ri->renderArea.extent.height,
            ri->layerCount, ri->colorAttachmentCount,
            ri->pDepthAttachment && ri->pDepthAttachment->imageView ? "yes" : "no");
        for (uint32_t i = 0; i < ri->colorAttachmentCount && ri->pColorAttachments; i++) {
            VkImageView v = ri->pColorAttachments[i].imageView;
            VkImage im = klvk_view_image(v);
            klvk_trace_log("[dynr]   color[%u] view %#llx -> img %#llx (%s k=%d)\n",
                i, (unsigned long long)(uintptr_t)v,
                (unsigned long long)(uintptr_t)im, klvk_img_tag(im), klvk_layer_image_key(im));
        }
    }
    if (ri && klvk_profile()) {
        int loads = 0, stores = 0, atts = 0, samples = 0, resolves = 0;
        int views = ri->viewMask ? __builtin_popcount(ri->viewMask) : (int)ri->layerCount;
        for (uint32_t i = 0; i < ri->colorAttachmentCount && ri->pColorAttachments; i++) {
            if (!ri->pColorAttachments[i].imageView) continue;
            atts++;
            if (!samples)
                samples = (int)klvk_prof_img_samples(klvk_view_image(ri->pColorAttachments[i].imageView));
            if (ri->pColorAttachments[i].resolveImageView) resolves++;
            if (ri->pColorAttachments[i].loadOp == 0) loads++;
            if (ri->pColorAttachments[i].storeOp == 0) stores++;
        }
        if (ri->pDepthAttachment && ri->pDepthAttachment->imageView) {
            atts++;
            if (ri->pDepthAttachment->loadOp == 0) loads++;
            if (ri->pDepthAttachment->storeOp == 0) stores++;
            if (!samples)
                samples = (int)klvk_prof_img_samples(klvk_view_image(ri->pDepthAttachment->imageView));
            if (ri->pDepthAttachment->resolveImageView) resolves++;
        }
        pthread_mutex_lock(&g_prof_mu);
        klvk_prof_shape_add(ri->renderArea.extent.width, ri->renderArea.extent.height, samples,
                            atts, loads, stores, 8 + (int)(ri->flags & 7), views, resolves);
        pthread_mutex_unlock(&g_prof_mu);
    }
    ri = klvk_widen_area(ri, &widened, widened_colours);
    klvk_in_rendering_set(cb, 1);
    if (real) real(cb, ri);
}
static void VKAPI_CALL klvk_CmdEndRendering(VkCommandBuffer cb) {
    static PFN_vkCmdEndRendering real;
    if (!real) {
        real = (PFN_vkCmdEndRendering)mvk_sym("vkCmdEndRendering");
        if (!real) real = (PFN_vkCmdEndRendering)mvk_sym("vkCmdEndRenderingKHR");
    }
    klvk_in_rendering_set(cb, 0);
    if (t_held.cb == cb) {                      // nothing read it: it was not needed
        t_held.cb = VK_NULL_HANDLE;
        __atomic_add_fetch(&g_prof_pass_barriers_dropped, 1, __ATOMIC_RELAXED);
    }
    if (real) real(cb);
}
// KL_TRACE: catch a temp-RT allocation that FAILS at bind time — the step Unity
// does through MoltenVK directly, invisible until now. A failure here is what
// makes Unity mark the render target invalid and report "temporary render
// texture not found". Always logged (failures only), so it costs nothing.
// ---------------------------------------------------------------------------
// KL_VK_TEXELBUF_PROBE — decisive evidence for the olar grayscale/fragmented
// scene bug, and the reason a SPIR-V texel-buffer emulation is very likely the
// WRONG fix.
//
// OFFLINE FINDING (proven, no device required): spirv-cross is the exact library
// MoltenVK 1.4.2 uses to convert SPIR-V -> MSL, and it is vendored here
// (vendor/third_party/spirv-cross). Its own reference corpus shows that a
// uniform texel-buffer fetch translates CORRECTLY, in both modes MoltenVK can
// pick:
//   shaders-msl/vert/texture_buffer.vert  (input: samplerBuffer + texelFetch)
//   reference/shaders-msl/vert/texture_buffer.vert (2D fallback):
//       texture2d<float> uSamp; ... uSamp.read(spvTexelBufferCoord(idx))
//       spvTexelBufferCoord(tc) = uint2(tc % 4096, tc / 4096)
//   reference/shaders-msl/vert/texture_buffer.texture-buffer-native.msl21.vert:
//       texture_buffer<float> uSamp; ... uSamp.read(uint(idx))
// The fallback math is byte-identical to kl_glfb's GLES emulation (width 4096
// vs klfb's 2048 — a width choice, not a defect). On Apple Vision Pro MoltenVK
// enables msl_options.texture_buffer_native, so olar takes the native path and
// reads element N linearly and correctly. => The OpImageFetch-on-Dim=Buffer
// translation is NOT where the picture is lost. A SPIR-V rewrite (Dim=Buffer ->
// 2D) would reproduce exactly the code MoltenVK already emits and could not fix
// the bug.
//
// That leaves two live suspects. This probe forks them, and MUST run before any
// large fix is built:
//   (A) the texel buffer's backing bytes are ZERO / absent at draw time — the
//       guest never uploaded them (a compute skinning / transform pass that is
//       not running under Klepton). Emulation is wasted; the fix is elsewhere.
//   (B) the bytes are PRESENT — then the fault is in MoltenVK's *runtime*
//       VkBufferView -> MTLTexture creation for olar's pattern (256+ views into
//       ONE VkBuffer at many offsets/formats). The fix is to replace MoltenVK's
//       per-view texture object, NOT to rewrite the shader.
//
// The probe reads the first few texel-buffer views' backing bytes. When the
// backing memory is host-visible AND the guest has mapped it, the read is direct
// — NO out-of-band GPU submit, so nothing about the guest's queue is perturbed.
// Otherwise it reports provenance (device-local? ever a transfer destination?),
// which still forks A vs B. Everything here is inert unless KL_VK_TEXELBUF_PROBE
// is set; when off, the added CreateBuffer/BindBufferMemory/MapMemory/CmdCopy*
// wrappers are faithful passthrough (one cached-flag test, then the real call).
static int probe_on(void) {
    static int v = -1;
    if (v < 0) v = kl_env_on("KL_VK_TEXELBUF_PROBE", 0);
    return v;
}

// KL_VK_EMU_TEXELBUF — the fork-B fix (see the big block below CreateBufferView).
// Per-target default ON for the two UE5-mobile titles whose Manual-Vertex-Fetch
// texel buffers MoltenVK mis-binds (olar, wanderer) and OFF for every other
// Vulkan title, so nothing else changes. KL_VK_EMU_TEXELBUF overrides the
// default either way.
static int emu_on(void) {
    static int v = -1;
    if (v < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        int def = (t && (strcmp(t, "olar") == 0 || strcmp(t, "wanderer") == 0)) ? 1 : 0;
        v = kl_env_on("KL_VK_EMU_TEXELBUF", def);
    }
    return v;
}
// Buffer/memory-binding tracking is needed by BOTH the probe and the emu fix
// (the fix has to know which VkDeviceMemory + offset a source buffer occupies).
static int track_on(void) { return probe_on() || emu_on(); }

// KL_VK_EMU_PROBE (default OFF) — the 2D-mirror source readback diagnostic
// (see klvk_mirror_gpu_copy). Purely observational and OFF unless explicitly set,
// so the normal emu path costs nothing but a cached int test.
static int emu_probe(void) {
    static int v = -1;
    if (v < 0) v = kl_env_on("KL_VK_EMU_PROBE", 0);
    return v;
}

#define KLVK_PROBE_BUFS 8192
#define KLVK_PROBE_MEMS 4096
#define KLVK_PROBE_TBVS 512

static struct probe_buf { VkBuffer buf; VkDeviceSize size; VkBufferUsageFlags usage;
                          VkDeviceMemory mem; VkDeviceSize memoff; int upload_dst; }
    g_probe_bufs[KLVK_PROBE_BUFS];
static int g_probe_nbuf;

static struct probe_mem { VkDeviceMemory mem; uint32_t type_index;
                          void *mapped; VkDeviceSize map_off; VkDeviceSize map_size; }
    g_probe_mems[KLVK_PROBE_MEMS];
static int g_probe_nmem;

static struct probe_tbv { VkBuffer buf; VkDeviceSize off; VkDeviceSize range; VkFormat fmt; }
    g_probe_tbvs[KLVK_PROBE_TBVS];
static int g_probe_ntbv;

static uint32_t g_probe_hostvis_mask;   // bit i set == memory type i is HOST_VISIBLE
static int      g_probe_hostvis_ready;
static int      g_probe_done;

// All of the following run under g_lock.
static struct probe_buf *probe_buf_find(VkBuffer b) {
    for (int i = 0; i < g_probe_nbuf; i++)
        if (g_probe_bufs[i].buf == b) return &g_probe_bufs[i];
    return NULL;
}
static struct probe_mem *probe_mem_find(VkDeviceMemory m) {
    for (int i = 0; i < g_probe_nmem; i++)
        if (g_probe_mems[i].mem == m) return &g_probe_mems[i];
    return NULL;
}
static void probe_compute_hostvis(void) {
    if (g_probe_hostvis_ready || !g_devs[0]) return;
    PFN_vkGetPhysicalDeviceMemoryProperties gmp =
        (PFN_vkGetPhysicalDeviceMemoryProperties)
            real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!gmp) return;
    VkPhysicalDeviceMemoryProperties mp; memset(&mp, 0, sizeof mp);
    gmp(g_devs[0]->phys, &mp);
    uint32_t mask = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            mask |= (1u << i);
    g_probe_hostvis_mask = mask;
    g_probe_hostvis_ready = 1;
}
static void probe_run(void) {
    pthread_mutex_lock(&g_lock);
    if (g_probe_done) { pthread_mutex_unlock(&g_lock); return; }
    g_probe_done = 1;
    probe_compute_hostvis();
    VKI("TEXELBUF_PROBE: %d texel-buffer view(s), %d buffer(s), %d memory alloc(s) tracked\n",
        g_probe_ntbv, g_probe_nbuf, g_probe_nmem);
    int shown = 0;
    for (int i = 0; i < g_probe_ntbv && shown < 16; i++) {
        struct probe_tbv *v = &g_probe_tbvs[i];
        struct probe_buf *b = probe_buf_find(v->buf);
        struct probe_mem *m = (b && b->mem) ? probe_mem_find(b->mem) : NULL;
        int hostvis = m && (g_probe_hostvis_mask & (1u << m->type_index));
        shown++;
        if (m && m->mapped && hostvis) {
            VkDeviceSize mem_abs = b->memoff + v->off;   // offset within the VkDeviceMemory
            VkDeviceSize map_end = (m->map_size == VK_WHOLE_SIZE)
                                 ? (VkDeviceSize)~0ull : m->map_off + m->map_size;
            if (mem_abs >= m->map_off && mem_abs < map_end) {
                const uint8_t *p = (const uint8_t *)m->mapped + (size_t)(mem_abs - m->map_off);
                VkDeviceSize rng = v->range;
                if (rng == VK_WHOLE_SIZE || rng > 64) rng = 64;
                size_t nz = 0;
                for (VkDeviceSize k = 0; k < rng; k++) if (p[k]) nz++;
                uint32_t w[4] = {0,0,0,0};
                memcpy(w, p, (size_t)(rng < 16 ? rng : 16));
                float f[4];
                for (int j = 0; j < 4; j++) memcpy(&f[j], &w[j], 4);
                VKI("  tbv[%d] buf %#llx off %llu fmt %d MAPPED+HOST-VISIBLE: "
                    "%zu/%llu nonzero; u32[%#x %#x %#x %#x] f32[%.4g %.4g %.4g %.4g] %s\n",
                    i, (unsigned long long)(uintptr_t)v->buf,
                    (unsigned long long)v->off, (int)v->fmt,
                    nz, (unsigned long long)rng, w[0], w[1], w[2], w[3],
                    f[0], f[1], f[2], f[3],
                    nz ? "<= DATA PRESENT (points at fork B: MoltenVK bind/read)"
                       : "<= DATA ABSENT (points at fork A: buffer never filled)");
            } else {
                VKI("  tbv[%d] buf %#llx off %llu: host-visible+mapped but view offset "
                    "outside the mapped range\n", i,
                    (unsigned long long)(uintptr_t)v->buf, (unsigned long long)v->off);
            }
        } else {
            const char *cls = !b ? "backing buffer not tracked"
                            : !m ? "backing memory not tracked"
                            : hostvis ? "host-visible but NOT mapped by the guest"
                            : "DEVICE-LOCAL (not host-readable without a GPU copy)";
            VKI("  tbv[%d] buf %#llx off %llu range %llu fmt %d: %s; backing buffer was %sa "
                "transfer destination\n", i, (unsigned long long)(uintptr_t)v->buf,
                (unsigned long long)v->off, (unsigned long long)v->range, (int)v->fmt,
                cls, (b && b->upload_dst) ? "" : "NOT ");
        }
    }
    VKI("TEXELBUF_PROBE: done. DATA PRESENT everywhere => build the buffer-view "
        "mirror (fork B); DATA ABSENT => find the missing upload/compute (fork A); "
        "all DEVICE-LOCAL+never-a-copy-dst => also fork A.\n");
    pthread_mutex_unlock(&g_lock);
}


static VkResult VKAPI_CALL klvk_AllocateMemory(VkDevice dev,
        const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    KFE(KFE_MEM_NEW);
    static PFN_vkAllocateMemory real;
    if (!real && real_gdpa) real = (PFN_vkAllocateMemory)real_gdpa(dev, "vkAllocateMemory");
    // KL_VK_MSAA_MEMORYLESS: the block the engine allocates for a noted image.
    VkMemoryAllocateInfo ml_ai;
    int ml_moved = 0;
    if (ai && g_ml_n && klvk_ml_on()) {
        int k = -1;
        uint32_t bits = 0;
        pthread_mutex_lock(&g_ml_mu);
        for (int i = 0; i < g_ml_n; i++)
            if (g_ml[i].open && g_ml[i].size == ai->allocationSize && ((g_ml[i].bits >> ai->memoryTypeIndex) & 1)) { k = i; break; }
        if (k >= 0) { g_ml[k].open = 0; bits = g_ml[k].bits; }
        pthread_mutex_unlock(&g_ml_mu);
        int lazy = k >= 0 ? klvk_ml_lazy_type(dev, bits) : -1;
        if (lazy >= 0) {
            ml_ai = *ai;
            ml_ai.memoryTypeIndex = (uint32_t)lazy;
            fprintf(stderr, "  [vk] memoryless: an allocation of %.1f MB moved from memory type %u to %d — "
                            "tile memory instead of real memory\n",
                    (double)ai->allocationSize / 1048576.0, ai->memoryTypeIndex, lazy);
            ai = &ml_ai;
            ml_moved = 1;
        }
    }
    VkResult r = real ? real(dev, ai, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (ml_moved && r == VK_SUCCESS && out) {
        pthread_mutex_lock(&g_ml_mu);
        if (g_ml_nmem < KLVK_ML_MAX) g_ml_mem[g_ml_nmem++] = *out;
        pthread_mutex_unlock(&g_ml_mu);
    }
    if (r != VK_SUCCESS)
        VKI("vkAllocateMemory FAILED result %d size %llu typeIndex %u\n", (int)r,
            ai ? (unsigned long long)ai->allocationSize : 0, ai ? ai->memoryTypeIndex : 0);
    if (r == VK_SUCCESS && ai && out && klvk_profile())
        klvk_memc_add(*out, ai->allocationSize, ai->memoryTypeIndex);
    if (probe_on() && r == VK_SUCCESS && ai && out) {
        pthread_mutex_lock(&g_lock);
        struct probe_mem *e = probe_mem_find(*out);
        if (!e && g_probe_nmem < KLVK_PROBE_MEMS) {
            e = &g_probe_mems[g_probe_nmem++];
            e->mem = *out; e->mapped = NULL; e->map_off = 0; e->map_size = 0;
        }
        if (e) e->type_index = ai->memoryTypeIndex;
        pthread_mutex_unlock(&g_lock);
    }
    return r;
}
static void VKAPI_CALL klvk_FreeMemory(VkDevice dev, VkDeviceMemory mem,
        const VkAllocationCallbacks *a) {
    KFE(KFE_MEM_DEL);
    static PFN_vkFreeMemory real;
    if (!real && real_gdpa) real = (PFN_vkFreeMemory)real_gdpa(dev, "vkFreeMemory");
    if (klvk_profile()) klvk_memc_del(mem);
    if (real) real(dev, mem, a);
}
static VkResult VKAPI_CALL klvk_BindImageMemory(VkDevice dev, VkImage img,
        VkDeviceMemory mem, VkDeviceSize off) {
    static PFN_vkBindImageMemory real;
    if (!real && real_gdpa) real = (PFN_vkBindImageMemory)real_gdpa(dev, "vkBindImageMemory");
    if (g_ml_nmem) {      // a memoryless block may only hold the image it was made for
        int lazy_mem = 0, ours = 0;
        pthread_mutex_lock(&g_ml_mu);
        for (int i = 0; i < g_ml_nmem; i++) if (g_ml_mem[i] == mem) lazy_mem = 1;
        ours = klvk_ml_find(img) >= 0;
        pthread_mutex_unlock(&g_ml_mu);
        if (lazy_mem)
            fprintf(stderr, "  [vk] memoryless: image bound to the moved allocation — %s\n",
                    ours ? "the multisampled depth it was moved for" : "NOT that image (this would be wrong)");
    }
    VkResult r = real ? real(dev, img, mem, off) : VK_ERROR_INITIALIZATION_FAILED;
    if (r != VK_SUCCESS)
        VKI("vkBindImageMemory FAILED result %d image %#llx\n", (int)r,
            (unsigned long long)(uintptr_t)img);
    return r;
}
// KL_TRACE / always-on-failure: the format-capability query Unity runs before
// committing a RenderTexture. If MoltenVK reports a limitation here that
// vkCreateImage does not enforce (a sample count, a usage, a max extent), Unity
// decides the temp RT is invalid, never registers it, and SetRenderTarget then
// fails "temporary render texture not found" — the menu-compositing wall.
static uint32_t klvk_sample_mask(void);
static VkResult VKAPI_CALL klvk_GetPhysicalDeviceImageFormatProperties(
        VkPhysicalDevice pd, VkFormat fmt, VkImageType type, VkImageTiling tiling,
        VkImageUsageFlags usage, VkImageCreateFlags flags,
        VkImageFormatProperties *out) {
    VKT("call GetPhysicalDeviceImageFormatProperties\n");   // KL_VK_TRACE: the order of a guest's capability queries
    static PFN_vkGetPhysicalDeviceImageFormatProperties real;
    if (!real && real_gipa) real = (PFN_vkGetPhysicalDeviceImageFormatProperties)
        real_gipa(g_instance, "vkGetPhysicalDeviceImageFormatProperties");
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceImageFormatProperties");
    VkResult r = real ? real(pd, fmt, type, tiling, usage, flags, out)
                      : VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (r == VK_SUCCESS && out) out->sampleCounts &= klvk_sample_mask();
    if (r != VK_SUCCESS || (klvk_trace() && (usage & 0x30u))) {
        fprintf(stderr, "[TRACE] [imgfmt] fmt %d type %d tiling %d usage %#x flags %#x -> %d%s\n",
            (int)fmt, (int)type, (int)tiling, usage, flags, (int)r,
            r != VK_SUCCESS ? "  <== UNSUPPORTED" : "");
        if (r == VK_SUCCESS && out)
            fprintf(stderr, "[TRACE] [imgfmt]   maxExtent %ux%ux%u maxMips %u maxLayers %u samples %#x\n",
                out->maxExtent.width, out->maxExtent.height, out->maxExtent.depth,
                out->maxMipLevels, out->maxArrayLayers, out->sampleCounts);
    }
    return r;
}
// ---- GPU-identity spoof --------------------------------------------------
// Some titles gate on the GPU they see rather than on Build.MODEL. batman
// (Batman: Arkham Shadow) is Quest 3 / 3S exclusive: it makes a throwaway
// VkInstance, reads the physical device, and — finding "Apple M5 GPU" / vendor
// 0x106b (Apple) rather than a Qualcomm Adreno — pops "Your device does not
// match the hardware requirements", abandons Vulkan and drops to a GLES2 context
// whose shaders it does not ship. So for such a target we rewrite the device
// identity MoltenVK reports to a Quest 3's Adreno 740 (Qualcomm). Only the
// identity strings/IDs change; every limit and feature stays MoltenVK's real
// answer, so nothing downstream is promised a capability the host lacks.
// Default: on for batman; KL_VK_SPOOF_ADRENO=0/1 forces the A/B either way.
// ...and for every Unity title under the profile for untested games: LEGO
// Bricktales (Unity 6000.2) walked the device, read "Apple M4" / 0x106b and
// raised the same warning, then "Unable to initialize the Unity Engine
// Graphics API"; told Adreno 740 (and handed the VkPhysicalDevice as its
// display adapter, kl_ovrp.c) it creates its device and loads its first scene
// (Mac host, 2026-10-17). A Quest game expects a Quest's GPU.
static int klvk_spoof_gpu(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        extern int kl_untested_profile(void);
        extern kl_guest_kind kl_driver_kind(void);
        int dflt = (t && strcmp(t, "batman") == 0) ||
                   (kl_untested_profile() && kl_driver_kind() == KL_GUEST_UNITY);
        on = kl_env_on("KL_VK_SPOOF_ADRENO", dflt);
    }
    return on;
}
// KL_VK_MAX_SAMPLES=<1|2|4>: the most samples per pixel the guest is told this
// device can render with (0, the default, tells it the truth). A guest's
// multisampled targets are transient on a tiler and cost a Quest nothing; here
// they are real memory — AC Nexus's 4x colour and depth pair is 865 MB per eye
// stage at 3072 px — so the sample count is what an eye size is paid for with.
// Default 2 for AC Nexus, with a larger eye (KL_OVRP_EYE_MAX 3840): 2x at
// 4800x3080 is 676 MB a stage, and a level load peaks at 7.1 GB of the 8.2 GB
// limit, where 4x at that size was killed on its first load.
static uint32_t klvk_sample_mask(void) {
    static int v = -1;
    if (v < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        // Demeo (x3634830803298285) is the same engine with the same pairs — a
        // 4x colour and depth target for each of its three eye stages, at the
        // 3936x2465 its plugin widens the eye to — and the same arithmetic: the
        // app went from 0.6 to 5.5 GB in the second its eye layer was set up
        // and played at 7.5 to 7.7 of its 8.2 GB, 0.5 GB from being ended, with
        // presents arriving late now and then (on device, 2026-10-13).
        // BattleGroupVR2 (x9386827918099701, Unity 6.2): three 4x colour and
        // depth pairs again, which at its eye size ask for about 5.2 GiB at
        // its start, before a scene is loaded; the app was ended at its
        // per-process limit. Told 2x it makes its targets, passes and
        // pipelines at 2x itself, at the same eye size (the sender's figures
        // and device runs, fix 38, 2026-10-13; not measured here).
        v = kl_env_int("KL_VK_MAX_SAMPLES", t && (strcmp(t, "x5812519008825194") == 0 ||
                                                  strcmp(t, "x3634830803298285") == 0 ||
                                                  strcmp(t, "x9386827918099701") == 0) ? 2 : 0);
    }
    return v == 1 ? 0x1u : v == 2 ? 0x3u : v == 4 ? 0x7u : ~0u;
}
static void klvk_cap_sample_limits(VkPhysicalDeviceProperties *p) {
    uint32_t m = klvk_sample_mask();
    if (!p || m == ~0u) return;
    p->limits.framebufferColorSampleCounts &= m;
    p->limits.framebufferDepthSampleCounts &= m;
    p->limits.framebufferStencilSampleCounts &= m;
    p->limits.framebufferNoAttachmentsSampleCounts &= m;
    p->limits.sampledImageColorSampleCounts &= m;
    p->limits.sampledImageIntegerSampleCounts &= m;
    p->limits.sampledImageDepthSampleCounts &= m;
    p->limits.sampledImageStencilSampleCounts &= m;
    p->limits.storageImageSampleCounts &= m;
    static int said;
    if (!said++) VKI("KL_VK_MAX_SAMPLES: sample counts reported to the guest masked to %#x\n", m);
}
static void klvk_spoof_props(VkPhysicalDeviceProperties *p) {
    klvk_cap_sample_limits(p);
    if (!p || !klvk_spoof_gpu()) return;
    p->vendorID = 0x5143;                 // Qualcomm
    p->deviceID = 0x43051401;             // an Adreno 740-class id (plausible)
    snprintf(p->deviceName, sizeof p->deviceName, "Adreno (TM) 740");
    // DIAGNOSTIC, off unless set: the two version words that ride beside the
    // identity. An Adreno name over MoltenVK's own driverVersion reads to a
    // guest as a Qualcomm driver from years ago, and Unity keeps a list of
    // those it will not start Vulkan on. KL_VK_SPOOF_DRIVER / KL_VK_SPOOF_API
    // take the raw 32-bit value (hex), to find out which word a guest gates on
    // before either is made a default.
    const char *dv = kl_env_str("KL_VK_SPOOF_DRIVER", NULL);
    const char *av = kl_env_str("KL_VK_SPOOF_API", NULL);
    uint32_t real_drv = p->driverVersion, real_api = p->apiVersion;
    if (dv && *dv) p->driverVersion = (uint32_t)strtoul(dv, NULL, 16);
    if (av && *av) p->apiVersion    = (uint32_t)strtoul(av, NULL, 16);
    static int said;
    if (!said) { said = 1;
        VKI("GPU identity spoofed to '%s' vendor %#x for the guest's "
            "hardware check (KL_VK_SPOOF_ADRENO); driverVersion %#x%s, "
            "apiVersion %u.%u.%u%s\n", p->deviceName, p->vendorID,
            p->driverVersion, p->driverVersion != real_drv ? " (spoofed)" : "",
            p->apiVersion >> 22, (p->apiVersion >> 12) & 0x3ff, p->apiVersion & 0xfff,
            p->apiVersion != real_api ? " (spoofed)" : ""); }
}
static void VKAPI_CALL klvk_GetPhysicalDeviceProperties(
        VkPhysicalDevice pd, VkPhysicalDeviceProperties *props) {
    VKT("call GetPhysicalDeviceProperties\n");   // KL_VK_TRACE: the order of a guest's capability queries
    static PFN_vkGetPhysicalDeviceProperties real;
    if (!real && real_gipa)
        real = (PFN_vkGetPhysicalDeviceProperties)
            real_gipa(g_instance, "vkGetPhysicalDeviceProperties");
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceProperties");
    if (real) real(pd, props);
    klvk_spoof_props(props);
}
static void VKAPI_CALL klvk_GetPhysicalDeviceProperties2(
        VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *props) {
    VKT("call GetPhysicalDeviceProperties2\n");   // KL_VK_TRACE: the order of a guest's capability queries
    if (!props) return;
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceProperties2");
    static PFN_vkGetPhysicalDeviceProperties2 real;
    static int tried;
    if (!tried) { tried = 1; if (real_gipa) {
        real = (PFN_vkGetPhysicalDeviceProperties2)
            real_gipa(g_instance, "vkGetPhysicalDeviceProperties2");
        if (!real) real = (PFN_vkGetPhysicalDeviceProperties2)
            real_gipa(g_instance, "vkGetPhysicalDeviceProperties2KHR");
    } }
    if (real) { real(pd, props); }
    else {
        PFN_vkGetPhysicalDeviceProperties v1 = real_gipa
            ? (PFN_vkGetPhysicalDeviceProperties)
                  real_gipa(g_instance, "vkGetPhysicalDeviceProperties") : NULL;
        if (v1) v1(pd, &props->properties);
    }
    klvk_spoof_props(&props->properties);

    // Make the deviceLUID Unity reads here EQUAL the one ovrp_GetDisplayAdapterId2
    // handed it, so OculusXR's "which enumerated device is the VR GPU?" match
    // lands instead of leaving the selection NULL (-> getQueueFamilies(NULL) ->
    // MVKPhysicalDevice::getQueueFamilies crash). Returning the right LUID from
    // OVRPlugin was not enough: MoltenVK's deviceLUID/deviceLUIDValid here can
    // differ from what the cache query saw (per-instance, or reported invalid to
    // the guest), so the two sides never agreed. Force agreement at the source.
    // Also logs what MoltenVK actually returned, to confirm the mismatch.
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)props->pNext; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES) continue;
        VkPhysicalDeviceIDProperties *idp = (VkPhysicalDeviceIDProperties *)p;
        VKI("guest reads deviceLUID %02x%02x%02x%02x%02x%02x%02x%02x valid=%u"
            "%s\n",
            idp->deviceLUID[0], idp->deviceLUID[1], idp->deviceLUID[2],
            idp->deviceLUID[3], idp->deviceLUID[4], idp->deviceLUID[5],
            idp->deviceLUID[6], idp->deviceLUID[7], idp->deviceLUIDValid,
            g_display_luid_valid ? " -> forcing to the ovrp adapter id" : "");
        if (g_display_luid_valid) {
            memcpy(idp->deviceLUID, g_display_luid, 8 /* VK_LUID_SIZE */);
            idp->deviceLUIDValid = VK_TRUE;
        }
    }
}

// Safety net for a guest that reaches vkGetPhysicalDeviceQueueFamilyProperties
// with a NULL physicalDevice — Unity does exactly this when its GPU match leaves
// the selection null, and MoltenVK then dereferences null in MVKPhysicalDevice::
// getQueueFamilies. A null handle is never legitimate (there is exactly one GPU),
// so substitute the live instance's first physical device. Pure passthrough for a
// real handle, so no working title is affected. The LUID-force above should make
// the match land in the first place; this catches any residual null path.
static void VKAPI_CALL klvk_GetPhysicalDeviceQueueFamilyProperties(
        VkPhysicalDevice pd, uint32_t *count, VkQueueFamilyProperties *props) {
    VKT("call GetPhysicalDeviceQueueFamilyProperties\n");   // KL_VK_TRACE: the order of a guest's capability queries
    static PFN_vkGetPhysicalDeviceQueueFamilyProperties real;
    static int tried;
    if (!tried) { tried = 1; if (real_gipa) real =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
            real_gipa(g_instance, "vkGetPhysicalDeviceQueueFamilyProperties"); }
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceQueueFamilyProperties");
    if (real) real(pd, count, props);
    else if (count) *count = 0;
    // Queue spread: family 0 reports one queue for each family like it.
    if (props && count && *count > 1 && klvk_queue_spread_on()) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < *count; i++)
            if (props[i].queueFlags == props[0].queueFlags && props[i].queueCount >= 1) n++; else break;
        if (n > props[0].queueCount) {
            static int said;
            if (!said) { said = 1; VKI("queue spread: family 0 reported with %u queues (MoltenVK: %u)\n", n, props[0].queueCount); }
            props[0].queueCount = n;
        }
    }
}

// Queue i of family 0 under the spread is family i's only queue.
static void VKAPI_CALL klvk_GetDeviceQueue(VkDevice dev, uint32_t family, uint32_t index, VkQueue *queue) {
    static PFN_vkGetDeviceQueue real;
    if (!real && real_gdpa) real = (PFN_vkGetDeviceQueue)real_gdpa(dev, "vkGetDeviceQueue");
    klvk_device *d = dev_find(dev);
    if (d && d->spread && family == 0 && index > 0 && index < d->spread) { family = index; index = 0; }
    if (real) real(dev, family, index, queue);
    else if (queue) *queue = VK_NULL_HANDLE;
}

// The rest of the vkGetPhysicalDevice* family Unity's device init queries on the
// same NULL handle — each substitutes the live device and passes through. Kept
// terse; they share the resolve-once + guard shape.
static void VKAPI_CALL klvk_GetPhysicalDeviceFeatures2(
        VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) {
    VKT("call GetPhysicalDeviceFeatures2\n");   // KL_VK_TRACE: the order of a guest's capability queries
    static PFN_vkGetPhysicalDeviceFeatures2 real; static int t;
    if (!t) { t = 1; if (real_gipa) { real = (PFN_vkGetPhysicalDeviceFeatures2)
        real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2");
        if (!real) real = (PFN_vkGetPhysicalDeviceFeatures2)
            real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2KHR"); } }
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceFeatures2");
    if (real) real(pd, f);
}
static void VKAPI_CALL klvk_GetPhysicalDeviceFeatures(
        VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) {
    static PFN_vkGetPhysicalDeviceFeatures real; static int t;
    if (!t) { t = 1; if (real_gipa) real = (PFN_vkGetPhysicalDeviceFeatures)
        real_gipa(g_instance, "vkGetPhysicalDeviceFeatures"); }
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceFeatures");
    if (real) real(pd, f);
}
// How much memory the guest is told the GPU has (KL_VK_HEAP_MB; 0 = whatever
// MoltenVK says).
//
// MoltenVK reports a heap of the size Metal recommends as a working set, which
// on a headset is MORE than the whole process may use: this is unified memory,
// a texture is the app's footprint, and the footprint has a limit the system
// enforces by ending the app. An engine that streams textures against the
// budget it is told — Source 2 logs "set texture memory budget to 10922 MB" on a
// Vision Pro whose app limit is 8191 MB — therefore loads until it is killed:
// Half-Life: Alyx went from 7.4 to 8.17 GB in the two and a half minutes after
// a level load and was gone, with the streamer still 10 GB under its budget
// (2026-10-06). The cap is the budget AND the heap size, so the two numbers the
// guest reads agree; usage is left as measured.
static void klvk_heap_cap(VkPhysicalDeviceMemoryProperties *mp, const void *chain) {
    static int64_t cap_mb = -1;
    if (cap_mb < 0) cap_mb = kl_env_int("KL_VK_HEAP_MB", 0);
    if (cap_mb <= 0 || !mp) return;
    VkDeviceSize cap = (VkDeviceSize)cap_mb << 20;
    static int said;
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++) {
        if (mp->memoryHeaps[i].size <= cap) continue;
        if (!said++)
            fprintf(stderr, "  [vk] heap %u: the guest is told %lld MB, not the %llu MB "
                    "MoltenVK reports (KL_VK_HEAP_MB)\n", i, (long long)cap_mb,
                    (unsigned long long)(mp->memoryHeaps[i].size >> 20));
        mp->memoryHeaps[i].size = cap;
    }
    for (const VkBaseOutStructure *n = chain; n; n = n->pNext) {
        if (n->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) continue;
        VkPhysicalDeviceMemoryBudgetPropertiesEXT *b = (VkPhysicalDeviceMemoryBudgetPropertiesEXT *)n;
        for (uint32_t i = 0; i < mp->memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
            if (b->heapBudget[i] > cap) b->heapBudget[i] = cap;
    }
}
static void VKAPI_CALL klvk_GetPhysicalDeviceMemoryProperties(
        VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *m) {
    VKT("call GetPhysicalDeviceMemoryProperties\n");   // KL_VK_TRACE: the order of a guest's capability queries
    static PFN_vkGetPhysicalDeviceMemoryProperties real; static int t;
    if (!t) { t = 1; if (real_gipa) real = (PFN_vkGetPhysicalDeviceMemoryProperties)
        real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties"); }
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceMemoryProperties");
    if (real) { real(pd, m); klvk_heap_cap(m, NULL); }
}
// LDR ASTC block formats occupy VK_FORMAT_ASTC_4x4_UNORM_BLOCK(157) ..
// VK_FORMAT_ASTC_12x12_SRGB_BLOCK(184) contiguously.
static int klvk_is_astc(VkFormat f) { return (int)f >= 157 && (int)f <= 184; }

// CANDIDATE FIX (olar/wanderer gray hunt): make ASTC formats report full sampling
// support. If MoltenVK on visionOS under-reports optimalTilingFeatures for ASTC
// (missing SAMPLED_IMAGE / SAMPLED_IMAGE_FILTER_LINEAR), UE decides the ASTC texture
// is not usable for sampling and keeps a default placeholder bound -> gray world.
// Apple GPUs DO sample and linearly filter ASTC, so forcing these bits on is correct
// for this hardware. Also logs what MoltenVK reported first (the probe). emu_on()
// gates it to olar/wanderer. Returns nonzero if it patched (for the log).
#define KLVK_FF_SAMPLED        0x00000001u
#define KLVK_FF_FILTER_LINEAR  0x00001000u
#define KLVK_FF_BLIT_SRC       0x00000400u
#define KLVK_FF_TRANSFER_SRC   0x00004000u
#define KLVK_FF_TRANSFER_DST   0x00008000u
static void klvk_astc_patch_features(VkFormat fmt, VkFormatProperties *p) {
    if (!p || !emu_on() || !klvk_is_astc(fmt)) return;
    uint32_t want = KLVK_FF_SAMPLED | KLVK_FF_FILTER_LINEAR |
                    KLVK_FF_BLIT_SRC | KLVK_FF_TRANSFER_SRC | KLVK_FF_TRANSFER_DST;
    uint32_t before = p->optimalTilingFeatures;
    static int said;
    if (said < 24 && (before & want) != want) { said++;
        VKI("[fix] ASTC fmt %d optimalTilingFeatures %#x (missing %#x) — forcing full "
            "sampled+linear support so UE uses the real texture, not a default\n",
            (int)fmt, before, want & ~before); }
    p->optimalTilingFeatures |= want;
}
static void VKAPI_CALL klvk_GetPhysicalDeviceFormatProperties(
        VkPhysicalDevice pd, VkFormat fmt, VkFormatProperties *p) {
    VKT("call GetPhysicalDeviceFormatProperties\n");   // KL_VK_TRACE: the order of a guest's capability queries
    static PFN_vkGetPhysicalDeviceFormatProperties real; static int t;
    if (!t) { t = 1; if (real_gipa) real = (PFN_vkGetPhysicalDeviceFormatProperties)
        real_gipa(g_instance, "vkGetPhysicalDeviceFormatProperties"); }
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceFormatProperties");
    if (real) real(pd, fmt, p);
    klvk_astc_patch_features(fmt, p);
}
static void VKAPI_CALL klvk_GetPhysicalDeviceFormatProperties2(
        VkPhysicalDevice pd, VkFormat fmt, VkFormatProperties2 *p) {
    VKT("call GetPhysicalDeviceFormatProperties2\n");   // KL_VK_TRACE: the order of a guest's capability queries
    static PFN_vkGetPhysicalDeviceFormatProperties2 real; static int t;
    if (!t) { t = 1; if (real_gipa) { real = (PFN_vkGetPhysicalDeviceFormatProperties2)
        real_gipa(g_instance, "vkGetPhysicalDeviceFormatProperties2");
        if (!real) real = (PFN_vkGetPhysicalDeviceFormatProperties2)
            real_gipa(g_instance, "vkGetPhysicalDeviceFormatProperties2KHR"); } }
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceFormatProperties2");
    if (real) real(pd, fmt, p);
    if (p) klvk_astc_patch_features(fmt, &p->formatProperties);
}
// The two device-enumeration queries Unity runs while VALIDATING a physical
// device (layers, then extensions) — the phase where AC Nexus was passing NULL.
// After CreateDevice (also guarded) the physical device stops mattering.
static VkResult VKAPI_CALL klvk_EnumerateDeviceLayerProperties(
        VkPhysicalDevice pd, uint32_t *count, VkLayerProperties *props) {
    VKT("call EnumerateDeviceLayerProperties\n");   // KL_VK_TRACE: the order of a guest's capability queries
    static PFN_vkEnumerateDeviceLayerProperties real; static int t;
    if (!t) { t = 1; if (real_gipa) real = (PFN_vkEnumerateDeviceLayerProperties)
        real_gipa(g_instance, "vkEnumerateDeviceLayerProperties"); }
    pd = KLVK_PHYS_OR_SUB(pd, "EnumerateDeviceLayerProperties");
    if (real) return real(pd, count, props);
    if (count) *count = 0;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL klvk_EnumerateDeviceExtensionProperties(
        VkPhysicalDevice pd, const char *layer, uint32_t *count,
        VkExtensionProperties *props) {
    VKT("call EnumerateDeviceExtensionProperties\n");   // KL_VK_TRACE: the order of a guest's capability queries
    static PFN_vkEnumerateDeviceExtensionProperties real; static int t;
    if (!t) { t = 1; if (real_gipa) real = (PFN_vkEnumerateDeviceExtensionProperties)
        real_gipa(g_instance, "vkEnumerateDeviceExtensionProperties"); }
    pd = KLVK_PHYS_OR_SUB(pd, "EnumerateDeviceExtensionProperties");
    if (real) return real(pd, layer, count, props);
    if (count) *count = 0;
    return VK_SUCCESS;
}
static void VKAPI_CALL klvk_GetPhysicalDeviceMemoryProperties2(
        VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *props) {
    VKT("call GetPhysicalDeviceMemoryProperties2\n");   // KL_VK_TRACE: the order of a guest's capability queries
    if (!props) return;
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceMemoryProperties2");
    // The app made a Vulkan 1.0 instance, so MoltenVK does not export the core
    // 1.1 name — only the KHR alias (VK_KHR_get_physical_device_properties2 is
    // enabled). Prefer the real one so the pNext chain (VK_EXT_memory_budget)
    // is filled; fall back to the v1 struct so the guest at least learns its
    // heaps and memory types. Without this the guest got a zeroed struct — no
    // usable memory — and UE4 called AndroidThunkJava_ForceQuit.
    static PFN_vkGetPhysicalDeviceMemoryProperties2 real;
    static int tried;
    if (!tried) {
        tried = 1;
        if (real_gipa) {
            real = (PFN_vkGetPhysicalDeviceMemoryProperties2)
                real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties2");
            if (!real) real = (PFN_vkGetPhysicalDeviceMemoryProperties2)
                real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties2KHR");
        }
    }
    if (real) { real(pd, props); klvk_heap_cap(&props->memoryProperties, props->pNext); return; }
    PFN_vkGetPhysicalDeviceMemoryProperties v1 = real_gipa
        ? (PFN_vkGetPhysicalDeviceMemoryProperties)
              real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties")
        : NULL;
    if (v1) { v1(pd, &props->memoryProperties); klvk_heap_cap(&props->memoryProperties, NULL); }
}

static VkResult VKAPI_CALL klvk_GetPhysicalDeviceImageFormatProperties2(
        VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info,
        VkImageFormatProperties2 *props) {
    VKT("call GetPhysicalDeviceImageFormatProperties2\n");   // KL_VK_TRACE: the order of a guest's capability queries
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceImageFormatProperties2");
    static PFN_vkGetPhysicalDeviceImageFormatProperties2 real;
    if (!real && real_gipa) {
        real = (PFN_vkGetPhysicalDeviceImageFormatProperties2)
            real_gipa(g_instance, "vkGetPhysicalDeviceImageFormatProperties2");
        if (!real) real = (PFN_vkGetPhysicalDeviceImageFormatProperties2)
            real_gipa(g_instance, "vkGetPhysicalDeviceImageFormatProperties2KHR");
    }
    VkResult r = real ? real(pd, info, props) : VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (r == VK_SUCCESS && props) props->imageFormatProperties.sampleCounts &= klvk_sample_mask();
    if (info && (r != VK_SUCCESS || (klvk_trace() && (info->usage & 0x30u))))
        fprintf(stderr, "[TRACE] [imgfmt2] fmt %d type %d tiling %d usage %#x flags %#x -> %d%s\n",
            (int)info->format, (int)info->type, (int)info->tiling, info->usage,
            info->flags, (int)r, r != VK_SUCCESS ? "  <== UNSUPPORTED" : "");
    return r;
}
// KL_TRACE: which thread submits guest work. Compared with [thr] createRT this
// shows whether Unity creates its temp render targets on the same thread that
// records/submits the frame, or a different one (the temp-RT race hypothesis).
static VkResult VKAPI_CALL klvk_QueueSubmit(VkQueue q, uint32_t n,
        const VkSubmitInfo *si, VkFence fence) {
    KFE(KFE_SUBMIT);
    static PFN_vkQueueSubmit real;
    if (!real && real_gdpa) real = (PFN_vkQueueSubmit)real_gdpa(g_devs[0]?g_devs[0]->dev:NULL, "vkQueueSubmit");
    if (klvk_trace()) {
        static int c;
        if (c < 40) { c++;
            unsigned cb = 0; for (uint32_t i=0;i<n && si;i++) cb += si[i].commandBufferCount;
            fprintf(stderr, "[TRACE] [thr] QueueSubmit tid=%u batches=%u cmdbufs=%u fence=%d\n",
                klvk_tid(), n, cb, fence != VK_NULL_HANDLE);
        }
    }
    VkResult r = real ? real(q, n, si, fence) : VK_ERROR_INITIALIZATION_FAILED;
    // Same thread, same queue, right after the guest's work: copy every wanted
    // overlay texture into its persistent shadow (see klvk_shadow_pump).
    if (r == VK_SUCCESS) klvk_shadow_pump();
    // KL_VK_TEXELBUF_PROBE: after enough submits that the guest is well past its
    // resource uploads, dump the backing bytes of the first few texel buffers
    // once. Deferred to a submit count (not frame 0) so device-local uploads
    // have actually happened by the time we look.
    if (probe_on()) {
        static unsigned subs;
        static int at = -1;
        if (at < 0) at = kl_env_int("KL_VK_TEXELBUF_PROBE_AT", 240);
        if ((int)(++subs) == at) probe_run();
    }
    return r;
}
// UE4's Variable Rate Shading / foveation probe. Two-call enumeration:
// (count, NULL) asks how many rates exist, (count, array) fills that many. The
// generic no-op stub returned VK_SUCCESS WITHOUT writing *count, leaving the
// guest to size an array from an undefined number and then walk it — a field
// read off a bad pointer that lands as a null-ish deref inside UE4's own render
// code, with no Klepton frame on the stack. MoltenVK has no fragment shading
// rate support, so the truthful answer is zero rates: write *count = 0 and
// return VK_SUCCESS, and the guest cleanly takes its no-foveation path. `rates`
// is left untouched because zero rates are being reported into it.
static VkResult VKAPI_CALL klvk_GetPhysicalDeviceFragmentShadingRatesKHR(
        VkPhysicalDevice pd, uint32_t *count, void *rates) {
    VKT("call GetPhysicalDeviceFragmentShadingRatesKHR\n");   // KL_VK_TRACE: the order of a guest's capability queries
    (void)pd; (void)rates;
    if (count) *count = 0;
    return VK_SUCCESS;
}

// Diagnostic pass-through. UE4/UE5 pick a GPU here and, if none is acceptable,
// silently fall back and put up "This device does not support Vulkan…" (a fatal
// message box on a Vulkan-only package) — with NOTHING in our log to say why.
// Wrath2 dies exactly this way: one probe VkInstance, no device ever created.
// So log, once, the count MoltenVK returns and the properties the engine reads
// off each device (apiVersion, type, vendor/device ID, graphics-queue support).
// A count of 0 means the portability-enumeration gate ate the device; a count of
// 1+ means the engine rejected it on a property the dump then names. Faithful
// forward otherwise, so every other target is unaffected.
static VkResult VKAPI_CALL klvk_EnumeratePhysicalDevices(
        VkInstance inst, uint32_t *count, VkPhysicalDevice *devs) {
    static PFN_vkEnumeratePhysicalDevices real;
    if (!real && real_gipa) real = (PFN_vkEnumeratePhysicalDevices)
        real_gipa(inst ? inst : g_instance, "vkEnumeratePhysicalDevices");
    if (!real) { if (count) *count = 0; return VK_ERROR_INITIALIZATION_FAILED; }

    static int logged;
    if (!logged) {
        logged = 1;
        uint32_t n = 0;
        VkResult rr = real(inst, &n, NULL);
        VKI("vkEnumeratePhysicalDevices -> %u device(s) (result %d)\n", n, (int)rr);
        if (n) {
            VkPhysicalDevice tmp[8];
            uint32_t give = n < 8 ? n : 8;
            real(inst, &give, tmp);
            PFN_vkGetPhysicalDeviceProperties gp =
                (PFN_vkGetPhysicalDeviceProperties)
                    real_gipa(inst, "vkGetPhysicalDeviceProperties");
            PFN_vkGetPhysicalDeviceQueueFamilyProperties gq =
                (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
                    real_gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
            for (uint32_t i = 0; i < give && gp; i++) {
                VkPhysicalDeviceProperties p; memset(&p, 0, sizeof p);
                gp(tmp[i], &p);
                unsigned nq = 0, gfx = 0;
                if (gq) {
                    gq(tmp[i], &nq, NULL);
                    VkQueueFamilyProperties qf[16];
                    unsigned gv = nq < 16 ? nq : 16;
                    gq(tmp[i], &gv, qf);
                    for (unsigned k = 0; k < gv; k++) {
                        if (qf[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) gfx = 1;
                        VKI("    family %u: flags %#x, %u queue(s)\n", k,
                            (unsigned)qf[k].queueFlags, qf[k].queueCount);
                    }
                }
                {
                    PFN_vkGetPhysicalDeviceFeatures gf =
                        (PFN_vkGetPhysicalDeviceFeatures)real_gipa(inst, "vkGetPhysicalDeviceFeatures");
                    VkPhysicalDeviceFeatures pf; memset(&pf, 0, sizeof pf);
                    if (gf) gf(tmp[i], &pf);
                    VKI("  phys[%u] logicOp %u dualSrcBlend %u (MVK_CONFIG_USE_METAL_PRIVATE_API=%s)\n", i,
                        pf.logicOp, pf.dualSrcBlend, getenv("MVK_CONFIG_USE_METAL_PRIVATE_API") ? getenv("MVK_CONFIG_USE_METAL_PRIVATE_API") : "unset");
                }
                VKI("  phys[%u] '%s' api %u.%u.%u type %d vendor %#x device %#x "
                    "queueFamilies %u graphics %d\n", i, p.deviceName,
                    VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
                    VK_VERSION_PATCH(p.apiVersion), (int)p.deviceType,
                    p.vendorID, p.deviceID, nq, gfx);
            }
        }
    }
    VkResult rr = real(inst, count, devs);
    // Every FETCH (devs != NULL), not just the first: AC Nexus enumerates on a
    // throwaway instance, destroys it, makes a second, and reaches device init
    // with a NULL physicalDevice — so the question "what did enumerate hand the
    // guest on THIS instance?" has to be answerable per instance, not once.
    if (devs && count)
        VKI("vkEnumeratePhysicalDevices(inst %p) FETCH -> %u, devs[0] %p (result %d)\n",
            (void *)inst, *count, (void *)devs[0], (int)rr);
    return rr;
}

// Draw-time null-resource tracers (Into the Radius: MVKCmdDrawIndexed::encode
// walks a null object at +0x20 after ~31 clean frames). A null VkBuffer bound as
// the index/vertex source is the usual cause; log it (once each) so the culprit
// is named without a trace flag, and forward unchanged.
// Per-command-buffer record of the index buffer bound since vkBeginCommandBuffer.
// The Into the Radius crash is a nil MTLBuffer handed to Metal's
// drawIndexedPrimitives from MVKCmdDrawIndexed::encode (verified by disassembly:
// the faulting arg x5 is loaded from the encoder's index binding at +0x4c8, and
// x8=0x18 / fault at 0x20 is a base-null object walk). That happens when the
// index binding has no live MTLBuffer — either the guest issued vkCmdDrawIndexed
// with no vkCmdBindIndexBuffer in this command buffer, or it bound a VkBuffer
// whose backing MTLBuffer is nil. Neither is caught by the NULL-*handle* check
// below (the handle is non-null; the *Metal* buffer is missing). So track the
// bound handle per command buffer and re-check it at draw time. 64 slots covers
// UE4's in-flight command buffers with room to spare; a table miss simply loses
// the guard for that one buffer, it never forwards a draw we know to be bad.
#define KLVK_CB_SLOTS 64
static struct { void *cb; uint64_t idxbuf; } g_cb_idx[KLVK_CB_SLOTS];
static void klvk_cb_set_idx(void *cb, uint64_t buf) {
    if (!cb) return;
    int freei = -1;
    for (int i = 0; i < KLVK_CB_SLOTS; i++) {
        if (g_cb_idx[i].cb == cb) { g_cb_idx[i].idxbuf = buf; return; }
        if (freei < 0 && !g_cb_idx[i].cb) freei = i;
    }
    if (freei >= 0) { g_cb_idx[freei].cb = cb; g_cb_idx[freei].idxbuf = buf; }
}
static uint64_t klvk_cb_get_idx(void *cb) {
    for (int i = 0; i < KLVK_CB_SLOTS; i++)
        if (g_cb_idx[i].cb == cb) return g_cb_idx[i].idxbuf;
    return 0;
}
// Ask MoltenVK for the MTLBuffer backing a VkBuffer. Returns NULL when the
// buffer has no memory bound yet (the state that feeds the nil-indexBuffer
// crash) or when MoltenVK does not export the accessor. The ABI is a pointer
// out-param, declared plain-C here just like klvk_overlay_mtl_texture above.
static void *klvk_buf_mtl(uint64_t buf) {
    typedef void (*pfn_get_mtl_buffer_mvk)(uint64_t, void **);
    static pfn_get_mtl_buffer_mvk get;
    static int resolved;
    if (!resolved) { resolved = 1; get = (pfn_get_mtl_buffer_mvk)mvk_sym("vkGetMTLBufferMVK"); }
    if (!get || !buf) return NULL;
    void *b = NULL;
    get(buf, &b);
    return b;
}
static int klvk_have_buf_mtl(void) {
    return mvk_sym("vkGetMTLBufferMVK") != NULL;
}
// vkBeginCommandBuffer restarts recording: any index binding from a previous use
// of this command buffer is gone, so clear our record. Without this reset the
// table would report a stale (now-irrelevant) index buffer as "still bound".
static VkResult VKAPI_CALL klvk_BeginCommandBuffer(void *cb, const void *bi) {
    static VkResult (*real)(void *, const void *);
    if (!real && real_gdpa) real = (VkResult (*)(void *, const void *))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkBeginCommandBuffer");
    klvk_cb_set_idx(cb, 0);
    klvk_vin_begin(cb);
    klvk_vi_begin(cb);
    fove_sec_begin((VkCommandBuffer)cb);   // a re-recorded secondary no longer carries its old indirect draw
    return real ? real(cb, bi) : VK_ERROR_INITIALIZATION_FAILED;
}
static void VKAPI_CALL klvk_CmdBindIndexBuffer(void *cb, uint64_t buffer,
        uint64_t offset, uint32_t indexType) {
    static void (*real)(void *, uint64_t, uint64_t, uint32_t);
    if (!real && real_gdpa) real = (void (*)(void *, uint64_t, uint64_t, uint32_t))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdBindIndexBuffer");
    if (!buffer) {
        static int said; if (said < 8) { said++;
            VKI("vkCmdBindIndexBuffer: NULL index buffer bound (offset %llu type %u) "
                "— a following vkCmdDrawIndexed will dereference null in MoltenVK\n",
                (unsigned long long)offset, indexType); }
    }
    klvk_cb_set_idx(cb, buffer);
    if (real) real(cb, buffer, offset, indexType);
}
// The actual crash site. MoltenVK will feed the index binding's MTLBuffer to
// Metal's drawIndexedPrimitives; when that MTLBuffer is nil the AGX driver walks
// a null object and the process dies (SIGSEGV KERN_INVALID_ADDRESS at 0x20,
// inside MVKCmdDrawIndexed::encode+0x768). Check the bound index buffer here and,
// if it has no live MTLBuffer, skip the draw instead — one dropped draw is far
// better than taking the process down. Guarded so we only ever skip a draw we can
// PROVE is bad: if MoltenVK does not export the MTLBuffer accessor we cannot
// verify anything and forward unchanged (preserving old behaviour).
// The other things a guest can do inside a render pass, counted for
// KL_VK_PROFILE and otherwise passed straight on. They are here because of
// what MoltenVK does with some of them: an indirect draw in a multiview pass
// needs its instance count rewritten by a compute step, and Metal cannot run one
// inside a render pass — so MoltenVK ENDS the pass, storing every sample of it,
// and re-opens it with a load. A Vulkan-side count of loads and stores shows
// none of that (kl_mtl_foveate.m's trace does: two Metal encoders for one
// Vulkan scene pass).
#define KLVK_REAL(name) \
    static void *real; \
    if (!real && real_gdpa) real = (void *)real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, name)
// KL_VK_GRID_DILATE (default on for HITMAN 3 VR: Reloaded): conservative
// rasterization for draws through a 100x60 viewport, made of five draws — the
// draw, then four more with the viewport moved by just under half a pixel each
// way, so that every pixel a primitive touches gets a fragment. The game sorts
// the gates between its rooms (6114aaf2) and its lights (8653a979) into two
// screen grids of 100x60 cells by drawing their volumes into such a viewport, and
// a lighting shader takes a pixel's room and lights from its cell. A cell at a
// slanted edge whose centre a volume misses was left out here but not on the
// Quest, and was lit as the wrong room: lighter steps along the window in the VR
// tutorial's third room and along shadow edges (headset, 2026-10-22). Both shaders
// keep an index once (an atomic list, a logic OR), so the repeats add nothing
// twice. klvk_grid_dilate says how many repeats a draw gets (0 or 4).
static int klvk_grid_dilate(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_VK_GRID_DILATE", t && !strcmp(t, "x8012387035448196"));
        if (on) VKI("KL_VK_GRID_DILATE: draws through a 100x60 viewport are repeated with it moved by "
                    "+-0.49 pixel (conservative rasterization)\n");
    }
    if (!on || !t_vp_n) return 0;
    float h = t_vp[0].height < 0 ? -t_vp[0].height : t_vp[0].height;
    return t_vp[0].width == 100.0f && h == 60.0f ? 4 : 0;
}
// Repeat k (0..3) moves the viewports set last on this thread; -1 puts them back.
static void klvk_grid_dilate_vp(void *cb, int k) {
    static void (*setvp)(void *, uint32_t, uint32_t, const VkViewport *);
    if (!setvp && real_gdpa) setvp = (void (*)(void *, uint32_t, uint32_t, const VkViewport *))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdSetViewport");
    if (!setvp || !t_vp_n) return;
    if (k < 0) { setvp(cb, t_vp_first, t_vp_n, t_vp); return; }
    static const float o[4][2] = { { -0.49f, -0.49f }, { 0.49f, -0.49f }, { -0.49f, 0.49f }, { 0.49f, 0.49f } };
    VkViewport v[4];
    for (uint32_t i = 0; i < t_vp_n; i++) { v[i] = t_vp[i]; v[i].x += o[k][0]; v[i].y += o[k][1]; }
    setvp(cb, t_vp_first, t_vp_n, v);
}
static void VKAPI_CALL klvk_CmdDraw(void *cb, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    KLVK_REAL("vkCmdDraw");
    if (klvk_ap_skip(cb)) return;
    __atomic_add_fetch(&g_prof_nonindexed, 1, __ATOMIC_RELAXED);
    if (klvk_fs_draw()) return;
    if (real) ((void (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t))real)(cb, a, b, c, d);
    for (int k = 0, n = real ? klvk_grid_dilate() : 0; k < n; k++) {
        klvk_grid_dilate_vp(cb, k);
        ((void (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t))real)(cb, a, b, c, d);
        if (k == n - 1) klvk_grid_dilate_vp(cb, -1);
    }
}
static void VKAPI_CALL klvk_CmdDrawIndirect(void *cb, VkBuffer buf, VkDeviceSize off,
        uint32_t count, uint32_t stride) {
    KLVK_REAL("vkCmdDrawIndirect");
    if (klvk_ap_skip(cb)) return;
    __atomic_add_fetch(&g_prof_indirect, 1, __ATOMIC_RELAXED);
    if (real) ((void (*)(void *, VkBuffer, VkDeviceSize, uint32_t, uint32_t))real)(cb, buf, off, count, stride);
    for (int k = 0, n = real ? klvk_grid_dilate() : 0; k < n; k++) {
        klvk_grid_dilate_vp(cb, k);
        ((void (*)(void *, VkBuffer, VkDeviceSize, uint32_t, uint32_t))real)(cb, buf, off, count, stride);
        if (k == n - 1) klvk_grid_dilate_vp(cb, -1);
    }
}
static void VKAPI_CALL klvk_CmdDrawIndexedIndirect(void *cb, VkBuffer buf, VkDeviceSize off,
        uint32_t count, uint32_t stride) {
    KLVK_REAL("vkCmdDrawIndexedIndirect");
    if (klvk_ap_skip(cb)) return;
    __atomic_add_fetch(&g_prof_indirect, 1, __ATOMIC_RELAXED);
    if (real) ((void (*)(void *, VkBuffer, VkDeviceSize, uint32_t, uint32_t))real)(cb, buf, off, count, stride);
    for (int k = 0, n = real ? klvk_grid_dilate() : 0; k < n; k++) {
        klvk_grid_dilate_vp(cb, k);
        ((void (*)(void *, VkBuffer, VkDeviceSize, uint32_t, uint32_t))real)(cb, buf, off, count, stride);
        if (k == n - 1) klvk_grid_dilate_vp(cb, -1);
    }
}
static void VKAPI_CALL klvk_CmdClearAttachments(void *cb, uint32_t na, const VkClearAttachment *a,
        uint32_t nr, const VkClearRect *r) {
    KLVK_REAL("vkCmdClearAttachments");
    __atomic_add_fetch(&g_prof_clear_att, 1, __ATOMIC_RELAXED);
    if (real) ((void (*)(void *, uint32_t, const VkClearAttachment *, uint32_t, const VkClearRect *))real)(cb, na, a, nr, r);
}
static int klvk_no_query(void) {
    static int v = -1;
    if (v < 0) v = kl_env_on("KL_VK_NO_QUERY", 0);
    return v;
}
static void VKAPI_CALL klvk_CmdBeginQuery(void *cb, VkQueryPool pool, uint32_t q, VkQueryControlFlags f) {
    KLVK_REAL("vkCmdBeginQuery");
    __atomic_add_fetch(&g_prof_queries, 1, __ATOMIC_RELAXED);
    if (klvk_no_query()) return;
    if (kl_env_on("KL_VK_QUERY_TRACE", 0)) {
        static unsigned n;
        if (__atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 400)
            fprintf(stderr, "  [vk] begin query: pool %p index %u flags %#x\n", (void *)pool, q, f);
    }
    if (real) ((void (*)(void *, VkQueryPool, uint32_t, VkQueryControlFlags))real)(cb, pool, q, f);
}
static VkResult VKAPI_CALL klvk_CreateQueryPool(VkDevice dev, const VkQueryPoolCreateInfo *ci,
        const VkAllocationCallbacks *a, VkQueryPool *out) {
    static PFN_vkCreateQueryPool real;
    if (!real && real_gdpa) real = (PFN_vkCreateQueryPool)real_gdpa(dev, "vkCreateQueryPool");
    VkResult r = real ? real(dev, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (ci) fprintf(stderr, "  [vk] vkCreateQueryPool: type %d (0 occlusion, 1 statistics, 2 timestamp), "
                            "%u queries -> %d\n", (int)ci->queryType, ci->queryCount, (int)r);
    return r;
}
// KL_VK_QUERY_TRACE=1: what the guest is told its queries counted.
static VkResult VKAPI_CALL klvk_GetQueryPoolResults(VkDevice dev, VkQueryPool pool, uint32_t first,
        uint32_t count, size_t size, void *data, VkDeviceSize stride, VkQueryResultFlags flags) {
    static PFN_vkGetQueryPoolResults real;
    if (!real && real_gdpa) real = (PFN_vkGetQueryPoolResults)real_gdpa(dev, "vkGetQueryPoolResults");
    VkResult r = real ? real(dev, pool, first, count, size, data, stride, flags) : VK_ERROR_DEVICE_LOST;
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_QUERY_TRACE", 0);
    if (on && data && count) {
        static unsigned n;
        unsigned k = __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED);
        if (k < 40000) {
            char line[512]; int o = 0;
            for (uint32_t i = 0; i < count && i < 6 && o < 440; i++) {
                const uint8_t *p = (const uint8_t *)data + i * stride;
                uint64_t v = (flags & VK_QUERY_RESULT_64_BIT) ? *(const uint64_t *)p : *(const uint32_t *)p;
                uint64_t av = ~0ull;
                if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
                    av = (flags & VK_QUERY_RESULT_64_BIT) ? ((const uint64_t *)p)[1] : ((const uint32_t *)p)[1];
                o += snprintf(line + o, sizeof line - (size_t)o, " %llu%s", (unsigned long long)v,
                              av == ~0ull ? "" : av ? "(ready)" : "(NOT ready)");
            }
            fprintf(stderr, "  [vk] query results #%u: pool %p first %u count %u flags %#x -> %d:%s\n",
                    k, (void *)pool, first, count, flags, (int)r, line);
        }
    }
    return r;
}
static void VKAPI_CALL klvk_CmdEndQuery(void *cb, VkQueryPool pool, uint32_t q) {
    KLVK_REAL("vkCmdEndQuery");
    if (klvk_no_query()) return;
    if (real) ((void (*)(void *, VkQueryPool, uint32_t))real)(cb, pool, q);
}
// vkCmdSetBlendConstants. KL_VK_BLEND_CONST_TRACE=1 says which values a guest
// sets and how often; KL_VK_BLEND_CONST=r,g,b,a replaces every one of them, which
// shows what on screen is drawn with a constant-colour blend.
static void VKAPI_CALL klvk_CmdSetBlendConstants(void *cb, const float c[4]) {
    KLVK_REAL("vkCmdSetBlendConstants");
    static int trace = -1, forced = -1;
    static float fc[4];
    if (trace < 0) {
        trace = kl_env_on("KL_VK_BLEND_CONST_TRACE", 0);
        const char *e = kl_env_str("KL_VK_BLEND_CONST", NULL);
        forced = e && sscanf(e, "%f,%f,%f,%f", &fc[0], &fc[1], &fc[2], &fc[3]) == 4;
    }
    if (trace && c) {
        static struct { float c[4]; unsigned n; } seen[24];
        static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
        static unsigned total;
        pthread_mutex_lock(&mu);
        for (unsigned i = 0; i < 24; i++) {
            if (seen[i].n && !memcmp(seen[i].c, c, sizeof seen[i].c)) { seen[i].n++; break; }
            if (!seen[i].n) { memcpy(seen[i].c, c, sizeof seen[i].c); seen[i].n = 1; break; }
        }
        if (!(++total % 20000)) {
            fprintf(stderr, "  [vk] blend constants after %u calls:", total);
            for (unsigned i = 0; i < 24 && seen[i].n; i++)
                fprintf(stderr, " (%.2f %.2f %.2f %.2f)x%u", (double)seen[i].c[0], (double)seen[i].c[1],
                        (double)seen[i].c[2], (double)seen[i].c[3], seen[i].n);
            fprintf(stderr, "\n");
        }
        pthread_mutex_unlock(&mu);
    }
    if (forced) c = fc;
    if (real) ((void (*)(void *, const float *))real)(cb, c);
}
// Custom multisample positions (VK_EXT_sample_locations). KL_VK_SAMPLE_LOC_TRACE=1
// says what the guest asks for; KL_VK_NO_SAMPLE_LOCATIONS=1 turns every request
// into "the standard positions".
static int klvk_no_sample_loc(void) {
    static int v = -1;
    if (v < 0) v = kl_env_on("KL_VK_NO_SAMPLE_LOCATIONS", 0);
    return v;
}
static void VKAPI_CALL klvk_CmdSetSampleLocationsEXT(void *cb, const VkSampleLocationsInfoEXT *info) {
    KLVK_REAL("vkCmdSetSampleLocationsEXT");
    static int trace = -1;
    if (trace < 0) trace = kl_env_on("KL_VK_SAMPLE_LOC_TRACE", 0);
    if (trace && info) {
        static unsigned n;
        unsigned k = __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED);
        if (k < 6 || (k >= 6000 && k < 6070)) {
            char line[256]; int o = 0;
            for (uint32_t i = 0; i < info->sampleLocationsCount && i < 8 && o < 220; i++)
                o += snprintf(line + o, sizeof line - (size_t)o, " (%.3f %.3f)",
                              (double)info->pSampleLocations[i].x, (double)info->pSampleLocations[i].y);
            fprintf(stderr, "  [vk] sample locations #%u: %d per pixel, grid %ux%u:%s\n", k,
                    (int)info->sampleLocationsPerPixel, info->sampleLocationGridSize.width,
                    info->sampleLocationGridSize.height, line);
        }
    }
    if (klvk_no_sample_loc()) return;
    if (real) ((void (*)(void *, const VkSampleLocationsInfoEXT *))real)(cb, info);
}
static void VKAPI_CALL klvk_CmdSetSampleLocationsEnableEXT(void *cb, VkBool32 on) {
    KLVK_REAL("vkCmdSetSampleLocationsEnableEXT");
    static int trace = -1;
    if (trace < 0) trace = kl_env_on("KL_VK_SAMPLE_LOC_TRACE", 0);
    if (trace) {
        static unsigned n[2];
        unsigned k = __atomic_add_fetch(&n[on ? 1 : 0], 1, __ATOMIC_RELAXED);
        if (k == 1 || k == 1000 || k == 100000)
            fprintf(stderr, "  [vk] sample locations enable = %d (%u time(s) so far)\n", (int)on, k);
    }
    if (klvk_no_sample_loc()) on = VK_FALSE;
    if (real) ((void (*)(void *, VkBool32))real)(cb, on);
}
static void VKAPI_CALL klvk_CmdDrawIndexed(void *cb, uint32_t indexCount,
        uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
        uint32_t firstInstance) {
    static void (*real)(void *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
    if (!real && real_gdpa) real = (void (*)(void *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdDrawIndexed");
    __atomic_add_fetch(&g_prof_draws, 1, __ATOMIC_RELAXED);
    if (klvk_ap_skip(cb)) return;
    if (klvk_fs_draw()) return;
    // KL_VK_DRAW_GUARD (default OFF): the skip below drops a draw whenever it
    // cannot find a live MTLBuffer for the index binding. That decision rests on
    // g_cb_idx, a 64-slot, UNLOCKED table that is never drained (no
    // vkFreeCommandBuffers/vkResetCommandPool removal) and is written from every
    // command-recording thread. Under a heavily multithreaded recorder (UE5
    // mobile parallel translate) it (a) races and (b) fills with stale command-
    // buffer handles, after which a legitimately-bound draw reads buf==0 and is
    // SKIPPED — i.e. geometry silently goes INVISIBLE. That is exactly the class
    // of symptom olar shows, so the guard must be opt-in: enable it only for the
    // title whose nil-indexBuffer crash it was written for (Into the Radius), and
    // leave every other title on faithful pass-through. Defaulted per-target ON
    // for intotheradius (so its crash-fix survives) and OFF elsewhere — a plain
    // default-OFF would regress intotheradius straight back into the MoltenVK
    // nil-indexBuffer crash the skip exists to prevent.
    extern const char *kl_driver_target_name(void);
    static int guard = -1;
    if (guard < 0) {
        const char *t = kl_driver_target_name();
        int def = (t && (strcmp(t, "intotheradius") == 0 ||
                        strcmp(t, "x5817243991680545") == 0)) ? 1 : 0;
        guard = kl_env_on("KL_VK_DRAW_GUARD", def);
    }
    if (guard && klvk_have_buf_mtl()) {
        uint64_t buf = klvk_cb_get_idx(cb);
        if (!buf || !klvk_buf_mtl(buf)) {
            static int said; if (said < 16) { said++;
                VKI("vkCmdDrawIndexed: bound index buffer %#llx has no MTLBuffer "
                    "(%s) — skipping draw (indexCount %u instanceCount %u "
                    "firstIndex %u) to avoid the nil-indexBuffer crash in "
                    "MoltenVK's MVKCmdDrawIndexed::encode\n",
                    (unsigned long long)buf, buf ? "unbacked" : "none bound",
                    indexCount, instanceCount, firstIndex); }
            return;
        }
    }
    if (real) real(cb, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    for (int k = 0, n = real ? klvk_grid_dilate() : 0; k < n; k++) {
        klvk_grid_dilate_vp(cb, k);
        real(cb, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
        if (k == n - 1) klvk_grid_dilate_vp(cb, -1);
    }
}
static void VKAPI_CALL klvk_CmdBindPipeline(void *cb, uint32_t bindPoint, uint64_t pipeline) {
    static void (*real)(void *, uint32_t, uint64_t);
    if (!real && real_gdpa) real = (void (*)(void *, uint32_t, uint64_t))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdBindPipeline");
    if (!pipeline) { static int said; if (said < 8) { said++;
        VKI("vkCmdBindPipeline: NULL pipeline bound (bindPoint %u) — a following "
            "draw will dereference null in MoltenVK\n", bindPoint); } }
    // A variant bound by the dynamic-vertex-input emulation keeps the shader
    // hash its deferred pipeline was bound with (klvk_vi_settle).
    if (bindPoint == 0 /* graphics */ && klvk_fs_on() && !t_vi_internal) {
        t_fs_hash = klvk_fs_get(g_fs_pipe, pipeline);
        if (!t_fs_hash) t_fs_hash = 0xffffffffu;
        t_fs_skip = 0;
        for (int i = 0; i < g_fs_nskip; i++) if (g_fs_skip[i] == t_fs_hash) { t_fs_skip = 1; break; }
    }
    if (bindPoint == 0 /* graphics */) {
        t_bind_cb = (VkCommandBuffer)cb;
        t_bind_reads = klvk_handle_set_has(g_subpass_pipe, pipeline);
        if (t_bind_reads) klvk_held_barrier_flush((VkCommandBuffer)cb);
        if (t_bind_reads) klvk_after_note(0, 1);
        if (t_bind_reads) klvk_ev_start();
    }
    int drop = 0;
    pipeline = klvk_ap_bind(cb, bindPoint, pipeline, &drop);
    if (drop) return;
    if (klvk_vi_bind(cb, bindPoint, pipeline)) return;   // dynamic vertex input: bound by the layout
    if (real) real(cb, bindPoint, pipeline);
    if (bindPoint == 0 /* graphics */) klvk_vin_pipeline_bound(cb, pipeline);
}
// Defined in the 2D-mirror block below; used here to record the bind-time GPU copy.
#define KLVK_BIND_SEEN 128   /* max distinct mirrors deduped per bind / push call */
static void klvk_dset_gpu_copy(VkCommandBuffer cb, uint64_t set, int *seen, int *nseen);
static void VKAPI_CALL klvk_CmdBindDescriptorSets(void *cb, uint32_t bindPoint,
        uint64_t layout, uint32_t firstSet, uint32_t count, const uint64_t *sets,
        uint32_t dynCount, const uint32_t *dyn) {
    static void (*real)(void *, uint32_t, uint64_t, uint32_t, uint32_t, const uint64_t *, uint32_t, const uint32_t *);
    if (!real && real_gdpa) real = (void (*)(void *, uint32_t, uint64_t, uint32_t, uint32_t, const uint64_t *, uint32_t, const uint32_t *))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdBindDescriptorSets");
    if (sets) for (uint32_t i = 0; i < count; i++) klvk_ev(" B%llu:%llx", firstSet + i, sets[i] & 0xffffff);
    if (sets) for (uint32_t i = 0; i < count; i++) if (!sets[i]) {
        static int said; if (said < 8) { said++;
            VKI("vkCmdBindDescriptorSets: NULL descriptor set at %u — a draw using "
                "it may dereference null in MoltenVK\n", firstSet + i); } break; }
    // 2D-mirror freshness (olar/wanderer only): record a GPU vkCmdCopyBufferToImage
    // (bracketed by barriers) that fills each mirror image used by the sets being
    // bound, ON THIS command buffer, right before the draws that sample it. The
    // copy executes at submit — AFTER the guest's per-frame CPU writes to its pool
    // — so the mirror is always fresh, and OPTIMAL tiling makes it sampleable
    // (both problems the KL_VK_EMU_PROBE run exposed). Deduplicated per bind so one
    // bind copies each mirror once; a later re-bind copies again (freshness).
    // When emu_on() is false this whole block is skipped and the call below is the
    // exact passthrough it was.
    if (emu_on() && sets) {
        int seen[KLVK_BIND_SEEN]; int nseen = 0;
        pthread_mutex_lock(&g_lock);
        for (uint32_t i = 0; i < count; i++)
            if (sets[i]) klvk_dset_gpu_copy((VkCommandBuffer)cb, sets[i], seen, &nseen);
        pthread_mutex_unlock(&g_lock);
    }
    if (real) real(cb, bindPoint, layout, firstSet, count, sets, dynCount, dyn);
}
static void VKAPI_CALL klvk_CmdBindVertexBuffers(void *cb, uint32_t first,
        uint32_t count, const uint64_t *buffers, const uint64_t *offsets) {
    static void (*real)(void *, uint32_t, uint32_t, const uint64_t *, const uint64_t *);
    if (!real && real_gdpa) real = (void (*)(void *, uint32_t, uint32_t, const uint64_t *, const uint64_t *))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdBindVertexBuffers");
    if (buffers) for (uint32_t i = 0; i < count; i++) if (!buffers[i]) {
        static int said; if (said < 8) { said++;
            VKI("vkCmdBindVertexBuffers: NULL vertex buffer at binding %u — a draw "
                "using it will dereference null in MoltenVK\n", first + i); }
        break;
    }
    if (real) real(cb, first, count, buffers, offsets);
    klvk_vin_guest_bound(cb, first, count, buffers, offsets);
}

// KL_VK_DUMP_TEXELBUF (diagnostic, default OFF) — name every UNIFORM/STORAGE
// texel buffer (samplerBuffer / imageBuffer) the guest creates, with its format
// and byte range. This is the cheapest A/B for the black-vs-invisible split on a
// pass-through title like olar: Klepton neither substitutes, drops, nor rejects
// any of olar's own resources (log-proven), so the corruption is inside
// MoltenVK's translation of specific draws. UE mobile fetches per-instance and
// per-bone transforms through texel buffers, and a texel buffer MoltenVK reads
// as zero collapses that geometry to the origin — i.e. it goes INVISIBLE — while
// a colour/param texel buffer read wrong renders BLACK. This hook does not
// change behaviour; it only reveals whether olar uses texel buffers and which
// formats, so the two symptom sets can be correlated to a resource class.
// (There is no in-repo texel-buffer emulation on the Vulkan path — only the GLES
// path has klfb_rewrite_texel_buffers — so a confirmed miss here points at a
// MoltenVK gap, not a Klepton wrapper.)
// Faithful passthrough wrappers that also feed the probe. When the probe is off
// they are one cached-flag test plus the real call.
static VkResult VKAPI_CALL klvk_CreateBuffer(VkDevice dev, const VkBufferCreateInfo *ci,
        const VkAllocationCallbacks *a, VkBuffer *out) {
    KFE(KFE_BUF_NEW);
    static PFN_vkCreateBuffer real;
    if (!real && real_gdpa) real = (PFN_vkCreateBuffer)real_gdpa(dev, "vkCreateBuffer");
    // 2D-mirror (olar/wanderer only): the per-bind GPU copy uses the guest's texel-
    // buffer POOL as a vkCmdCopyBufferToImage SOURCE, which requires TRANSFER_SRC
    // usage the guest did not ask for. Add it to any buffer created with texel-
    // buffer usage. Adding a usage bit is transparent to the guest (the memory
    // requirements it queries afterwards already reflect it), and it is gated so no
    // other title's buffers change. Everything else is the exact passthrough.
    VkBufferCreateInfo mod;
    if (emu_on() && ci &&
        (ci->usage & (VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                      VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)) &&
        !(ci->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
        mod = *ci;
        mod.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        ci = &mod;
    }
    VkResult r = real ? real(dev, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (track_on() && r == VK_SUCCESS && ci && out) {
        pthread_mutex_lock(&g_lock);
        if (g_probe_nbuf < KLVK_PROBE_BUFS) {
            struct probe_buf *e = &g_probe_bufs[g_probe_nbuf++];
            e->buf = *out; e->size = ci->size; e->usage = ci->usage;
            e->mem = VK_NULL_HANDLE; e->memoff = 0; e->upload_dst = 0;
        }
        pthread_mutex_unlock(&g_lock);
    }
    return r;
}
static VkResult VKAPI_CALL klvk_BindBufferMemory(VkDevice dev, VkBuffer buf,
        VkDeviceMemory mem, VkDeviceSize off) {
    static PFN_vkBindBufferMemory real;
    if (!real && real_gdpa) real = (PFN_vkBindBufferMemory)real_gdpa(dev, "vkBindBufferMemory");
    VkResult r = real ? real(dev, buf, mem, off) : VK_ERROR_INITIALIZATION_FAILED;
    if (track_on() && r == VK_SUCCESS) {
        pthread_mutex_lock(&g_lock);
        struct probe_buf *e = probe_buf_find(buf);
        if (e) { e->mem = mem; e->memoff = off; }
        pthread_mutex_unlock(&g_lock);
    }
    return r;
}
static VkResult VKAPI_CALL klvk_MapMemory(VkDevice dev, VkDeviceMemory mem,
        VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags flags, void **ppData) {
    static PFN_vkMapMemory real;
    if (!real && real_gdpa) real = (PFN_vkMapMemory)real_gdpa(dev, "vkMapMemory");
    VkResult r = real ? real(dev, mem, off, size, flags, ppData)
                      : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && klvk_profile()) klvk_memc_map(mem);
    // track_on(), not probe_on(): the 2D-mirror freshness copy needs the source
    // pool's mapped pointer, so emu_on() (which track_on() includes) must record
    // it too. Off for every non-olar/wanderer, non-probe title.
    if (track_on() && r == VK_SUCCESS && ppData) {
        pthread_mutex_lock(&g_lock);
        struct probe_mem *e = probe_mem_find(mem);
        if (!e && g_probe_nmem < KLVK_PROBE_MEMS) {
            e = &g_probe_mems[g_probe_nmem++];
            e->mem = mem; e->type_index = 0;
        }
        if (e) { e->mapped = *ppData; e->map_off = off; e->map_size = size; }
        pthread_mutex_unlock(&g_lock);
    }
    return r;
}
static void klvk_probe_uv_staging(VkBuffer src, VkBuffer dst, uint32_t rc,
                                  const VkBufferCopy *regions);  // defined after emu_mirror
static void VKAPI_CALL klvk_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst,
        uint32_t rc, const VkBufferCopy *regions) {
    static PFN_vkCmdCopyBuffer real;
    if (!real && real_gdpa) real = (PFN_vkCmdCopyBuffer)
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdCopyBuffer");
    if (probe_on()) {
        pthread_mutex_lock(&g_lock);
        struct probe_buf *e = probe_buf_find(dst);
        if (e) e->upload_dst = 1;
        pthread_mutex_unlock(&g_lock);
    }
    // PROBE (olar gray hunt): when this copy fills a DEVICE-LOCAL UV texel buffer we
    // mirror, read the staging SRC (host-visible) to see the UV data at upload time.
    // "has data" => UVs are uploaded fine (gray is then ASTC decode or the mirror
    // copy, not missing UVs); "ZERO" => UV texcoords are absent => collapsed UVs.
    if (emu_on() && regions && rc) klvk_probe_uv_staging(src, dst, rc, regions);
    if (real) real(cb, src, dst, rc, regions);
}
static void VKAPI_CALL klvk_CmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst,
        VkDeviceSize off, VkDeviceSize size, const void *data) {
    static PFN_vkCmdUpdateBuffer real;
    if (!real && real_gdpa) real = (PFN_vkCmdUpdateBuffer)
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdUpdateBuffer");
    if (probe_on()) {
        pthread_mutex_lock(&g_lock);
        struct probe_buf *e = probe_buf_find(dst);
        if (e) e->upload_dst = 1;
        pthread_mutex_unlock(&g_lock);
    }
    if (real) real(cb, dst, off, size, data);
}

// ---------------------------------------------------------------------------
// KL_VK_EMU_TEXELBUF — the fork-B fix, "2D-mirror" implementation (default ON
// for olar / wanderer via emu_on(); see the KL_VK_TEXELBUF_PROBE block above
// for the full diagnosis).
//
// PROVEN ROOT CAUSE. olar/wanderer are UE5-mobile Vulkan titles driving Manual
// Vertex Fetch: the vertex factory reads position/tangent and per-material data
// through UNIFORM texel buffers (samplerBuffer). UE packs hundreds of those
// VkBufferViews (256+) as small windows into ONE big shared host-visible
// VkBuffer (olar's pool 0x130a78f00) at many NON-ZERO byte offsets. The probe
// proved the bytes are PRESENT and live (host-visible, mapped, real f32 values
// e.g. a view at offset 132864 holding f32[0.5534 0.1493 0.8195]), and the
// vendored spirv-cross corpus proved the texelFetch->MSL translation is correct.
// What remains — and what this fixes — is MoltenVK's *runtime* VkBufferView ->
// MTLTexture creation for a view at a NON-ZERO offset: MoltenVK reads it WRONG
// (the shader gets zero -> grayscale materials + fragmented/collapsed geometry;
// UI is fine because it uses no texel buffers).
//
// WHY THE OLD ALIAS IS DEAD. The previous fix rebased each view to MTLBuffer
// offset 0 by binding a fresh VkBuffer into the same VkDeviceMemory at the
// window's absolute offset. That is a confirmed no-op even with
// MVK_CONFIG_USE_MTLHEAP=2: MoltenVK will not place mappable memory at MTLBuffer
// offset 0, so the rebased buffer resolves to the same broken absolute offset.
// The alias is removed; this replaces it.
//
// THE 2D-MIRROR. Avoid texel buffers ENTIRELY. For each texel-buffer view we
// create a Klepton-owned OPTIMAL-tiled, DEVICE_LOCAL 2D VkImage of the view's
// format, W=4096 texels wide by ceil(texelCount/4096) tall, usage SAMPLED |
// TRANSFER_DST. At descriptor-BIND time (vkCmdBindDescriptorSets / push /
// push-with-template) we record a GPU vkCmdCopyBufferToImage — bracketed by
// TRANSFER/ SHADER barriers — from the guest's pool VkBuffer into the mirror, on
// the guest's own command buffer, right before the draws that sample it.
//
// WHY GPU-COPY-AT-BIND AND NOT CPU-MEMCPY / LINEAR (what a KL_VK_EMU_PROBE run
// proved). An earlier version used a LINEAR host-visible mirror filled by a CPU
// memcpy at record time. The probe showed BOTH halves of that were broken:
//   * MoltenVK does not sample a LINEAR image reliably even when it holds the
//     exact correct bytes (probe: img==src, real data, still gray) -> OPTIMAL.
//   * a record-time CPU memcpy is STALE: the guest fills its pool per frame AFTER
//     we copy (probe: src-ZERO at copy time for the vertex mirrors) -> the copy
//     must be a GPU command that executes at submit, after the guest's writes.
// The GPU copy at bind fixes both: OPTIMAL is sampleable, and the copy runs in
// queue order after the CPU pool writes, so the mirror is always fresh; the
// barriers order it before the sampling draws. Three companion rewrites keep the
// pipeline consistent, all gated by emu_on():
//   * vkCreateShaderModule    — every OpTypeImage Dim=Buffer,Sampled=1 becomes
//                               Dim=2D, and every OpImageFetch on it gets its
//                               scalar index turned into (idx%4096, idx/4096).
//   * vkCreateDescriptorSetLayout / vkCreateDescriptorPool — UNIFORM_TEXEL_BUFFER
//                               bindings/pool sizes become SAMPLED_IMAGE, so the
//                               layout matches the rewritten shader.
//   * vkUpdateDescriptorSets   — a write to such a binding (pTexelBufferView) is
//                               replaced by a SAMPLED_IMAGE write pointing at the
//                               mirror image's VkImageView.
//
// SCOPE. This handles UNIFORM texel buffers (samplerBuffer, SPIR-V Sampled=1),
// which is exactly what olar/wanderer's Manual Vertex Fetch uses. STORAGE texel
// buffers (imageBuffer, Sampled=2, OpImageRead/Write) are intentionally LEFT ON
// PASSTHROUGH — rewriting them would need a parallel STORAGE_IMAGE path, and the
// two titles this exists for do not use them. Everything below is gated by
// emu_on(); non-olar/wanderer titles never enter any of it and take the exact
// pre-existing passthrough.
// ---------------------------------------------------------------------------

// W: texels per mirror row. Our SPIR-V rewrite computes (idx%KLVK_MIRROR_W,
// idx/KLVK_MIRROR_W); the image is laid out KLVK_MIRROR_W texels per row. The
// two MUST agree — this single constant is the only place the width lives. (4096
// also matches MoltenVK's own spvTexelBufferCoord width, which is where the
// value comes from, but nothing here depends on MoltenVK's constant since the
// Buffer dimensionality is gone by the time MoltenVK sees the module.)
#define KLVK_MIRROR_W 4096u

// Bytes per texel for the uncompressed single-plane formats a texel buffer can
// carry. 0 == unknown: the caller then builds a valid-but-dummy mirror so the
// descriptor stays a well-formed SAMPLED_IMAGE (no layout mismatch) and simply
// skips the freshness copy. olar's set (R32_UINT / R32_SFLOAT /
// R32G32B32A32_SFLOAT / R8G8B8A8_UNORM / R8G8B8A8_SNORM) is all covered here.
static uint32_t klvk_texel_size(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_UINT:  case VK_FORMAT_R8_SINT:
        case VK_FORMAT_R8_SRGB:
            return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_UINT:  case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R16_UNORM:  case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_UINT:   case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16_SFLOAT:
            return 2;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_UINT:  case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16_UINT:  case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32_SFLOAT:
            return 4;
        case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_UINT:  case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32_SFLOAT:
            return 8;
        case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32_SFLOAT:
            return 12;
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return 16;
        default:
            return 0;
    }
}

// One mirror per (srcBuf, srcOff, range, format). The source is addressed by
// VkBuffer handle + view offset (not by absolute VkDeviceMemory) so a view whose
// buffer is bound AFTER the view is created still resolves correctly at copy
// time via probe_buf_find/probe_mem_find.
struct emu_mirror {
    VkBuffer      srcBuf;      // guest pool buffer the view windows into
    VkDeviceSize  srcOff;      // byte offset of the window within srcBuf
    VkDeviceSize  range;       // window size in bytes
    VkFormat      fmt;
    uint32_t      texelSize;   // 0 == dummy mirror (unknown format): skip copies
    uint32_t      texelCount;
    uint32_t      width;       // texels per row (== texelCount when <= W, else W)
    uint32_t      height;
    VkImage       img;         // OPTIMAL-tiled, DEVICE_LOCAL, SAMPLED|TRANSFER_DST
    VkDeviceMemory mem;
    VkImageView   view;        // what the rewritten SAMPLED_IMAGE descriptor uses
    int           everCopied;  // has a GPU copy filled it at least once
};
#define KLVK_EMU_MIRRORS 8192   /* distinct (buf,off,range,fmt) windows; olar's
                                   >4k views collapse to far fewer distinct keys */
static struct emu_mirror g_mirrors[KLVK_EMU_MIRRORS];
static int g_nmirror;

// VkBufferView handle -> mirror index, open-addressed. A bv handle is nonzero,
// so a zero slot is empty; a handle the guest destroys and the driver later
// re-mints is simply overwritten on the next CreateBufferView. Sized well above
// the observed live-view count so probes terminate quickly.
#define KLVK_EMU_BVMAP 32768
static struct { VkBufferView bv; int mirror; } g_bvmap[KLVK_EMU_BVMAP];

static uint32_t klvk_hash64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
    return (uint32_t)x;
}
// Runs under g_lock. Overwrite semantics keep the table bounded under handle reuse.
static void klvk_bvmap_put(VkBufferView bv, int mirror) {
    if (!bv) return;
    uint32_t h = klvk_hash64((uint64_t)(uintptr_t)bv) & (KLVK_EMU_BVMAP - 1);
    for (int probe = 0; probe < KLVK_EMU_BVMAP; probe++) {
        uint32_t k = (h + (uint32_t)probe) & (KLVK_EMU_BVMAP - 1);
        if (g_bvmap[k].bv == bv || g_bvmap[k].bv == VK_NULL_HANDLE) {
            g_bvmap[k].bv = bv; g_bvmap[k].mirror = mirror; return;
        }
    }
    // Table full: the view falls back to no substitution. Logged once by caller.
}
static int klvk_bvmap_find(VkBufferView bv) {
    if (!bv) return -1;
    uint32_t h = klvk_hash64((uint64_t)(uintptr_t)bv) & (KLVK_EMU_BVMAP - 1);
    for (int probe = 0; probe < KLVK_EMU_BVMAP; probe++) {
        uint32_t k = (h + (uint32_t)probe) & (KLVK_EMU_BVMAP - 1);
        if (g_bvmap[k].bv == bv) return g_bvmap[k].mirror;
        if (g_bvmap[k].bv == VK_NULL_HANDLE) return -1;
    }
    return -1;
}

// (descriptorSet, binding, arrayElement) -> mirror index, for bind-time freshness.
// Chained by a hash of the set handle so a bind can walk just its own set's
// entries. Overwrite-by-key keeps it bounded under descriptor-set handle reuse
// (UE resets/recycles pools, so handles repeat rather than growing without end).
#define KLVK_EMU_DSET_ENTS    65536
#define KLVK_EMU_DSET_BUCKETS 16384
struct emu_dset_ent { uint64_t set; uint32_t binding; uint32_t elem; int mirror; int next; };
static struct emu_dset_ent g_dset_ents[KLVK_EMU_DSET_ENTS];
static int g_ndset_ent;
static int g_dset_head[KLVK_EMU_DSET_BUCKETS];
static int g_dset_ready;
static void klvk_dset_init(void) {   // runs under g_lock
    if (g_dset_ready) return;
    for (int i = 0; i < KLVK_EMU_DSET_BUCKETS; i++) g_dset_head[i] = -1;
    g_dset_ready = 1;
}
// Runs under g_lock.
static void klvk_dset_put(uint64_t set, uint32_t binding, uint32_t elem, int mirror) {
    klvk_dset_init();
    uint32_t b = klvk_hash64(set) & (KLVK_EMU_DSET_BUCKETS - 1);
    for (int i = g_dset_head[b]; i >= 0; i = g_dset_ents[i].next) {
        struct emu_dset_ent *e = &g_dset_ents[i];
        if (e->set == set && e->binding == binding && e->elem == elem) {
            e->mirror = mirror; return;                 // rewrite in place
        }
    }
    if (g_ndset_ent >= KLVK_EMU_DSET_ENTS) return;      // full: bind-time refresh
                                                        // skipped; the update-time
                                                        // copy already ran, so the
                                                        // window is at worst stale
                                                        // across frames, never a crash
    struct emu_dset_ent *e = &g_dset_ents[g_ndset_ent];
    e->set = set; e->binding = binding; e->elem = elem; e->mirror = mirror;
    e->next = g_dset_head[b];
    g_dset_head[b] = g_ndset_ent++;
}

// Record a GPU copy of the live source window into the mirror image on `cb`,
// bracketed by the layout/visibility barriers. Runs under g_lock (touches only
// the mirror record + records commands into the caller's own command buffer).
//
// WHY GPU + BIND TIME. The probe proved two things: MoltenVK does not sample a
// LINEAR image reliably even when it holds correct bytes, and a record-time CPU
// memcpy is stale because the guest fills its pool per frame AFTER we copy. Both
// are fixed here: the mirror is OPTIMAL (sampleable), and the copy is a real
// vkCmdCopyBufferToImage recorded onto the guest's command buffer at descriptor
// bind — it executes at submit, AFTER the guest's CPU writes, and the barriers
// order it before the draws that sample it. The source is the guest pool VkBuffer
// itself (klvk_CreateBuffer adds TRANSFER_SRC to texel-buffer pools so it can be
// a copy source); bufferOffset is the VIEW offset relative to that buffer (the
// same ci->offset the view was created with), NOT any device-memory offset.
// See the forward declaration at klvk_CmdCopyBuffer: read the staging SRC bytes that
// a staging->device copy is about to write into a UV texel buffer we mirror, so we
// learn whether UV texcoords are real or zero. Runs under its own g_lock section.
static void klvk_probe_uv_staging(VkBuffer src, VkBuffer dst, uint32_t rc,
                                  const VkBufferCopy *regions) {
    static int shownUV, attempts;
    pthread_mutex_lock(&g_lock);
    if (shownUV < 24 && attempts < 600) {
        attempts++;
        for (int mi = 0; mi < g_nmirror && shownUV < 24; mi++) {
            struct emu_mirror *m = &g_mirrors[mi];
            if (m->srcBuf != dst) continue;
            if (!(m->fmt == 103 || m->fmt == 83 || m->fmt == 77)) continue;  // UV formats
            struct probe_buf *sb = probe_buf_find(src);
            struct probe_mem *pm = (sb && sb->mem) ? probe_mem_find(sb->mem) : NULL;
            if (!pm || !pm->mapped) continue;
            for (uint32_t ri = 0; ri < rc; ri++) {
                if (regions[ri].dstOffset > m->srcOff ||
                    regions[ri].dstOffset + regions[ri].size < m->srcOff + m->range) continue;
                VkDeviceSize srcByte = regions[ri].srcOffset + (m->srcOff - regions[ri].dstOffset);
                VkDeviceSize abs = sb->memoff + srcByte;
                VkDeviceSize mend = (pm->map_size == VK_WHOLE_SIZE) ? (VkDeviceSize)~0ull
                                                                   : pm->map_off + pm->map_size;
                if (abs < pm->map_off || abs >= mend) continue;
                const uint8_t *s = (const uint8_t *)pm->mapped + (size_t)(abs - pm->map_off);
                uint32_t su[4] = {0,0,0,0};
                size_t nb = m->range < 16 ? (size_t)m->range : 16;
                memcpy(su, s, nb);
                shownUV++;
                VKI("[emu] UV staging src: mirror #%d fmt %d range %llu: "
                    "[%08x %08x %08x %08x] -> %s\n", mi, (int)m->fmt,
                    (unsigned long long)m->range, su[0], su[1], su[2], su[3],
                    (su[0] | su[1] | su[2] | su[3]) ? "has data" : "ZERO");
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
}
static void klvk_mirror_gpu_copy(VkCommandBuffer cb, struct emu_mirror *m) {
    static PFN_vkCmdPipelineBarrier   rBarrier;
    static PFN_vkCmdCopyBufferToImage rCopy;
    static int resolved;
    if (!resolved) { resolved = 1;
        if (real_gdpa) {
            VkDevice dv = g_devs[0] ? g_devs[0]->dev : NULL;
            rBarrier = (PFN_vkCmdPipelineBarrier)  real_gdpa(dv, "vkCmdPipelineBarrier");
            rCopy    = (PFN_vkCmdCopyBufferToImage)real_gdpa(dv, "vkCmdCopyBufferToImage");
        }
    }
    if (!rBarrier || !rCopy || !cb || !m || m->texelSize == 0 || !m->img || !m->srcBuf)
        return;

    // (1) -> TRANSFER_DST. oldLayout UNDEFINED discards the previous frame's bytes
    // (the copy overwrites the whole sampled region), while srcStage waits for the
    // prior frame's shader reads (WAR) before the transfer overwrites.
    VkImageMemoryBarrier b; memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = m->img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    rBarrier(cb, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    // (2) copy source pool bytes -> image, row-major KLVK_MIRROR_W texels/row.
    // Split into whole rows + a partial last row so we never read past the window.
    VkBufferImageCopy rgn[2]; memset(rgn, 0, sizeof rgn);
    uint32_t nr = 0, W = KLVK_MIRROR_W, tc = m->texelCount, ts = m->texelSize;
    if (m->height <= 1) {
        rgn[0].bufferOffset = m->srcOff;
        rgn[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rgn[0].imageSubresource.layerCount = 1;
        rgn[0].imageExtent.width = tc; rgn[0].imageExtent.height = 1; rgn[0].imageExtent.depth = 1;
        nr = 1;
    } else {
        uint32_t fullRows = tc / W, rem = tc % W;
        rgn[0].bufferOffset = m->srcOff;
        rgn[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rgn[0].imageSubresource.layerCount = 1;
        rgn[0].imageExtent.width = W; rgn[0].imageExtent.height = fullRows; rgn[0].imageExtent.depth = 1;
        nr = 1;
        if (rem) {
            rgn[1].bufferOffset = m->srcOff + (VkDeviceSize)fullRows * W * ts;
            rgn[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rgn[1].imageSubresource.layerCount = 1;
            rgn[1].imageOffset.y = (int32_t)fullRows;
            rgn[1].imageExtent.width = rem; rgn[1].imageExtent.height = 1; rgn[1].imageExtent.depth = 1;
            nr = 2;
        }
    }
    rCopy(cb, m->srcBuf, m->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nr, rgn);

    // DECISIVE TEST (KL_VK_UV_CONST=1): overwrite UV-format mirrors with a constant
    // (0.5,0.5) right after the copy (still TRANSFER_DST). Every vertex then samples
    // the CENTER of its albedo texture. If the gray world VISIBLY CHANGES (flat
    // per-surface colors appear), the mirror IS the UV source and the ASTC albedo
    // does hold colour -> collapsed UVs are the bug. If it STAYS gray, ASTC is
    // decoding to gray. If UNCHANGED, UVs come from elsewhere. Diagnostic; off by default.
    if (kl_env_on("KL_VK_UV_CONST", 0) &&
        (m->fmt == 103 || m->fmt == 83 || m->fmt == 77)) {
        static PFN_vkCmdClearColorImage clr;
        if (!clr) clr = (PFN_vkCmdClearColorImage)mvk_sym("vkCmdClearColorImage");
        if (clr) {
            VkClearColorValue uv; uv.float32[0] = 0.5f; uv.float32[1] = 0.5f;
            uv.float32[2] = 0.0f; uv.float32[3] = 0.0f;
            VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            clr(cb, m->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &uv, 1, &rng);
        }
    }

    // (3) TRANSFER_DST -> SHADER_READ_ONLY, transfer write visible to shader reads.
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    rBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             0, 0, NULL, 0, NULL, 1, &b);
    m->everCopied = 1;

    // KL_VK_EMU_PROBE (default OFF) — record-time SOURCE readback. The mirror is
    // now DEVICE_LOCAL/OPTIMAL and not mappable, so we can only inspect the source
    // pool (host-visible+mapped, tracked). Confirms the pool window holds real data
    // at the moment we RECORD the copy (the copy itself runs later at submit, reading
    // the then-current bytes). src-ZERO here on a meaningful mirror hints the guest
    // fills this window even later than record time.
    if (emu_probe() || emu_on()) {
        // Broadened (olar gray hunt): ALSO dump the 2-component UV texel buffers
        // (103=R32G32_SFLOAT, 83=R16G16_SFLOAT) and other small attribute buffers —
        // the old filter only showed vec4 transforms, so UV data was never inspected.
        // If UV buffers read ZERO here, texcoords collapse -> every surface samples
        // one texel -> flat gray (matches the symptom); if they hold sane [0,1]-ish
        // values, UVs are fine and the gray is an ASTC-decode issue instead.
        int isUV = (m->fmt == 103 /*R32G32_SFLOAT*/) || (m->fmt == 83 /*R16G16_SFLOAT*/) ||
                   (m->fmt == 77 /*R16G16_UNORM*/)   || (m->fmt == 104 /*R32G32B32*/);
        int interesting = (m->fmt == VK_FORMAT_R32G32B32A32_SFLOAT) ||
                          (m->width == KLVK_MIRROR_W) || isUV;
        struct probe_buf *sb = interesting ? probe_buf_find(m->srcBuf) : NULL;
        struct probe_mem *pm = (sb && sb->mem) ? probe_mem_find(sb->mem) : NULL;
        if (pm && pm->mapped) {
            VkDeviceSize abs = sb->memoff + m->srcOff;
            VkDeviceSize mend = (pm->map_size == VK_WHOLE_SIZE)
                              ? (VkDeviceSize)~0ull : pm->map_off + pm->map_size;
            if (abs >= pm->map_off && abs < mend) {
                const uint8_t *s = (const uint8_t *)pm->mapped + (size_t)(abs - pm->map_off);
                uint32_t su[4] = {0,0,0,0};
                size_t nb = (size_t)m->range < 16 ? (size_t)m->range : 16;
                memcpy(su, s, nb);
                float sf[4]; for (int k = 0; k < 4; k++) memcpy(&sf[k], &su[k], 4);
                // Show UV-format buffers on their own budget so the vec4 transforms
                // (which we already know have data) don't crowd them out.
                static int shown, shownUV;
                int budget = isUV ? (shownUV < 24) : (shown < 8);
                if (budget) { if (isUV) shownUV++; else shown++;
                    VKI("[emu] probe src mirror #%d fmt %d %ux%u%s: pool[%08x %08x %08x %08x] "
                        "f[%.4g %.4g %.4g %.4g] at record time -> %s\n",
                        (int)(m - g_mirrors), (int)m->fmt, m->width, m->height,
                        isUV ? " (UV?)" : "",
                        su[0], su[1], su[2], su[3], sf[0], sf[1], sf[2], sf[3],
                        (su[0] | su[1] | su[2] | su[3]) ? "has data" : "ZERO"); }
            }
        }
    }
}

// Record the GPU copy for every mirror bound through descriptor set `set`, into
// command buffer `cb`, deduplicated against `seen` so one bind copies each mirror
// at most once (but a later re-bind copies again — that is the freshness). Runs
// under g_lock.
static void klvk_dset_gpu_copy(VkCommandBuffer cb, uint64_t set, int *seen, int *nseen) {
    if (!g_dset_ready) return;
    uint32_t bkt = klvk_hash64(set) & (KLVK_EMU_DSET_BUCKETS - 1);
    for (int i = g_dset_head[bkt]; i >= 0; i = g_dset_ents[i].next) {
        if (g_dset_ents[i].set != set) continue;
        int mi = g_dset_ents[i].mirror;
        if (mi < 0 || mi >= g_nmirror) continue;
        int dup = 0;
        for (int s = 0; s < *nseen; s++) if (seen[s] == mi) { dup = 1; break; }
        if (dup) continue;
        if (*nseen < KLVK_BIND_SEEN) seen[(*nseen)++] = mi;
        klvk_mirror_gpu_copy(cb, &g_mirrors[mi]);
    }
}


// Create-or-find the mirror for a texel-buffer view. Returns the mirror index,
// or -1 only if even the dummy fallback could not be built (then the caller
// keeps the plain passthrough view — the sole case where a layout/descriptor
// mismatch is possible, and it is logged). Runs under g_lock.
static int klvk_mirror_get(VkDevice dev, VkBuffer srcBuf, VkDeviceSize srcOff,
                           VkDeviceSize ciRange, VkFormat fmt) {
    static PFN_vkCreateImage                 rCreateImg;
    static PFN_vkGetImageMemoryRequirements  rImgReq;
    static PFN_vkAllocateMemory              rAlloc;
    static PFN_vkBindImageMemory            rBind;
    static PFN_vkCreateImageView             rCreateView;
    static PFN_vkDestroyImage                rDestroyImg;
    static PFN_vkFreeMemory                  rFree;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        if (real_gdpa) {
            rCreateImg  = (PFN_vkCreateImage)                real_gdpa(dev, "vkCreateImage");
            rImgReq     = (PFN_vkGetImageMemoryRequirements) real_gdpa(dev, "vkGetImageMemoryRequirements");
            rAlloc      = (PFN_vkAllocateMemory)             real_gdpa(dev, "vkAllocateMemory");
            rBind       = (PFN_vkBindImageMemory)            real_gdpa(dev, "vkBindImageMemory");
            rCreateView = (PFN_vkCreateImageView)            real_gdpa(dev, "vkCreateImageView");
            rDestroyImg = (PFN_vkDestroyImage)               real_gdpa(dev, "vkDestroyImage");
            rFree       = (PFN_vkFreeMemory)                 real_gdpa(dev, "vkFreeMemory");
        }
    }
    if (!rCreateImg || !rImgReq || !rAlloc || !rBind || !rCreateView)
        return -1;

    // Resolve VK_WHOLE_SIZE against the tracked source buffer size.
    VkDeviceSize range = ciRange;
    if (range == VK_WHOLE_SIZE) {
        struct probe_buf *sb = probe_buf_find(srcBuf);
        range = (sb && sb->size > srcOff) ? (sb->size - srcOff) : 0;
    }
    if (range == 0) return -1;

    // Cache hit?
    for (int i = 0; i < g_nmirror; i++) {
        struct emu_mirror *e = &g_mirrors[i];
        if (e->srcBuf == srcBuf && e->srcOff == srcOff &&
            e->range == range && e->fmt == fmt) return i;
    }
    if (g_nmirror >= KLVK_EMU_MIRRORS) return -1;

    uint32_t tsz = klvk_texel_size(fmt);
    // Whole texels only (floor): the GPU copy must not read past the window, and a
    // trailing partial texel is never fetched by the shader anyway.
    uint32_t tcount = tsz ? (uint32_t)(range / tsz) : 1;
    if (tcount == 0) tcount = 1;
    uint32_t width, height;
    if (tcount <= KLVK_MIRROR_W) { width = tcount; height = 1; }
    else { width = KLVK_MIRROR_W; height = (tcount + KLVK_MIRROR_W - 1) / KLVK_MIRROR_W; }

    klvk_device *d = dev_find(dev);
    if (!d) return -1;

    // Build the OPTIMAL-tiled DEVICE_LOCAL sampled 2D image in the view's own
    // format, also usable as a transfer destination (the per-bind GPU copy fills
    // it). OPTIMAL is what MoltenVK samples reliably (linear proved unreliable
    // even holding correct bytes). If the exact format cannot be an optimal
    // sampled+transfer_dst image on this driver, fall back to a 1x1 RGBA8 image so
    // the descriptor stays a valid SAMPLED_IMAGE (no layout mismatch); its GPU
    // copy is then disabled (texelSize left 0).
    VkFormat imgFmt = fmt;
    uint32_t useTsz = tsz, useW = width, useH = height;
    VkImage img = VK_NULL_HANDLE;
    for (int attempt = 0; attempt < 2; attempt++) {
        VkImageCreateInfo ici;
        memset(&ici, 0, sizeof ici);
        ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = imgFmt;
        ici.extent.width  = useW; ici.extent.height = useH; ici.extent.depth = 1;
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (rCreateImg(dev, &ici, NULL, &img) == VK_SUCCESS && img) break;
        img = VK_NULL_HANDLE;
        imgFmt = VK_FORMAT_R8G8B8A8_UNORM; useTsz = 0; useW = 1; useH = 1;  // dummy
    }
    if (!img) return -1;

    VkMemoryRequirements mr; memset(&mr, 0, sizeof mr);
    rImgReq(dev, img, &mr);
    uint32_t mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) mt = mem_type(d, mr.memoryTypeBits, 0);
    if (mt == UINT32_MAX) { if (rDestroyImg) rDestroyImg(dev, img, NULL); return -1; }

    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mt;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (rAlloc(dev, &ai, NULL, &mem) != VK_SUCCESS || !mem) {
        if (rDestroyImg) rDestroyImg(dev, img, NULL); return -1;
    }
    if (rBind(dev, img, mem, 0) != VK_SUCCESS) {
        if (rFree) rFree(dev, mem, NULL);
        if (rDestroyImg) rDestroyImg(dev, img, NULL); return -1;
    }

    VkImageViewCreateInfo vi;
    memset(&vi, 0, sizeof vi);
    vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image    = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = imgFmt;
    vi.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    vi.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    vi.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    vi.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel   = 0; vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.baseArrayLayer = 0; vi.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (rCreateView(dev, &vi, NULL, &view) != VK_SUCCESS || !view) {
        if (rFree) rFree(dev, mem, NULL);
        if (rDestroyImg) rDestroyImg(dev, img, NULL); return -1;
    }

    struct emu_mirror *m = &g_mirrors[g_nmirror];
    m->srcBuf = srcBuf; m->srcOff = srcOff; m->range = range; m->fmt = fmt;
    m->texelSize = useTsz; m->texelCount = tcount;
    m->width = useW; m->height = useH;
    m->img = img; m->mem = mem;
    m->view = view;
    m->everCopied = 0;
    {
        static int said; if (said < 8) { said++;
            VKI("[emu] mirror #%d fmt %d %ux%u texelSize %u tcount %u: OPTIMAL "
                "device-local, filled by GPU copy at bind (shader idx width = %u)\n",
                g_nmirror, (int)fmt, useW, useH, useTsz, tcount, (unsigned)KLVK_MIRROR_W); }
    }
    return g_nmirror++;
}

// ---------------------------------------------------------------------------
// SPIR-V rewrite: OpTypeImage Dim=Buffer,Sampled=1 -> Dim=2D, and each
// OpImageFetch on such an image gets its scalar index turned into the 2-vector
// (idx % KLVK_MIRROR_W, idx / KLVK_MIRROR_W). Done directly on the word stream —
// no spirv-cross at runtime. Returns a malloc'd module (caller frees) or NULL to
// mean "leave the original untouched" (no buffer images present, or malformed).
// ---------------------------------------------------------------------------
// SPIR-V opcodes/enums we touch (values verified against the vendored
// spirv-headers unified1/spirv.h; hardcoded so this file needs no new include).
#define KLSPV_MAGIC              0x07230203u
#define KLSPV_OP_TYPE_INT        21
#define KLSPV_OP_TYPE_VECTOR     23
#define KLSPV_OP_TYPE_IMAGE      25
#define KLSPV_OP_CONSTANT        43
#define KLSPV_OP_FUNCTION        54
#define KLSPV_OP_LOAD            61
#define KLSPV_OP_COMPOSITE_CONSTRUCT 80
#define KLSPV_OP_IMAGE_FETCH     95
#define KLSPV_OP_IMAGE           100
#define KLSPV_OP_UDIV            134
#define KLSPV_OP_UMOD            137
#define KLSPV_OP_COPY_OBJECT     83
#define KLSPV_DIM_2D             1u
#define KLSPV_DIM_BUFFER         5u
#define KLSPV_IMGOP_LOD          0x2u   /* Image Operands bit: Lod (integer mip) */

static uint32_t *klvk_spirv_mirror_rewrite(const uint32_t *in, size_t nwords,
                                           size_t *out_nwords) {
    if (!in || nwords < 5 || in[0] != KLSPV_MAGIC) return NULL;
    uint32_t origBound = in[3];
    if (origBound == 0) return NULL;

    uint8_t *isBufImgType = (uint8_t *)calloc(origBound, 1);   // OpTypeImage Buffer,Sampled=1
    uint8_t *isBufImgVal  = (uint8_t *)calloc(origBound, 1);   // values loaded from one
    if (!isBufImgType || !isBufImgVal) { free(isBufImgType); free(isBufImgVal); return NULL; }

    // --- Pass 1: global declarations (everything before the first OpFunction).
    // Find buffer-image types, a reusable uint32 type / const 4096 / v2uint, and
    // the offset of the first function (our global-insertion point).
    uint32_t uintType = 0, c4096 = 0, c0 = 0, v2uintType = 0, firstFuncOff = 0;
    // Track which type ids are 32-bit integer types, to validate the const's type.
    uint8_t *isInt32Type = (uint8_t *)calloc(origBound, 1);
    if (!isInt32Type) { free(isBufImgType); free(isBufImgVal); return NULL; }
    {
        size_t off = 5;
        while (off < nwords) {
            uint32_t w = in[off];
            uint32_t op = w & 0xFFFFu, wc = w >> 16;
            if (wc == 0 || off + wc > nwords) break;   // malformed: bail to passthrough
            if (op == KLSPV_OP_FUNCTION) { firstFuncOff = (uint32_t)off; break; }
            if (op == KLSPV_OP_TYPE_IMAGE && wc >= 9) {
                uint32_t id = in[off + 1];
                uint32_t dim = in[off + 3], sampled = in[off + 7];
                if (dim == KLSPV_DIM_BUFFER && sampled == 1 && id < origBound)
                    isBufImgType[id] = 1;
                // PROBE (olar gray hunt): a Dim=Buffer image with Sampled=2 is a
                // STORAGE texel buffer (imageBuffer / OpImageRead) — the emu does NOT
                // rewrite these, so their reads hit the raw MoltenVK-misread buffer.
                // If olar's world shaders declare these, that is the gray/invisible
                // root (mirror created, never bound-through-a-rewrite). Count + log so
                // one run tells us uniform-vs-storage definitively.
                if (dim == KLSPV_DIM_BUFFER && sampled == 2) {
                    static int said;
                    if (said < 12) { said++;
                        VKI("[emu] PROBE: shader declares a STORAGE texel buffer "
                            "(OpTypeImage Dim=Buffer Sampled=2, fmt word=%u) — NOT "
                            "rewritten by the emu; this is a gray/invisible suspect\n",
                            wc >= 9 ? in[off + 8] : 0); }
                }
            } else if (op == KLSPV_OP_TYPE_INT && wc >= 4) {
                uint32_t id = in[off + 1], wdt = in[off + 2], sgn = in[off + 3];
                if (wdt == 32 && id < origBound) {
                    isInt32Type[id] = 1;
                    if (sgn == 0 && uintType == 0) uintType = id;   // prefer unsigned
                }
            } else if (op == KLSPV_OP_CONSTANT && wc >= 4) {
                uint32_t rt = in[off + 1], id = in[off + 2], val = in[off + 3];
                if (val == 4096 && rt < origBound && isInt32Type[rt] && c4096 == 0) {
                    c4096 = id;
                    if (uintType == 0) uintType = rt;   // usable as our coord scalar type
                }
                if (val == 0 && rt < origBound && isInt32Type[rt] && c0 == 0)
                    c0 = id;                            // any 32-bit int 0 works as the
                                                        // integer Lod (sign irrelevant)
            } else if (op == KLSPV_OP_TYPE_VECTOR && wc >= 4) {
                uint32_t id = in[off + 1], comp = in[off + 2], cnt = in[off + 3];
                if (cnt == 2 && uintType != 0 && comp == uintType && v2uintType == 0)
                    v2uintType = id;
            }
            off += wc;
        }
    }

    // No texel-buffer image in this module -> nothing to do (UI/other shaders).
    int anyBufImg = 0;
    for (uint32_t i = 0; i < origBound; i++) if (isBufImgType[i]) { anyBufImg = 1; break; }
    if (!anyBufImg) { free(isBufImgType); free(isBufImgVal); free(isInt32Type); return NULL; }

    // --- Pass 2: over the function bodies, mark values of buffer-image type and
    // count the fetches we will rewrite (so we can size the new id space).
    uint32_t fetchCount = 0;
    if (firstFuncOff) {
        size_t off = firstFuncOff;
        while (off < nwords) {
            uint32_t w = in[off];
            uint32_t op = w & 0xFFFFu, wc = w >> 16;
            if (wc == 0 || off + wc > nwords) break;
            if ((op == KLSPV_OP_LOAD || op == KLSPV_OP_IMAGE || op == KLSPV_OP_COPY_OBJECT)
                && wc >= 3) {
                uint32_t rt = in[off + 1], rid = in[off + 2];
                if (rt < origBound && isBufImgType[rt] && rid < origBound)
                    isBufImgVal[rid] = 1;
                else if (op == KLSPV_OP_COPY_OBJECT && wc >= 4) {
                    uint32_t srcv = in[off + 3];
                    if (srcv < origBound && isBufImgVal[srcv] && rid < origBound)
                        isBufImgVal[rid] = 1;   // propagate through a copy of the value
                }
            } else if (op == KLSPV_OP_IMAGE_FETCH && wc >= 5) {
                uint32_t imgv = in[off + 3];
                if (imgv < origBound && isBufImgVal[imgv]) fetchCount++;
            }
            off += wc;
        }
    }

    // Plan the new ids. uintType/c4096/v2uintType may be created if absent.
    uint32_t bound = origBound;
    uint32_t newUint = 0, newC4096 = 0, newC0 = 0, newV2 = 0;
    if (uintType == 0)  { newUint  = bound++; uintType   = newUint; }
    if (c4096 == 0)     { newC4096 = bound++; c4096      = newC4096; }
    if (c0 == 0)        { newC0    = bound++; c0         = newC0; }
    if (v2uintType == 0){ newV2    = bound++; v2uintType = newV2; }
    // 3 ids per fetch (x = umod, y = udiv, coord vector = composite).
    // (allocated inline while emitting so they land in order.)

    // Size the output generously: original + injected globals + per-fetch work.
    // Per fetch: OpUMod(5) + OpUDiv(5) + OpCompositeConstruct(5) + up to 2 extra
    // words appended to the fetch for the Lod image operand (mask + value).
    size_t injectedGlobals = (newUint ? 4 : 0) + (newC4096 ? 4 : 0) +
                             (newC0 ? 4 : 0) + (newV2 ? 4 : 0);
    size_t perFetch = (size_t)fetchCount * (5 + 5 + 5 + 2);
    size_t cap = nwords + injectedGlobals + perFetch + 16;
    uint32_t *out = (uint32_t *)malloc(cap * sizeof(uint32_t));
    if (!out) { free(isBufImgType); free(isBufImgVal); free(isInt32Type); return NULL; }

    size_t o = 0;
    // Header (bound patched at the end).
    for (int i = 0; i < 5; i++) out[o++] = in[i];

    int globalsInjected = 0;
    size_t off = 5;
    while (off < nwords) {
        uint32_t w = in[off];
        uint32_t op = w & 0xFFFFu, wc = w >> 16;
        if (wc == 0 || off + wc > nwords) {
            // Malformed tail: copy the remainder verbatim and stop interpreting.
            while (off < nwords) out[o++] = in[off++];
            break;
        }
        // Inject new global types/constants immediately before the first function.
        if (!globalsInjected && firstFuncOff && off == firstFuncOff) {
            if (newUint) {   // OpTypeInt 32 0
                out[o++] = (4u << 16) | KLSPV_OP_TYPE_INT;
                out[o++] = newUint; out[o++] = 32; out[o++] = 0;
            }
            if (newC4096) {  // OpConstant uintType 4096
                out[o++] = (4u << 16) | KLSPV_OP_CONSTANT;
                out[o++] = uintType; out[o++] = newC4096; out[o++] = KLVK_MIRROR_W;
            }
            if (newC0) {     // OpConstant uintType 0  (the Lod)
                out[o++] = (4u << 16) | KLSPV_OP_CONSTANT;
                out[o++] = uintType; out[o++] = newC0; out[o++] = 0;
            }
            if (newV2) {     // OpTypeVector uintType 2
                out[o++] = (4u << 16) | KLSPV_OP_TYPE_VECTOR;
                out[o++] = newV2; out[o++] = uintType; out[o++] = 2;
            }
            globalsInjected = 1;
        }

        if (op == KLSPV_OP_TYPE_IMAGE && wc >= 9 && in[off + 1] < origBound &&
            isBufImgType[in[off + 1]]) {
            // Copy the type but flip Dim Buffer(5) -> 2D(1).
            for (uint32_t k = 0; k < wc; k++) out[o++] = in[off + k];
            out[o - wc + 3] = KLSPV_DIM_2D;
        } else if (op == KLSPV_OP_IMAGE_FETCH && wc >= 5 &&
                   in[off + 3] < origBound && isBufImgVal[in[off + 3]]) {
            uint32_t coord = in[off + 4];
            uint32_t xid = bound++, yid = bound++, vid = bound++;
            // OpUMod uintType xid coord c4096
            out[o++] = (5u << 16) | KLSPV_OP_UMOD;
            out[o++] = uintType; out[o++] = xid; out[o++] = coord; out[o++] = c4096;
            // OpUDiv uintType yid coord c4096
            out[o++] = (5u << 16) | KLSPV_OP_UDIV;
            out[o++] = uintType; out[o++] = yid; out[o++] = coord; out[o++] = c4096;
            // OpCompositeConstruct v2uintType vid xid yid
            out[o++] = (5u << 16) | KLSPV_OP_COMPOSITE_CONSTRUCT;
            out[o++] = v2uintType; out[o++] = vid; out[o++] = xid; out[o++] = yid;
            // The fetch itself. The original was OpImageFetch on Dim=Buffer, which
            // carries NO Lod operand; on the now-Dim=2D SAMPLED image a fetch is
            // only valid WITH an explicit integer Lod, and drivers/spirv-cross that
            // tolerate its absence often just return 0 (-> gray/collapsed). So:
            //   * replace the scalar coordinate (word 4) with our 2-vector, and
            //   * ensure the Lod image operand (mask 0x2, value = int 0) is present.
            // Word layout: [0]=op/wc [1]=resultType [2]=result [3]=image
            //              [4]=coordinate [5]=ImageOperands mask [6..]=operand ids.
            size_t fbase = o;
            uint32_t hasOperands = (wc >= 6);
            uint32_t oldMask = hasOperands ? in[off + 5] : 0;
            if (oldMask & KLSPV_IMGOP_LOD) {
                // Already has a Lod (unexpected for a buffer fetch): copy verbatim,
                // only swapping the coordinate.
                for (uint32_t k = 0; k < wc; k++) out[o++] = in[off + k];
                out[fbase + 4] = vid;
            } else {
                // Rebuild: header (patched wc), resultType, result, image, coord,
                // (mask|Lod), Lod value (int 0, inserted as the first operand since
                // no lower Image-Operands bit is valid on a fetch), then any
                // pre-existing operand values.
                uint32_t newWc = wc + (hasOperands ? 1u : 2u);
                out[o++] = (newWc << 16) | KLSPV_OP_IMAGE_FETCH;
                out[o++] = in[off + 1];        // result type
                out[o++] = in[off + 2];        // result id
                out[o++] = in[off + 3];        // image
                out[o++] = vid;                // coordinate (our 2-vector)
                out[o++] = oldMask | KLSPV_IMGOP_LOD;   // image operands mask
                out[o++] = c0;                 // Lod = 0
                for (uint32_t k = 6; k < wc; k++) out[o++] = in[off + k];  // existing operands
            }
        } else {
            for (uint32_t k = 0; k < wc; k++) out[o++] = in[off + k];
        }
        off += wc;
    }
    // If the module had no function (so the injection point never hit) append the
    // new globals at the very end — still valid, they precede no use.
    if (!globalsInjected) {
        if (newUint) { out[o++] = (4u << 16) | KLSPV_OP_TYPE_INT;
                       out[o++] = newUint; out[o++] = 32; out[o++] = 0; }
        if (newC4096){ out[o++] = (4u << 16) | KLSPV_OP_CONSTANT;
                       out[o++] = uintType; out[o++] = newC4096; out[o++] = KLVK_MIRROR_W; }
        if (newC0)   { out[o++] = (4u << 16) | KLSPV_OP_CONSTANT;
                       out[o++] = uintType; out[o++] = newC0; out[o++] = 0; }
        if (newV2)   { out[o++] = (4u << 16) | KLSPV_OP_TYPE_VECTOR;
                       out[o++] = newV2; out[o++] = uintType; out[o++] = 2; }
    }

    out[3] = bound;   // patch id bound
    *out_nwords = o;
    free(isBufImgType); free(isBufImgVal); free(isInt32Type);
    return out;
}

// ---------------------------------------------------------------------------
// Multisampled subpass reads that name no sample (KL_VK_SUBPASS_MS_SAMPLE).
//
// Pinball FX VR (x7255396864545733, UE) declares its mobile depth-fetch input
// attachment (GENERATED_SubpassDepthFetchAttachment) as a MULTISAMPLED
// subpass input (OpTypeImage Dim=SubpassData MS=1) but reads it with a bare
// subpassLoad: an OpImageRead with no Sample image operand. The SPIR-V spec
// requires one for MS=1; Quest's driver tolerates its absence. SPIRV-Cross
// translates it as written, texture2d_ms_array::read(coord, layer) with no
// sample argument, and Metal's compiler rejects that ("no matching member
// function for call to 'read'"). The pipeline fails and UE treats a failed
// PSO as fatal: it calls ForceQuit (which we ignore) about twenty times a run,
// and a guest worker later faulted on a garbage branch. With this rewrite all
// of them compile and both are gone. (The title still shows a black screen
// after its logos; that is a separate, unresolved cause.)
//
// The fix supplies the operand the shader omitted: Sample = 0. The type is
// kept as declared, so the shader still matches the multisampled attachment
// it is bound to; sample 0 is what a tolerant driver reads for a missing
// index, and for a depth fetch it is the value the effect was tuned against.
// Reads that already name a sample are left alone; other image operands a
// read carries are kept, with Sample put in its place among them. A module
// with no such read is returned unchanged (NULL).
//
// Sent in for Pinball FX VR (fix 17, 2026-10-11) and on for every Unreal
// guest: the shader is not that game's doing but the engine's — its Vulkan
// backend turns a subpass input into a multisampled one when the pass is, and
// names the sample only if the module already holds a constant it can use —
// and what is rewritten is a read no Metal compiler accepts, so there is no
// working game for this to change. KL_VK_SUBPASS_MS_SAMPLE=0|1 overrides.
static int klvk_subpass_ms_on(void) {
    static int on = -1;
    extern kl_guest_kind kl_driver_kind(void);
    if (on < 0) on = kl_env_on("KL_VK_SUBPASS_MS_SAMPLE", kl_driver_kind() == KL_GUEST_UE4);
    return on;
}
// Words an image operand takes after the mask, by bit, in the order the spec
// lays them out (Bias, Lod, Grad = 2, ConstOffset, Offset, ConstOffsets,
// Sample, MinLod, MakeTexelAvailable, MakeTexelVisible, then flag-only bits,
// then Offsets). Used to find where a Sample operand belongs.
static unsigned klspv_imgop_words(uint32_t bit) {
    switch (bit) {
    case 0x1: case 0x2: case 0x8: case 0x10: case 0x20: case 0x40: case 0x80:
    case 0x100: case 0x200: case 0x10000: return 1;
    case 0x4: return 2;
    default: return 0;
    }
}
static uint32_t *klvk_spirv_subpass_ms_sample(const uint32_t *code, size_t nw, size_t *out_nw) {
    if (!code || nw < 6 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3];
    if (bound < 2 || bound > (1u << 22)) return NULL;
    uint8_t *spimg = calloc(bound, 1);        // 1 = SubpassData image type, 2 = and MS=1
    uint8_t *msval = calloc(bound, 1);        // id is a value of an MS=1 such type
    if (!spimg || !msval) { free(spimg); free(msval); return NULL; }
    uint32_t intType = 0, zero = 0, nfix = 0, nhas = 0, nsub = 0, nms = 0, nread = 0;
    size_t firstFn = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t len = code[i] >> 16, op = code[i] & 0xffffu;
        if (!len || i + len > nw) { free(spimg); free(msval); return NULL; }
        if (op == 25 && len >= 9 && code[i + 1] < bound && code[i + 3] == 6) {  // OpTypeImage SubpassData
            nsub++;
            spimg[code[i + 1]] = code[i + 6] == 1 ? 2 : 1;
            if (code[i + 6] == 1) nms++;
        } else if (op == 21 && len == 4 && code[i + 2] == 32 && !intType)       // OpTypeInt 32
            intType = code[i + 1];
        else if (op == 43 && len == 4 && intType && code[i + 1] == intType &&
                 code[i + 3] == 0 && !zero)                                  // OpConstant int 0
            zero = code[i + 2];
        else if (op == 54 && !firstFn)                                       // OpFunction
            firstFn = i;
        else if ((op == 61 || op == 83) && len >= 4 && code[i + 1] < bound &&
                 code[i + 2] < bound && spimg[code[i + 1]] == 2)            // OpLoad / OpCopyObject
            msval[code[i + 2]] = 1;
        else if (op == 98 && len >= 5 && code[i + 3] < bound && msval[code[i + 3]]) {  // OpImageRead
            nread++;
            if (len > 5 && (code[i + 5] & 0x40u)) nhas++; else nfix++;
        }
        i += len;
    }
    if (!nfix || !firstFn) { free(spimg); free(msval); return NULL; }
    size_t extra = 2u * nfix + (intType ? 0 : 4) + (zero ? 0 : 4);
    uint32_t *out = malloc((nw + extra) * sizeof *out);
    if (!out) { free(spimg); free(msval); return NULL; }
    const uint32_t nbound = bound;            // msval is sized to this
    size_t o = 0;
    for (size_t i = 0; i < 5; i++) out[o++] = code[i];
    for (size_t i = 5; i < nw; ) {
        uint32_t len = code[i] >> 16, op = code[i] & 0xffffu;
        if (i == firstFn) {                       // end of the global section
            if (!intType) { intType = bound++;
                out[o++] = (4u << 16) | 21u; out[o++] = intType; out[o++] = 32; out[o++] = 1; }
            if (!zero) { zero = bound++;
                out[o++] = (4u << 16) | 43u; out[o++] = intType; out[o++] = zero; out[o++] = 0; }
        }
        if (op == 98 && len >= 5 && code[i + 3] < nbound && msval[code[i + 3]] &&
            !(len > 5 && (code[i + 5] & 0x40u))) {
            uint32_t mask = len > 5 ? code[i + 5] : 0;
            out[o++] = ((len == 5 ? 7u : len + 1u) << 16) | 98u;
            for (uint32_t k = 1; k < 5; k++) out[o++] = code[i + k];
            out[o++] = mask | 0x40u;
            // The operands for bits below Sample come first, then Sample, then the rest.
            // An instruction whose mask promises more operand words than it has is
            // not one to rewrite: the module goes on as the guest wrote it.
            size_t src = i + 6;
            int short_of_words = 0;
            for (uint32_t bit = 1; bit && bit < 0x40u; bit <<= 1)
                if (mask & bit)
                    for (unsigned w = 0; w < klspv_imgop_words(bit); w++) {
                        if (src >= i + len) { short_of_words = 1; break; }
                        out[o++] = code[src++];
                    }
            if (short_of_words) { free(out); free(spimg); free(msval); return NULL; }
            out[o++] = zero;
            while (src < i + len) out[o++] = code[src++];
        } else {
            for (uint32_t k = 0; k < len; k++) out[o++] = code[i + k];
        }
        i += len;
    }
    out[3] = bound;
    *out_nw = o;
    static int said;
    if (said < 4) { said++;
        fprintf(stderr, "  [vk] KL_VK_SUBPASS_MS_SAMPLE: %u multisampled subpass read(s) given "
                        "the missing Sample operand (0); the module has %u subpass type(s), %u of "
                        "them multisampled, and %u read(s) of which %u named a sample\n",
                nfix, nsub, nms, nread, nhas); }
    free(spimg); free(msval);
    return out;
}

// KL_VK_SKY_FAR: a sky drawn as a mesh, moved to the far plane. LEGO Bricktales
// draws its sky as a dome around the player (a vertical-gradient vertex shader,
// depth test and write like any opaque object) AFTER the clouds, and the clouds
// stand partly outside that dome: in VR the dome's wall covered the lower half
// of every cloud along one straight line, while in mixed reality — no dome —
// they were whole (his report and a device capture, 2026-10-18). The vertex
// shader is given one more step before it returns, z = w * 1e-6: with the
// reversed depth Unity uses, that is a hair in front of the cleared far value,
// so the dome only fills what nothing else has drawn on, as a skybox does.
// Which shaders: the FNV of the module's words, from KL_VK_MOD_DUMP; the
// default list is per target. KL_VK_SKY_FAR=<hex,hex,...> replaces it, =0 is off.
static int klvk_sky_far_wanted(const uint32_t *code, size_t bytes) {
    static char list[256];
    static int init;
    if (!init) {
        const char *t = kl_driver_target_name();
        const char *dflt = t && strcmp(t, "x6521909757843713") == 0 ? "1758a968" : "";
        const char *v = kl_env_str("KL_VK_SKY_FAR", dflt);
        snprintf(list, sizeof list, "%s", v && strcmp(v, "0") ? v : "");
        init = 1;
    }
    if (!list[0]) return 0;
    char h[16];
    snprintf(h, sizeof h, "%08x", klvk_fnv(code, bytes));
    return strstr(list, h) != NULL;
}
static uint32_t *klvk_spirv_far_position(const uint32_t *code, size_t nw, size_t *out_nw) {
    if (!code || nw < 6 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3];
    if (bound < 2 || bound > (1u << 22)) return NULL;
    uint32_t entry = 0, posvar = 0, posblock = 0, posmember = 0, blockptr = 0, blockvar = 0;
    uint32_t outvec4ptr = 0, vec4 = 0, flt = 0, memberidx = 0, intType[2] = {0, 0};
    size_t firstFn = 0, nret = 0;
    // First pass: the entry point, how Position is reached, and the types the new steps need.
    for (size_t i = 5; i < nw; ) {
        uint32_t len = code[i] >> 16, op = code[i] & 0xffffu;
        if (!len || i + len > nw) return NULL;
        if (op == 15 && len >= 3 && code[i + 1] == 0 && !entry) entry = code[i + 2];   // OpEntryPoint Vertex
        else if (op == 71 && len >= 4 && code[i + 2] == 11 && code[i + 3] == 0) posvar = code[i + 1];
        else if (op == 72 && len >= 5 && code[i + 3] == 11 && code[i + 4] == 0) {
            posblock = code[i + 1]; posmember = code[i + 2];
        } else if (op == 22 && len == 3 && code[i + 2] == 32 && !flt) flt = code[i + 1];
        else if (op == 23 && len == 4 && code[i + 2] == flt && code[i + 3] == 4 && !vec4) vec4 = code[i + 1];
        else if (op == 21 && len == 4 && code[i + 2] == 32 && !intType[code[i + 3] ? 1 : 0])
            intType[code[i + 3] ? 1 : 0] = code[i + 1];
        else if (op == 32 && len == 4 && code[i + 2] == 3) {                              // OpTypePointer Output
            if (vec4 && code[i + 3] == vec4) outvec4ptr = code[i + 1];
            if (posblock && code[i + 3] == posblock) blockptr = code[i + 1];
        } else if (op == 59 && len >= 4 && code[i + 3] == 3 && blockptr && code[i + 1] == blockptr)
            blockvar = code[i + 2];
        else if (op == 43 && len == 4 && code[i + 3] == posmember && !memberidx &&
                 (code[i + 1] == intType[0] || code[i + 1] == intType[1]))
            memberidx = code[i + 2];
        else if (op == 54 && !firstFn) firstFn = i;
        i += len;
    }
    int viaBlock = !posvar;
    if (!entry || !firstFn || !flt || !vec4 || !outvec4ptr) return NULL;
    if (viaBlock && (!blockvar || !memberidx)) return NULL;
    // Count the returns of the entry function, which is where the steps go.
    int inEntry = 0;
    for (size_t i = firstFn; i < nw; ) {
        uint32_t len = code[i] >> 16, op = code[i] & 0xffffu;
        if (op == 54) inEntry = code[i + 2] == entry;
        else if (op == 56) inEntry = 0;
        else if (op == 253 && inEntry) nret++;
        i += len;
    }
    if (!nret) return NULL;
    uint32_t *out = malloc((nw + 4 + nret * 26) * sizeof *out);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < 5; i++) out[o++] = code[i];
    uint32_t eps = 0;
    union { float f; uint32_t u; } e = { 1.0e-6f };
    inEntry = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t len = code[i] >> 16, op = code[i] & 0xffffu;
        if (i == firstFn) {
            eps = bound++;
            out[o++] = (4u << 16) | 43u; out[o++] = flt; out[o++] = eps; out[o++] = e.u;   // OpConstant
        }
        if (op == 54) inEntry = code[i + 2] == entry;
        else if (op == 56) inEntry = 0;
        if (op == 253 && inEntry) {
            uint32_t ptr = posvar;
            if (viaBlock) {
                ptr = bound++;
                out[o++] = (5u << 16) | 65u; out[o++] = outvec4ptr; out[o++] = ptr;      // OpAccessChain
                out[o++] = blockvar; out[o++] = memberidx;
            }
            uint32_t v = bound++, w = bound++, z = bound++, v2 = bound++;
            out[o++] = (4u << 16) | 61u; out[o++] = vec4; out[o++] = v; out[o++] = ptr;          // OpLoad
            out[o++] = (5u << 16) | 81u; out[o++] = flt; out[o++] = w; out[o++] = v; out[o++] = 3; // OpCompositeExtract .w
            out[o++] = (5u << 16) | 133u; out[o++] = flt; out[o++] = z; out[o++] = w; out[o++] = eps; // OpFMul
            out[o++] = (6u << 16) | 82u; out[o++] = vec4; out[o++] = v2; out[o++] = z;           // OpCompositeInsert .z
            out[o++] = v; out[o++] = 2;
            out[o++] = (3u << 16) | 62u; out[o++] = ptr; out[o++] = v2;                          // OpStore
        }
        for (uint32_t k = 0; k < len; k++) out[o++] = code[i + k];
        i += len;
    }
    out[3] = bound;
    *out_nw = o;
    return out;
}

// vkCreateShaderModule — gated SPIR-V rewrite. emu_on() false, or a module with
// no texel-buffer image, is EXACT passthrough. A rewrite MoltenVK rejects falls
// back to the original module (buggy but never a hard failure).
static VkResult VKAPI_CALL klvk_CreateShaderModule0(VkDevice dev,
        const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkShaderModule *out);
static int klvk_eye_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_VK_EYE_ENVGUARD", t && strcmp(t, "alyx") == 0);
    }
    return on;
}

// ---------------------------------------------------------------------------
// KL_VK_VRS_STRIP (default on): the shading-rate built-ins, made plain
// variables. HITMAN 3 VR: Reloaded's fragment shaders read ShadingRateKHR (a
// debug tint by the rate the Quest shaded at) and SPIRV-Cross has no Metal
// type for it — "unsupported-built-in-type", and the whole shader fails, so
// the world was black on the headset while the interface drew (2026-10-20).
// Metal has no variable-rate shading, so the honest value is the full rate,
// 0: each such variable (and a vertex shader's PrimitiveShadingRateKHR output,
// whose writes nothing reads) becomes a Private variable initialised to null,
// with its own pointer type; the decoration, the capability and the extension
// go. Only modules that use one are touched. Returns a malloc'd module or NULL.
static uint32_t *klvk_spirv_strip_vrs(const uint32_t *code, size_t nw, size_t *out_nw) {
    if (!code || nw < 6 || code[0] != 0x07230203u) return NULL;
    enum { MAXS = 8 };
    uint32_t sid[MAXS]; int ns = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t len = code[i] >> 16, op = code[i] & 0xffffu;
        if (!len || i + len > nw) return NULL;
        if (op == 71 /* OpDecorate */ && len >= 4 && code[i + 2] == 11 /* BuiltIn */ &&
            (code[i + 3] == 4444 /* ShadingRateKHR */ || code[i + 3] == 4432 /* PrimitiveShadingRateKHR */) && ns < MAXS)
            sid[ns++] = code[i + 1];
        i += len;
    }
    if (!ns) return NULL;
    uint32_t bound = code[3];
    // pointee of each pointer type, looked up as variables come
    uint32_t *o = malloc((nw + 16 * ns) * 4);
    if (!o) return NULL;
    memcpy(o, code, 5 * 4);
    size_t n = 5;
    int old14 = code[1] < 0x00010400u;      // before 1.4 the interface lists only Input/Output
    for (size_t i = 5; i < nw; ) {
        uint32_t len = code[i] >> 16, op = code[i] & 0xffffu;
        const uint32_t *w = &code[i];
        int skip = 0;
        if (op == 17 /* OpCapability */ && w[1] == 4422 /* FragmentShadingRateKHR */) skip = 1;
        if (op == 10 /* OpExtension */ && !strcmp((const char *)&w[1], "SPV_KHR_fragment_shading_rate")) skip = 1;
        if ((op == 71 /* OpDecorate */) && len >= 2)
            for (int k = 0; k < ns; k++) if (w[1] == sid[k]) skip = 1;
        if (op == 59 /* OpVariable */ && len >= 4) {
            int hit = 0;
            for (int k = 0; k < ns; k++) if (w[2] == sid[k]) hit = 1;
            if (hit) {
                uint32_t pointee = 0;
                for (size_t j = 5; j < i; ) {     // its pointer type was declared before it
                    uint32_t l2 = code[j] >> 16, o2 = code[j] & 0xffffu;
                    if (!l2) break;
                    if (o2 == 32 /* OpTypePointer */ && l2 == 4 && code[j + 1] == w[1]) pointee = code[j + 3];
                    j += l2;
                }
                if (!pointee) { free(o); return NULL; }
                uint32_t ptr = bound++, nul = bound++;
                o[n++] = (4u << 16) | 32; o[n++] = ptr; o[n++] = 6 /* Private */; o[n++] = pointee;
                o[n++] = (3u << 16) | 46 /* OpConstantNull */; o[n++] = pointee; o[n++] = nul;
                o[n++] = (5u << 16) | 59; o[n++] = ptr; o[n++] = w[2]; o[n++] = 6; o[n++] = nul;
                i += len;
                continue;
            }
        }
        if (op == 15 /* OpEntryPoint */ && old14 && len > 3) {
            // the name runs from word 3 to the word holding its terminating zero
            size_t j = 3;
            while (j < len) { uint32_t x = w[j++]; if (!(x & 0xff000000u) || !(x & 0xff0000u) || !(x & 0xff00u) || !(x & 0xffu)) break; }
            size_t start = n;
            for (size_t k = 0; k < j; k++) o[n++] = w[k];
            for (; j < len; j++) {
                int drop = 0;
                for (int k = 0; k < ns; k++) if (w[j] == sid[k]) drop = 1;
                if (!drop) o[n++] = w[j];
            }
            o[start] = ((uint32_t)(n - start) << 16) | 15;
            i += len;
            continue;
        }
        if (!skip) { memcpy(&o[n], w, len * 4); n += len; }
        i += len;
    }
    o[3] = bound;
    *out_nw = n;
    return o;
}
// KL_VK_VIEWINDEX_COPY (default on): a shader that hands the ViewIndex
// built-in to a function by pointer. SPIRV-Cross makes gl_ViewIndex a const
// local of main() for Metal and the function's parameter a `thread uint&`, and
// Metal refuses the call ("no matching function ... would lose const
// qualifier"). HITMAN 3 VR: Reloaded's level shaders do it in GetRoomIndex
// (Mac, Guided Training, 2026-10-20: the world drew with pieces missing). So
// the built-in is copied once, at the top of the entry point, into a Private
// variable, and every use of it inside functions goes to the copy — which
// SPIRV-Cross passes as an ordinary `thread uint&`. Only modules that pass the
// built-in to a function are touched. Returns a malloc'd module or NULL.
static uint32_t *klvk_spirv_viewindex_copy(const uint32_t *code, size_t nw, size_t *out_nw) {
    if (!code || nw < 6 || code[0] != 0x07230203u) return NULL;
    uint32_t v = 0, vptr = 0, pointee = 0, entry = 0;
    int passed = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t len = code[i] >> 16, op = code[i] & 0xffffu;
        if (!len || i + len > nw) return NULL;
        if (op == 71 && len >= 4 && code[i + 2] == 11 && code[i + 3] == 4440 /* ViewIndex */) v = code[i + 1];
        if (op == 15 && len >= 3 && !entry) entry = code[i + 2];
        i += len;
    }
    if (!v || !entry) return NULL;
    // Passed by pointer, or read inside a function other than the entry point —
    // SPIRV-Cross then makes it a parameter of that function itself.
    uint32_t fn = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t len = code[i] >> 16, op = code[i] & 0xffffu;
        if (op == 54 && len >= 3) fn = code[i + 2];
        if (op == 59 && len >= 4 && code[i + 2] == v) vptr = code[i + 1];
        if (op == 57 /* OpFunctionCall */)
            for (uint32_t k = 4; k < len; k++) if (code[i + k] == v) passed = 1;
        if (fn && fn != entry && ((op == 61 && len >= 4 && code[i + 3] == v) ||
                                  ((op == 65 || op == 66) && len >= 4 && code[i + 3] == v)))
            passed = 1;
        i += len;
    }
    if (!passed || !vptr) return NULL;
    for (size_t i = 5; i < nw; ) {
        uint32_t len = code[i] >> 16, op = code[i] & 0xffffu;
        if (op == 32 && len == 4 && code[i + 1] == vptr) pointee = code[i + 3];
        i += len;
    }
    if (!pointee) return NULL;
    uint32_t bound = code[3], tp = bound++, pv = bound++, tmp = bound++;
    uint32_t *o = malloc((nw + 16) * 4);
    if (!o) return NULL;
    memcpy(o, code, 5 * 4);
    size_t n = 5;
    int in_funcs = 0, in_entry = 0, inserted = 0, after_label = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t len = code[i] >> 16, op = code[i] & 0xffffu;
        const uint32_t *w = &code[i];
        // the load and store go after the entry block's label and its OpVariables
        if (in_entry && after_label && !inserted && op != 59 && op != 8 && op != 317) {
            o[n++] = (4u << 16) | 61; o[n++] = pointee; o[n++] = tmp; o[n++] = v;
            o[n++] = (3u << 16) | 62; o[n++] = pv; o[n++] = tmp;
            inserted = 1;
        }
        size_t start = n;
        memcpy(&o[n], w, len * 4); n += len;
        // From SPIR-V 1.4 every global an entry point uses is in its interface.
        if (op == 15 && code[1] >= 0x00010400u && w[2] == entry) {
            o[n++] = pv;
            o[start] = ((uint32_t)(n - start) << 16) | 15;
        }
        if (op == 54 /* OpFunction */) { in_funcs = 1; in_entry = w[2] == entry; after_label = 0; }
        if (op == 248 /* OpLabel */ && in_entry) after_label = 1;
        if (op == 59 && w[2] == v && !in_funcs) {
            o[n++] = (4u << 16) | 32; o[n++] = tp; o[n++] = 6 /* Private */; o[n++] = pointee;
            o[n++] = (4u << 16) | 59; o[n++] = tp; o[n++] = pv; o[n++] = 6;
        }
        if (in_funcs) {
            uint32_t *x = &o[start];
            switch (op) {
            case 61: if (len >= 4 && x[3] == v) x[3] = pv; break;                  // OpLoad
            case 62: if (len >= 3 && x[1] == v) x[1] = pv; break;                  // OpStore
            case 63: for (uint32_t k = 1; k <= 2 && k < len; k++) if (x[k] == v) x[k] = pv; break;
            case 65: case 66: case 67: if (len >= 4 && x[3] == v) x[3] = pv; break; // access chains
            case 57: for (uint32_t k = 4; k < len; k++) if (x[k] == v) x[k] = pv; break;
            default: break;
            }
        }
        i += len;
    }
    if (!inserted) { free(o); return NULL; }
    // the inserted load reads the built-in itself; it was written after the rewrite pass saw it
    o[3] = bound;
    *out_nw = n;
    return o;
}
static int klvk_viewindex_copy_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_VIEWINDEX_COPY", 1);
    return on;
}
static int klvk_vrs_strip_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_VRS_STRIP", 1);
    return on;
}

static VkResult VKAPI_CALL klvk_CreateShaderModule(VkDevice dev,
        const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkShaderModule *out) {
    // KL_VK_MOD_DUMP=<dir>, a diagnostic: every module the guest creates, as
    // <fnv of its words>.spv, to find one shader among hundreds.
    {
        const char *dd = kl_env_str("KL_VK_MOD_DUMP", NULL);
        if (dd && *dd && ci && ci->pCode && !(ci->codeSize & 3u)) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%08x.spv", dd, klvk_fnv(ci->pCode, ci->codeSize));
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(ci->pCode, 1, ci->codeSize, f); fclose(f); }
        }
    }
    // KL_VK_SUBPASS_NOKILL=1, a diagnostic: the shaders that read a subpass input
    // lose their discards (OpKill -> OpReturn), so that a query counting what
    // they let through counts everything — which tells a query that counts
    // nothing apart from a shader that discards everything.
    uint32_t *nokill = NULL;
    VkShaderModuleCreateInfo nk;
    if (ci && ci->pCode && !(ci->codeSize & 3u) && kl_env_on("KL_VK_SUBPASS_NOKILL", 0) &&
        klvk_spirv_reads_subpass(ci->pCode, ci->codeSize / 4) && (nokill = malloc(ci->codeSize))) {
        size_t nw = ci->codeSize / 4;
        memcpy(nokill, ci->pCode, ci->codeSize);
        for (size_t i = 5; i < nw; ) {
            uint32_t len = nokill[i] >> 16, op = nokill[i] & 0xffffu;
            if (!len) break;
            if (op == 252 && len == 1) nokill[i] = (1u << 16) | 253u;
            i += len;
        }
        nk = *ci; nk.pCode = nokill; ci = &nk;
    }
    // KL_VK_SUBPASS_PROBE=<bits>, the companion diagnostic: the subpass read
    // itself is replaced by a constant vector of the float constant with that
    // bit pattern (0x3f800000 is 1.0), if the shader has one. What the query
    // then counts is what a KNOWN input makes of the shader's thresholds, to
    // set beside what the real read makes of them.
    unsigned probe = kl_env_uint("KL_VK_SUBPASS_PROBE", 0);
    if (!nokill && probe && ci && ci->pCode && !(ci->codeSize & 3u) &&
        klvk_spirv_reads_subpass(ci->pCode, ci->codeSize / 4) && (nokill = malloc(ci->codeSize))) {
        size_t nw = ci->codeSize / 4;
        memcpy(nokill, ci->pCode, ci->codeSize);
        uint32_t fl = 0, cid = 0;
        for (size_t i = 5; i < nw; ) {
            uint32_t len = nokill[i] >> 16, op = nokill[i] & 0xffffu;
            if (!len) break;
            if (op == 22 && len == 3 && nokill[i + 2] == 32) fl = nokill[i + 1];          // OpTypeFloat 32
            if (op == 43 && len == 4 && nokill[i + 1] == fl && nokill[i + 3] == probe) cid = nokill[i + 2];
            if (op == 98 && len == 7 && cid) {                                            // OpImageRead
                uint32_t type = nokill[i + 1], res = nokill[i + 2];
                nokill[i] = (7u << 16) | 80u;                                             // OpCompositeConstruct
                nokill[i + 1] = type; nokill[i + 2] = res;
                nokill[i + 3] = nokill[i + 4] = nokill[i + 5] = nokill[i + 6] = cid;
                fprintf(stderr, "  [vk] KL_VK_SUBPASS_PROBE: a subpass read replaced by the constant %#x\n", probe);
            }
            i += len;
        }
        nk = *ci; nk.pCode = nokill; ci = &nk;
    }
    // KL_VK_EYE_ENVGUARD (default on, Half-Life: Alyx): the vr_eyeball pixel shaders
    // read past the end of a constant buffer, which Metal does not bounds-check —
    // the colour flashes in NPC eyes. They are rewritten to index it as every
    // other shader of the game does (kl_vk_eyefix.c, after Glovebox). Before the
    // pipeline list is replayed as well as in the game: the replay creates its
    // modules through here, so what it warms is the rewritten shader.
    uint32_t *eye = NULL;
    VkShaderModuleCreateInfo ey;
    if (ci && ci->pCode && !(ci->codeSize & 3u) && klvk_eye_on()) {
        size_t ew = 0;
        if ((eye = kl_vk_eye_envguard(ci->pCode, ci->codeSize / 4, &ew))) {
            static unsigned said;
            unsigned k = __atomic_add_fetch(&said, 1, __ATOMIC_RELAXED);
            if (k <= 8) fprintf(stderr, "  [vk] eye cubemap fix: a vr_eyeball pixel shader of %zu bytes rewritten (#%u)\n",
                                (size_t)ci->codeSize, k);
            ey = *ci; ey.pCode = eye; ey.codeSize = ew * 4; ci = &ey;
        }
    }
    uint32_t *sky = NULL;
    VkShaderModuleCreateInfo sk;
    if (ci && ci->pCode && !(ci->codeSize & 3u) && klvk_sky_far_wanted(ci->pCode, ci->codeSize)) {
        size_t sw = 0;
        if ((sky = klvk_spirv_far_position(ci->pCode, ci->codeSize / 4, &sw))) {
            fprintf(stderr, "  [vk] KL_VK_SKY_FAR: sky vertex shader %08x moved to the far plane "
                            "(it now fills only what nothing else drew on)\n",
                    klvk_fnv(ci->pCode, ci->codeSize));
            sk = *ci; sk.pCode = sky; sk.codeSize = sw * 4; ci = &sk;
        } else {
            fprintf(stderr, "  [vk] KL_VK_SKY_FAR: shader %08x matched but its shape is not one "
                            "this rewrite knows — left as it is\n", klvk_fnv(ci->pCode, ci->codeSize));
        }
    }
    uint32_t *msfix = NULL;
    VkShaderModuleCreateInfo mf;
    if (ci && ci->pCode && !(ci->codeSize & 3u) && klvk_subpass_ms_on()) {
        size_t mw = 0;
        if ((msfix = klvk_spirv_subpass_ms_sample(ci->pCode, ci->codeSize / 4, &mw))) {
            mf = *ci; mf.pCode = msfix; mf.codeSize = mw * 4; ci = &mf;
        }
    }
    uint32_t *vrs = NULL;
    VkShaderModuleCreateInfo vf;
    if (ci && ci->pCode && !(ci->codeSize & 3u) && klvk_vrs_strip_on()) {
        size_t vw = 0;
        if ((vrs = klvk_spirv_strip_vrs(ci->pCode, ci->codeSize / 4, &vw))) {
            vf = *ci; vf.pCode = vrs; vf.codeSize = vw * 4; ci = &vf;
            static unsigned said;
            if (said++ < 3 || !(said % 200))
                VKI("shading-rate built-ins made plain variables (rate 0, the full rate) in %u module(s) so far\n", said);
        }
    }
    uint32_t *vic = NULL;
    VkShaderModuleCreateInfo vcf;
    if (ci && ci->pCode && !(ci->codeSize & 3u) && klvk_viewindex_copy_on()) {
        size_t vw = 0;
        if ((vic = klvk_spirv_viewindex_copy(ci->pCode, ci->codeSize / 4, &vw))) {
            vcf = *ci; vcf.pCode = vic; vcf.codeSize = vw * 4; ci = &vcf;
            static unsigned said;
            if (said++ < 3 || !(said % 200))
                VKI("ViewIndex handed to a function: copied into a private variable in %u module(s) so far\n", said);
        }
    }
    VkResult r = klvk_CreateShaderModule0(dev, ci, alloc, out);
    if (r == VK_SUCCESS && out && ci && ci->pCode && !(ci->codeSize & 3u) && klvk_vin_on()) {
        klvk_vin_ent in;
        if (klvk_spirv_vertex_inputs(ci->pCode, ci->codeSize / 4, &in))
            klvk_vin_put(g_vin_mod, (uint64_t)(uintptr_t)*out, in.f, in.i, in.u);
        static const char sfk[] = "SCALEFORM_CONSTANTS_BLOCK";
        if (memmem(ci->pCode, ci->codeSize, sfk, sizeof sfk - 1))
            klvk_handle_set_add(g_sf_mod, (uint64_t)(uintptr_t)*out);
    }
    if (r == VK_SUCCESS && out && ci && ci->pCode && klvk_fs_on())
        klvk_fs_put(g_fs_mod, (uint64_t)(uintptr_t)*out, klvk_fnv(ci->pCode, ci->codeSize));
    free(nokill);
    if (r == VK_SUCCESS && out && ci && !(ci->codeSize & 3u) && !nokill &&
        klvk_spirv_reads_subpass(ci->pCode, ci->codeSize / 4)) {
        klvk_handle_set_add(g_subpass_mod, (uint64_t)(uintptr_t)*out);
        static int said;
        if (!said++) fprintf(stderr, "  [vk] a shader that reads a subpass input exists — a barrier "
                                     "inside a pass can matter (KL_VK_PASS_BARRIERS)\n");
        // KL_VK_SUBPASS_DUMP=<dir>: keep the SPIR-V of every such shader, to see
        // what the effect is.
        const char *dir = kl_env_str("KL_VK_SUBPASS_DUMP", NULL);
        if (dir && *dir) {
            static unsigned n;
            char path[1024];
            snprintf(path, sizeof path, "%s/subpass_%u.spv", dir, __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED));
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(ci->pCode, 1, ci->codeSize, f); fclose(f); }
        }
    }
    free(vic);
    free(vrs);
    free(msfix);
    free(sky);
    free(eye);
    return r;
}
static VkResult VKAPI_CALL klvk_CreateShaderModule0(VkDevice dev,
        const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkShaderModule *out) {
    static PFN_vkCreateShaderModule real;
    if (!real && real_gdpa) real = (PFN_vkCreateShaderModule)real_gdpa(dev, "vkCreateShaderModule");
    if (!emu_on() || !ci || !ci->pCode || (ci->codeSize & 3u) || !real)
        return real ? real(dev, ci, alloc, out) : VK_ERROR_INITIALIZATION_FAILED;
    size_t nwords = ci->codeSize / 4;
    size_t outw = 0;
    uint32_t *nc = klvk_spirv_mirror_rewrite(ci->pCode, nwords, &outw);
    if (!nc) return real(dev, ci, alloc, out);   // nothing to change
    VkShaderModuleCreateInfo m = *ci;
    m.pCode = nc;
    m.codeSize = outw * 4;
    VkResult r = real(dev, &m, alloc, out);
    free(nc);
    if (r != VK_SUCCESS) {
        static int said; if (said < 8) { said++;
            VKI("[emu] SPIR-V texel->2D rewrite rejected (result %d); using the "
                "original module (that shader keeps the MoltenVK texel-buffer bug)\n",
                (int)r); }
        r = real(dev, ci, alloc, out);
    } else {
        static int said; if (said < 8) { said++;
            VKI("[emu] SPIR-V texel->2D rewrite applied (%zu -> %zu words)\n",
                nwords, outw); }
    }
    return r;
}

// vkCreateDescriptorSetLayout — UNIFORM_TEXEL_BUFFER bindings become SAMPLED_IMAGE
// so the layout matches the rewritten shader. Exact passthrough when emu off.
static VkResult VKAPI_CALL klvk_CreateDescriptorSetLayout(VkDevice dev,
        const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkDescriptorSetLayout *out) {
    static PFN_vkCreateDescriptorSetLayout real;
    if (!real && real_gdpa) real = (PFN_vkCreateDescriptorSetLayout)real_gdpa(dev, "vkCreateDescriptorSetLayout");
    if (!emu_on() || !ci || ci->bindingCount == 0 || !ci->pBindings || !real)
        return real ? real(dev, ci, alloc, out) : VK_ERROR_INITIALIZATION_FAILED;
    int any = 0;
    for (uint32_t i = 0; i < ci->bindingCount; i++)
        if (ci->pBindings[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) { any = 1; break; }
    // PROBE (olar gray hunt): count STORAGE_TEXEL_BUFFER bindings. The emu rewrites
    // only UNIFORM_TEXEL_BUFFER (layout, shader, descriptor writes); a layout that
    // binds its texel buffers as STORAGE gets a mirror created but never bound
    // through a rewrite, so those reads stay raw -> gray/invisible. If this fires
    // for olar, the fix is to extend the emu to STORAGE texel buffers.
    for (uint32_t i = 0; i < ci->bindingCount; i++)
        if (ci->pBindings[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            static int said;
            if (said < 12) { said++;
                VKI("[emu] PROBE: descriptor-set layout binds a STORAGE_TEXEL_BUFFER "
                    "(binding %u, count %u) — NOT rewritten by the emu; gray/invisible "
                    "suspect\n", ci->pBindings[i].binding, ci->pBindings[i].descriptorCount); }
        }
    if (!any) return real(dev, ci, alloc, out);
    VkDescriptorSetLayoutBinding *nb =
        (VkDescriptorSetLayoutBinding *)malloc(ci->bindingCount * sizeof *nb);
    if (!nb) return real(dev, ci, alloc, out);
    memcpy(nb, ci->pBindings, ci->bindingCount * sizeof *nb);
    for (uint32_t i = 0; i < ci->bindingCount; i++)
        if (nb[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
            nb[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            nb[i].pImmutableSamplers = NULL;   // sampled images take no immutable sampler
        }
    VkDescriptorSetLayoutCreateInfo m = *ci;
    m.pBindings = nb;
    VkResult r = real(dev, &m, alloc, out);
    free(nb);
    return r;
}

// vkCreateDescriptorPool — mirror the layout rewrite in pool sizing so a set with
// SAMPLED_IMAGE bindings can be allocated from a pool the guest sized for
// UNIFORM_TEXEL_BUFFER. Exact passthrough when emu off.
static VkResult VKAPI_CALL klvk_CreateDescriptorPool(VkDevice dev,
        const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkDescriptorPool *out) {
    static PFN_vkCreateDescriptorPool real;
    if (!real && real_gdpa) real = (PFN_vkCreateDescriptorPool)real_gdpa(dev, "vkCreateDescriptorPool");
    if (!emu_on() || !ci || ci->poolSizeCount == 0 || !ci->pPoolSizes || !real)
        return real ? real(dev, ci, alloc, out) : VK_ERROR_INITIALIZATION_FAILED;
    int any = 0;
    for (uint32_t i = 0; i < ci->poolSizeCount; i++)
        if (ci->pPoolSizes[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) { any = 1; break; }
    if (!any) return real(dev, ci, alloc, out);
    VkDescriptorPoolSize *ns = (VkDescriptorPoolSize *)malloc(ci->poolSizeCount * sizeof *ns);
    if (!ns) return real(dev, ci, alloc, out);
    memcpy(ns, ci->pPoolSizes, ci->poolSizeCount * sizeof *ns);
    for (uint32_t i = 0; i < ci->poolSizeCount; i++)
        if (ns[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
            ns[i].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;   // duplicate types are legal; MoltenVK sums them
    VkDescriptorPoolCreateInfo m = *ci;
    m.pPoolSizes = ns;
    VkResult r = real(dev, &m, alloc, out);
    free(ns);
    return r;
}

// vkUpdateDescriptorSets — a write to a rewritten (now SAMPLED_IMAGE) binding
// arrives as a UNIFORM_TEXEL_BUFFER write with pTexelBufferView. Substitute a
// SAMPLED_IMAGE write pointing at each view's mirror image, refresh the mirror
// from the live pool bytes, and record (set,binding,elem)->mirror for bind-time
// refresh. Exact passthrough when emu off or when no write targets a texel buffer.
// PROBE (olar gray hunt): classify a COMBINED_IMAGE_SAMPLER / SAMPLED_IMAGE binding
// by the size of the image it points at. The world renders gray with textures that
// DO have data, so the suspect is materials binding UE's 1x1 default (white/gray)
// textures instead of their real albedo. Tally real (>1x1) vs 1x1 across all image
// bindings; a heavy 1x1 skew on the world material sets is the confirmation.
static int klvk_is_astc(VkFormat f);   // defined below
static void klvk_probe_img_binding(VkImageView v) {
    if (!(emu_on() || probe_on())) return;
    static unsigned real_n, tiny_n, null_n, astc_n; static int said;
    VkImage img = v ? klvk_view_image(v) : VK_NULL_HANDLE;
    VkFormat f = 0; uint32_t w = 0, h = 0;
    if (!img || !klvk_img_info_get(img, &f, &w, &h)) { null_n++; }
    else if (klvk_is_astc(f)) { astc_n++; real_n++; }   // an ASTC texture bound to sample
    else if (w <= 1 && h <= 1) tiny_n++;
    else real_n++;
    unsigned tot = real_n + tiny_n + null_n;
    if (tot && (tot & (tot - 1)) == 0 && said < 24) { said++;
        VKI("[probe] image-sampler bindings so far: %u real(>1x1) of which %u ASTC, "
            "%u tiny(1x1 default?), %u unknown-view\n", real_n, astc_n, tiny_n, null_n); }
}

static VkDescriptorUpdateTemplate g_ia_tmpl;
static size_t g_ia_tmpl_off;
static void klvk_trace_input_att_tmpl(const char *how, VkDescriptorUpdateTemplate t, const void *data);

// KL_VK_INPUT_ATT_FIX (default on) — an input attachment bound to a 1x1
// placeholder is bound to the scene's multisampled colour instead.
//
// With dynamic-rendering local read a tile-based driver serves an input
// attachment from the tile it is rendering: which attachment is named by the
// shader's InputAttachmentIndex, and the image in the descriptor is never
// looked at. Source 2 relies on that — the descriptor its exposure meter is
// given holds a 1x1 RGBA8 dummy. MoltenVK has no framebuffer fetch; it reads
// the texture the descriptor names, so the meter read one black texel of the
// wrong kind (not multisampled, not layered), discarded every fragment, and
// every one of its occlusion queries came back 0 — the engine has been
// exposing for a scene it measured as black. (Proven by replacing the read
// with a constant: KL_VK_SUBPASS_PROBE=0x3f800000 makes one query in sixteen
// count the whole picture; the real read made none count anything.)
//
// The fix names the attachment the driver would have used. It still needs that
// attachment's contents to be in memory when the meter reads them, which is
// what KL_VK_PASS_BARRIERS decides (the split, or last frame's stored copy).
#define KLVK_IA_TMPLS 8
static struct { VkDescriptorUpdateTemplate t; size_t off; } g_ia_tmpls[KLVK_IA_TMPLS];
static VkImageView klvk_input_att_fix(VkDescriptorUpdateTemplate t, const void *data, VkImageView **where) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_INPUT_ATT_FIX", 1);
    *where = NULL;
    if (!on || !t || !data) return VK_NULL_HANDLE;
    for (int i = 0; i < KLVK_IA_TMPLS; i++) {
        if (g_ia_tmpls[i].t != t) continue;
        VkDescriptorImageInfo *ii = (VkDescriptorImageInfo *)((uint8_t *)(uintptr_t)data + g_ia_tmpls[i].off);
        VkImageView scene = __atomic_load_n(&g_scene_colour_view, __ATOMIC_ACQUIRE);
        uint32_t w = 0, h = 0;
        if (!scene || ii->imageView == scene || klvk_msaa_view_size(ii->imageView, &w, &h)) return VK_NULL_HANDLE;
        // Only a placeholder is replaced. A real single-sampled input
        // attachment is somebody's picture and stays what it is.
        if (!klvk_handle_set_has(g_tiny_view, (uint64_t)(uintptr_t)ii->imageView)) return VK_NULL_HANDLE;
        static int said;
        if (!said++)
            fprintf(stderr, "  [vk] KL_VK_INPUT_ATT_FIX: an input attachment bound to a 1x1 placeholder "
                            "is bound to the scene's multisampled colour instead\n");
        VkImageView was = ii->imageView;
        ii->imageView = scene;
        *where = &ii->imageView;
        return was;
    }
    return VK_NULL_HANDLE;
}
// KL_VK_QUERY_TRACE: how the guest binds what its input-attachment shaders read.
static void klvk_trace_input_att(const char *how, uint32_t n, const VkWriteDescriptorSet *w) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_QUERY_TRACE", 0);
    if (!on || !w) return;
    for (uint32_t i = 0; i < n; i++) {
        if (w[i].descriptorType != VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) continue;
        static unsigned said;
        if (said++ >= 12) return;
        uint32_t mw = 0, mh = 0;
        VkImageView v = w[i].pImageInfo ? w[i].pImageInfo[0].imageView : VK_NULL_HANDLE;
        fprintf(stderr, "  [vk] input attachment bound by %s: binding %u, view %p%s, layout %d\n", how,
                w[i].dstBinding, (void *)v, klvk_msaa_view_size(v, &mw, &mh) ? " (a multisampled target)" : "",
                w[i].pImageInfo ? (int)w[i].pImageInfo[0].imageLayout : -1);
    }
}
static void klvk_trace_input_att_tmpl(const char *how, VkDescriptorUpdateTemplate t, const void *data) {
    if (!g_ia_tmpl || t != g_ia_tmpl || !data) return;
    static unsigned said;
    if (said++ >= 12) return;
    const VkDescriptorImageInfo *ii = (const VkDescriptorImageInfo *)((const uint8_t *)data + g_ia_tmpl_off);
    uint32_t mw = 0, mh = 0, iw = 0, ih = 0;
    VkFormat f = 0;
    VkImage img = klvk_view_image(ii->imageView);
    klvk_img_info_get(img, &f, &iw, &ih);
    fprintf(stderr, "  [vk] input attachment bound by %s: view %p%s, layout %d — image %p format %d %ux%u%s\n",
            how, (void *)ii->imageView,
            klvk_msaa_view_size(ii->imageView, &mw, &mh) ? " (a multisampled target)" : " (NOT a multisampled target)",
            (int)ii->imageLayout, (void *)img, (int)f, iw, ih,
            klvk_msaa_img_size(img, &mw, &mh) ? ", multisampled" : ", single-sampled");
}
static void VKAPI_CALL klvk_UpdateDescriptorSets(VkDevice dev,
        uint32_t writeCount, const VkWriteDescriptorSet *writes,
        uint32_t copyCount, const VkCopyDescriptorSet *copies) {
    klvk_trace_input_att("vkUpdateDescriptorSets", writeCount, writes);
    for (uint32_t i = 0; writes && i < writeCount; i++)
        klvk_ev(" u:%llx/%llu", (unsigned long long)(uintptr_t)writes[i].dstSet & 0xffffff, writes[i].dstBinding);
    // PROBE: tally image-sampler bindings (see klvk_probe_img_binding).
    if ((emu_on() || probe_on()) && writes)
        for (uint32_t i = 0; i < writeCount; i++)
            if ((writes[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                 writes[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) &&
                writes[i].pImageInfo)
                for (uint32_t j = 0; j < writes[i].descriptorCount; j++)
                    klvk_probe_img_binding(writes[i].pImageInfo[j].imageView);
    static PFN_vkUpdateDescriptorSets real;
    if (!real && real_gdpa) real = (PFN_vkUpdateDescriptorSets)real_gdpa(dev, "vkUpdateDescriptorSets");
    if (!real) return;
    if (!emu_on() || !writes || writeCount == 0) { real(dev, writeCount, writes, copyCount, copies); return; }
    uint32_t total = 0; int any = 0;
    for (uint32_t i = 0; i < writeCount; i++)
        if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
            any = 1; total += writes[i].descriptorCount;
        }
    if (!any) { real(dev, writeCount, writes, copyCount, copies); return; }

    VkWriteDescriptorSet *nw = (VkWriteDescriptorSet *)malloc(writeCount * sizeof *nw);
    VkDescriptorImageInfo *imgs = total ? (VkDescriptorImageInfo *)malloc(total * sizeof *imgs) : NULL;
    if (!nw || (total && !imgs)) {   // OOM: safest is the unmodified call
        free(nw); free(imgs);
        real(dev, writeCount, writes, copyCount, copies);
        return;
    }
    memcpy(nw, writes, writeCount * sizeof *nw);

    uint32_t ii = 0; unsigned nsubst = 0;
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < writeCount; i++) {
        if (writes[i].descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
            !writes[i].pTexelBufferView)
            continue;
        VkDescriptorImageInfo *slot = &imgs[ii];
        uint64_t set = (uint64_t)(uintptr_t)writes[i].dstSet;
        for (uint32_t j = 0; j < writes[i].descriptorCount; j++) {
            int mi = klvk_bvmap_find(writes[i].pTexelBufferView[j]);
            slot[j].sampler = VK_NULL_HANDLE;
            slot[j].imageView = (mi >= 0) ? g_mirrors[mi].view : VK_NULL_HANDLE;
            slot[j].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (mi >= 0) {
                nsubst++;
                // No copy here (no command buffer): just remember set->mirror so the
                // GPU copy is recorded at the following vkCmdBindDescriptorSets.
                klvk_dset_put(set, writes[i].dstBinding, writes[i].dstArrayElement + j, mi);
            }
        }
        nw[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        nw[i].pImageInfo = slot;
        nw[i].pBufferInfo = NULL;
        nw[i].pTexelBufferView = NULL;
        ii += writes[i].descriptorCount;
    }
    pthread_mutex_unlock(&g_lock);
    {
        static unsigned cum, calls; static int said;
        cum += nsubst; calls++;
        if (nsubst && said < 12) { said++;
            VKI("[emu] update-set: %u texel writes -> mirror image (cum %u over %u "
                "vkUpdateDescriptorSets calls)\n", nsubst, cum, calls); }
    }
    real(dev, writeCount, nw, copyCount, copies);
    free(nw); free(imgs);
}

// ---------------------------------------------------------------------------
// Descriptor UPDATE TEMPLATES and PUSH DESCRIPTORS — the bind paths UE5's Vulkan
// RHI actually uses (olar enables VK_KHR_descriptor_update_template AND
// VK_KHR_push_descriptor). Plain vkUpdateDescriptorSets is NOT how olar binds
// its texel buffers, so the SAMPLED_IMAGE substitution has to happen here too or
// the mirror images are created but never bound (scene stays gray). Same rule as
// everywhere else: exact passthrough when emu_on() is false, and only
// UNIFORM_TEXEL_BUFFER is rewritten (matching the layout/shader/pool rewrites).
// ---------------------------------------------------------------------------

// Byte size of one descriptor element in template pData, by type. 0 == a type we
// do not know how to repack (then a template carrying a texel entry alongside it
// is left un-rewritten rather than corrupted — logged). INLINE_UNIFORM_BLOCK is
// handled specially (its descriptorCount is a byte count, not an element count).
static uint32_t klvk_descr_elem_size(VkDescriptorType t) {
    switch (t) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            return (uint32_t)sizeof(VkDescriptorImageInfo);
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return (uint32_t)sizeof(VkBufferView);
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            return (uint32_t)sizeof(VkDescriptorBufferInfo);
        default:
            return 0;
    }
}

// One recorded template: the guest's ORIGINAL entry layout plus the REBUILT
// (tightly-packed, texel->image-expanded) layout the real rewritten template
// reads. vkUpdateDescriptorSetWithTemplate / vkCmdPushDescriptorSetWithTemplateKHR
// transform the guest's pData from the original into the rebuilt layout.
struct emu_tmpl_entry {
    VkDescriptorType type;            // original descriptor type
    uint32_t dstBinding, dstArrayElement, descriptorCount;
    size_t   origOffset, origStride;  // where the guest wrote it
    size_t   newOffset,  newStride;   // where the rewritten template reads it
    uint32_t elemSize;                // bytes copied per element into the new layout
    int      isTexel;                 // UNIFORM_TEXEL_BUFFER -> SAMPLED_IMAGE
    int      isInline;                // INLINE_UNIFORM_BLOCK: copy descriptorCount raw bytes
};
#define KLVK_EMU_TMPL_MAX     512
#define KLVK_EMU_TMPL_ENTRIES 64
struct emu_tmpl {
    VkDescriptorUpdateTemplate handle;
    int      nentry;
    size_t   dataSize;                // size of the rebuilt pData buffer
    struct emu_tmpl_entry entries[KLVK_EMU_TMPL_ENTRIES];
};
static struct emu_tmpl g_tmpls[KLVK_EMU_TMPL_MAX];
static int g_ntmpl;

// Runs under g_lock.
static struct emu_tmpl *klvk_tmpl_find(VkDescriptorUpdateTemplate h) {
    if (!h) return NULL;
    for (int i = 0; i < g_ntmpl; i++)
        if (g_tmpls[i].handle == h) return &g_tmpls[i];
    return NULL;
}

// Transform the guest's template pData into the rebuilt layout, substituting each
// texel VkBufferView for its mirror image's VkDescriptorImageInfo and refreshing
// the mirror bytes (a template update / push is a bind point). `setKey` is the
// descriptor-set handle for a set update (0 for push, where there is no persistent
// set to record for bind-time refresh — the refresh here already covers it).
// Runs under g_lock. Returns a malloc'd buffer the caller frees, or NULL on OOM.
static void *klvk_tmpl_transform(struct emu_tmpl *t, const void *pData, uint64_t setKey,
                                 unsigned *nsubst, int *mirrors, int *nmir, int maxmir) {
    uint8_t *nd = (uint8_t *)malloc(t->dataSize ? t->dataSize : 1);
    if (!nd) return NULL;
    unsigned subst = 0;
    const uint8_t *base = (const uint8_t *)pData;
    for (int e = 0; e < t->nentry; e++) {
        struct emu_tmpl_entry *en = &t->entries[e];
        const uint8_t *src = base + en->origOffset;
        uint8_t *dst = nd + en->newOffset;
        if (en->isInline) {                       // raw byte block, count == bytes
            memcpy(dst, src, en->descriptorCount);
            continue;
        }
        for (uint32_t j = 0; j < en->descriptorCount; j++) {
            const uint8_t *sj = src + (size_t)j * en->origStride;
            uint8_t *dj = dst + (size_t)j * en->newStride;
            if (en->isTexel) {
                VkBufferView bv;
                memcpy(&bv, sj, sizeof bv);
                int mi = klvk_bvmap_find(bv);
                VkDescriptorImageInfo ii;
                ii.sampler     = VK_NULL_HANDLE;
                ii.imageView   = (mi >= 0) ? g_mirrors[mi].view : VK_NULL_HANDLE;
                ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                memcpy(dj, &ii, sizeof ii);
                if (mi >= 0) {
                    subst++;
                    // Set-path: remember (set,binding,elem)->mirror so the copy is
                    // recorded at the following vkCmdBindDescriptorSets. Push-path:
                    // hand the mirror indices back so the caller records the copy on
                    // its command buffer right here (no persistent set to bind).
                    if (setKey)
                        klvk_dset_put(setKey, en->dstBinding, en->dstArrayElement + j, mi);
                    else if (mirrors && nmir && *nmir < maxmir)
                        mirrors[(*nmir)++] = mi;
                }
            } else {
                memcpy(dj, sj, en->elemSize);
                // PROBE (olar gray hunt): if this entry is an image sampler, tally
                // whether it binds a real texture or a 1x1 default (see
                // klvk_probe_img_binding). Templates are a bind path olar uses.
                if (en->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                    en->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                    VkDescriptorImageInfo di;
                    memcpy(&di, sj, sizeof di);
                    klvk_probe_img_binding(di.imageView);
                }
            }
        }
    }
    if (nsubst) *nsubst = subst;
    return nd;
}

// vkCreateDescriptorUpdateTemplate(+KHR) — rewrite UNIFORM_TEXEL_BUFFER entries
// to SAMPLED_IMAGE, rebuild a tightly-packed data layout (image info is 24 bytes
// vs a bufferview's 8), create the REAL template from the rewritten entries, and
// record the original<->new layout for the update path. Exact passthrough when
// emu off, when no entry is a texel buffer, or when an entry type we cannot
// repack shares the template with a texel entry (logged; that template stays on
// the broken path rather than corrupting pData).
static VkResult VKAPI_CALL klvk_CreateDescriptorUpdateTemplate(VkDevice dev,
        const VkDescriptorUpdateTemplateCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkDescriptorUpdateTemplate *out) {
    static PFN_vkCreateDescriptorUpdateTemplate real;
    if (!real && real_gdpa) {
        real = (PFN_vkCreateDescriptorUpdateTemplate)real_gdpa(dev, "vkCreateDescriptorUpdateTemplate");
        if (!real) real = (PFN_vkCreateDescriptorUpdateTemplate)real_gdpa(dev, "vkCreateDescriptorUpdateTemplateKHR");
    }
    if (ci && ci->pDescriptorUpdateEntries && !emu_on()) {
        // Note where an input attachment sits in a template's data, so that the
        // updates through it can be read and corrected (klvk_input_att_fix).
        int has = 0;
        for (uint32_t i = 0; i < ci->descriptorUpdateEntryCount; i++)
            if (ci->pDescriptorUpdateEntries[i].descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) has = 1;
        if (has) {
            VkResult tr = real ? real(dev, ci, alloc, out) : VK_ERROR_INITIALIZATION_FAILED;
            for (uint32_t i = 0; tr == VK_SUCCESS && out && i < ci->descriptorUpdateEntryCount; i++)
                if (ci->pDescriptorUpdateEntries[i].descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
                    fprintf(stderr, "  [vk] a descriptor update template carries an input attachment "
                                    "(binding %u, type %d: 0 set, 1 push)\n",
                            ci->pDescriptorUpdateEntries[i].dstBinding, (int)ci->templateType);
                    g_ia_tmpl = *out; g_ia_tmpl_off = ci->pDescriptorUpdateEntries[i].offset;
                    for (int k = 0; k < KLVK_IA_TMPLS; k++)
                        if (!g_ia_tmpls[k].t) {
                            g_ia_tmpls[k].off = ci->pDescriptorUpdateEntries[i].offset;
                            g_ia_tmpls[k].t = *out;
                            break;
                        }
                }
            return tr;
        }
    }
    if (!emu_on() || !ci || !ci->pDescriptorUpdateEntries ||
        ci->descriptorUpdateEntryCount == 0 || !real)
        return real ? real(dev, ci, alloc, out) : VK_ERROR_INITIALIZATION_FAILED;

    uint32_t n = ci->descriptorUpdateEntryCount;
    int anyTexel = 0;
    for (uint32_t i = 0; i < n; i++)
        if (ci->pDescriptorUpdateEntries[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) { anyTexel = 1; break; }
    if (!anyTexel || n > KLVK_EMU_TMPL_ENTRIES)
        return real(dev, ci, alloc, out);

    VkDescriptorUpdateTemplateEntry *ne =
        (VkDescriptorUpdateTemplateEntry *)malloc(n * sizeof *ne);
    if (!ne) return real(dev, ci, alloc, out);

    // Build the rewritten entries + rebuilt layout; bail (passthrough) if any
    // non-texel entry uses a type we cannot size.
    struct emu_tmpl rec;
    memset(&rec, 0, sizeof rec);
    size_t running = 0;
    int cannot = 0;
    for (uint32_t i = 0; i < n; i++) {
        const VkDescriptorUpdateTemplateEntry *oe = &ci->pDescriptorUpdateEntries[i];
        struct emu_tmpl_entry *re = &rec.entries[i];
        running = (running + 7u) & ~(size_t)7u;   // keep each element 8-byte aligned
                                                  // (VkDescriptor*Info hold pointers),
                                                  // even after a raw inline block
        re->type = oe->descriptorType;
        re->dstBinding = oe->dstBinding;
        re->dstArrayElement = oe->dstArrayElement;
        re->descriptorCount = oe->descriptorCount;
        re->origOffset = oe->offset;
        re->origStride = oe->stride;
        re->isTexel = (oe->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
        re->isInline = (oe->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK);

        ne[i] = *oe;
        if (re->isInline) {
            re->elemSize = 0; re->newStride = 0;
            re->newOffset = running;
            ne[i].offset = running;
            running += oe->descriptorCount;               // count is a byte count
            continue;
        }
        uint32_t esz = re->isTexel ? (uint32_t)sizeof(VkDescriptorImageInfo)
                                   : klvk_descr_elem_size(oe->descriptorType);
        if (esz == 0) { cannot = 1; break; }
        re->elemSize  = esz;
        re->newStride = esz;
        re->newOffset = running;
        if (re->isTexel) ne[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        ne[i].offset = running;
        ne[i].stride = esz;
        running += (size_t)oe->descriptorCount * esz;
    }
    if (cannot) {
        static int said; if (said < 8) { said++;
            VKI("[emu] descriptor update template has a texel entry beside an "
                "unrepackable type — left on passthrough (that template's texel "
                "buffers keep the MoltenVK bug)\n"); }
        free(ne);
        return real(dev, ci, alloc, out);
    }
    rec.nentry = (int)n;
    rec.dataSize = running;

    VkDescriptorUpdateTemplateCreateInfo m = *ci;
    m.pDescriptorUpdateEntries = ne;
    VkResult r = real(dev, &m, alloc, out);
    free(ne);
    if (r != VK_SUCCESS || !out || !*out) return r;

    pthread_mutex_lock(&g_lock);
    rec.handle = *out;
    struct emu_tmpl *slot = klvk_tmpl_find(*out);   // overwrite a reused handle
    if (!slot && g_ntmpl < KLVK_EMU_TMPL_MAX) slot = &g_tmpls[g_ntmpl++];
    if (slot) *slot = rec;
    else {
        static int said; if (said < 8) { said++;
            VKI("[emu] descriptor-update-template table full (%d) — this template's "
                "texel buffers stay unbound (gray)\n", KLVK_EMU_TMPL_MAX); }
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

// vkUpdateDescriptorSetWithTemplate(+KHR) — transform the guest pData into the
// rewritten template's layout (texel bufferviews -> mirror image infos) and call
// through. Exact passthrough when emu off or the template was not rewritten.
static void VKAPI_CALL klvk_UpdateDescriptorSetWithTemplate(VkDevice dev,
        VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl, const void *pData) {
    static PFN_vkUpdateDescriptorSetWithTemplate real;
    if (!real && real_gdpa) {
        real = (PFN_vkUpdateDescriptorSetWithTemplate)real_gdpa(dev, "vkUpdateDescriptorSetWithTemplate");
        if (!real) real = (PFN_vkUpdateDescriptorSetWithTemplate)real_gdpa(dev, "vkUpdateDescriptorSetWithTemplateKHR");
    }
    if (!real) return;
    klvk_trace_input_att_tmpl("a template", tmpl, pData);
    KFE(KFE_TMPL);
    klvk_ev(" U:%llx", (unsigned long long)(uintptr_t)set & 0xffffff, 0);
    void *nd = NULL;
    unsigned nsubst = 0;
    if (emu_on() && pData) {
        pthread_mutex_lock(&g_lock);
        struct emu_tmpl *t = klvk_tmpl_find(tmpl);
        // Set path: transform substitutes image infos and records set->mirror; the
        // GPU copy fires at the following vkCmdBindDescriptorSets (no cb here).
        if (t) nd = klvk_tmpl_transform(t, pData, (uint64_t)(uintptr_t)set, &nsubst,
                                        NULL, NULL, 0);
        pthread_mutex_unlock(&g_lock);
    }
    if (nd) {
        static unsigned cum, calls; static int said;
        cum += nsubst; calls++;
        if (nsubst && said < 12) { said++;
            VKI("[emu] template(set): %u texel writes -> mirror image (cum %u over "
                "%u update-with-template calls)\n", nsubst, cum, calls); }
        real(dev, set, tmpl, nd); free(nd);
    } else {
        VkImageView *where, was = klvk_input_att_fix(tmpl, pData, &where);
        real(dev, set, tmpl, pData);
        if (where) *where = was;            // the guest's data is the guest's again
    }
}

// vkCmdPushDescriptorSetKHR — same VkBufferView -> mirror-image substitution as
// klvk_UpdateDescriptorSets, refreshing each mirror (a push is a bind point;
// there is no persistent set handle, so no dset table entry — the refresh here
// covers the draws that follow in this command buffer). Passthrough when emu off
// or no write targets a texel buffer.
static void VKAPI_CALL klvk_CmdPushDescriptorSetKHR(VkCommandBuffer cb,
        VkPipelineBindPoint bindPoint, VkPipelineLayout layout, uint32_t set,
        uint32_t writeCount, const VkWriteDescriptorSet *writes) {
    static PFN_vkCmdPushDescriptorSetKHR real;
    if (!real && real_gdpa) real = (PFN_vkCmdPushDescriptorSetKHR)
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdPushDescriptorSetKHR");
    if (!real) return;
    klvk_trace_input_att("vkCmdPushDescriptorSetKHR", writeCount, writes);
    if (!emu_on() || !writes || writeCount == 0) { real(cb, bindPoint, layout, set, writeCount, writes); return; }
    uint32_t total = 0; int any = 0;
    for (uint32_t i = 0; i < writeCount; i++)
        if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
            any = 1; total += writes[i].descriptorCount;
        }
    if (!any) { real(cb, bindPoint, layout, set, writeCount, writes); return; }

    VkWriteDescriptorSet *nw = (VkWriteDescriptorSet *)malloc(writeCount * sizeof *nw);
    VkDescriptorImageInfo *imgs = total ? (VkDescriptorImageInfo *)malloc(total * sizeof *imgs) : NULL;
    if (!nw || (total && !imgs)) {
        free(nw); free(imgs);
        real(cb, bindPoint, layout, set, writeCount, writes);
        return;
    }
    memcpy(nw, writes, writeCount * sizeof *nw);
    uint32_t ii = 0; unsigned nsubst = 0;
    int seen[KLVK_BIND_SEEN]; int nseen = 0;
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < writeCount; i++) {
        if (writes[i].descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
            !writes[i].pTexelBufferView)
            continue;
        VkDescriptorImageInfo *slot = &imgs[ii];
        for (uint32_t j = 0; j < writes[i].descriptorCount; j++) {
            int mi = klvk_bvmap_find(writes[i].pTexelBufferView[j]);
            slot[j].sampler     = VK_NULL_HANDLE;
            slot[j].imageView   = (mi >= 0) ? g_mirrors[mi].view : VK_NULL_HANDLE;
            slot[j].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (mi >= 0) {                            // record the fill on this cb now
                nsubst++;
                int dup = 0; for (int s = 0; s < nseen; s++) if (seen[s] == mi) { dup = 1; break; }
                if (!dup) { if (nseen < KLVK_BIND_SEEN) seen[nseen++] = mi;
                            klvk_mirror_gpu_copy(cb, &g_mirrors[mi]); }
            }
        }
        nw[i].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        nw[i].pImageInfo       = slot;
        nw[i].pBufferInfo      = NULL;
        nw[i].pTexelBufferView = NULL;
        ii += writes[i].descriptorCount;
    }
    pthread_mutex_unlock(&g_lock);
    {
        static unsigned cum, calls; static int said;
        cum += nsubst; calls++;
        if (nsubst && said < 12) { said++;
            VKI("[emu] push: %u texel writes -> mirror image (cum %u over %u "
                "push calls)\n", nsubst, cum, calls); }
    }
    real(cb, bindPoint, layout, set, writeCount, nw);
    free(nw); free(imgs);
}

// vkCmdPushDescriptorSetWithTemplateKHR — the template transform (1)+(2) applied
// at a command-buffer push. Passthrough when emu off or the template was not
// rewritten.
static void VKAPI_CALL klvk_CmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer cb,
        VkDescriptorUpdateTemplate tmpl, VkPipelineLayout layout, uint32_t set,
        const void *pData) {
    static PFN_vkCmdPushDescriptorSetWithTemplateKHR real;
    if (!real && real_gdpa) real = (PFN_vkCmdPushDescriptorSetWithTemplateKHR)
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdPushDescriptorSetWithTemplateKHR");
    if (!real) return;
    klvk_trace_input_att_tmpl("a template", tmpl, pData);
    void *nd = NULL;
    unsigned nsubst = 0;
    if (emu_on() && pData) {
        int mirrors[KLVK_BIND_SEEN]; int nmir = 0;
        pthread_mutex_lock(&g_lock);
        struct emu_tmpl *t = klvk_tmpl_find(tmpl);
        // Push path: no persistent set to bind, so record the GPU copy for every
        // substituted mirror onto THIS command buffer right here (deduped).
        if (t) {
            nd = klvk_tmpl_transform(t, pData, 0, &nsubst, mirrors, &nmir, KLVK_BIND_SEEN);
            if (nd) {
                int seen[KLVK_BIND_SEEN]; int nseen = 0;
                for (int k = 0; k < nmir; k++) {
                    int mi = mirrors[k], dup = 0;
                    for (int s = 0; s < nseen; s++) if (seen[s] == mi) { dup = 1; break; }
                    if (dup) continue;
                    if (nseen < KLVK_BIND_SEEN) seen[nseen++] = mi;
                    if (mi >= 0 && mi < g_nmirror) klvk_mirror_gpu_copy(cb, &g_mirrors[mi]);
                }
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
    if (nd) {
        static unsigned cum, calls; static int said;
        cum += nsubst; calls++;
        if (nsubst && said < 12) { said++;
            VKI("[emu] template(push): %u texel writes -> mirror image (cum %u over "
                "%u push-with-template calls)\n", nsubst, cum, calls); }
        real(cb, tmpl, layout, set, nd); free(nd);
    } else {
        VkImageView *where, was = klvk_input_att_fix(tmpl, pData, &where);
        real(cb, tmpl, layout, set, pData);
        if (where) *where = was;
    }
}

// DIAGNOSTIC ONLY — vkCmdBindDescriptorBuffersEXT. VK_EXT_descriptor_buffer is a
// THIRD descriptor bind path (buffers holding descriptor data at GPU addresses,
// consumed via vkCmdBindDescriptorBuffersEXT + vkCmdSetDescriptorBufferOffsetsEXT
// instead of VkDescriptorSet / templates / push). The mirror does NOT cover it.
// If olar binds this way, none of the substitution paths above fire and the scene
// stays gray no matter what. This wrapper logs the first calls (so a run tells us
// whether the extension is merely RESOLVED or actually CALLED) and forwards to the
// real MoltenVK entry when present — so it is a faithful passthrough that only adds
// a log, and never shadows a working implementation. If this fires for olar, the
// remaining work is a descriptor_buffer-specific substitution (out of scope here).
static void VKAPI_CALL klvk_CmdBindDescriptorBuffersEXT(VkCommandBuffer cb,
        uint32_t bufferCount, const VkDescriptorBufferBindingInfoEXT *pBindingInfos) {
    static PFN_vkCmdBindDescriptorBuffersEXT real;
    static int resolved;
    if (!resolved) { resolved = 1;
        if (real_gdpa) real = (PFN_vkCmdBindDescriptorBuffersEXT)
            real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdBindDescriptorBuffersEXT"); }
    if (emu_on()) {
        static int said; if (said < 8) { said++;
            VKI("[emu] vkCmdBindDescriptorBuffersEXT CALLED (bufferCount %u, real %s) "
                "— olar is using VK_EXT_descriptor_buffer, a THIRD bind path the 2D "
                "mirror does NOT cover. This is the prime suspect if still gray.\n",
                bufferCount, real ? "present" : "ABSENT/no-op"); }
    }
    if (real) real(cb, bufferCount, pBindingInfos);
}

static VkResult VKAPI_CALL klvk_CreateBufferView(VkDevice dev,
        const VkBufferViewCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkBufferView *out) {
    static PFN_vkCreateBufferView real;
    if (!real && real_gdpa)
        real = (PFN_vkCreateBufferView)real_gdpa(dev, "vkCreateBufferView");
    // The returned handle is ALWAYS a real MoltenVK view (a valid, destroyable
    // handle the guest may store and later vkDestroyBufferView). For olar/wanderer
    // (emu_on) we additionally build the 2D mirror for this window and remember
    // bv -> mirror, so vkUpdateDescriptorSets can substitute a SAMPLED_IMAGE write
    // pointing at the mirror. The real view is never actually sampled — the shader
    // and layout are rewritten to read the mirror — but creating it keeps handle
    // lifetime exactly as the guest expects. When emu_on() is false — every other
    // title — none of the mirror path runs and this is the EXACT faithful
    // passthrough it always was.
    VkResult r = real ? real(dev, ci, alloc, out)
                      : VK_ERROR_INITIALIZATION_FAILED;
    if (emu_on() && r == VK_SUCCESS && ci && out && *out) {
        pthread_mutex_lock(&g_lock);
        int mi = klvk_mirror_get(dev, ci->buffer, ci->offset, ci->range, ci->format);
        if (mi >= 0) klvk_bvmap_put(*out, mi);
        static int okn, fbn;
        if (mi >= 0) okn++; else fbn++;
        static int said;
        if ((okn + fbn) <= 4 || ((okn + fbn) % 128) == 0) {
            if (said < 40) { said++;
                VKI("[emu] texel-buffer views: %d mirrored to 2D image, "
                    "%d unmirrored (left as passthrough texel buffer)\n", okn, fbn); }
        }
        pthread_mutex_unlock(&g_lock);
    }
    static int dump = -1;
    if (dump < 0) dump = kl_env_on("KL_VK_DUMP_TEXELBUF", 0);
    if (dump && ci) {
        static int logged;
        if (logged < 256) { logged++;
            VKI("vkCreateBufferView: -> %#llx result %d buffer %#llx format %d "
                "offset %llu range %llu (texel buffer; a MoltenVK misread here "
                "reads as zero -> collapsed/INVISIBLE geometry or BLACK params)\n",
                (unsigned long long)(uintptr_t)(out ? *out : 0), (int)r,
                (unsigned long long)(uintptr_t)ci->buffer, (int)ci->format,
                (unsigned long long)ci->offset, (unsigned long long)ci->range);
        }
    }
    if (probe_on() && r == VK_SUCCESS && ci && out) {
        pthread_mutex_lock(&g_lock);
        if (g_probe_ntbv < KLVK_PROBE_TBVS) {
            struct probe_tbv *e = &g_probe_tbvs[g_probe_ntbv++];
            e->buf = ci->buffer; e->off = ci->offset;
            e->range = ci->range; e->fmt = ci->format;
        }
        pthread_mutex_unlock(&g_lock);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Recording wrappers: the function above, then the pipeline hooks with the
// guest's own create info (kl_vk_pipe_hooks.h). These are what the guest and a
// replayer are handed; a replayer's own creates come through here too and are
// not recorded.
// ---------------------------------------------------------------------------
// KL_VK_ANISO=<n> (2..16, 0 = off): a guest's linearly filtered samplers get at least n-fold anisotropic
// filtering. A game built for a Quest asks for what a Quest can afford: Moss runs with r.MaxAnisotropy 1 or 2
// on the device profile a Quest 2 gets (its own config sets 8 only for the Quest 1's chip), and its
// table-top scenes are seen at a shallow angle – the surfaces blur a short way from the eye, on a Quest 2 as
// here. Compare and unnormalized samplers are left alone. The census line says what the guest asked for.
static int klvk_aniso_floor(void) {
    static int v = -1;
    if (v < 0) {
        // 16 for Moss (and Moss: Book II): the owner's setting on the headset (2026-10-13). KL_VK_ANISO=0 is off.
        v = kl_env_int("KL_VK_ANISO", kl_driver_target_is_moss() ? 16 : 0);
        if (v < 2) v = 0;
        if (v > 16) v = 16;
    }
    return v;
}
static VkResult VKAPI_CALL klvkrec_CreateSampler(VkDevice dev, const VkSamplerCreateInfo *ci,
        const VkAllocationCallbacks *a, VkSampler *out) {
    static PFN_vkCreateSampler real;
    if (!real && real_gdpa) real = (PFN_vkCreateSampler)real_gdpa(dev, "vkCreateSampler");
    const VkSamplerCreateInfo *guest = ci;
    VkSamplerCreateInfo mine;
    if (ci) {
        static unsigned n, by[4], raised;                 // asked: none, up to 2, up to 8, more
        float asked = ci->anisotropyEnable ? ci->maxAnisotropy : 1.0f;
        int floor = klvk_aniso_floor();
        by[asked <= 1.0f ? 0 : asked <= 2.0f ? 1 : asked <= 8.0f ? 2 : 3]++;
        if (floor && ci->magFilter == VK_FILTER_LINEAR && ci->minFilter == VK_FILTER_LINEAR &&
            !ci->compareEnable && !ci->unnormalizedCoordinates && asked < (float)floor) {
            mine = *ci;
            mine.anisotropyEnable = VK_TRUE;
            mine.maxAnisotropy = (float)floor;
            ci = &mine;
            raised++;
        }
        n++;
        if (n == 8 || n == 64 || n == 512)
            fprintf(stderr, "  [vk] samplers so far: %u; anisotropy asked: none %u, up to 2x %u, up to 8x %u, more %u; "
                            "raised to %dx: %u (KL_VK_ANISO)\n", n, by[0], by[1], by[2], by[3], floor, raised);
    }
    VkResult r = real ? real(dev, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && guest && out) PIPE_HOOK(sampler, *out, guest);
    return r;
}
static VkResult VKAPI_CALL klvkrec_CreatePipelineLayout(VkDevice dev, const VkPipelineLayoutCreateInfo *ci,
        const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    static PFN_vkCreatePipelineLayout real;
    if (!real && real_gdpa) real = (PFN_vkCreatePipelineLayout)real_gdpa(dev, "vkCreatePipelineLayout");
    VkResult r = real ? real(dev, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && ci && out) PIPE_HOOK(pipeline_layout, *out, ci);
    return r;
}
static VkResult VKAPI_CALL klvkrec_CreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo *ci,
        const VkAllocationCallbacks *a, VkShaderModule *out) {
    VkResult r = klvk_CreateShaderModule(dev, ci, a, out);
    if (r == VK_SUCCESS && ci && out) PIPE_HOOK(shader_module, *out, ci);
    return r;
}
static VkResult VKAPI_CALL klvkrec_CreateDescriptorSetLayout(VkDevice dev, const VkDescriptorSetLayoutCreateInfo *ci,
        const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    VkResult r = klvk_CreateDescriptorSetLayout(dev, ci, a, out);
    if (r == VK_SUCCESS && ci && out) PIPE_HOOK(set_layout, *out, ci);
    return r;
}
static VkResult VKAPI_CALL klvkrec_CreateRenderPass(VkDevice dev, const VkRenderPassCreateInfo *ci,
        const VkAllocationCallbacks *a, VkRenderPass *out) {
    VkResult r = klvk_CreateRenderPass(dev, ci, a, out);
    if (r == VK_SUCCESS && ci && out) PIPE_HOOK(render_pass, *out, ci);
    return r;
}
static VkResult VKAPI_CALL klvkrec_CreateRenderPass2(VkDevice dev, const VkRenderPassCreateInfo2 *ci,
        const VkAllocationCallbacks *a, VkRenderPass *out) {
    VkResult r = klvk_CreateRenderPass2(dev, ci, a, out);
    if (r == VK_SUCCESS && ci && out) PIPE_HOOK(render_pass2, *out, ci);
    return r;
}
static VkResult VKAPI_CALL klvkrec_CreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
        const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    VkResult r = klvk_CreateGraphicsPipelines(dev, cache, n, ci, a, out);
    if (ci && out) for (uint32_t i = 0; i < n; i++) if (out[i]) PIPE_HOOK(graphics_pipeline, out[i], &ci[i]);
    return r;
}
static VkResult VKAPI_CALL klvkrec_CreateComputePipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
        const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    VkResult r = klvk_CreateComputePipelines(dev, cache, n, ci, a, out);
    if (ci && out) for (uint32_t i = 0; i < n; i++) if (out[i]) PIPE_HOOK(compute_pipeline, out[i], &ci[i]);
    return r;
}

// ---- fixed foveation (kl_fove.m, docs/FOVEATION.md) -----------------------
//
// kl_fove does the Metal side: it maps the encoders that draw a recognised eye
// target, patches the guest's fragment shaders and expands the result. What it
// cannot see from Metal is WHICH targets are eyes; that is plain here, in the
// guest's Vulkan calls. A guest renders its eyes with vkCmdBeginRenderPass
// into the images the runtime handed it (kl_ovrp's eye layer, or an OpenXR
// swapchain from kl_vulkan_xr_image) — usually one 2-layer image, both eyes as
// two slices (multiview). Three shapes are recognised at render-pass begin:
//
//   direct        the pass's colour attachment 0 IS a runtime eye image;
//   MSAA + resolve an eye-size MSAA colour whose subpass-0 resolve attachment
//                 is a runtime eye image;
//   intermediate  the guest's OWN eye-size scene target that reaches an eye
//                 later: written by a later subpass of the same pass, or
//                 confirmed by a copy/blit/resolve into the eye, or by the
//                 next pass that draws into the eye.
//
// Each needs a colour and a depth attachment of the same extent and sample
// count. Their MTLTextures go to kl_fove (kl_fove_note_eye_array /
// kl_fove_note_eye_scene). Every new pass shape, eye pass or near miss, is
// logged once with its attachment table.
//
// Off (the default): kl_fove_on() is 0, none of the fove-only entry points is
// handed out, and every note below returns at once — the tables stay empty.

// One hash for every handle-keyed table here: handles are pointers (or
// pointer-like), so the low bits alone cluster.
static unsigned ptr_slot(uint64_t h, unsigned n) {
    h ^= h >> 17; h *= 0x9e3779b97f4a7c15ull; h ^= h >> 29;
    return (unsigned)(h % n);
}

// Open-addressed handle tables with tombstones: keys[] holds the handle, 0 =
// never used, KLVK_FH_TOMB = freed. Returns the slot (made when `make`), or -1.
#define KLVK_FH_TOMB 1ull
static int fh_slot(uint64_t *keys, unsigned n, uint64_t k, int make) {
    if (!k || k == KLVK_FH_TOMB) return -1;
    unsigned i = ptr_slot(k, n);
    int tomb = -1;
    for (unsigned c = 0; c < n; c++, i = (i + 1) % n) {
        if (keys[i] == k) return (int)i;
        if (keys[i] == KLVK_FH_TOMB) { if (tomb < 0) tomb = (int)i; continue; }
        if (!keys[i]) {
            if (!make) return -1;
            if (tomb >= 0) i = (unsigned)tomb;
            keys[i] = k;
            return (int)i;
        }
    }
    if (make && tomb >= 0) { keys[tomb] = k; return tomb; }
    return -1;
}
static void fh_drop(uint64_t *keys, unsigned n, uint64_t k) {
    int i = fh_slot(keys, n, k, 0);
    if (i >= 0) keys[i] = KLVK_FH_TOMB;
}

// The guest's images (vkCreateImage): extent, samples, layers, format — what
// the eye-pass check compares. The runtime's own eye images are not here (they
// are allocated by klvk_image_alloc); their extent comes from the eye tables.
#define KLVK_GIMG_N 8192
typedef struct { uint32_t w, h, samples, layers, fmt; } klvk_gimg;
static uint64_t  g_gimg_key[KLVK_GIMG_N];
static klvk_gimg g_gimg[KLVK_GIMG_N];
static pthread_mutex_t g_gimg_lock = PTHREAD_MUTEX_INITIALIZER;
static klvk_gimg *gimg_find_locked(VkImage img) {
    int i = fh_slot(g_gimg_key, KLVK_GIMG_N, (uint64_t)(uintptr_t)img, 0);
    return i < 0 ? NULL : &g_gimg[i];
}
static void fove_note_image(VkImage img, const VkImageCreateInfo *ci) {
    if (!img || !ci || !kl_fove_on()) return;
    pthread_mutex_lock(&g_gimg_lock);
    int i = fh_slot(g_gimg_key, KLVK_GIMG_N, (uint64_t)(uintptr_t)img, 1);
    if (i >= 0)
        g_gimg[i] = (klvk_gimg){ ci->extent.width, ci->extent.height, (uint32_t)ci->samples,
                                 ci->arrayLayers, (uint32_t)ci->format };
    pthread_mutex_unlock(&g_gimg_lock);
}

// The guest's image views: which image, which array layers.
#define KLVK_GVIEW_N 16384
typedef struct { VkImage img; uint32_t base, count; } klvk_gview;
static uint64_t   g_gview_key[KLVK_GVIEW_N];
static klvk_gview g_gview[KLVK_GVIEW_N];
static pthread_mutex_t g_gview_lock = PTHREAD_MUTEX_INITIALIZER;
static void fove_note_view(VkImageView v, const VkImageViewCreateInfo *ci) {
    if (!v || !ci || !kl_fove_on()) return;
    pthread_mutex_lock(&g_gview_lock);
    int i = fh_slot(g_gview_key, KLVK_GVIEW_N, (uint64_t)(uintptr_t)v, 1);
    if (i >= 0) {
        uint32_t n = ci->subresourceRange.layerCount;
        g_gview[i] = (klvk_gview){ ci->image, ci->subresourceRange.baseArrayLayer,
                                   n == VK_REMAINING_ARRAY_LAYERS ? 0 : n };   // 0 = to the end
    }
    pthread_mutex_unlock(&g_gview_lock);
}
static void fove_forget_view(VkImageView v) {
    if (!v || !kl_fove_on()) return;
    pthread_mutex_lock(&g_gview_lock);
    fh_drop(g_gview_key, KLVK_GVIEW_N, (uint64_t)(uintptr_t)v);
    pthread_mutex_unlock(&g_gview_lock);
}
// 0 known (and *img set), 1 unknown. NULL is "known" with no image.
static int fove_view_state(VkImageView v, VkImage *img) {
    if (img) *img = VK_NULL_HANDLE;
    if (!v) return 0;
    pthread_mutex_lock(&g_gview_lock);
    int i = fh_slot(g_gview_key, KLVK_GVIEW_N, (uint64_t)(uintptr_t)v, 0);
    if (i >= 0 && img) *img = g_gview[i].img;
    pthread_mutex_unlock(&g_gview_lock);
    return i < 0;
}
static void fove_view_range(VkImageView v, uint32_t *base, uint32_t *count) {
    *base = 0; *count = 1;
    pthread_mutex_lock(&g_gview_lock);
    int i = fh_slot(g_gview_key, KLVK_GVIEW_N, (uint64_t)(uintptr_t)v, 0);
    if (i >= 0) { *base = g_gview[i].base; *count = g_gview[i].count; }   // count 0 = to the end
    pthread_mutex_unlock(&g_gview_lock);
}

// Framebuffers: the attachment views (none for an imageless framebuffer, whose
// views arrive with the begin) and the extent MoltenVK compares the render
// area with.
#define KLVK_FFB_N 4096
typedef struct { uint32_t nv; VkImageView v[16]; uint32_t w, h; } klvk_fbh;
static uint64_t g_ffb_key[KLVK_FFB_N];
static klvk_fbh g_ffb[KLVK_FFB_N];
static pthread_mutex_t g_ffb_lock = PTHREAD_MUTEX_INITIALIZER;
static klvk_fbh *ffb_get_locked(VkFramebuffer fb, int make) {
    int i = fh_slot(g_ffb_key, KLVK_FFB_N, (uint64_t)(uintptr_t)fb, make);
    return i < 0 ? NULL : &g_ffb[i];
}
static void fove_note_fb(VkFramebuffer fb, const VkFramebufferCreateInfo *ci) {
    if (!fb || !ci || !kl_fove_on()) return;
    int views = !(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && ci->pAttachments;
    pthread_mutex_lock(&g_ffb_lock);
    klvk_fbh *f = ffb_get_locked(fb, 1);
    if (f) {
        f->nv = views ? (ci->attachmentCount < 16 ? ci->attachmentCount : 16) : 0;
        if (f->nv) memcpy(f->v, ci->pAttachments, f->nv * sizeof *f->v);
        f->w = ci->width; f->h = ci->height;
    }
    pthread_mutex_unlock(&g_ffb_lock);
}
static void fove_forget_fb(VkFramebuffer fb) {
    if (!fb || !kl_fove_on()) return;
    pthread_mutex_lock(&g_ffb_lock);
    fh_drop(g_ffb_key, KLVK_FFB_N, (uint64_t)(uintptr_t)fb);
    pthread_mutex_unlock(&g_ffb_lock);
}

// VkImage -> MTLTexture for the images handed to kl_fove (the export is not
// free, and kl_fove keys its tables on the texture).
#define KLVK_FOVE_N 32
static struct { VkImage img; void *tex; } g_fove_img[KLVK_FOVE_N];
static pthread_mutex_t g_fove_lock = PTHREAD_MUTEX_INITIALIZER;

static void *fove_tex_locked(VkImage img) {
    if (!img) return NULL;
    for (int i = 0; i < KLVK_FOVE_N; i++) if (g_fove_img[i].img == img) return g_fove_img[i].tex;
    void *t = eye_mtl_texture(g_devs[0], img);
    if (!t) return NULL;
    for (int i = 0; i < KLVK_FOVE_N; i++)
        if (!g_fove_img[i].img) { g_fove_img[i].img = img; g_fove_img[i].tex = t; return t; }
    g_fove_img[0].img = img; g_fove_img[0].tex = t;
    return t;
}

// The OpenXR swapchain colour images this file created (kl_vulkan_xr_image).
#define KLVK_XR_EYE_N 64
static struct { VkImage img; uint32_t layers, w, h; } g_xr_col[KLVK_XR_EYE_N];
static pthread_mutex_t g_xr_col_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_xr_col_n;            // live entries (destroy paths skip the lock at 0)
static void fove_xr_image(VkImage img, uint32_t layers, uint32_t w, uint32_t h) {
    if (!img || !kl_fove_on()) return;
    int put = 0;
    pthread_mutex_lock(&g_xr_col_lock);
    for (int i = 0; i < KLVK_XR_EYE_N; i++)
        if (!g_xr_col[i].img) {
            g_xr_col[i].img = img; g_xr_col[i].layers = layers ? layers : 1; g_xr_col[i].w = w; g_xr_col[i].h = h;
            __atomic_add_fetch(&g_xr_col_n, 1, __ATOMIC_RELAXED);
            put = 1;
            break;
        }
    pthread_mutex_unlock(&g_xr_col_lock);
    static int said;
    if (!put && !said++)
        VKI("[fove] OpenXR swapchain image table full (%d colour images live): image %p %ux%u is NOT "
            "recognised as an eye (drawn unfoveated)\n", KLVK_XR_EYE_N, (void *)img, w, h);
}

static void fove_xr_reg_drop_locked(VkImage img);
// An image going away (the guest's vkDestroyImage): every table drops it — a
// guest that recreates its targets must not fill them, a VkImage handle reused
// for an unrelated image must not be taken for an eye, and a confirmed
// intermediate is sticky until here.
static void fove_image_gone(VkImage img) {
    if (!img || !kl_fove_on()) return;
    void *t = NULL;
    pthread_mutex_lock(&g_fove_lock);
    for (int i = 0; i < KLVK_FOVE_N; i++)
        if (g_fove_img[i].img == img) { t = g_fove_img[i].tex; g_fove_img[i].img = VK_NULL_HANDLE; g_fove_img[i].tex = NULL; }
    pthread_mutex_unlock(&g_fove_lock);
    if (t) kl_fove_forget(t);
    kl_fove_inter_forget((uint64_t)(uintptr_t)img);
    if (__atomic_load_n(&g_xr_col_n, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&g_xr_col_lock);
        for (int i = 0; i < KLVK_XR_EYE_N; i++)
            if (g_xr_col[i].img == img) {
                memset(&g_xr_col[i], 0, sizeof g_xr_col[i]);
                __atomic_sub_fetch(&g_xr_col_n, 1, __ATOMIC_RELAXED);
            }
        fove_xr_reg_drop_locked(img);
        pthread_mutex_unlock(&g_xr_col_lock);
    }
    pthread_mutex_lock(&g_gimg_lock);
    fh_drop(g_gimg_key, KLVK_GIMG_N, (uint64_t)(uintptr_t)img);
    pthread_mutex_unlock(&g_gimg_lock);
}

// Is img an eye image the runtime handed the guest? *layers = its array size,
// *eye = 0/1 for a one-eye image (kl_ovrp Stereo layout), -1 for a pair or
// when the owner does not say (an OpenXR swapchain: eye 0 is assumed).
// *w/*h = its extent (0 when the owner does not record it).
static int fove_runtime_eye_wh(VkImage img, uint32_t *layers, int *eye, uint32_t *w, uint32_t *h) {
    if (!img) return 0;
    for (int s = 0; s < KLVK_EYE_STAGES; s++)
        for (int e = 0; e < 2; e++)
            if (g_eye[s][e].img == img) {
                *layers = g_eye[s][e].layers ? g_eye[s][e].layers : 1;
                *eye = *layers >= 2 ? -1 : e;
                if (w) *w = g_eye[s][e].w;
                if (h) *h = g_eye[s][e].h;
                return 1;
            }
    int hit = 0;
    pthread_mutex_lock(&g_xr_col_lock);
    for (int i = 0; i < KLVK_XR_EYE_N && !hit; i++)
        if (g_xr_col[i].img == img) {
            *layers = g_xr_col[i].layers; *eye = -1; hit = 1;
            if (w) *w = g_xr_col[i].w;
            if (h) *h = g_xr_col[i].h;
        }
    pthread_mutex_unlock(&g_xr_col_lock);
    return hit;
}
static int fove_runtime_eye(VkImage img, uint32_t *layers, int *eye) {
    return fove_runtime_eye_wh(img, layers, eye, NULL, NULL);
}

// A runtime image is taken for an eye from the moment it is made, because an
// OpenXR swapchain does not say what it is for: only xrEndFrame shows a
// projection layer or a flat one. HITMAN 3 VR: Reloaded draws its loading and
// menu screens into a 1920x1080 swapchain it submits as a QUAD layer; foveated
// around the eye's straight-ahead point, half of that panel was shaded at an
// eighth of the rate and its text could not be read (headset, 2026-10-20;
// Mac: "map 1920x1080 ... 47.5% of the fragments"). kl_openxr calls
// kl_vulkan_xr_image_not_eye for such a swapchain's images: it stops being an
// eye, and the eye targets a pass registered through it (its MSAA colour and
// depth, which kl_fove marks by themselves) are dropped. g_xr_reg remembers
// which those were.
#define KLVK_XR_REG_N 64
static struct { VkImage img; void *ct, *dt; } g_xr_reg[KLVK_XR_REG_N];
static void fove_xr_reg_note(VkImage img, void *ct, void *dt) {   // under nothing; g_xr_col_lock taken here
    if (!img) return;
    pthread_mutex_lock(&g_xr_col_lock);
    int free_i = -1;
    for (int i = 0; i < KLVK_XR_REG_N; i++) {
        if (g_xr_reg[i].img == img && g_xr_reg[i].ct == ct && g_xr_reg[i].dt == dt) { free_i = -2; break; }
        if (!g_xr_reg[i].img && free_i == -1) free_i = i;
    }
    if (free_i >= 0) { g_xr_reg[free_i].img = img; g_xr_reg[free_i].ct = ct; g_xr_reg[free_i].dt = dt; }
    pthread_mutex_unlock(&g_xr_col_lock);
}
static void fove_xr_reg_drop_locked(VkImage img) {        // under g_xr_col_lock
    for (int i = 0; i < KLVK_XR_REG_N; i++)
        if (g_xr_reg[i].img == img) memset(&g_xr_reg[i], 0, sizeof g_xr_reg[i]);
}
void kl_vulkan_xr_image_not_eye(unsigned long long image) {
    VkImage img = (VkImage)(uintptr_t)image;
    if (!img || !kl_fove_on()) return;
    void *drop[2 * KLVK_XR_REG_N];
    int nd = 0, was = 0;
    pthread_mutex_lock(&g_xr_col_lock);
    for (int i = 0; i < KLVK_XR_EYE_N; i++)
        if (g_xr_col[i].img == img) {
            memset(&g_xr_col[i], 0, sizeof g_xr_col[i]);
            __atomic_sub_fetch(&g_xr_col_n, 1, __ATOMIC_RELAXED);
            was = 1;
        }
    for (int i = 0; i < KLVK_XR_REG_N; i++)
        if (g_xr_reg[i].img == img) {
            if (g_xr_reg[i].ct) drop[nd++] = g_xr_reg[i].ct;
            if (g_xr_reg[i].dt) drop[nd++] = g_xr_reg[i].dt;
            memset(&g_xr_reg[i], 0, sizeof g_xr_reg[i]);
        }
    pthread_mutex_unlock(&g_xr_col_lock);
    for (int i = 0; i < nd; i++) kl_fove_forget(drop[i]);
    if (was)
        VKI("[fove] swapchain image %p is a flat layer's, not an eye: drawn unfoveated "
            "(%d eye target(s) registered through it dropped)\n", (void *)img, nd);
}

// Any kl_ovrp eye image of this extent? A target the size of a runtime eye
// image — or a few px larger, as Unreal rounds its scene target up (a
// 5088x3236 scene for a 5088x3234 eye, which an exact match left silently
// unfoveated). The map is then sized to the INTERMEDIATE and the expand runs
// on the intermediate itself, so the eye being the smaller of the two changes
// nothing downstream.
static int fove_eye_extent_known(uint32_t w, uint32_t h) {
    for (int s = 0; s < KLVK_EYE_STAGES; s++)
        for (int e = 0; e < 2; e++)
            if (g_eye[s][e].img && kl_fove_inter_extent_ok(g_eye[s][e].w, g_eye[s][e].h, w, h)) return 1;
    return 0;
}

// The guest's own eye-size scene target (an "intermediate"). The last
// candidate's command buffer, for the "sampled" confirmation line.
static VkCommandBuffer g_inter_last_cb;
static int fove_probe_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_FOVE_PROBE", 0);
    return on;
}

// The render passes' attachment structure, recorded at create time: which
// attachment is the colour, depth and resolve of subpass 0, what the later
// subpasses write, and the view masks.
#define KLVK_FRP_N 2048
typedef struct {
    VkRenderPass rp;
    uint8_t natt, nsub, ncol;
    int8_t col0, dep, res0;
    uint32_t viewmask;
    // intermediates: attachments any subpass writes as colour or resolve (bit per
    // attachment < 8; col0 excluded), and whether the subpasses' view masks differ
    uint8_t outs, vm_mixed;
    // transient MSAA intermediates: the attachment a LATER subpass (k >= 1)
    // resolves col0 into (col0 as its colour c, pResolveAttachments[c] set), or
    // -1. UE4's depth-read subpass: subpass 0 draws the MSAA scene, subpass 1
    // LOADs it, reads depth as an input and resolves into the single-sample
    // scene colour on its way out.
    int8_t res_late;
    struct { uint32_t fmt; uint8_t samples, load, store, sload; } a[8];
} klvk_frp;
static klvk_frp g_frp[KLVK_FRP_N];
static pthread_mutex_t g_frp_lock = PTHREAD_MUTEX_INITIALIZER;

static klvk_frp *frp_get_locked(VkRenderPass rp, int make) {
    unsigned i = ptr_slot((uint64_t)(uintptr_t)rp, KLVK_FRP_N);
    for (unsigned n = 0; n < KLVK_FRP_N; n++, i = (i + 1) % KLVK_FRP_N) {
        if (g_frp[i].rp == rp) return &g_frp[i];
        if (!g_frp[i].rp) {
            if (!make) return NULL;
            memset(&g_frp[i], 0, sizeof g_frp[i]);
            g_frp[i].rp = rp;
            return &g_frp[i];
        }
    }
    return NULL;
}

static int8_t frp_ref(uint32_t a) { return a < 8 ? (int8_t)a : (int8_t)-1; }   // VK_ATTACHMENT_UNUSED too

static void fove_rp_note1(const VkRenderPassCreateInfo *ci, VkRenderPass rp) {
    if (!ci || !rp || !kl_fove_on()) return;
    pthread_mutex_lock(&g_frp_lock);
    klvk_frp *r = frp_get_locked(rp, 1);
    if (r) {
        r->natt = (uint8_t)(ci->attachmentCount < 8 ? ci->attachmentCount : 8);
        for (unsigned i = 0; i < r->natt && ci->pAttachments; i++) {
            const VkAttachmentDescription *a = &ci->pAttachments[i];
            r->a[i].fmt = (uint32_t)a->format; r->a[i].samples = (uint8_t)a->samples;
            r->a[i].load = (uint8_t)a->loadOp; r->a[i].store = (uint8_t)a->storeOp;
            r->a[i].sload = (uint8_t)a->stencilLoadOp;
        }
        r->nsub = (uint8_t)(ci->subpassCount < 255 ? ci->subpassCount : 255);
        r->col0 = r->dep = r->res0 = -1; r->ncol = 0; r->viewmask = 0;
        r->outs = r->vm_mixed = 0;
        r->res_late = -1;
        if (ci->subpassCount && ci->pSubpasses) {
            const VkSubpassDescription *s = &ci->pSubpasses[0];
            r->ncol = (uint8_t)s->colorAttachmentCount;
            if (s->colorAttachmentCount && s->pColorAttachments) r->col0 = frp_ref(s->pColorAttachments[0].attachment);
            if (s->colorAttachmentCount && s->pResolveAttachments) r->res0 = frp_ref(s->pResolveAttachments[0].attachment);
            if (s->pDepthStencilAttachment) r->dep = frp_ref(s->pDepthStencilAttachment->attachment);
            for (uint32_t k = 0; k < ci->subpassCount; k++) {
                const VkSubpassDescription *t = &ci->pSubpasses[k];
                for (uint32_t c = 0; c < t->colorAttachmentCount; c++) {
                    int8_t a = t->pColorAttachments ? frp_ref(t->pColorAttachments[c].attachment) : -1;
                    int8_t z = t->pResolveAttachments ? frp_ref(t->pResolveAttachments[c].attachment) : -1;
                    if (a >= 0) r->outs |= (uint8_t)(1u << a);
                    if (z >= 0) r->outs |= (uint8_t)(1u << z);
                    if (k >= 1 && a >= 0 && a == r->col0 && z >= 0) r->res_late = z;
                }
            }
            if (r->col0 >= 0) r->outs &= (uint8_t)~(1u << r->col0);
        }
        for (const VkBaseInStructure *b = (const VkBaseInStructure *)ci->pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) {
                const VkRenderPassMultiviewCreateInfo *mv = (const VkRenderPassMultiviewCreateInfo *)b;
                if (mv->subpassCount && mv->pViewMasks) r->viewmask = mv->pViewMasks[0];
                for (uint32_t k = 1; mv->pViewMasks && k < mv->subpassCount; k++)
                    if (mv->pViewMasks[k] != r->viewmask) r->vm_mixed = 1;
            }
    }
    pthread_mutex_unlock(&g_frp_lock);
}

static void fove_rp_note2(const VkRenderPassCreateInfo2 *ci, VkRenderPass rp) {
    if (!ci || !rp || !kl_fove_on()) return;
    pthread_mutex_lock(&g_frp_lock);
    klvk_frp *r = frp_get_locked(rp, 1);
    if (r) {
        r->natt = (uint8_t)(ci->attachmentCount < 8 ? ci->attachmentCount : 8);
        for (unsigned i = 0; i < r->natt && ci->pAttachments; i++) {
            const VkAttachmentDescription2 *a = &ci->pAttachments[i];
            r->a[i].fmt = (uint32_t)a->format; r->a[i].samples = (uint8_t)a->samples;
            r->a[i].load = (uint8_t)a->loadOp; r->a[i].store = (uint8_t)a->storeOp;
            r->a[i].sload = (uint8_t)a->stencilLoadOp;
        }
        r->nsub = (uint8_t)(ci->subpassCount < 255 ? ci->subpassCount : 255);
        r->col0 = r->dep = r->res0 = -1; r->ncol = 0; r->viewmask = 0;
        r->outs = r->vm_mixed = 0;
        r->res_late = -1;
        if (ci->subpassCount && ci->pSubpasses) {
            const VkSubpassDescription2 *s = &ci->pSubpasses[0];
            r->ncol = (uint8_t)s->colorAttachmentCount;
            if (s->colorAttachmentCount && s->pColorAttachments) r->col0 = frp_ref(s->pColorAttachments[0].attachment);
            if (s->colorAttachmentCount && s->pResolveAttachments) r->res0 = frp_ref(s->pResolveAttachments[0].attachment);
            if (s->pDepthStencilAttachment) r->dep = frp_ref(s->pDepthStencilAttachment->attachment);
            r->viewmask = s->viewMask;
            for (uint32_t k = 0; k < ci->subpassCount; k++) {
                const VkSubpassDescription2 *t = &ci->pSubpasses[k];
                for (uint32_t c = 0; c < t->colorAttachmentCount; c++) {
                    int8_t a = t->pColorAttachments ? frp_ref(t->pColorAttachments[c].attachment) : -1;
                    int8_t z = t->pResolveAttachments ? frp_ref(t->pResolveAttachments[c].attachment) : -1;
                    if (a >= 0) r->outs |= (uint8_t)(1u << a);
                    if (z >= 0) r->outs |= (uint8_t)(1u << z);
                    if (k >= 1 && a >= 0 && a == r->col0 && z >= 0) r->res_late = z;
                }
                if (t->viewMask != r->viewmask) r->vm_mixed = 1;
            }
            if (r->col0 >= 0) r->outs &= (uint8_t)~(1u << r->col0);
        }
    }
    pthread_mutex_unlock(&g_frp_lock);
}

// Where an eye-size intermediate reaches the runtime's eye image (transfer
// census, fove only): the first transfer of each kind into one is named once,
// with its first region's shape (KL_FOVE_PROBE=1: the first 4 of each kind).
// A transfer from a CANDIDATE intermediate confirms it.
static void fove_xfer_note(const char *what, const char *via, VkImage src, VkImage dst, uint32_t n,
                           const VkImageSubresourceLayers *ss, const VkImageSubresourceLayers *ds,
                           int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w, uint32_t h) {
    uint32_t el = 0; int ee = -1;
    if (!kl_fove_on() || !fove_runtime_eye(dst, &el, &ee)) return;
    if (kl_fove_intermediate() && kl_fove_inter_confirm((uint64_t)(uintptr_t)src, KL_FOVE_VIA_XFER) == 1)
        VKI("[fove] intermediate CONFIRMED: VkImage %p -> eye image %p via %s (%s)\n", (void *)src, (void *)dst, via, what);
    static struct { const char *what; int n; } said[8]; static unsigned nsaid;
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    int k = -1, show = 0;
    pthread_mutex_lock(&lk);
    for (unsigned i = 0; i < nsaid; i++) if (said[i].what == what) k = (int)i;
    if (k < 0 && nsaid < 8) { k = (int)nsaid++; said[k].what = what; said[k].n = 0; }
    if (k >= 0 && said[k].n < (fove_probe_on() ? 4 : 1)) { said[k].n++; show = 1; }
    pthread_mutex_unlock(&lk);
    if (!show) return;
    uint32_t sw = 0, sh = 0, sm = 0, sl = 0;
    pthread_mutex_lock(&g_gimg_lock);
    klvk_gimg *g = gimg_find_locked(src);
    if (g) { sw = g->w; sh = g->h; sm = g->samples; sl = g->layers; }
    pthread_mutex_unlock(&g_gimg_lock);
    int ist = kl_fove_inter_state((uint64_t)(uintptr_t)src, NULL);
    VKI("[fove] %s INTO a runtime eye image %p (%u layer(s)) from %p %ux%u x%u (%u layer(s)), %u region(s) — "
        "the eye content is drawn elsewhere and transferred | region 0: src layers %u+%u at %d,%d -> dst layers "
        "%u+%u at %d,%d, %ux%u; source is %s\n", what, (void *)dst, el, (void *)src, sw, sh, sm, sl, n,
        ss ? ss->baseArrayLayer : 0, ss ? ss->layerCount : 0, sx, sy, ds ? ds->baseArrayLayer : 0,
        ds ? ds->layerCount : 0, dx, dy, w, h,
        ist == 2 ? "a CONFIRMED intermediate" : ist == 1 ? "a candidate intermediate"
        : "not a recorded eye-size pass target");
}
// What the render pass last begun on a
// command buffer is to kl_fove, for the indirect-draw hooks. MoltenVK splits a
// multiview pass around every indirect draw (a compute encoder adjusts the
// instance count, then the pass resumes with LOAD); kl_fove's compute hook
// flushes the expand there, so the resumed half draws in screen layout over a
// depth left in physical layout. Paint the Town Red's first DrawIndirect is
// its "Combat Started" (every attempt) — the bar went black outside
// ~30% of the view (the skybox filling the far-cleared depth), or see-through.
// kind: 0 nothing foveated, 1 candidate intermediate, 2 confirmed intermediate,
// 3 a pass into a runtime eye image (counted only). Direct-mapped by cb.
#define KLVK_FCB_N 256
static struct { VkCommandBuffer cb; VkImage img; void *tex; int kind, mv; } g_fcb[KLVK_FCB_N];
static pthread_mutex_t g_fcb_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_fcb_eye_indirect;
static void fcb_set(VkCommandBuffer cb, VkImage img, void *tex, int kind, int mv) {
    unsigned h = (unsigned)(((uintptr_t)cb >> 4) % KLVK_FCB_N);
    pthread_mutex_lock(&g_fcb_lock);
    if (kind || g_fcb[h].cb == cb) {
        g_fcb[h].cb = cb; g_fcb[h].img = img; g_fcb[h].tex = tex; g_fcb[h].kind = kind; g_fcb[h].mv = mv;
    }
    pthread_mutex_unlock(&g_fcb_lock);
}
// Secondaries: a run with only the lookup above showed 340 SPLIT PASS encoders
// and ZERO refusals — Unity records its scene draws into SECONDARY command
// buffers (vkCmdExecuteCommands ~7 a frame), so the indirect draw arrives on a
// command buffer that never began a render pass and the lookup found nothing.
// A command buffer with no pass of its own that records an indirect draw is
// remembered here (direct-mapped, cleared when it is begun again); the
// vkCmdExecuteCommands that plays it inside a foveated multiview pass refuses
// that pass's intermediate exactly as an inline indirect draw would.
#define KLVK_FSEC_N 1024
static struct { VkCommandBuffer cb; const char *what; } g_fsec[KLVK_FSEC_N];
static uint32_t g_fsec_seen;
static void fove_sec_begin(VkCommandBuffer cb) {
    if (!__atomic_load_n(&g_fsec_seen, __ATOMIC_RELAXED)) return;
    unsigned h = (unsigned)(((uintptr_t)cb >> 4) % KLVK_FSEC_N);
    pthread_mutex_lock(&g_fcb_lock);
    if (g_fsec[h].cb == cb) { g_fsec[h].cb = VK_NULL_HANDLE; g_fsec[h].what = NULL; }
    pthread_mutex_unlock(&g_fcb_lock);
}
static void fove_refuse_in(VkCommandBuffer cb, VkImage img, void *tex, int kind, int mv, const char *what,
                           const char *where) {
    if (kind == 3) {
        uint32_t n = __atomic_add_fetch(&g_fcb_eye_indirect, 1, __ATOMIC_RELAXED);
        if (n <= 2)
            VKI("[fove] %s inside a multiview pass into a runtime EYE image%s (#%u): MoltenVK splits that pass; if a "
                "`SPLIT PASS` line follows, that eye pass draws its second half over physical-layout depth\n", what,
                where, n);
        return;
    }
    if (!kl_fove_intermediate()) return;
    int was = kl_fove_inter_refuse((uint64_t)(uintptr_t)img);
    if (was == 3) return;
    if (tex) kl_fove_demote(tex);
    fcb_set(cb, img, tex, 0, mv);
    VKI("[fove] intermediate REFUSED: VkImage %p (was %s) — %s inside its multiview pass%s; MoltenVK splits that pass "
        "around a compute encoder and resumes it with LOAD, and the second half would draw over a depth still in "
        "physical layout. Left UNFOVEATED until it is destroyed\n", (void *)img,
        was == 2 ? "CONFIRMED" : was == 1 ? "a candidate" : "unseen", what, where);
}
static void fove_exec(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *sec) {
    if (!sec || !n || !kl_fove_on()) return;
    unsigned h = (unsigned)(((uintptr_t)cb >> 4) % KLVK_FCB_N);
    VkImage img = VK_NULL_HANDLE; void *tex = NULL; int kind = 0, mv = 0;
    const char *what = NULL;
    pthread_mutex_lock(&g_fcb_lock);
    if (g_fcb[h].cb == cb) { img = g_fcb[h].img; tex = g_fcb[h].tex; kind = g_fcb[h].kind; mv = g_fcb[h].mv; }
    if (kind && mv)
        for (uint32_t i = 0; i < n && !what; i++) {
            unsigned k = (unsigned)(((uintptr_t)sec[i] >> 4) % KLVK_FSEC_N);
            if (sec[i] && g_fsec[k].cb == sec[i]) what = g_fsec[k].what;
        }
    pthread_mutex_unlock(&g_fcb_lock);
    if (what) fove_refuse_in(cb, img, tex, kind, mv, what, " (recorded in a SECONDARY command buffer, G10b)");
}
static void fove_indirect(VkCommandBuffer cb, const char *what) {
    if (!kl_fove_on()) return;
    unsigned h = (unsigned)(((uintptr_t)cb >> 4) % KLVK_FCB_N);
    VkImage img = VK_NULL_HANDLE; void *tex = NULL; int kind = 0, mv = 0, own = 0;
    pthread_mutex_lock(&g_fcb_lock);
    if (g_fcb[h].cb == cb) { own = 1; img = g_fcb[h].img; tex = g_fcb[h].tex; kind = g_fcb[h].kind; mv = g_fcb[h].mv; }
    else {
        // No pass begun on this command buffer — a secondary (or a slot
        // collision, which only costs foveation). Remembered for its execute.
        unsigned k = (unsigned)(((uintptr_t)cb >> 4) % KLVK_FSEC_N);
        if (g_fsec[k].cb != cb) { g_fsec[k].cb = cb; g_fsec[k].what = what; }
    }
    pthread_mutex_unlock(&g_fcb_lock);
    if (!own) {
        uint32_t c = __atomic_add_fetch(&g_fsec_seen, 1, __ATOMIC_RELAXED);
        if (c == 1)
            VKI("[fove] %s on a command buffer with no render pass of its own (a SECONDARY): remembered; the "
                "vkCmdExecuteCommands that plays it inside a foveated multiview pass refuses that pass (G10b)\n", what);
        return;
    }
    if (!kind || !mv) return;
    fove_refuse_in(cb, img, tex, kind, mv, what, "");
}

// t_fove: Paint the Town Red's shape with fake handles — a confirmed
// intermediate's multiview pass begun on a PRIMARY, the indirect draw recorded
// on a SECONDARY, then the primary executes it. Needs KL_FOVEATION on. Returns
// failed checks, -1 when foveation is off.
int kl_vulkan_fove_indirect_selftest(void) {
    if (!kl_fove_intermediate()) return -1;
    int bad = 0;
    VkCommandBuffer P = (VkCommandBuffer)(uintptr_t)0x5a0000100, S = (VkCommandBuffer)(uintptr_t)0x5a0000a40,
                    S2 = (VkCommandBuffer)(uintptr_t)0x5a0000c80;
    const uint64_t A = 0x5b0001000, LD = 0x5b0002000;
    kl_fove_inter_reset_for_test();
    kl_fove_inter_note(A, NULL); kl_fove_inter_note(LD, NULL);
    if (kl_fove_inter_eye_reached(1, NULL, 0) != 2) bad++;
    // 1. a secondary WITHOUT an indirect draw played in A's pass: nothing.
    fove_sec_begin(S2);
    fcb_set(P, (VkImage)(uintptr_t)A, NULL, 2, 1);
    fove_exec(P, 1, &S2);
    if (kl_fove_inter_state(A, NULL) != 2) bad++;
    // 2. the secondary carries the indirect draw: remembered, then refused at execute.
    fove_sec_begin(S);
    fove_indirect(S, "vkCmdDrawIndirect");
    if (kl_fove_inter_state(A, NULL) != 2) bad++;           // nothing known at record time
    fove_exec(P, 1, &S);
    if (kl_fove_inter_state(A, NULL) != 3) bad++;
    // 3. the LDR pass on the same primary, the same secondary RE-RECORDED without one: LDR stays.
    fove_sec_begin(S);
    fcb_set(P, (VkImage)(uintptr_t)LD, NULL, 2, 1);
    fove_exec(P, 1, &S);
    if (kl_fove_inter_state(LD, NULL) != 2) bad++;
    // 4. a non-multiview pass executing an indirect secondary: not refused.
    fove_indirect(S, "vkCmdDrawIndirect");
    fcb_set(P, (VkImage)(uintptr_t)LD, NULL, 2, 0);
    fove_exec(P, 1, &S);
    if (kl_fove_inter_state(LD, NULL) != 2) bad++;
    fcb_set(P, VK_NULL_HANDLE, NULL, 0, 0);
    fove_sec_begin(S);
    kl_fove_inter_reset_for_test();
    return bad;
}

static int fove_rp(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi) {
    if (!bi || !kl_fove_on()) return 0;
    fcb_set(cb, VK_NULL_HANDLE, NULL, 0, 0);
    klvk_frp r;
    pthread_mutex_lock(&g_frp_lock);
    klvk_frp *rr = frp_get_locked(bi->renderPass, 0);
    if (rr) r = *rr;
    pthread_mutex_unlock(&g_frp_lock);
    if (!rr || r.col0 < 0) return 0;
    VkImageView v[16]; uint32_t nv = 0;
    for (const VkBaseInStructure *b = (const VkBaseInStructure *)bi->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
            const VkRenderPassAttachmentBeginInfo *ab = (const VkRenderPassAttachmentBeginInfo *)b;
            nv = ab->attachmentCount < 16 ? ab->attachmentCount : 16;
            if (ab->pAttachments) memcpy(v, ab->pAttachments, nv * sizeof *v); else nv = 0;
        }
    if (!nv) {
        pthread_mutex_lock(&g_ffb_lock);
        klvk_fbh *f = ffb_get_locked(bi->framebuffer, 0);
        if (f) { nv = f->nv; memcpy(v, f->v, nv * sizeof *v); }
        pthread_mutex_unlock(&g_ffb_lock);
    }
    if ((uint32_t)r.col0 >= nv) return 0;
    VkImage ci = VK_NULL_HANDLE, di = VK_NULL_HANDLE, ri = VK_NULL_HANDLE;
    if (fove_view_state(v[r.col0], &ci) || !ci) return 0;
    if (r.dep >= 0 && (uint32_t)r.dep < nv && fove_view_state(v[r.dep], &di)) di = VK_NULL_HANDLE;
    if (r.res0 >= 0 && (uint32_t)r.res0 < nv && fove_view_state(v[r.res0], &ri)) ri = VK_NULL_HANDLE;
    uint32_t cw = 0, ch = 0, cs = 0, cl = 0, dw = 0, dh = 0, ds = 0, rw = 0, rh = 0;
    pthread_mutex_lock(&g_gimg_lock);
    klvk_gimg *g = gimg_find_locked(ci);
    if (g) { cw = g->w; ch = g->h; cs = g->samples; cl = g->layers; }
    if ((g = gimg_find_locked(di))) { dw = g->w; dh = g->h; ds = g->samples; }
    if ((g = gimg_find_locked(ri))) { rw = g->w; rh = g->h; }
    pthread_mutex_unlock(&g_gimg_lock);
    uint32_t el = 0, ew = 0, eh = 0; int ee = -1;
    int direct = fove_runtime_eye_wh(ci, &el, &ee, &ew, &eh);
    if (direct && !cw) { cw = ew; ch = eh; cs = 1; cl = el; }
    // An eye image attached at ANOTHER extent than the colour (resolve, or a
    // later subpass's output): named on the near-miss line instead of silence.
    // Resident Evil 4 (UE4 4.25) quantized its scene to 4032x3236 over a
    // 4032x3234 eye and nothing was printed at all.
    uint32_t mis_w = 0, mis_h = 0;
    int via_res = 0;
    if (!direct && ri && fove_runtime_eye_wh(ri, &el, &ee, &rw, &rh)) {
        if (rw == cw && rh == ch) via_res = 1;
        else { mis_w = rw; mis_h = rh; }
    }
    uint32_t vb = 0, vc = 1;
    fove_view_range(v[r.col0], &vb, &vc);
    uint32_t L = r.viewmask ? (uint32_t)__builtin_popcount(r.viewmask) : (vc ? vc : (cl > vb ? cl - vb : 1));
    // The colour is the guest's OWN eye-size target. (a) In-pass: a
    // later subpass of this render pass writes (or resolves into) a runtime eye
    // image — Batman: Arkham Shadow: 2-sample scene + depth, the post subpass reads
    // the scene as an input attachment and writes the eye. (b) A candidate the
    // guest copies/blits/samples into the eye later: confirmed by that
    // transfer or pass (the confirming frame stays unfoveated), sticky until
    // vkDestroyImage, then marked like an eye on every later frame.
    VkImage oi = VK_NULL_HANDLE; int oj = -1;          // (a): the eye image and its attachment index
    int conf = 0, conf_via = 0, cand = 0, refused = 0; // (b); refused: an indirect draw in the pass
    int inter_on = !direct && !via_res && kl_fove_intermediate() && cw && ch && r.dep >= 0;
    if (inter_on) {
        for (unsigned j = 0; j < r.natt && j < nv && !oi; j++) {
            if (!(r.outs >> j & 1)) continue;
            VkImage im = VK_NULL_HANDLE; uint32_t ol = 0, ow = 0, oh = 0; int oe = -1;
            if (fove_view_state(v[j], &im) || !im) continue;
            if (fove_runtime_eye_wh(im, &ol, &oe, &ow, &oh)) {
                if (!ow || (ow == cw && oh == ch)) { oi = im; oj = (int)j; el = ol; ee = oe; }
                else if (!mis_w) { mis_w = ow; mis_h = oh; }
            }
        }
        if (!oi && fove_eye_extent_known(cw, ch)) {
            int st = kl_fove_inter_note((uint64_t)(uintptr_t)ci, &conf_via);
            if (st == 2) { conf = 1; el = cl; ee = -1; }
            else if (st == 3) refused = 1;
            else { cand = 1; g_inter_last_cb = cb; }
        }
    }
    // Why this pass is (not) an eye pass, for the once-per-shape line.
    const char *why = NULL;
    static __thread char mis_why[256];
    if (!direct && !via_res && !oi && !conf && mis_w) {
        snprintf(mis_why, sizeof mis_why, "a runtime eye image (%ux%u) is attached as the resolve or a later "
                 "subpass's output at ANOTHER extent than the colour (%ux%u) — the guest quantized its scene buffer "
                 "(an eye size that is a multiple of 4 matches an Unreal guest)", mis_w, mis_h, cw, ch);
        why = mis_why;
    } else if (refused)
        why = "eye-size intermediate REFUSED (an indirect draw inside its multiview pass: MoltenVK splits the pass) — "
              "left unfoveated";
    else if (!direct && !via_res && !oi && !conf)
        why = r.dep >= 0 && fove_eye_extent_known(cw, ch)
            ? (cand ? "eye-size pass that does not draw into (or resolve into) a runtime eye image — CANDIDATE "
                      "intermediate: marked from the next frame on once a copy/blit/resolve into the eye, or a pass "
                      "drawing into the eye after it, confirms it"
                    : "eye-size pass that does not draw into (or resolve into) a runtime eye image — an intermediate "
                      "the guest copies/blits later is not foveated by this path (KL_FOVE_INTERMEDIATE=0)")
            : NULL;
    else if (!di) why = "no depth attachment (a post/blit pass, not the scene)";
    else if (dw != cw || dh != ch) why = "depth extent differs from colour";
    else if (cs != ds) why = "colour and depth sample counts differ";
    else if (cs > 1 && !kl_fove_msaa()) why = "MSAA and KL_FOVE_MSAA=0";
    else if (L >= 2 && (r.viewmask ? (r.viewmask & 3u) != 3u || L != 2 : vb != 0 || L != 2))
        why = "a multiview pass over other than views/slices 0 and 1";
    else if (L >= 2 && el < 2) why = "multiview pass over a one-layer image";
    else if (oi && r.a[r.col0].store != (uint8_t)VK_ATTACHMENT_STORE_OP_DONT_CARE)
        why = "in-pass intermediate whose scene colour is STORED (it outlives the pass in physical layout)";
    else if (oi && (uint32_t)oj < r.natt && r.a[oj].load == (uint8_t)VK_ATTACHMENT_LOAD_OP_LOAD)
        why = "in-pass intermediate whose eye attachment is LOADed";
    else if (oi && r.vm_mixed) why = "in-pass intermediate whose subpasses have different view masks";
    int eye_pass = (direct || via_res || oi || conf) && !why;
    if (eye_pass) {
        pthread_mutex_lock(&g_fove_lock);
        void *ct = fove_tex_locked(ci), *dt = fove_tex_locked(di), *ot = oi ? fove_tex_locked(oi) : NULL;
        pthread_mutex_unlock(&g_fove_lock);
        int lay = L >= 2 ? 2 : 1, one = L >= 2 ? -1 : (ee == 1 ? 1 : 0);
        if (!ct || (oi && !ot)) why = "no MTLTexture behind the colour (or eye) image", eye_pass = 0;
        else if (oi) {
            kl_fove_note_eye_scene(ct, dt, ot, cw, ch, cs, (uint32_t)lay, one, KL_FOVE_VIA_SUBPASS);
            kl_fove_inter_note((uint64_t)(uintptr_t)ci, NULL);
            if (kl_fove_inter_confirm((uint64_t)(uintptr_t)ci, KL_FOVE_VIA_SUBPASS) == 1)
                VKI("[fove] intermediate CONFIRMED: VkImage %p -> eye image %p via subpass (attachment #%d of render "
                    "pass %p, written by a later subpass; %u subpass(es), scene %ux%u x%u)\n", (void *)ci, (void *)oi,
                    oj, (void *)bi->renderPass, r.nsub, cw, ch, cs);
        } else if (conf) {
            kl_fove_note_eye_scene(ct, dt, NULL, cw, ch, cs, (uint32_t)lay, one, conf_via);
            // Transient MSAA (Deadpool's UE4 shape): an MSAA scene the guest does NOT store
            // (storeOp DONT_CARE) and a later subpass of this pass resolves into
            // a single-sample attachment. Its samples die with the pass, so kl_fove
            // may leave them in physical layout across the subpass encoders and
            // expand only what the resolve writes (no per-sample expand).
            int tr = cs > 1 && r.a[r.col0].store == (uint8_t)VK_ATTACHMENT_STORE_OP_DONT_CARE && r.res_late >= 0 &&
                     r.res_late < (int8_t)r.natt && r.a[r.res_late].samples == 1;
            kl_fove_note_ms_transient(ct, tr);
            static int said_tr;
            if (tr && !said_tr++)
                VKI("[fove] intermediate %p: MSAA x%u scene with storeOp DONT_CARE, resolved by a later subpass into "
                    "attachment #%d (x1): its samples stay in physical layout through the pass, the resolve target "
                    "is what gets expanded\n", (void *)ci, cs, r.res_late);
        } else {
            kl_fove_note_eye_array(ct, dt, cw, ch, cs, (uint32_t)lay, one);
            fove_xr_reg_note(direct ? ci : ri, ct, dt);
        }
    }
    // What this pass is, for an indirect draw recorded inside it.
    {
        int mv = r.viewmask != 0;
        if (conf && eye_pass) {
            pthread_mutex_lock(&g_fove_lock);
            void *ct = fove_tex_locked(ci);
            pthread_mutex_unlock(&g_fove_lock);
            fcb_set(cb, ci, ct, 2, mv);
        } else if (cand) fcb_set(cb, ci, NULL, 1, mv);
        else if (eye_pass && (direct || via_res || oi)) fcb_set(cb, ci, NULL, 3, mv);
    }
    // A pass drawing into an eye image: it may be the one that samples the
    // candidate intermediate(s) into the eye, with OR without a depth
    // attachment — Unity's HDR path draws its scene into a 1x R16G16B16A16
    // eye-size target, then its final pass into the eye binds the scene depth
    // (DONT_CARE); a rule that wanted `!di` here never confirmed anything and
    // such a level ran unfoveated (Paint the Town Red: `exp 0 marked 0`, guest
    // GPU p99 24.7 ms). Every candidate noted since the
    // previous pass into an eye is confirmed (a post chain HDR -> LDR -> eye
    // confirms each hop). An MSAA pass RESOLVING into the eye, or an in-pass
    // intermediate, only closes that window.
    if (kl_fove_intermediate() && (direct || via_res || oi)) {
        uint64_t last = kl_fove_inter_last();
        static int probed;
        if (direct && fove_probe_on() && probed < 4) {
            probed++;
            VKI("[fove] probe: pass into eye image %p with%s depth (render pass %p, %u subpass(es), area %ux%u); last "
                "candidate intermediate %p (same command buffer: %s)\n", (void *)ci, di ? "" : " no",
                (void *)bi->renderPass, r.nsub, bi->renderArea.extent.width, bi->renderArea.extent.height,
                (void *)(uintptr_t)last, last && g_inter_last_cb == cb ? "yes" : "no");
        }
        uint64_t got[8];
        int ng = kl_fove_inter_eye_reached(direct, got, 8);
        for (int k = 0; k < ng && k < 8; k++)
            VKI("[fove] intermediate CONFIRMED: VkImage %p -> eye image %p via sampled (a later pass draws into the "
                "eye, %s depth, render pass %p; %d candidate(s) in this window; the last one's command buffer is "
                "this one: %s)\n", (void *)(uintptr_t)got[k], (void *)ci, di ? "with" : "no",
                (void *)bi->renderPass, ng, g_inter_last_cb == cb ? "yes" : "no");
    }
    if (!eye_pass && !why) return 0;   // an unrelated pass: silent
    // One line per new shape (render pass + which images).
    static struct { VkRenderPass rp; int eye; } seen[64];
    static unsigned nseen;
    static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;
    int fresh = 1;
    pthread_mutex_lock(&seen_lock);
    for (unsigned i = 0; i < nseen; i++) if (seen[i].rp == bi->renderPass && seen[i].eye == eye_pass) fresh = 0;
    if (fresh && nseen < 64) { seen[nseen].rp = bi->renderPass; seen[nseen].eye = eye_pass; nseen++; }
    else if (fresh) fresh = 0;
    pthread_mutex_unlock(&seen_lock);
    if (!fresh) return eye_pass;
    char att[512]; int o = 0;
    for (unsigned i = 0; i < r.natt && o < (int)sizeof att - 64; i++)
        o += snprintf(att + o, sizeof att - (size_t)o, "%s#%u fmt %u x%u load %u store %u%s", i ? "; " : "", i,
                      r.a[i].fmt, r.a[i].samples, r.a[i].load, r.a[i].store,
                      (int)i == r.col0 ? " =colour" : (int)i == r.dep ? " =depth" : (int)i == r.res0 ? " =resolve" : "");
    if (!eye_pass) {
        char eyes[256]; int eo = 0;
        for (int s2 = 0; s2 < KLVK_EYE_STAGES && eo < (int)sizeof eyes - 40; s2++)
            for (int e2 = 0; e2 < 2; e2++)
                if (g_eye[s2][e2].img) eo += snprintf(eyes + eo, sizeof eyes - (size_t)eo, " s%d/e%d=%p", s2, e2, (void *)g_eye[s2][e2].img);
        VKI("[fove] near miss images: colour %p depth %p resolve %p (resolve %ux%u); runtime eye images:%s\n",
            (void *)ci, (void *)di, (void *)ri, rw, rh, eo ? eyes : " none");
    }
    VKI("[fove] %s render pass %p: %s %ux%u x%u (%u layer(s), view base %u count %u, viewMask 0x%x)%s, depth %ux%u x%u, "
        "%u subpass(es), %u colour attachment(s), area %d,%d %ux%u | %s%s%s\n",
        eye_pass ? "EYE PASS (marked)" : "near miss, NOT marked:", (void *)bi->renderPass,
        direct ? "colour = runtime eye image" : via_res ? "MSAA colour resolved into a runtime eye image"
        : oi ? "eye-size intermediate, a later subpass writes a runtime eye image"
        : conf ? "confirmed eye-size intermediate (the guest transfers/samples it into the eye)" : "colour",
        cw, ch, cs, cl, vb, vc, r.viewmask, via_res ? "" : "", dw, dh, ds, r.nsub, r.ncol,
        bi->renderArea.offset.x, bi->renderArea.offset.y, bi->renderArea.extent.width, bi->renderArea.extent.height,
        att, why ? " | why: " : "", why ? why : "");
    return eye_pass;
}

// KL_FOVE_AREA_SNAP (default 1, foveation only): an EYE PASS whose render
// area falls a few px short of its framebuffer is widened to the whole
// framebuffer. MoltenVK honours loadOp CLEAR as a Metal clear only when the
// render area IS the framebuffer extent; otherwise every attachment opens with
// MTLLoadActionLoad and the clear is drawn as a quad — and kl_fove can never
// map a pass that LOADs an eye with nothing foveated pending (it would read
// screen-layout pixels as physical). Pinball FX (UE 5.3.2) without this:
// area 4608x2935 on a 4608x2936 eye (Unreal quantizes its scene
// buffer UP to a multiple of 4 and renders the unquantized height), so every
// eye pass LOADed and the whole run was foveated 0% ("eye LOADs left
// unfoveated" 1970, encoders marked 0). The extra rows/columns lie outside
// every viewport the guest submits; they are only cleared and stored.
// Returns bi itself, or `tmp` holding the widened copy.
static const VkRenderPassBeginInfo *fove_area_snap(const VkRenderPassBeginInfo *bi, VkRenderPassBeginInfo *tmp) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_FOVE_AREA_SNAP", 1);
    if (!on || !bi || bi->renderArea.offset.x || bi->renderArea.offset.y) return bi;
    uint32_t fw = 0, fh = 0;
    pthread_mutex_lock(&g_ffb_lock);
    klvk_fbh *f = ffb_get_locked(bi->framebuffer, 0);
    if (f) { fw = f->w; fh = f->h; }
    pthread_mutex_unlock(&g_ffb_lock);
    uint32_t aw = bi->renderArea.extent.width, ah = bi->renderArea.extent.height;
    static uint64_t n_snap, n_same, n_far;
    if (!fw || !fh || aw > fw || ah > fh || fw - aw >= 8 || fh - ah >= 8) {
        if (fw && fh && __atomic_add_fetch(&n_far, 1, __ATOMIC_RELAXED) == 1)
            VKI("[fove] area snap: eye pass area %ux%u vs framebuffer %ux%u — too far apart to widen (MoltenVK "
                "LOADs instead of clearing; the pass may stay unfoveated)\n", aw, ah, fw, fh);
        return bi;
    }
    if (aw == fw && ah == fh) {
        if (__atomic_add_fetch(&n_same, 1, __ATOMIC_RELAXED) == 1)
            VKI("[fove] area snap: first eye pass already covers its framebuffer (%ux%u) — nothing to widen\n", fw, fh);
        return bi;
    }
    *tmp = *bi;
    tmp->renderArea.extent.width = fw;
    tmp->renderArea.extent.height = fh;
    uint64_t n = __atomic_add_fetch(&n_snap, 1, __ATOMIC_RELAXED);
    if (n == 1 || n == 1000 || n == 100000)
        VKI("[fove] area snap #%llu: eye pass area %ux%u widened to its framebuffer %ux%u so MoltenVK clears it "
            "instead of LOADing it (KL_FOVE_AREA_SNAP=0 turns this off)\n", (unsigned long long)n, aw, ah, fw, fh);
    return tmp;
}

// The fove-only entry points: handed out by fove_entry() only while
// foveation is on, so an OFF run never routes through them.
static PFN_vkVoidFunction fove_real(const char *name, const char *alt) {
    PFN_vkVoidFunction f = (PFN_vkVoidFunction)mvk_sym(name);
    if (!f && alt) f = (PFN_vkVoidFunction)mvk_sym(alt);
    return f;
}
// The Vulkan 1.3 / KHR_copy_commands2 spellings of the transfers (the 1.0 ones
// are noted in klvk_CmdCopyImage / klvk_CmdBlitImage / klvk_CmdResolveImage).
static void VKAPI_CALL klvk_CmdCopyImage2F(VkCommandBuffer cb, const VkCopyImageInfo2 *i) {
    static PFN_vkCmdCopyImage2 real;
    if (!real) real = (PFN_vkCmdCopyImage2)fove_real("vkCmdCopyImage2", "vkCmdCopyImage2KHR");
    if (i && i->regionCount && i->pRegions) {
        const VkImageCopy2 *r = i->pRegions;
        fove_xfer_note("vkCmdCopyImage2", "copy", i->srcImage, i->dstImage, i->regionCount, &r->srcSubresource,
                       &r->dstSubresource, r->srcOffset.x, r->srcOffset.y, r->dstOffset.x, r->dstOffset.y,
                       r->extent.width, r->extent.height);
    }
    if (real) real(cb, i);
}
static void VKAPI_CALL klvk_CmdBlitImage2F(VkCommandBuffer cb, const VkBlitImageInfo2 *i) {
    static PFN_vkCmdBlitImage2 real;
    if (!real) real = (PFN_vkCmdBlitImage2)fove_real("vkCmdBlitImage2", "vkCmdBlitImage2KHR");
    if (i && i->regionCount && i->pRegions) {
        const VkImageBlit2 *r = i->pRegions;
        fove_xfer_note("vkCmdBlitImage2", "blit", i->srcImage, i->dstImage, i->regionCount, &r->srcSubresource,
                       &r->dstSubresource, r->srcOffsets[0].x, r->srcOffsets[0].y, r->dstOffsets[0].x, r->dstOffsets[0].y,
                       (uint32_t)(r->dstOffsets[1].x - r->dstOffsets[0].x), (uint32_t)(r->dstOffsets[1].y - r->dstOffsets[0].y));
    }
    if (real) real(cb, i);
}
static void VKAPI_CALL klvk_CmdResolveImage2F(VkCommandBuffer cb, const VkResolveImageInfo2 *i) {
    static PFN_vkCmdResolveImage2 real;
    if (!real) real = (PFN_vkCmdResolveImage2)fove_real("vkCmdResolveImage2", "vkCmdResolveImage2KHR");
    if (i && i->regionCount && i->pRegions) {
        const VkImageResolve2 *r = i->pRegions;
        fove_xfer_note("vkCmdResolveImage2", "resolve", i->srcImage, i->dstImage, i->regionCount, &r->srcSubresource,
                       &r->dstSubresource, r->srcOffset.x, r->srcOffset.y, r->dstOffset.x, r->dstOffset.y,
                       r->extent.width, r->extent.height);
    }
    if (real) real(cb, i);
}

// The indirect draws (see fove_indirect) and the execute that plays a
// secondary carrying one.
static void VKAPI_CALL klvk_CmdDrawIndirectF(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n,
                                             uint32_t stride) {
    // Through this fork's own wrapper rather than straight to MoltenVK: it
    // carries the draw guard (klvk_ap_skip) and the profile counters, and the
    // foveation entry would otherwise take the call away from both.
    fove_indirect(cb, "vkCmdDrawIndirect");
    klvk_CmdDrawIndirect(cb, b, o, n, stride);
}
static void VKAPI_CALL klvk_CmdDrawIndexedIndirectF(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n,
                                                    uint32_t stride) {
    fove_indirect(cb, "vkCmdDrawIndexedIndirect");
    klvk_CmdDrawIndexedIndirect(cb, b, o, n, stride);   // see klvk_CmdDrawIndirectF
}
static void VKAPI_CALL klvk_CmdDrawIndirectCountF(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf,
                                                  VkDeviceSize co, uint32_t max, uint32_t stride) {
    static PFN_vkCmdDrawIndirectCount real;
    if (!real) real = (PFN_vkCmdDrawIndirectCount)fove_real("vkCmdDrawIndirectCount", "vkCmdDrawIndirectCountKHR");
    fove_indirect(cb, "vkCmdDrawIndirectCount");
    if (real) real(cb, b, o, cbuf, co, max, stride);
}
static void VKAPI_CALL klvk_CmdDrawIndexedIndirectCountF(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o,
                                                         VkBuffer cbuf, VkDeviceSize co, uint32_t max,
                                                         uint32_t stride) {
    static PFN_vkCmdDrawIndexedIndirectCount real;
    if (!real) real = (PFN_vkCmdDrawIndexedIndirectCount)fove_real("vkCmdDrawIndexedIndirectCount",
                                                                   "vkCmdDrawIndexedIndirectCountKHR");
    fove_indirect(cb, "vkCmdDrawIndexedIndirectCount");
    if (real) real(cb, b, o, cbuf, co, max, stride);
}
static void VKAPI_CALL klvk_CmdExecuteCommandsF(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *sec) {
    static PFN_vkCmdExecuteCommands real;
    if (!real) real = (PFN_vkCmdExecuteCommands)fove_real("vkCmdExecuteCommands", NULL);
    fove_exec(cb, n, sec);
    if (real) real(cb, n, sec);
}

// The view and framebuffer tables drop their entries with the object.
static void VKAPI_CALL klvk_DestroyImageViewF(VkDevice dev, VkImageView v, const VkAllocationCallbacks *a) {
    static PFN_vkDestroyImageView real;
    if (!real && real_gdpa) real = (PFN_vkDestroyImageView)real_gdpa(dev, "vkDestroyImageView");
    fove_forget_view(v);
    if (real) real(dev, v, a);
}
static void VKAPI_CALL klvk_DestroyFramebufferF(VkDevice dev, VkFramebuffer fb, const VkAllocationCallbacks *a) {
    static PFN_vkDestroyFramebuffer real;
    if (!real && real_gdpa) real = (PFN_vkDestroyFramebuffer)real_gdpa(dev, "vkDestroyFramebuffer");
    fove_forget_fb(fb);
    if (real) real(dev, fb, a);
}

static PFN_vkVoidFunction fove_entry(const char *name) {
    if (!name || !kl_fove_on()) return NULL;
    static const entry fove[] = {
        { "vkCmdCopyImage2",                  (PFN_vkVoidFunction)klvk_CmdCopyImage2F },
        { "vkCmdCopyImage2KHR",               (PFN_vkVoidFunction)klvk_CmdCopyImage2F },
        { "vkCmdBlitImage2",                  (PFN_vkVoidFunction)klvk_CmdBlitImage2F },
        { "vkCmdBlitImage2KHR",               (PFN_vkVoidFunction)klvk_CmdBlitImage2F },
        { "vkCmdResolveImage2",               (PFN_vkVoidFunction)klvk_CmdResolveImage2F },
        { "vkCmdResolveImage2KHR",            (PFN_vkVoidFunction)klvk_CmdResolveImage2F },
        { "vkCmdDrawIndirect",                (PFN_vkVoidFunction)klvk_CmdDrawIndirectF },
        { "vkCmdDrawIndexedIndirect",         (PFN_vkVoidFunction)klvk_CmdDrawIndexedIndirectF },
        { "vkCmdDrawIndirectCount",           (PFN_vkVoidFunction)klvk_CmdDrawIndirectCountF },
        { "vkCmdDrawIndirectCountKHR",        (PFN_vkVoidFunction)klvk_CmdDrawIndirectCountF },
        { "vkCmdDrawIndexedIndirectCount",    (PFN_vkVoidFunction)klvk_CmdDrawIndexedIndirectCountF },
        { "vkCmdDrawIndexedIndirectCountKHR", (PFN_vkVoidFunction)klvk_CmdDrawIndexedIndirectCountF },
        { "vkCmdExecuteCommands",             (PFN_vkVoidFunction)klvk_CmdExecuteCommandsF },
        { "vkDestroyImageView",               (PFN_vkVoidFunction)klvk_DestroyImageViewF },
        { "vkDestroyFramebuffer",             (PFN_vkVoidFunction)klvk_DestroyFramebufferF },
    };
    for (size_t i = 0; i < sizeof fove / sizeof *fove; i++)
        if (!strcmp(name, fove[i].name)) {
            // Only where MoltenVK has the command (either spelling): ours
            // would otherwise drop it.
            if (!strncmp(name, "vkCmd", 5)) {
                char core[64];
                snprintf(core, sizeof core, "%s", name);
                size_t l = strlen(core);
                if (l > 3 && !strcmp(core + l - 3, "KHR")) core[l - 3] = 0;
                if (!fove_real(name, core)) return NULL;
            }
            return fove[i].fn;
        }
    return NULL;
}

// kl_fove swizzles MoltenVK's own MTLDevice (shader source) and command-buffer
// classes, once, when the first device is registered — before the guest
// compiles anything.
static void fove_install(klvk_device *d) {
    if (!kl_fove_on()) return;
    typedef void (*pfn_get_mtl_device_mvk)(VkPhysicalDevice, void **);
    pfn_get_mtl_device_mvk get_dev = (pfn_get_mtl_device_mvk)mvk_sym("vkGetMTLDeviceMVK");
    void *mtl = NULL;
    if (get_dev && d && d->phys) get_dev(d->phys, &mtl);
    if (mtl) kl_fove_install(mtl);
    else VKI("[fove] no MTLDevice behind the Vulkan device (vkGetMTLDeviceMVK) — foveation not installed\n");
}

// vkCmdBeginRenderPass[2]: the eye-pass check, and the area snap for a pass it
// marks. Returns bi or `tmp`.
static const VkRenderPassBeginInfo *fove_begin(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi,
                                               VkRenderPassBeginInfo *tmp) {
    if (fove_rp(cb, bi)) return fove_area_snap(bi, tmp);
    return bi;
}

static const entry g_ours[] = {
#define E(n) { "vk" #n, (PFN_vkVoidFunction)klvk_##n }
#define R(n) { "vk" #n, (PFN_vkVoidFunction)klvkrec_##n }
    E(CreateInstance), E(EnumerateInstanceExtensionProperties),
    E(EnumeratePhysicalDevices),
    E(CreateDevice), E(DestroyDevice), E(DestroyInstance),
    E(CreateAndroidSurfaceKHR), E(DestroySurfaceKHR),
    // Same synthetic surface, same signature; the create-info is not read.
    { "vkCreateHeadlessSurfaceEXT", (PFN_vkVoidFunction)klvk_CreateAndroidSurfaceKHR },
    { "vkCreateXcbSurfaceKHR",      (PFN_vkVoidFunction)klvk_CreateAndroidSurfaceKHR },
    { "vkCreateXlibSurfaceKHR",     (PFN_vkVoidFunction)klvk_CreateAndroidSurfaceKHR },
    { "vkCreateWaylandSurfaceKHR",  (PFN_vkVoidFunction)klvk_CreateAndroidSurfaceKHR },
    E(GetPhysicalDeviceSurfaceSupportKHR), E(GetPhysicalDeviceSurfaceCapabilitiesKHR),
    E(GetPhysicalDeviceSurfaceFormatsKHR), E(GetPhysicalDeviceSurfacePresentModesKHR),
    E(CreateSwapchainKHR), E(DestroySwapchainKHR), E(GetSwapchainImagesKHR),
    E(AcquireNextImageKHR), E(QueuePresentKHR),
    E(AllocateMemory), E(BindImageMemory),
    E(QueueSubmit),
    E(GetPhysicalDeviceProperties),
    E(GetPhysicalDeviceProperties2),
    E(GetPhysicalDeviceQueueFamilyProperties), E(GetDeviceQueue), E(CmdSetVertexInputEXT),
    E(GetPhysicalDeviceFeatures), E(GetPhysicalDeviceFeatures2),
    { "vkGetPhysicalDeviceFeatures2KHR", (PFN_vkVoidFunction)klvk_GetPhysicalDeviceFeatures2 },
    E(GetPhysicalDeviceMemoryProperties),
    E(GetPhysicalDeviceFormatProperties), E(GetPhysicalDeviceFormatProperties2),
    { "vkGetPhysicalDeviceFormatProperties2KHR", (PFN_vkVoidFunction)klvk_GetPhysicalDeviceFormatProperties2 },
    E(EnumerateDeviceLayerProperties), E(EnumerateDeviceExtensionProperties),
    { "vkGetPhysicalDeviceProperties2KHR", (PFN_vkVoidFunction)klvk_GetPhysicalDeviceProperties2 },
    E(GetPhysicalDeviceImageFormatProperties),
    E(GetPhysicalDeviceImageFormatProperties2),
    E(GetPhysicalDeviceMemoryProperties2),
    E(GetPhysicalDeviceFragmentShadingRatesKHR),
    { "vkGetPhysicalDeviceMemoryProperties2KHR", (PFN_vkVoidFunction)klvk_GetPhysicalDeviceMemoryProperties2 },
    { "vkGetPhysicalDeviceImageFormatProperties2KHR", (PFN_vkVoidFunction)klvk_GetPhysicalDeviceImageFormatProperties2 },
    E(CreateImage), E(DestroyImage), E(CreateImageView), E(CmdCopyImage),
    E(BeginCommandBuffer),
    E(CmdBindIndexBuffer), E(CmdBindVertexBuffers), E(CmdDrawIndexed),
    E(CmdSetSampleLocationsEXT), E(CmdSetSampleLocationsEnableEXT), E(CmdSetBlendConstants),
    E(CmdDraw), E(CmdDrawIndirect), E(CmdDrawIndexedIndirect), E(CmdClearAttachments), E(CmdBeginQuery), E(CmdEndQuery), E(CreateQueryPool), E(GetQueryPoolResults),
    E(CmdBindPipeline), E(CmdBindDescriptorSets), E(CmdSetScissor), E(CmdSetViewport),
    E(GetImageMemoryRequirements), E(CreateBufferView),
    R(CreateShaderModule), R(CreateDescriptorSetLayout), E(CreateDescriptorPool),
    R(CreateSampler), R(CreatePipelineLayout),
    E(UpdateDescriptorSets),
    E(CreateDescriptorUpdateTemplate), E(UpdateDescriptorSetWithTemplate),
    { "vkCreateDescriptorUpdateTemplateKHR", (PFN_vkVoidFunction)klvk_CreateDescriptorUpdateTemplate },
    { "vkUpdateDescriptorSetWithTemplateKHR", (PFN_vkVoidFunction)klvk_UpdateDescriptorSetWithTemplate },
    E(CmdPushDescriptorSetKHR), E(CmdPushDescriptorSetWithTemplateKHR),
    E(CmdBindDescriptorBuffersEXT),
    E(CreateBuffer), E(BindBufferMemory), E(MapMemory), E(FreeMemory),
    E(CmdCopyBuffer), E(CmdUpdateBuffer),
    E(CmdBlitImage), E(CmdResolveImage), E(CmdClearColorImage),
    E(CmdCopyBufferToImage), E(CmdCopyImageToBuffer), E(CmdPipelineBarrier),
    R(CreateGraphicsPipelines), R(CreateComputePipelines), E(CreatePipelineCache),
    E(DestroyPipeline), E(DestroyShaderModule), E(DestroyPipelineLayout),
    E(SetDebugUtilsObjectNameEXT), E(SetDebugUtilsObjectTagEXT),
    E(DebugMarkerSetObjectNameEXT), E(DebugMarkerSetObjectTagEXT),
    E(CreateFramebuffer), E(CmdBeginRenderPass),
    E(CmdBeginRenderPass2),
    { "vkCmdBeginRenderPass2KHR", (PFN_vkVoidFunction)klvk_CmdBeginRenderPass2 },
    R(CreateRenderPass), R(CreateRenderPass2), E(DestroyRenderPass),
    { "vkCreateRenderPass2KHR", (PFN_vkVoidFunction)klvkrec_CreateRenderPass2 },
    { "vkCreateRenderPassKHR",  (PFN_vkVoidFunction)klvkrec_CreateRenderPass },
    E(CmdBeginRendering), E(CmdEndRendering),
    { "vkCmdEndRenderingKHR", (PFN_vkVoidFunction)klvk_CmdEndRendering },
    { "vkCmdBeginRenderingKHR", (PFN_vkVoidFunction)klvk_CmdBeginRendering },
#undef E
#undef R
};

static PFN_vkVoidFunction ours(const char *name) {
    // KL_TRACE: reveal which render-pass / dynamic-rendering entry points the
    // guest actually resolves, and whether we hand back our wrapper or fall
    // through to the real MoltenVK one. This is how we learn why the compositing
    // render pass is invisible to the census.
    if (name && klvk_trace() && (strstr(name, "RenderPass") || strstr(name, "Rendering") || strstr(name, "Framebuffer"))) {
        static int rl;
        if (rl < 80) { rl++;
            PFN_vkVoidFunction hit = NULL;
            for (size_t i = 0; i < sizeof g_ours / sizeof *g_ours; i++)
                if (!strcmp(g_ours[i].name, name)) { hit = g_ours[i].fn; break; }
            fprintf(stderr, "[TRACE] [resolve] guest asked \"%s\" -> %s\n",
                name, hit ? "OUR wrapper" : "real MoltenVK (passthrough)");
        }
    }
    PFN_vkVoidFunction f = fove_entry(name);   // KL_FOVEATION only (off: NULL)
    if (f) return f;
    for (size_t i = 0; i < sizeof g_ours / sizeof *g_ours; i++)
        if (!strcmp(g_ours[i].name, name)) return g_ours[i].fn;
    return NULL;
}

static PFN_vkVoidFunction VKAPI_CALL klvk_GetInstanceProcAddr(VkInstance inst,
                                                              const char *name);
static PFN_vkVoidFunction VKAPI_CALL klvk_GetDeviceProcAddr(VkDevice dev,
                                                            const char *name);

// A Vulkan entry point the guest resolved that neither we nor MoltenVK provide.
// Returning NULL here is what killed Wrath2: UE4 resolves device functions and
// calls them WITHOUT a null-check, so a NULL becomes a jump to 0x0 — which the
// visionOS crash report flags as CODESIGNING "Invalid Page" (an unmapped, and
// therefore unsigned, executable page). A named no-op stub is strictly safer:
// the guest gets a valid pointer, the call returns VK_SUCCESS (0 — also a safe
// value for a void or pointer-returning entry), and we log the name once so the
// missing function can be implemented properly if the guest actually depends on
// it. Same "name it when it's used" contract as the OVRP and JNI shims.
static VKAPI_ATTR VkResult VKAPI_CALL klvk_missing_trap(void) {
    return VK_SUCCESS;
}
static void klvk_note_missing(const char *name) {
    static char *seen[64];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (!strcmp(seen[i], name)) return;   // already reported
    if (nseen < 64) seen[nseen++] = strdup(name);   // copied — the guest's string need not outlive the call
    VKI("guest resolved '%s' but neither we nor MoltenVK provide it — handing back "
        "a no-op stub instead of NULL (a NULL here is the jump-to-0x0 that AMFI "
        "kills as an invalid page)\n", name);
}

static PFN_vkVoidFunction VKAPI_CALL klvk_GetInstanceProcAddr(VkInstance inst,
                                                              const char *name) {
    vk_init();
    if (!name) return NULL;
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)klvk_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)klvk_GetDeviceProcAddr;
    PFN_vkVoidFunction m = ours(name);
    VKT("gipa(%s, \"%s\")%s\n", inst ? "instance" : "NULL", name, m ? " -> ours" : "");
    if (m) return m;
    if (!g_avail) return NULL;
    PFN_vkVoidFunction fn = real_gipa(inst ? inst : g_instance, name);
    if (!fn) { klvk_note_missing(name); return (PFN_vkVoidFunction)klvk_missing_trap; }
    return fn;
}

static PFN_vkVoidFunction VKAPI_CALL klvk_GetDeviceProcAddr(VkDevice dev,
                                                            const char *name) {
    vk_init();
    if (!name) return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)klvk_GetDeviceProcAddr;
    PFN_vkVoidFunction m = ours(name);
    if (m) return m;
    if (!g_avail || !real_gdpa) return NULL;
    PFN_vkVoidFunction fn = real_gdpa(dev, name);
    if (!fn) { klvk_note_missing(name); return (PFN_vkVoidFunction)klvk_missing_trap; }
    return fn;
}

// ---------------------------------------------------------------------------
// The synthetic-library face
// ---------------------------------------------------------------------------
static const int g_handle_tag = 0;
#define KLVK_HANDLE ((void *)&g_handle_tag)

int kl_vulkan_claims(const char *soname) {
    if (!name_is_vulkan(soname)) return 0;
    vk_init();
    return g_avail;
}

void *kl_vulkan_dlopen(const char *soname) {
    if (!name_is_vulkan(soname)) return NULL;
    vk_init();
    if (!g_avail) {
        VKI("guest dlopen(\"%s\") refused — MoltenVK is not vendored ('make mvk')\n",
            soname);
        return NULL;
    }
    VKI("guest dlopen(\"%s\") -> synthetic Vulkan loader over MoltenVK\n", soname);
    return KLVK_HANDLE;
}

int kl_vulkan_is_handle(const void *h) { return h == KLVK_HANDLE; }

void *kl_vulkan_sym(const char *name) {
    vk_init();
    if (!name || !g_avail) return NULL;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (void *)klvk_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (void *)klvk_GetDeviceProcAddr;
    PFN_vkVoidFunction m = ours(name);
    if (m) return (void *)m;
    // MoltenVK exports the whole API statically, so a guest that resolves entry
    // points by dlsym rather than through vkGetInstanceProcAddr — which is legal
    // and which libSLZQuestNative does — gets a working pointer either way.
    return mvk_sym(name);
}

void *kl_vulkan_lookup(const char *name) {
    if (!name || strncmp(name, "vk", 2)) return NULL;
    return kl_vulkan_sym(name);
}

#endif /* __has_include(<vulkan/vulkan.h>) */
