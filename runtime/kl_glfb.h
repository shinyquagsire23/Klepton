// The one-eye reference renderer. See kl_glfb.c for why this is host-only and
// cannot ship: visionOS is Metal-only, so the device path is ANGLE or native
// Metal. This exists to produce the known-good frame those will be diffed against.
//
// Opt-in with KL_GLFB=1; without it the null driver in kl_egl.c is unchanged.
#ifndef KL_GLFB_H
#define KL_GLFB_H
#include <stdint.h>
#include <stdio.h>

int  kl_glfb_enabled(void);
int  kl_glfb_init(void);
void kl_glfb_set_size(int w, int h);

// The host's GL entry point for `name`, or NULL to mean "keep your own answer" —
// which is what the capability queries get, since the guest must go on believing
// it is driving GLES 3.2.
void *kl_glfb_sym(const char *name);

// Bind the context to the calling thread. Call from eglMakeCurrent: a GL context
// is per-thread, and the guest's render thread is not the one that created it.
void kl_glfb_make_current(void);

// The release half: call from eglMakeCurrent when the guest makes no context
// current. Migration mode frees the root context for the next thread.
void kl_glfb_release_current(void);

// Read the eye framebuffer back and write <dir>/frame_NNN.png. Returns the number
// of frames presented so far. When a frame sink is registered the buffer goes
// there instead (dir may then be NULL) and no PNG is written.
unsigned kl_glfb_present(const char *dir);

// The frontend seam, frame out. Registering a sink turns the per-swap capture
// into a handoff: instead of PNG-encoding to KL_GLFB_OUT, the readback buffer
// (bottom-up RGBA rows, exactly as glReadPixels produced it) is handed to `fn`
// on the GL thread, and a capture is requested on EVERY eglSwapBuffers. The
// sink must be fast — it runs inside the guest's frame — and must not call back
// into GL or SDL; memcpy and go. With no sink registered the KL_GLFB_OUT PNG
// behaviour is byte-identical to before.
typedef void (*kl_glfb_frame_sink)(const uint8_t *rgba, int w, int h, void *ctx);
void kl_glfb_set_frame_sink(kl_glfb_frame_sink fn, void *ctx);
int  kl_glfb_has_frame_sink(void);

// Lit-pixel count (luma > 12) of the most recent readback — the same diagnostic
// the PNG path prints, kept available so a sink frontend can tell a blank frame
// from a dead pipeline.
unsigned long kl_glfb_last_frame_lit(void);

// ---------------------------------------------------------------------------
// The frame-out seam, GPU edition — what a hardware compositor needs instead of
// the sink above. The sink hands over *pixels*, which costs a glReadPixels of
// the whole eye (a full pipeline stall plus ~40 MB at 2198x2304 RGBA16F) and a
// tone map and two memcpys before anything is displayed. A compositor that
// samples the eye's MTLTexture directly needs none of that; what it needs is
// *ordering*, because the guest's rendering is in ANGLE's Metal command queue
// and the composite is in ours, and Metal does not order across queues.
//
// So: register an id<MTLSharedEvent>, and each swap encodes a signal of it into
// ANGLE's command stream after the guest's frame (EGL_ANGLE_metal_shared_event
// _sync) and flushes. The compositor reads the value below and does
// encodeWaitForEvent: before its pass. Nothing is read back, nothing is copied.
//
// Registering a fence takes priority over the CPU sink: they are two answers to
// the same question and the swap does one or the other, never both.
void kl_glfb_set_gpu_fence(void *mtl_shared_event);
int  kl_glfb_has_gpu_fence(void);

// The value the most recent swap's signal will reach, or 0 before the first.
// Read from the compositor thread; monotonically increasing, so an unchanged
// value means "no new guest frame since you last looked".
uint64_t kl_glfb_gpu_fence_value(void);

// Print the internalformats the guest allocated immutable texture storage with.
// KL_GLFB_SKIP bisected the AGX abort to glTexStorage* + glTexSubImage*, which
// makes this the population that matters. Safe to call when nothing was allocated.
void kl_glfb_report_formats(void);

