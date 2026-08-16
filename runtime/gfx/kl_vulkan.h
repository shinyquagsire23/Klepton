// libvulkan.so — the synthetic Vulkan loader, over MoltenVK.
//
// BONELAB boots completely and cannot render because its graphics API is Vulkan
//. Unity `dlopen`s `libvulkan.so` as the first thing it
// tries, and it is the only API this build probes: when the open fails, Unity
// warns that the device does not meet the application's hardware requirements
// and falls back to a GLES 2 context whose shaders do not exist in the blob.
// So this file is what turns the first domino.
//
// It is served, not loaded, for exactly the reason kl_ovrp and kl_openxr are:
// the real `libvulkan.so` is Android's loader and would go looking for an ICD
// through Android's driver plumbing. MoltenVK IS the ICD here, so this stands
// in for the loader and nothing else.
//
// **There is no ABI translation anywhere in this file, and that is a measured
// property rather than an assumption.** Vulkan's structs are plain C with
// explicit types and the guest is aarch64 LP64 little-endian, exactly like the
// host — so a `VkImageCreateInfo` the guest fills in is byte-identical to one
// this process would build. That is what makes a 182-entry-point API tractable
// as a forwarding table, and it is emphatically NOT true of the JNI, libc or
// OVRPlugin seams next door. `make vkabi` is the gate that keeps it true.
//
// Three things are genuinely ours rather than forwarded, and they are all the
// same thing wearing different hats — Android's window system does not exist:
//
//   1. `VK_KHR_android_surface` / `vkCreateAndroidSurfaceKHR`. MoltenVK has no
//      such extension and never will. The surface is synthesized.
//   2. the `VK_KHR_swapchain` family. Ours end to end, backed by ordinary
//      VkImages we allocate — no CAMetalLayer, no presentation engine, nothing
//      that has to be on a screen. This is what lets a frame be produced with
//      the compositor wiring still an open question.
//   3. `vkQueuePresentKHR`, which is where the frame is read back and dumped.
//
// Everything else is MoltenVK's, reached through its own `vkGetInstanceProcAddr`.
#ifndef KL_VULKAN_H
#define KL_VULKAN_H

// The synthetic-library quartet, same shape as kl_egl / kl_ovrp / kl_mediandk.
// See kl_dl.c's klb_dlopen and kl_can_dlopen for why `claims` exists separately
// from `dlopen`: an existence check the guest makes on its own behalf has to be
// answerable without loading anything.
void *kl_vulkan_dlopen(const char *soname);   // NULL if not one of ours
int   kl_vulkan_claims(const char *soname);   // the same test, without opening
int   kl_vulkan_is_handle(const void *h);
void *kl_vulkan_sym(const char *name);        // dlsym semantics: NULL when absent

// Tier lookup for kl_shim_lookup, for a guest that binds vk* at load time
// through a DT_NEEDED rather than through dlopen/dlsym. libunity takes the
// dlopen road; libSLZQuestNative names six vk* symbols directly.
void *kl_vulkan_lookup(const char *name);

// Was MoltenVK actually reachable? Separated from `claims` so the failure can be
// reported once, by name, at the point the guest asks — rather than as a NULL
// symbol somewhere inside Unity's device selection.
int   kl_vulkan_available(void);

// ---------------------------------------------------------------------------
// The eye-texture seam, for kl_ovrp.c
// ---------------------------------------------------------------------------
//
// BONELAB never creates a WSI swapchain: Unity's Oculus XR path takes its eye
// textures from `ovrp_GetLayerTexture2` and submits them to the Oculus
// compositor. kl_ovrp.c has always answered that with a GL texture name, and on
// a Vulkan guest the name goes straight into `vkCreateImageView` as a VkImage
// handle — a segfault inside MoltenVK with a small integer for an address.
//
// So on the Vulkan path the eye textures have to be real VkImages, and these
// are how kl_ovrp.c asks for them without knowing anything about Vulkan.

// Did the guest actually bring up a Vulkan device through us? This is the test
// kl_ovrp.c branches on, and it is a measurement of what the guest DID rather
// than a guess from a renderer enum: a guest that resolved vk* symbols and then
// chose GLES would answer false here, correctly.
int kl_vulkan_guest_active(void);

// Create (or return) the eye image for a swapchain stage and eye. The returned
// value is the VkImage handle widened to 64 bits, which is exactly what
// ovrp_GetLayerTexture2's out-parameter carries.
unsigned long long kl_vulkan_eye_image(int stage, int eye, unsigned w, unsigned h,
                                       int srgb);

// ...and the same thing for the layout where the two eyes are ARRAY LAYERS of
// one image rather than two images — ovrpLayout_Array, which is what Unity
// calls Single Pass Instanced / Multiview and what a Quest title normally ships
// with. `layers` is 2 there and 1 for Stereo. Under Array both eyes get the
// SAME handle back and the eye is the layer index, so kl_ovrp.c must not treat
// two identical handles as a mistake.
unsigned long long kl_vulkan_eye_image_layers(int stage, int eye, unsigned w, unsigned h,
                                              int srgb, int layers);

// ...and the general form, which is what `ovrp_GetLayerTexture2` actually asks:
// storage for ONE LAYER's stage/eye.
//
// The two above are this with `layer_key = KLVK_EYE_LAYER`, and until 2026-08-15
// they were the only form — so every layer the guest set up was handed the eye
// layer's images. A Unity guest never noticed (its one other layer is a 1x1
// dummy nothing renders into); an Unreal guest draws its splash and its stereo
// UI quads into real ones, and they landed in the corner of the eye texture.
//
// Only the eye layer is published into kl_glfb's eye table, because that table
// is what every compositor samples. A size that differs from the cached one
// re-allocates rather than answering with storage of the wrong shape.
#define KLVK_EYE_LAYER (-1)
unsigned long long kl_vulkan_layer_image(int layer_key, int stage, int eye,
                                         unsigned w, unsigned h, int srgb, int layers);

