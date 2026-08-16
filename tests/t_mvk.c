// MoltenVK, RUNNING — the host half of `make mvk-check`.
//
// This is the vendoring gate for the synthetic `libvulkan.so` arc
//  (BONELAB boots completely and cannot render because its graphics API is Vulkan). It answers one question and answers it by doing it
// rather than by inspecting a file: **does the MoltenVK we vendored actually
// bring up Vulkan on Metal on this machine?**
//
// It exists because every cheaper answer is a false one. `otool -L` says the
// dylib is well formed. `nm` says it exports 431 vk* entry points. Neither says
// a physical device can be enumerated, and a MoltenVK that loads and then finds
// no Metal device is indistinguishable from a working one until something tries
// to render — which, on the arc this serves, is several thousand lines away.
//
// Deliberately NOT in `make check`. Like `make angle`, it needs a vendored
// dependency that `make mvk-fetch` provides, and `make check` is the gate that
// must run against a bare checkout.
//
// Everything is resolved through dlopen/dlsym rather than linked, for the same
// reason kl_glfb.c dlopens ANGLE: the shipping consumer will load this by path
// out of the app bundle, so loading it by path here is the shape under test.
// VK_NO_PROTOTYPES keeps the header from declaring anything the linker would
// then want.
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static int g_fail;
static void ck(int ok, const char *what) {
    if (!ok) g_fail++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// The default is the retarget script's own output path. KL_MVK_DYLIB overrides
// it, which is what a bisect against another MoltenVK build wants.
static const char *mvk_path(void) {
    const char *p = getenv("KL_MVK_DYLIB");
    return p && *p ? p : "vendor-moltenvk/out/macos/libMoltenVK.dylib";
}

int main(void) {
    printf("=== MoltenVK vendoring gate\n");

// MoltenVK logs its whole device census at info level on stderr, which
    // buries the eleven lines this gate exists to produce. It reads the level
    // from the environment when it loads, so this has to precede the dlopen.
    // KL_MVK_VERBOSE=1 puts it back — the census is genuinely worth reading the
    // first time on a new machine, and it is the fastest answer to "which GPU
    // and which Metal feature set".
    if (!getenv("KL_MVK_VERBOSE")) setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);

    const char *path = mvk_path();
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        printf("  FAIL dlopen(%s): %s\n", path, dlerror());
        printf("\n  Run 'make mvk' first — the prebuilt is not committed.\n");
        return 2;
    }
    printf("  ok   dlopen %s\n", path);

    // vkGetInstanceProcAddr is the only symbol worth resolving by name; Vulkan's
    // whole contract is that everything else comes from it. Resolving the rest
    // with dlsym would work here and would NOT be the thing the guest does.
    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    ck(gipa != NULL, "vkGetInstanceProcAddr resolves");
    if (!gipa) return 2;

    // define GIPA(inst, name) ((PFN_##name)gipa((inst), #name))

    PFN_vkEnumerateInstanceExtensionProperties enum_ext =
        GIPA(VK_NULL_HANDLE, vkEnumerateInstanceExtensionProperties);
    PFN_vkCreateInstance create_inst = GIPA(VK_NULL_HANDLE, vkCreateInstance);
    ck(enum_ext != NULL, "vkEnumerateInstanceExtensionProperties from the loader");
    ck(create_inst != NULL, "vkCreateInstance from the loader");
    if (!enum_ext || !create_inst) return 2;

// The instance extension list. VK_EXT_metal_surface is the one that matters
    // downstream and is asserted rather than merely printed: it is how a
    // CAMetalLayer becomes a VkSurfaceKHR, and its absence would only surface
    // once the compositor seam is being written.
    uint32_t n = 0;
    enum_ext(NULL, &n, NULL);
    VkExtensionProperties *exts = calloc(n ? n : 1, sizeof *exts);
    enum_ext(NULL, &n, exts);
    printf("  ..   %u instance extensions\n", n);

    int have_metal_surface = 0, have_surface = 0, have_portability = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(exts[i].extensionName, "VK_EXT_metal_surface")) have_metal_surface = 1;
        if (!strcmp(exts[i].extensionName, "VK_KHR_surface")) have_surface = 1;
        if (!strcmp(exts[i].extensionName, "VK_KHR_portability_enumeration")) have_portability = 1;
    }
    ck(have_surface, "VK_KHR_surface");
    ck(have_metal_surface, "VK_EXT_metal_surface (CAMetalLayer -> VkSurfaceKHR)");

    // MoltenVK is a *portability* driver — it does not implement all of Vulkan
    // 1.x, and since the portability extension was promoted a conformant loader
    // will not enumerate it unless the flag says non-conformant drivers are
    // wanted. Setting it is not optional bookkeeping: without the flag
    // vkEnumeratePhysicalDevices legitimately answers zero devices, which reads
    // exactly like "Metal is unavailable".
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "klepton-mvk-gate",
        .apiVersion = VK_API_VERSION_1_0,
    };
    const char *want[] = { "VK_KHR_portability_enumeration" };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    if (have_portability) {
        ici.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        ici.enabledExtensionCount = 1;
        ici.ppEnabledExtensionNames = want;
    }

    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = create_inst(&ici, NULL, &inst);
    ck(r == VK_SUCCESS && inst != VK_NULL_HANDLE, "vkCreateInstance");
    if (r != VK_SUCCESS) { printf("       VkResult %d\n", r); return 2; }

    PFN_vkEnumeratePhysicalDevices enum_dev = GIPA(inst, vkEnumeratePhysicalDevices);
    PFN_vkGetPhysicalDeviceProperties dev_props = GIPA(inst, vkGetPhysicalDeviceProperties);
    ck(enum_dev && dev_props, "physical-device entry points from the instance");
    if (!enum_dev || !dev_props) return 2;

    // The measurement the whole gate exists for: a real GPU, named by Metal.
    uint32_t ndev = 0;
    enum_dev(inst, &ndev, NULL);
    ck(ndev > 0, "at least one physical device (Metal is reachable)");
    if (!ndev) return 2;

    VkPhysicalDevice *devs = calloc(ndev, sizeof *devs);
    enum_dev(inst, &ndev, devs);

    int saw_named = 0;
    for (uint32_t i = 0; i < ndev; i++) {
        VkPhysicalDeviceProperties p;
        dev_props(devs[i], &p);
        printf("  ..   device %u: %s — Vulkan %u.%u.%u, driver 0x%x\n", i, p.deviceName,
               VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
               VK_VERSION_PATCH(p.apiVersion), p.driverVersion);
        if (p.deviceName[0]) saw_named = 1;
    }
    ck(saw_named, "the device reports a name");

    PFN_vkDestroyInstance destroy = GIPA(inst, vkDestroyInstance);
    if (destroy) destroy(inst, NULL);

    printf("\n%s — %d failure(s)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