// The GL object census (KL_GL_CENSUS=<swaps>): per class, how many objects the
// guest has created and how many it has deleted, plus the immutable texture and
// renderbuffer storage still live. Every gen/delete goes straight to ANGLE, so
// without this nothing at the seam can say whether the guest's GL objects are
// being reclaimed — which is the first question when memory climbs across scene
// loads. A no-op unless KL_GL_CENSUS is set; safe to call at any time.
void kl_glfb_gl_census(FILE *f);

// The eye-texture seam, texture identity out. kl_ovrp's SetupEyeTexture2 calls
// this with the GL texture name Unity handed down; the capture finds "the
// framebuffer with the picture" by looking for the FBO whose color attachment
// is one of these (fb0 is black by construction — the VR frame goes to eye
// textures, not the backbuffer).
void kl_glfb_note_eye_texture(int eye, int stage, uint32_t tex);

// ...and which of an eye's images the guest most recently PRESENTED. Registering
// a swapchain leaves "the eye texture" naming whichever image was registered
// last — one of three, so the capture reads the picture the guest drew one frame
// in three and black the rest. The OpenXR path knows which image it presented
// (xrReleaseSwapchainImage names it) and says so here.
void kl_glfb_set_live_eye_texture(int eye, uint32_t tex);

// ...and the teardown half, from ovrp_DestroyEyeTexture. Drops both references
// that keep an eye texture's storage alive — the EGLImage and the GL texture —
// so the provider's own release on reallocation can actually reclaim it. A
// no-op for a slot that was never bound; out-of-range says so and does nothing.
void kl_glfb_release_eye_texture(int eye, int stage);

// Which swapchain stage the guest most recently RENDERED into, or -1 if that
// has not been observed.
//
// This exists because the stage is the guest's choice and we had been guessing
// it. With one stage the guess could not be wrong; with more than one it can,
// and a wrong answer is not a missing picture but a *stale* one paired with a
// *fresh* pose — reprojection then corrects by the wrong delta, which is
// judder. So the stage is measured instead: kl_ovrp registers each (eye,stage)
// texture name above, and the framebuffer thunks watch which of those names is
// the draw target. See kl_ovrp.c's EndFrame, which files the frame's pose
// record under whatever this reports.
int kl_glfb_last_render_stage(void);

// ...and the same observation with a window around it, which is what makes it
// checkable rather than merely available.
//
// `kl_glfb_last_render_stage` is sticky, so it answers even for a frame that
// drew into no eye texture at all — and that answer is the PREVIOUS frame's
// stage, which is exactly the off-by-one that pairs a fresh pose with a stale
// picture and shows up as doubling on head rotation. A sticky global cannot
// report that it did not know; a window can.
//
// kl_ovrp opens a window at ovrp_BeginFrame and reads it at ovrp_EndFrame:
//
//   *mask   bit per stage bound as the draw target inside the window. Exactly
//           one bit is the healthy case; zero means nothing was observed and
//           the returned stage belongs to another frame; more than one means
//           the frame committed to several stages and "one stage per frame" is
//           not true for this guest.
//   *binds  how many eye binds happened, for telling "did not draw" from "drew
//           once" from "bounced".
//   *tid    the thread that did the last of them. If it is not the thread
//           calling EndFrame, the window bounds nothing and the association
//           needs ordering rather than better observation.
//
// The return value is the last stage observed, sticky, as before.
void     kl_glfb_begin_render_window(void);
int      kl_glfb_render_stages(uint32_t *mask, uint32_t *binds, uint64_t *tid);

// How many times `stage` has been committed to as the draw target over the
// whole run. This is pure observation — it does not pass through the pose
// bookkeeping — so a stage that stops being drawn into is visible here even
// when every other counter still looks healthy.
uint64_t kl_glfb_stage_draw_count(int stage);

// ---------------------------------------------------------------------------
// The Metal interop seam (P5). Proven on host and device before any of this
// existed — `make mtltex` and device probe P13; PLANNING §12.9.
//
// On visionOS the guest's eye textures must live in MTLTextures the compositor
// can sample, so instead of allocating GL storage for the texture name Unity
// hands down, we back it with an MTLTexture the host allocated:
//
//     glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, h, w)   // host / no provider
//     glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image)   // with a provider
//
// With no provider registered nothing below changes behaviour, which is why the
// macOS path and `make check` are unaffected.

