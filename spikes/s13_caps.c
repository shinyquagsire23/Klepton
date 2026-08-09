// S13 — what does the vendored ANGLE/Metal ES 3.0 context actually allow for
// texture units? kl_glfb describes 32 combined units to the guest (kl_egl's
// capability table), and Unity's glActiveTexture(GL_TEXTURE31) raises
// GL_INVALID_ENUM on the real context, ~6x/frame, with Unity logging "OpenGL
// Error: Invalid texture unit!". ANGLE Metal's DisplayMtl claims
// maxCombinedTextureImageUnits = 32, so either the runtime context disagrees
// with the source, or the validation limit is a different number.
//
// Build: clang -o build/s13_caps spikes/s13_caps.c
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

// The vendored build (vendor/out/Debug, `make angle-debug`), always: ours is
// patched (angle-patches/), so silently measuring a different ANGLE on the
// machine would report on something the runtime never loads. KL_ANGLE_DIR
// overrides deliberately.
#define ANGLE_VENDORED_DIR "vendor/out/Debug"

static const char *kl_angle_dir(void) {
    const char *dir = getenv("KL_ANGLE_DIR");
    if (dir) return dir;
    return ANGLE_VENDORED_DIR;
}

#define EGL_DEFAULT_DISPLAY      ((void *)0)
#define EGL_NO_CONTEXT           ((void *)0)
#define EGL_NONE                 0x3038
#define EGL_WIDTH                0x3057
#define EGL_HEIGHT               0x3056
#define EGL_SURFACE_TYPE         0x3033
#define EGL_PBUFFER_BIT          0x0001
#define EGL_RENDERABLE_TYPE      0x3040
#define EGL_OPENGL_ES3_BIT       0x0040
#define EGL_RED_SIZE             0x3024
#define EGL_GREEN_SIZE           0x3023
#define EGL_BLUE_SIZE            0x3022
#define EGL_ALPHA_SIZE           0x3021
#define EGL_DEPTH_SIZE           0x3025
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_PLATFORM_ANGLE_ANGLE            0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE       0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489

#define GL_VENDOR                0x1F00
#define GL_RENDERER              0x1F01
#define GL_VERSION               0x1F02
#define GL_MAX_TEXTURE_IMAGE_UNITS           0x8872
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS  0x8B4D
#define GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS    0x8B4C
#define GL_MAX_TEXTURE_SIZE                  0x0D33
#define GL_TEXTURE0                          0x84C0

static void *g_egl, *g_gles;
static void *sym(const char *n) {
    void *p = g_gles ? dlsym(g_gles, n) : NULL;
    if (!p && g_egl) p = dlsym(g_egl, n);
    return p;
}

