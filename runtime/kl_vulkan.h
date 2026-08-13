// libvulkan.so — the synthetic Vulkan loader, over MoltenVK.
//
// BONELAB boots completely and cannot render because its graphics API is Vulkan
// (notes/BONELAB.md). Unity `dlopen`s `libvulkan.so` as the first thing it
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

#endif