// What the host supplies for one (eye, stage). `texture` is an id<MTLTexture>
// which the host must keep alive for as long as the guest holds the eye texture.
// `slice` addresses an array slice, which is how the two eyes arrive in a
// layered drawable; 0 for a plain 2D texture.
//
// `w`/`h` are the texture's ACTUAL dimensions, and the provider must fill them:
// EGL_ANGLE_metal_texture_client_buffer takes the size from the MTLTexture and
// ignores everything we say about it, so a provider that returns a texture of
// the wrong size gets a successful eglCreateImageKHR and a guest rendering into
// storage smaller than it believes it has. That is exactly what happened the
// first time this ran — Unity re-creates its eye textures at a *different* size
// partway through startup (measured: 1832x1920, then 2198x2304), the provider
// handed back its one cached texture, and the result was a partially-filled
// frame with nothing anywhere reporting a problem. So kl_glfb refuses a
// mismatch rather than trusting the caller.
typedef struct { void *texture; int slice; int w, h; } kl_mtl_eye_texture;

// Called when a guest asks for eye storage — ovrp_SetupEyeTexture2 for Unity,
// xrEndFrame's eye assertion for an OpenXR guest. Return non-zero having filled
// `out`, or 0 to let the guest get ordinary GL storage. w/h are the dimensions
// the texture must have — note they arrive already transposed relative to
// ovrp's arguments; see kl_ovrp.c's SetupEyeTexture2.
//
// **`internal_fmt` is not advisory.** ANGLE compares the GL internal format we
// claim against the MTLTexture's own pixel format and refuses the pair outright
// — `TextureImageSiblingMtl::ValidateClientBuffer`, "Incompatible format" — so
// a provider that always allocates RGBA16F simply cannot back a guest that
// asked for anything else. Unity asks for RGBA16F and Steam Link asks for
// SRGB8_ALPHA8, which is why this is a parameter rather than a constant.
typedef int (*kl_glfb_mtl_provider)(int eye, int stage, int w, int h,
                                    uint32_t internal_fmt,
                                    kl_mtl_eye_texture *out, void *ctx);
void kl_glfb_set_mtl_provider(kl_glfb_mtl_provider fn, void *ctx);
int  kl_glfb_has_mtl_provider(void);

// ANGLE's own MTLDevice (an id<MTLDevice>), or NULL. The host MUST allocate on
// this device: EGL_ANGLE_metal_texture_client_buffer requires the texture come
// from the display's device, and passing one from MTLCreateSystemDefaultDevice()
// fails with EGL_BAD_PARAMETER for a reason nothing else would explain.
void *kl_glfb_mtl_device(void);

// Ask the registered provider for (eye, stage) and give `gl_tex` storage backed
// by what it returns. Returns 1 if the texture is now MTLTexture-backed, 0 if
// the caller should allocate GL storage itself. `internal_fmt` is the GL
// internalformat the guest asked for.
int kl_glfb_bind_eye_mtl_texture(int eye, int stage, uint32_t gl_tex,
                                 int w, int h, uint32_t internal_fmt);

// The MTLTexture currently backing (eye, stage), or NULL — what the compositor
// pass samples, and what a test reads back to check the guest's frame arrived.
void *kl_glfb_eye_mtl_texture(int eye, int stage, int *out_slice);

// ---------------------------------------------------------------------------
// The decoded-video image (SL-13) — the guest's eglCreateImageKHR over an
// AHardwareBuffer, which here is a VideoToolbox CVPixelBuffer. kl_egl.c serves
// the guest-facing entry points; the ANGLE work is here because this file owns
// the display, the config and the entry-point resolution.
//
// `pixels` is a CVPixelBufferRef (void* so callers need not include CoreVideo).
// NULL means the frame could not be made samplable and the caller must say so to
// the guest rather than hand it an image that will not sample. See kl_glfb.c for
// why this is an IOSurface pbuffer and not an EGLImage: ANGLE's Metal backend
// has neither external images nor Android buffers.
void *kl_glfb_image_from_pixels(void *pixels, int *out_w, int *out_h);
void  kl_glfb_image_destroy(void *image);

