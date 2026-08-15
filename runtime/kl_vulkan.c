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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kl_vulkan.h"
#include "kl_env.h"

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
void  kl_vulkan_capture_eyes(unsigned f, int s) { (void)f; (void)s; }
void  kl_vulkan_frame_done(int s) { (void)s; }
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
static int g_init_done, g_avail;

static void *mvk_sym(const char *n) { return g_mvk ? dlsym(g_mvk, n) : NULL; }

static void vk_init(void) {
    if (g_init_done) return;
    g_init_done = 1;

    // MoltenVK logs its whole device census at info level, which buries
    // everything this port prints. Same choice as tests/t_mvk.c, same escape.
    if (!kl_env_on("KL_MVK_VERBOSE", 0)) setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);

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
static int          g_ndev;
static VkInstance   g_instance;
static unsigned     g_presented, g_captured;

void kl_vulkan_stats(unsigned *p, unsigned *c) {
    if (p) *p = g_presented;
    if (c) *c = g_captured;
}

static klvk_device *dev_find(VkDevice d) {
    for (int i = 0; i < g_ndev; i++) if (g_devs[i]->dev == d) return g_devs[i];
    return NULL;
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
    VkExtensionProperties *tmp = calloc(n + 2, sizeof *tmp);
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

    VkResult r = VK_SUCCESS;
    if (!props) *count = n;
    else {
        uint32_t give = *count < n ? *count : n;
        memcpy(props, tmp, give * sizeof *props);
        if (give < n) r = VK_INCOMPLETE;
        *count = give;
    }
    free(tmp);
    return r;
}