int main(void) {
    const char *dir = kl_angle_dir();
    char egl_path[1024], gles_path[1024];
    snprintf(egl_path,  sizeof egl_path,  "%s/libEGL.dylib", dir);
    snprintf(gles_path, sizeof gles_path, "%s/libGLESv2.dylib", dir);
    g_egl  = dlopen(egl_path, RTLD_NOW | RTLD_LOCAL);
    g_gles = dlopen(gles_path, RTLD_NOW | RTLD_LOCAL);
    if (!g_egl || !g_gles) {
        fprintf(stderr, "FAIL: dlopen ANGLE: %s\n", dlerror()); return 1;
    }
    printf("  loaded ANGLE from %s\n", dir);

    void *(*eglGetPlatformDisplayEXT)(uint32_t, void *, const int32_t *) =
        sym("eglGetPlatformDisplayEXT");
    unsigned (*eglInitialize)(void *, int32_t *, int32_t *) = sym("eglInitialize");
    unsigned (*eglChooseConfig)(void *, const int32_t *, void **, int32_t, int32_t *) = sym("eglChooseConfig");
    void *(*eglCreatePbufferSurface)(void *, void *, const int32_t *) = sym("eglCreatePbufferSurface");
    void *(*eglCreateContext)(void *, void *, void *, const int32_t *) = sym("eglCreateContext");
    unsigned (*eglMakeCurrent)(void *, void *, void *, void *) = sym("eglMakeCurrent");

    const int32_t dpy_attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
        EGL_NONE,
    };
    void *dpy = eglGetPlatformDisplayEXT
              ? eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE,
                                         EGL_DEFAULT_DISPLAY, dpy_attrs)
              : NULL;
    int32_t major = 0, minor = 0;
    if (!dpy || !eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "FAIL: eglInitialize\n"); return 1;
    }
    const int32_t cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_NONE,
    };
    void *cfg = NULL; int32_t ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "FAIL: eglChooseConfig\n"); return 1;
    }
    const int32_t surf_attrs[] = { EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE };
    void *surf = eglCreatePbufferSurface(dpy, cfg, surf_attrs);
    const int32_t ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    void *ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (!ctx || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "FAIL: no current ES3 context\n"); return 1;
    }

    const uint8_t *(*glGetString)(uint32_t)     = sym("glGetString");
    void (*glGetIntegerv)(uint32_t, int32_t *)  = sym("glGetIntegerv");
    void (*glActiveTexture)(uint32_t)           = sym("glActiveTexture");
    uint32_t (*glGetError)(void)                = sym("glGetError");
    printf("  GL_VERSION = %s — %s\n", glGetString(GL_VERSION),
           glGetString(GL_RENDERER));

    int32_t v = -1;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &v);
    printf("  MAX_TEXTURE_IMAGE_UNITS           = %d\n", v);
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &v);
    printf("  MAX_VERTEX_TEXTURE_IMAGE_UNITS    = %d\n", v);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &v);
    printf("  MAX_COMBINED_TEXTURE_IMAGE_UNITS  = %d\n", v);

    // The empirical test: which glActiveTexture units pass?
    int first_bad = -1;
    for (int u = 0; u < 40; u++) {
        while (glGetError() != 0) {}
        glActiveTexture(GL_TEXTURE0 + (uint32_t)u);
        uint32_t e = glGetError();
        if (e && first_bad < 0) first_bad = u;
        if (u >= 14 || (e && u < 14))
            printf("  glActiveTexture(unit %2d) -> %s\n", u,
                   e ? "GL_INVALID_ENUM" : "ok");
    }
    printf("  first failing unit: %d\n", first_bad);

    // The guest's real failure shape: assigning unit 31 to a FRAGMENT sampler
    // via glUniform1i. MAX_TEXTURE_IMAGE_UNITS is 16 while combined is 32 —
    // if ANGLE validates against the per-stage limit, unit 31 fails here and
    // Unity's "Invalid texture unit!" spam (and its sampler staying at unit
    // 0's texture) is exactly what follows.
    uint32_t (*glCreateShader)(uint32_t)              = sym("glCreateShader");
    void (*glShaderSource)(uint32_t, int32_t, const char *const *, const int32_t *) = sym("glShaderSource");
    void (*glCompileShader)(uint32_t)                 = sym("glCompileShader");
    uint32_t (*glCreateProgram)(void)                 = sym("glCreateProgram");
    void (*glAttachShader)(uint32_t, uint32_t)        = sym("glAttachShader");
    void (*glLinkProgram)(uint32_t)                   = sym("glLinkProgram");
    void (*glUseProgram)(uint32_t)                    = sym("glUseProgram");
    int32_t (*glGetUniformLocation)(uint32_t, const char *) = sym("glGetUniformLocation");
    void (*glUniform1i)(int32_t, int32_t)             = sym("glUniform1i");
    static const char *fs_src =
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform sampler2D s;\n"
        "out vec4 o;\n"
        "void main() { o = texture(s, vec2(0.0)); }\n";
    uint32_t fs = glCreateShader(0x8B30 /* FRAGMENT_SHADER */);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    static const char *vs_src =
        "#version 300 es\n"
        "void main() { gl_Position = vec4(0.0); }\n";
    uint32_t vs = glCreateShader(0x8B31 /* VERTEX_SHADER */);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    uint32_t prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    int32_t linked = 0;
    void (*glGetProgramiv)(uint32_t, uint32_t, int32_t *) = sym("glGetProgramiv");
    glGetProgramiv(prog, 0x8B82 /* LINK_STATUS */, &linked);
    printf("  link status = %d\n", linked);
    glUseProgram(prog);
    int32_t loc = glGetUniformLocation(prog, "s");
    printf("  sampler location = %d\n", loc);
    for (int u = 0; u <= 32; u++) {
        while (glGetError() != 0) {}
        glUniform1i(loc, u);
        uint32_t e = glGetError();
        if (1)
            printf("  glUniform1i(sampler, %2d) -> %s\n", u,
                   e ? (e == 0x501 ? "GL_INVALID_VALUE"
                                   : (e == 0x502 ? "GL_INVALID_OPERATION" : "err"))
                      : "ok");
    }
    return 0;
}
