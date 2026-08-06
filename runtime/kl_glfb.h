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

#endif