static VkResult klvk_CreateInstance(const VkInstanceCreateInfo *ci,
                                    const VkAllocationCallbacks *alloc,
                                    VkInstance *out) {
    vk_init();
    if (!g_avail) return VK_ERROR_INITIALIZATION_FAILED;
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
        if (!strcmp(e, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
            VKT("instance ext %s -> ours (MoltenVK has no Android WSI)\n", e);
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

    VkResult r = real(&mine, alloc, out);
    free(have);
    if (r == VK_SUCCESS) {
        g_instance = *out;
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

static VkResult klvk_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                                  const VkAllocationCallbacks *alloc, VkDevice *out) {
    PFN_vkCreateDevice real = (PFN_vkCreateDevice)real_gipa(g_instance, "vkCreateDevice");
    PFN_vkEnumerateDeviceExtensionProperties enum_ext =
        (PFN_vkEnumerateDeviceExtensionProperties)
            real_gipa(g_instance, "vkEnumerateDeviceExtensionProperties");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

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
        ext_add(&keep, e);
    }
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
    dev_resolve(d);
    // A queue of our own, for the acquire signal and the capture submit. Taken
    // from the guest's own first queue family, which is the one it will present
    // on — see the note in klvk_AcquireNextImageKHR about the sharing.
    if (d->GetDeviceQueue) d->GetDeviceQueue(d->dev, d->queue_family, 0, &d->queue);

    pthread_mutex_lock(&g_lock);
    if (g_ndev < (int)(sizeof g_devs / sizeof *g_devs)) g_devs[g_ndev++] = d;
    pthread_mutex_unlock(&g_lock);

    VKI("vkCreateDevice ok — %u extension(s), queue family %u\n", keep.n, d->queue_family);
    return VK_SUCCESS;
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

static const char *cap_dir(void) { return kl_env_str("KL_VK_OUT", NULL); }

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
    // Alpha is not a coverage value here — trap 33's shape, one API over. A
    // guest that never authors alpha leaves it 0, and a correct RGB frame then
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
static struct {
    VkImage        img;
    VkDeviceMemory mem;
    uint32_t       w, h;
    uint32_t       layers;
    VkFormat       fmt;
} g_eye[KLVK_EYE_STAGES][2];

int kl_vulkan_guest_active(void) { return g_avail && g_ndev > 0; }

// ---------------------------------------------------------------------------
// The compositor seam — the MTLTexture behind an eye VkImage
// ---------------------------------------------------------------------------
//
// P5's compositors (KleptonCompositor.swift on device, kl_view_mtl.m on the
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
    return img;
}

unsigned long long kl_vulkan_eye_image_layers(int stage, int eye, unsigned w, unsigned h,
                                              int srgb, int layers) {
    if (!kl_vulkan_guest_active()) return 0;
    if (stage < 0 || stage >= KLVK_EYE_STAGES || eye < 0 || eye > 1) return 0;
    if (layers < 1) layers = 1;
    if (layers > 2) layers = 2;
    // Under the Array layout there is one image for the stage and both eyes get
    // it; slot 0 owns it, so the eye argument selects a LAYER later and not a
    // second allocation here.
    if (layers > 1) eye = 0;
    klvk_device *d = g_devs[0];
    if (g_eye[stage][eye].img) return (uint64_t)(uintptr_t)g_eye[stage][eye].img;

    VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
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
    if (kl_env_on("KL_VK_EYE_TINT", 0) && d->CmdClearColorImage &&
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
        for (int ly = 0; ly < layers; ly++) {
            VkImageSubresourceRange rng =
                { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, (uint32_t)ly, 1 };
            d->CmdClearColorImage(d->cap_cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  &TINT[layers > 1 ? ly : (eye & 1)], 1, &rng);
        }
        d->EndCommandBuffer(d->cap_cmd);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1, .pCommandBuffers = &d->cap_cmd };
        d->ResetFences(d->dev, 1, &d->cap_fence);
        if (d->QueueSubmit(d->queue, 1, &si, d->cap_fence) == VK_SUCCESS)
            d->WaitForFences(d->dev, 1, &d->cap_fence, VK_TRUE, 2000000000ull);
        VKI("eye image stage %d pre-cleared: %s (KL_VK_EYE_TINT)\n", stage,
            layers > 1 ? "layer 0 GREEN, layer 1 BLUE"
                       : (eye ? "eye 1 BLUE" : "eye 0 GREEN"));
    }

    g_eye[stage][eye].img = img;
    g_eye[stage][eye].mem = mem;
    g_eye[stage][eye].w = w;
    g_eye[stage][eye].h = h;
    g_eye[stage][eye].layers = (uint32_t)layers;
    g_eye[stage][eye].fmt = fmt;
    eye_publish_mtl(stage, eye, layers);
    VKI("eye image stage %d %s = VkImage %p (%ux%u %s)\n", stage,
        layers > 1 ? "both eyes (2 array layers)" :
                     (eye ? "eye 1" : "eye 0"),
        (void *)img, w, h, srgb ? "R8G8B8A8_SRGB" : "R8G8B8A8_UNORM");
    return (uint64_t)(uintptr_t)img;
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
// is only asserted later, at xrEndFrame, from the projection layer. That is
// SL-19's finding and it is a property of the API, not of a guest.
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

void kl_vulkan_capture_eyes(unsigned frame, int stage) {
    const char *dir = cap_dir();
    if (!dir || !kl_vulkan_guest_active()) return;
    int every = kl_env_int("KL_VK_OUT_EVERY", 1);
    if (every < 1) every = 1;
    if (frame % (unsigned)every) return;
    if (stage < 0 || stage >= KLVK_EYE_STAGES) return;

    klvk_device *d = g_devs[0];
    // Under the Array layout slot 0 holds the only image and the two eyes are
    // its layers, so the eye index selects a layer there and an image here. The
    // filename stays `eyeN` either way — it names what was drawn, not where it
    // was stored.
    int layered = g_eye[stage][0].img && g_eye[stage][0].layers > 1;
    for (int eye = 0; eye < 2; eye++) {
        int slot = layered ? 0 : eye;
        if (!g_eye[stage][slot].img) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/vk_f%05u_s%d_eye%d.png", dir, frame, stage, eye);
        // The guest submitted this to a compositor, so it left it readable by
        // one — SHADER_READ_ONLY_OPTIMAL. See cap_image on why a wrong guess
        // here is cheap on MoltenVK specifically.
        if (cap_image_layer(d, d->queue, g_eye[stage][slot].img, g_eye[stage][slot].w,
                            g_eye[stage][slot].h, g_eye[stage][slot].fmt,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            layered ? (uint32_t)eye : 0u, path, NULL, 0))
            g_presented++;
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

void kl_vulkan_frame_done(int stage) {
    (void)stage;
    if (!kl_vulkan_guest_active()) return;
    klvk_device *d = g_devs[0];
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

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
typedef struct { const char *name; PFN_vkVoidFunction fn; } entry;

static const entry g_ours[] = {
#define E(n) { "vk" #n, (PFN_vkVoidFunction)klvk_##n }
    E(CreateInstance), E(EnumerateInstanceExtensionProperties),
    E(CreateDevice),
    E(CreateAndroidSurfaceKHR), E(DestroySurfaceKHR),
    E(GetPhysicalDeviceSurfaceSupportKHR), E(GetPhysicalDeviceSurfaceCapabilitiesKHR),
    E(GetPhysicalDeviceSurfaceFormatsKHR), E(GetPhysicalDeviceSurfacePresentModesKHR),
    E(CreateSwapchainKHR), E(DestroySwapchainKHR), E(GetSwapchainImagesKHR),
    E(AcquireNextImageKHR), E(QueuePresentKHR),
#undef E
};

static PFN_vkVoidFunction ours(const char *name) {
    for (size_t i = 0; i < sizeof g_ours / sizeof *g_ours; i++)
        if (!strcmp(g_ours[i].name, name)) return g_ours[i].fn;
    return NULL;
}

static PFN_vkVoidFunction VKAPI_CALL klvk_GetInstanceProcAddr(VkInstance inst,
                                                              const char *name);
static PFN_vkVoidFunction VKAPI_CALL klvk_GetDeviceProcAddr(VkDevice dev,
                                                            const char *name);

static PFN_vkVoidFunction VKAPI_CALL klvk_GetInstanceProcAddr(VkInstance inst,
                                                              const char *name) {
    vk_init();
    if (!name) return NULL;
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)klvk_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)klvk_GetDeviceProcAddr;
    PFN_vkVoidFunction m = ours(name);
    if (m) return m;
    if (!g_avail) return NULL;
    return real_gipa(inst ? inst : g_instance, name);
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
    return real_gdpa(dev, name);
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
