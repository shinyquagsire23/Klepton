// The viewer's hardware compositor — the macOS twin of KleptonCompositor.swift.
//
// kl_view.c's original frame path was the CPU one: kl_glfb read the eye back
// with glReadPixels, tone-mapped it, handed the buffer to a sink, which memcpy'd
// it, which the SDL loop row-flipped and uploaded to a streaming texture. At
// 2198x2304 RGBA16F that is a full pipeline stall plus ~40 MB read + 20 MB tone
// map + 40 MB of memcpy + 20 MB upload, every frame, to display something the
// GPU already had.
//
// This is the same seam done in hardware, and deliberately the same shape as
// KleptonCompositor.swift: the guest's eye textures are MTLTextures we allocated
// (the interop `make mtltex` gates), and a Metal pass samples one into the
// window's CAMetalLayer drawable with the GL->Metal vertical flip. Zero copies,
// zero readback. Ordering across ANGLE's command queue and ours comes from an
// MTLSharedEvent (see kl_glfb_set_gpu_fence).
//
// Host-only, like kl_view.c and tests/t_mtl_provider.m: named by the t_boot rule
// alone, never in RUNTIME_SHIP. Objective-C because it creates Metal objects;
// kl_view.c stays plain C and passes the CAMetalLayer through as a void *.
#ifndef KL_VIEW_MTL_H
#define KL_VIEW_MTL_H

// Bring the compositor up on `metal_layer` (a CAMetalLayer *, from SDL's
// SDL_Metal_GetLayer). Returns 1 when the hardware path is live, 0 when the
// caller should stay on the CPU sink — no ANGLE MTLDevice, no eye textures yet,
// or a pipeline that would not compile. Safe to call repeatedly until it
// succeeds, which is how kl_view.c starts it lazily: the guest has to have
// reached ovrp_SetupEyeTexture2 before there is anything to composite.
int kl_viewmtl_start(void *metal_layer);

// Composite the newest guest frame into the layer's next drawable, letterboxed
// into `win_w` x `win_h` pixels. Returns 1 if a frame was presented, 0 if there
// was nothing new (the guest has not swapped since the last call) — the caller
// leaves the previous frame on screen rather than re-presenting it.
int kl_viewmtl_present(int win_w, int win_h);

void kl_viewmtl_stop(void);

// Frames this compositor has presented.
unsigned kl_viewmtl_frames(void);

// ...and the GUEST frame the last of them showed. Monotonic, 0 before the first
// composite. It is kl_glfb's fence value for a GL guest and kl_vulkan's frame
// serial for a Vulkan one, which is exactly why the HUD asks here instead of
// reading either directly — the two are the same quantity and only this file
// knows which one is live.
unsigned long long kl_viewmtl_guest_frame(void);

// Lit-pixel count of the last composited frame, on kl_glfb's definition
// (tone-mapped channel sum > 12), estimated from a 64x64 downsample of the same
// pass and scaled to the eye's pixel count so the HUD line stays comparable to
// the readback path's exact figure. This is what replaces
// kl_glfb_last_frame_lit() when nothing is read back.
unsigned long kl_viewmtl_lit(void);

#endif
