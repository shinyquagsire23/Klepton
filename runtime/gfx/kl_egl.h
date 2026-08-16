// The EGL surface libunity.so imports.
//
// Exactly the 19 symbols in the unresolved list and nothing else. Notably that
// list contains no GL entry points at all: `eglGetProcAddress` is in it, so
// Unity resolves the whole of GLES through that single function at runtime.
// That is the entire graphics surface behind one door, which is what makes
// measuring it cheap — see kl_egl.c.
#ifndef KL_EGL_H
#define KL_EGL_H
#include <stdio.h>
#include <stdint.h>

// Resolve an EGL import by name. NULL if we do not provide it.
void *kl_egl_lookup(const char *name);

// The GLES level the device description reports (version string, GLSL version,
// MAJOR/MINOR limits). Default 3.2 for Beat Saber; call before any GL traffic.
void kl_egl_set_gles_version(int major, int minor);

// Unity reaches GLES through *two* doors, not one. eglGetProcAddress is the
// documented one; the other is dlopen("libGLESv2.so") followed by dlsym, and
// when that dlopen failed Unity called straight through the resulting NULL —
// a jump to address 0 with nothing on the stack to say why. So the loader hands
// out a synthetic handle for the GL sonames and resolves symbols on it through
// the same gateway eglGetProcAddress uses.
void *kl_egl_dlopen(const char *soname);   // NULL if this is not a GL library
// Same question with no side effects, for callers that only need to know
// whether this gateway would serve the name. See kl_can_dlopen (klepton.h).
int   kl_egl_claims(const char *soname);
int   kl_egl_is_handle(const void *h);
void *kl_egl_sym(const char *name);        // the gateway itself

// How many times the guest has called eglSwapBuffers. For the frame-complete
// seams that have to know whether the swap is the guest's presentation signal at
// all: Beat Saber 1.28 swaps, its 1.40 XR-SDK path never does, and neither does
// an OpenXR guest. Zero here means "the capture and frontend seams hanging off
// the swap have never fired, so this frame's completion is where they belong".
unsigned long kl_egl_swap_count(void);

// The EGLContext current on THIS thread, as the guest sees it. Diagnostic: a
// guest that keys resource ownership on the current context (Unity keys FBO
// ownership on it) fails in a way that names neither the context nor the
// resource, so a GL trace has to be able to say which context a name was made
// under. Not a GL call and not the ANGLE context — the handle the guest holds.
void *kl_egl_current_context(void);

// What the guest asked eglGetProcAddress for, and which of those it went on to
// call. This is the work list, in the order the guest wanted it.
void kl_egl_report(FILE *f);

// Shader sources the guest handed to glShaderSource. They are the input to the
// GLSL ES -> SPIR-V -> MSL pipeline needs, and this is the only place they
// exist in plain text — the APK stores them compressed inside Unity's assets.
unsigned kl_egl_shader_count(void);
void     kl_egl_dump_shaders(const char *dir);

// Write every uncompressed 8-bit glTexSubImage2D upload to <dir> as a PNG. There
// is no framebuffer to capture — nothing renders — but for a frame that is one
// textured quad these uploads are its visible content.
void kl_egl_dump_textures(const char *dir);
unsigned kl_egl_texture_count(void);

// The capability tables behind the query entry points, for kl_glfb: under the
// reference renderer the *description* of the device stays ours, but dynamic
// state (READ_BUFFER, bindings, viewport — anything the tables do not list)
// belongs to ANGLE, which actually tracks it. Each returns 1 when the pname
// is ours (answer written), 0 when it is not — no print, no zero-fill.
int kl_gl_cap_integerv(uint32_t pname, int32_t *params);
int kl_gl_cap_integeri_v(uint32_t target, uint32_t index, int32_t *data);
int kl_gl_cap_internalformativ(uint32_t target, uint32_t internalformat,
                               uint32_t pname, int32_t bufSize, int32_t *params);
int kl_gl_cap_floatv(uint32_t pname, float *data);
int kl_gl_cap_integer64v(uint32_t pname, int64_t *data);

#endif