// Bind `image` to whatever the current context has bound to GL_TEXTURE_2D —
// which is where the guest's glBindTexture(GL_TEXTURE_EXTERNAL_OES, ...) landed
// after klfb_detarget. This is what glEGLImageTargetTexture2DOES becomes.
int   kl_glfb_image_bind(void *image);

// Is this handle one of ours? Asked of pointers the guest chose, so it is a
// registry lookup and not a magic-word peek through a foreign pointer.
int   kl_glfb_is_image(const void *h);

// ---------------------------------------------------------------------------
// Foveation — variable rasterization rate for the guest's own rendering.
//
// `rate_map` is an id<MTLRasterizationRateMap> the PLATFORM built, on the same
// MTLDevice as the eye textures, for a screen size of `w` x `h`. The runtime
// never constructs one, for the same reason it never constructs an MTLTexture:
// that keeps this file plain C and puts the Metal object where the display
// numbers are (KleptonCompositor.swift on device, t_mtl_provider.m on host).
// NULL turns foveation off again.
//
// Two registrations, because the guest's per-eye chain has two render targets
// and they must share ONE map (measured, PLANNING §12.22):
//
//   * the eye texture, by identity, as each one is bound;
//   * the guest's multisampled SCENE target, by size — ANGLE allocates that
//     renderbuffer itself in response to glRenderbufferStorageMultisample, so
//     there is no object anyone outside ANGLE can name. The rule is qualified
//     by sample count so it hits only that target: the guest also allocates
//     single-sampled post-processing intermediates at the eye size, and
//     foveating one of THOSE is a wrong picture, because the guest samples it
//     with screen-space coordinates that know nothing about the warp.
//
// Sharing the map is what keeps the resolve between them a physical-to-physical
// copy rather than a second warp — see the passthrough mode in
// angle-patches/klepton.patch, and `make vrr` Q6, which measures it.
//
// **The eye texture is left in a WARPED layout by this**, so whoever samples it
// has to unwarp: the compositors, and every readback path
// (`KL_GLFB_OUT`, t_mtl_provider). Until those do, leaving this unset is the
// only correct configuration.
// `zones_x`/`zones_y` are the rate layer's sample counts — the number of equal
// screen-space regions the map divides each axis into. They travel with the map
// because a built MTLRasterizationRateMap does not report them, and the
// compositor's unwarp needs them: a rate map is piecewise linear with its
// breakpoints at those boundaries, so an unwarp grid of exactly this many cells
// is EXACT, where any other count only samples the curve. See kl_reproject.h.
void kl_glfb_set_eye_rate_map(int w, int h, int zones_x, int zones_y, void *rate_map);

// The zone counts the map in force was built with; both 0 when there is none.
void kl_glfb_eye_rate_zones(int *zones_x, int *zones_y);

// The map in force, or NULL. What a compositor's unwarp needs to build its
// coordinate conversion from, and how a readback path knows the eye texture is
// not an ordinary image.
void *kl_glfb_eye_rate_map(void);

// The foveation POLICY — how many zones and how hard the periphery falls off —
// held here rather than in either builder, because there are two of them
// (t_mtl_provider.m on the host, KleptonCompositor.swift on device) and a
// device that foveated differently from the host would make every host
// measurement a lie about the thing being shipped. The Metal object is still
// built where the display numbers are; only the numbers that describe it are
// shared.
//
// Returns 1 when foveation is asked for (`KL_VRR`), 0 otherwise. `*zones` gets
// the per-axis zone count (`KL_VRR_ZONES`, default 16) and `*edge` the rate at
// the border (`KL_VRR_EDGE`, default 0.35); both are clamped to what a rate map
// will accept. Either pointer may be NULL.
int kl_glfb_foveation_wanted(int *zones, float *edge);

// The rate layer's per-zone quality along one axis: 1.0 at the centre falling
// linearly to `edge` at the border, `n` entries. Both axes get the same curve,
// so this is called once per axis with the same arguments.
void kl_glfb_foveation_quality(float *q, int n, float edge);

// The largest zone count kl_glfb_foveation_wanted will report, so a caller can
// size a stack array for the curve above without allocating.
#define KL_FOVEATION_MAX_ZONES 64

#endif
