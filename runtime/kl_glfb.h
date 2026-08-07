// The one-eye reference renderer. See kl_glfb.c for why this is host-only and
// cannot ship: visionOS is Metal-only, so the device path is ANGLE or native
// Metal. This exists to produce the known-good frame those will be diffed against.
//
// Opt-in with KL_GLFB=1; without it the null driver in kl_egl.c is unchanged.
#ifndef KL_GLFB_H
#define KL_GLFB_H
#include <stdint.h>

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

// Print the internalformats the guest allocated immutable texture storage with.
// KL_GLFB_SKIP bisected the AGX abort to glTexStorage* + glTexSubImage*, which
// makes this the population that matters. Safe to call when nothing was allocated.
void kl_glfb_report_formats(void);

// The eye-texture seam, texture identity out. kl_ovrp's SetupEyeTexture2 calls
// this with the GL texture name Unity handed down; the capture finds "the
// framebuffer with the picture" by looking for the FBO whose color attachment
// is one of these (fb0 is black by construction — the VR frame goes to eye
// textures, not the backbuffer).
void kl_glfb_note_eye_texture(int eye, uint32_t tex);

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

// Called from ovrp_SetupEyeTexture2 when Unity asks for eye storage. Return
// non-zero having filled `out`, or 0 to let the guest get ordinary GL storage.
// w/h are the dimensions the texture must have — note they arrive already
// transposed relative to ovrp's arguments; see kl_ovrp.c's SetupEyeTexture2.
typedef int (*kl_glfb_mtl_provider)(int eye, int stage, int w, int h,
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

#endif