// ...and the MTLTexture behind one of those, for a compositor that has to draw
// the layer. The eye layer is deliberately NOT reachable here: its textures go
// through kl_glfb's eye table, where every consumer already looks, and a second
// door onto the same storage is how two answers start to differ.
//
// A layer allocated for eye 0 alone — the common case, one texture with a
// per-eye ViewportRect selecting the part — answers eye 1 from it rather than
// with nothing. NULL when the layer has no storage for that stage.
void *kl_vulkan_layer_mtl_texture(int layer_key, int stage, int eye,
                                  int *w, int *h);

// Write both eyes of a stage out as PNGs. Called at frame submission, which on
// this path is ovrp_EndFrame4 — the guest's own assertion that it has finished
// drawing them.
void kl_vulkan_capture_eyes(unsigned frame, int stage);

// "The guest has finished drawing this stage's eye textures." Called from the
// same place the capture is (ovrp_EndFrame4), and it is what a COMPOSITOR waits
// on: it makes the guest's queue idle and then advances a serial, so past that
// serial the picture in the eye MTLTexture is complete rather than merely
// submitted. `kl_vulkan_frame_serial` is the value — monotonic, 0 before the
// first frame, and exactly the role kl_glfb_gpu_fence_value plays on the GL
// path (where the wait is on the GPU instead). See the implementation for why
// this one is a CPU stall and what would remove it.
void kl_vulkan_frame_done(int stage);
unsigned long long kl_vulkan_frame_serial(void);

// How many frames have been presented, and how many were written out. The
// end-of-run report reads these; a run with a healthy frame loop and zero
// captures is a different bug from one that never presented.
void  kl_vulkan_stats(unsigned *presented, unsigned *captured);

// ---------------------------------------------------------------------------
// The OpenXR seam, for kl_openxr.c — XR_KHR_vulkan_enable
// ---------------------------------------------------------------------------
//
// Open Brush is the first guest at the junction of this project's two graphics
// halves: it speaks OPENXR (kl_openxr.c, which until now had only ever carried
// GLES guests) and its renderer is VULKAN (this file, which until now had only
// ever been reached through OVRPlugin). Neither half needed the other, so
// neither knew about it.
//
// The split below is the same one kl_openxr already keeps with kl_glfb: the XR
// file states WHAT it needs in OpenXR's vocabulary and this file answers it in
// Vulkan's, so kl_openxr.c includes no Vulkan header and stays linkable in a
// build with no MoltenVK vendored. Every Vulkan handle crossing this boundary is
// a `void *` because a dispatchable Vulkan handle IS a pointer on LP64 — the
// same measured property that makes the forwarding table upstairs possible.
//
// **Under XR_KHR_vulkan_enable the APP creates the VkInstance and VkDevice**,
// not the runtime (that inverts in `_enable2`), and it creates them through the
// synthetic loader in this file. So by the time the graphics binding arrives, a
// guest's device is already one klvk_CreateDevice interposed on — which is what
// makes the two extension lists below honestly empty.

// Is there a Vulkan device the OpenXR half could bind a session to? This gates
// whether XR_KHR_vulkan_enable is ADVERTISED at all: naming an extension we
// cannot back is the one thing the extension table upstairs refuses to do, and
// a checkout with no MoltenVK must fail the guest's xrCreateInstance rather
// than fail later, deeper, on a NULL entry point.
int kl_vulkan_xr_supported(void);

// The VkInstance / VkDevice extensions an app MUST enable for its Vulkan
// objects to be usable by this runtime, space-delimited as the spec wants them.
//
// Both are EMPTY, and that is a true answer rather than an unimplemented one:
// the only extension this runtime needs on the device is VK_EXT_metal_objects
// (how an eye VkImage's MTLTexture reaches the compositor), and klvk_CreateDevice
// already adds it to every device the guest creates. Requiring it of the app as
// well would be asking for something we have already taken.
const char *kl_vulkan_xr_instance_extensions(void);
const char *kl_vulkan_xr_device_extensions(void);

// The VkPhysicalDevice this runtime requires the app to render with, chosen
// from the app's OWN VkInstance (which is the handle the spec passes, and not
// necessarily one this file has seen). NULL when it cannot be answered.
void *kl_vulkan_xr_physical_device(void *vk_instance);

// The Vulkan API version range a session may be created against, as major/minor
// pairs. Answered from the physical device rather than asserted — MoltenVK's
// instance-level and device-level versions differ, and the one that governs an
// app's device is the device's.
void kl_vulkan_xr_api_range(unsigned *min_major, unsigned *min_minor,
                            unsigned *max_major, unsigned *max_minor);

// One OpenXR swapchain image, as a VkImage handle widened to 64 bits — which is
// exactly what XrSwapchainImageVulkanKHR carries. `vk_format` is a VkFormat, not
// a GL internal format: the two lists are different vocabularies for the same
// question and kl_openxr keeps one per graphics API. 0 on failure.
//
// Unlike kl_vulkan_eye_image this is not keyed on (stage, eye), because at
// xrCreateSwapchain time nothing knows which swapchain is an eye — see the
// implementation.
unsigned long long kl_vulkan_xr_image(unsigned w, unsigned h, unsigned layers,
                                      unsigned mips, long long vk_format, int depth);

// ...and the MTLTexture behind one, for the compositor seam. NULL when it cannot
// be exported.
void *kl_vulkan_xr_image_mtl(unsigned long long image);

#endif
