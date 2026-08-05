// M5, first cut — the EGL surface libunity.so imports.
//
// Exactly the 19 symbols in the unresolved list and nothing else. Notably that
// list contains no GL entry points at all: `eglGetProcAddress` is in it, so
// Unity resolves the whole of GLES through that single function at runtime.
// That is the entire graphics surface behind one door, which is what makes
// measuring it cheap — see kl_egl.c.
#ifndef KL_EGL_H
#define KL_EGL_H
#include <stdio.h>

// Resolve an EGL import by name. NULL if we do not provide it.
void *kl_egl_lookup(const char *name);

// Unity reaches GLES through *two* doors, not one. eglGetProcAddress is the
// documented one; the other is dlopen("libGLESv2.so") followed by dlsym, and
// when that dlopen failed Unity called straight through the resulting NULL —
// a jump to address 0 with nothing on the stack to say why. So the loader hands
// out a synthetic handle for the GL sonames and resolves symbols on it through
// the same gateway eglGetProcAddress uses.
void *kl_egl_dlopen(const char *soname);   // NULL if this is not a GL library
int   kl_egl_is_handle(const void *h);
void *kl_egl_sym(const char *name);        // the gateway itself

// What the guest asked eglGetProcAddress for, and which of those it went on to
// call. This is the M5 work list, in the order the guest wanted it.
void kl_egl_report(FILE *f);

// Shader sources the guest handed to glShaderSource. They are the input to the
// GLSL ES -> SPIR-V -> MSL pipeline M5 needs, and this is the only place they
// exist in plain text — the APK stores them compressed inside Unity's assets.
unsigned kl_egl_shader_count(void);
void     kl_egl_dump_shaders(const char *dir);

// Write every uncompressed 8-bit glTexSubImage2D upload to <dir> as a PNG. There
// is no framebuffer to capture — nothing renders — but for a frame that is one
// textured quad these uploads are its visible content.
void kl_egl_dump_textures(const char *dir);
unsigned kl_egl_texture_count(void);

#endif
