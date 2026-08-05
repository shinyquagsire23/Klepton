// The one-eye reference renderer. See kl_glfb.c for why this is host-only and
// cannot ship: visionOS is Metal-only, so the device path is ANGLE or native
// Metal. This exists to produce the known-good frame those will be diffed against.
//
// Opt-in with KL_GLFB=1; without it the null driver in kl_egl.c is unchanged.
#ifndef KL_GLFB_H
#define KL_GLFB_H

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

// Read the eye framebuffer back and write <dir>/frame_NNN.png. Returns the number
// of frames presented so far.
unsigned kl_glfb_present(const char *dir);

#endif
