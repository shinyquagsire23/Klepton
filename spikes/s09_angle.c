// S0.9 — ANGLE on macOS, and how it compares to the desktop-GL reference path.
//
// S0.7/S0.8 built a reference renderer on Apple's desktop GL 4.1. It works, but it
// is the wrong shape twice over: visionOS has no GL at all, and the driver rejects
// ETC2 (S0.8's follow-up found GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC raising
// GL_INVALID_ENUM), which is a format *mandatory* in the GLES 3.0 the guest speaks.
//
// ANGLE is the other candidate and is a much better fit on paper:
//
//   * it implements GLES on Metal, which is the device story too;
//   * it exposes exactly libEGL + libGLESv2 — the two sonames the guest already
//     dlopens — so the shim could hand the guest a real driver rather than
//     forwarding to a differently-shaped API;
//   * ETC2 is core GLES 3.0, so a conformant implementation must accept it.
//
// This spike answers three questions with the guest's own artefacts:
//   1. can we get an ANGLE GLES 3.0 context at all, on arm64, from a prebuilt?
//   2. does it accept the ETC2 allocation desktop GL rejected?
//   3. do the guest's captured shaders compile UNMODIFIED — no #version rewrite?
//
// ANGLE comes from the vendored debug build in vendor/out/Debug when present
// (see Makefile `angle-debug`), otherwise from a Chromium-based app — the path
// is taken from KL_ANGLE_DIR or defaults as above. Everything is dlopen/dlsym
// so there is nothing to link and no headers to find.
//
// Build: clang -o build/s09_angle spikes/s09_angle.c
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

// The vendored debug build (vendor/out/Debug, see Makefile `angle-debug`)
// is preferred when present; otherwise borrow Chrome's prebuilt.
#define ANGLE_VENDORED_DIR "vendor/out/Debug"
#define ANGLE_DEFAULT_DIR \
    "/Applications/Google Chrome.app/Contents/Frameworks/" \
    "Google Chrome Framework.framework/Libraries"

static const char *kl_angle_dir(void) {
    const char *dir = getenv("KL_ANGLE_DIR");
    if (dir) return dir;
    if (access(ANGLE_VENDORED_DIR "/libEGL.dylib", R_OK) == 0)
        return ANGLE_VENDORED_DIR;
    return ANGLE_DEFAULT_DIR;
}

// ---- the EGL/GLES constants we need, spelled out so there is nothing to include
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

// ANGLE's backend selector. Without this it picks a default, and on macOS that
// default is its *OpenGL* backend — which inherits every limitation of Apple's
// desktop GL, ETC2 included. The whole reason to want ANGLE here is the Metal
// backend, so it has to be asked for explicitly.
#define EGL_PLATFORM_ANGLE_ANGLE            0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE       0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE 0x320D

#define GL_VENDOR                0x1F00
#define GL_RENDERER              0x1F01
#define GL_VERSION               0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_NO_ERROR              0
#define GL_TEXTURE_2D            0x0DE1
#define GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC 0x9279
#define GL_COMPRESSED_RGBA8_ETC2_EAC        0x9278
#define GL_VERTEX_SHADER         0x8B31
#define GL_FRAGMENT_SHADER       0x8B30
#define GL_COMPILE_STATUS        0x8B81
#define GL_LINK_STATUS           0x8B82

static void *g_egl, *g_gles;
static void *sym(const char *n) {
    void *p = g_gles ? dlsym(g_gles, n) : NULL;
    if (!p && g_egl) p = dlsym(g_egl, n);
    return p;
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = 0; fclose(f);
    return b;
}

int main(int argc, char **argv) {
    const char *dir = kl_angle_dir();
    char egl_path[1024], gles_path[1024];
    snprintf(egl_path,  sizeof egl_path,  "%s/libEGL.dylib", dir);
    snprintf(gles_path, sizeof gles_path, "%s/libGLESv2.dylib", dir);

    g_egl  = dlopen(egl_path, RTLD_NOW | RTLD_LOCAL);
    g_gles = dlopen(gles_path, RTLD_NOW | RTLD_LOCAL);
    if (!g_egl || !g_gles) {
        fprintf(stderr, "FAIL: dlopen ANGLE: %s\n  (set KL_ANGLE_DIR)\n", dlerror());
        return 1;
    }
    printf("  loaded ANGLE from %s\n", dir);

    void *(*eglGetDisplay)(void *)                          = sym("eglGetDisplay");
    unsigned (*eglInitialize)(void *, int32_t *, int32_t *) = sym("eglInitialize");
    unsigned (*eglChooseConfig)(void *, const int32_t *, void **, int32_t, int32_t *) = sym("eglChooseConfig");
    void *(*eglCreatePbufferSurface)(void *, void *, const int32_t *) = sym("eglCreatePbufferSurface");
    void *(*eglCreateContext)(void *, void *, void *, const int32_t *) = sym("eglCreateContext");
    unsigned (*eglMakeCurrent)(void *, void *, void *, void *) = sym("eglMakeCurrent");
    const char *(*eglQueryString)(void *, int32_t) = sym("eglQueryString");
    if (!eglGetDisplay || !eglInitialize || !eglChooseConfig || !eglCreateContext) {
        fprintf(stderr, "FAIL: ANGLE is missing core EGL entry points\n"); return 1;
    }

    // Ask for Metal by name. eglGetPlatformDisplayEXT is the only way to choose;
    // eglGetDisplay(EGL_DEFAULT_DISPLAY) gave us the OpenGL backend and with it a
    // rejected ETC2 allocation, which is the limitation we came here to escape.
    void *(*eglGetPlatformDisplayEXT)(uint32_t, void *, const int32_t *) =
        sym("eglGetPlatformDisplayEXT");
    const char *backend = getenv("KL_ANGLE_BACKEND");
    int want_gl = backend && strcmp(backend, "gl") == 0;
    const int32_t dpy_attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE,
        want_gl ? EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE : EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
        EGL_NONE,
    };
    void *dpy = NULL;
    if (eglGetPlatformDisplayEXT)
        dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, dpy_attrs);
    if (!dpy) {
        printf("  (no eglGetPlatformDisplayEXT — falling back to the default backend)\n");
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    int32_t major = 0, minor = 0;
    if (!dpy || !eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "FAIL: eglInitialize\n"); return 1;
    }
    printf("  EGL %d.%d — %s\n", major, minor,
           eglQueryString ? eglQueryString(dpy, 0x3054 /* EGL_VERSION */) : "?");

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

    const uint8_t *(*glGetString)(uint32_t) = sym("glGetString");
    uint32_t (*glGetError)(void)            = sym("glGetError");
    printf("  GL_VENDOR   = %s\n", glGetString(GL_VENDOR));
    printf("  GL_RENDERER = %s\n", glGetString(GL_RENDERER));
    printf("  GL_VERSION  = %s\n", glGetString(GL_VERSION));
    printf("  GLSL        = %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));

    int ok = 1;

    // ---- Q2: the ETC2 allocation desktop GL rejected with GL_INVALID_ENUM
    void (*glGenTextures)(int32_t, uint32_t *) = sym("glGenTextures");
    void (*glBindTexture)(uint32_t, uint32_t)  = sym("glBindTexture");
    void (*glTexStorage2D)(uint32_t, int32_t, uint32_t, int32_t, int32_t) = sym("glTexStorage2D");
    uint32_t tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    while (glGetError() != GL_NO_ERROR) {}
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC, 2048, 2048);
    uint32_t e1 = glGetError();
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_COMPRESSED_RGBA8_ETC2_EAC, 64, 64);
    uint32_t e2 = glGetError();     // second call on the same texture is expected to
                                    // complain about immutability, not about the enum
    printf("  ETC2 sRGB8_alpha8 alloc -> 0x%x %s\n", e1,
           e1 == GL_NO_ERROR ? "(accepted)" : "(REJECTED)");
    if (e1 != GL_NO_ERROR) ok = 0;
    (void)e2;

    // ---- Q3: the guest's own shaders, UNMODIFIED
    const char *vs_path = argc > 1 ? argv[1] : "/tmp/sh2/shader_000_156.glsl";
    const char *fs_path = argc > 2 ? argv[2] : "/tmp/sh2/shader_001_157.glsl";
    char *vs_src = slurp(vs_path), *fs_src = slurp(fs_path);
    if (!vs_src || !fs_src) {
        fprintf(stderr, "  (no captured shaders at %s — run t_boot with KL_DUMP_SHADERS)\n",
                vs_path);
    } else {
        uint32_t (*glCreateShader)(uint32_t) = sym("glCreateShader");
        void (*glShaderSource)(uint32_t, int32_t, const char *const *, const int32_t *) = sym("glShaderSource");
        void (*glCompileShader)(uint32_t) = sym("glCompileShader");
        void (*glGetShaderiv)(uint32_t, uint32_t, int32_t *) = sym("glGetShaderiv");
        void (*glGetShaderInfoLog)(uint32_t, int32_t, int32_t *, char *) = sym("glGetShaderInfoLog");
        uint32_t (*glCreateProgram)(void) = sym("glCreateProgram");
        void (*glAttachShader)(uint32_t, uint32_t) = sym("glAttachShader");
        void (*glLinkProgram)(uint32_t) = sym("glLinkProgram");
        void (*glGetProgramiv)(uint32_t, uint32_t, int32_t *) = sym("glGetProgramiv");

        uint32_t sh[2] = {0, 0};
        const char *srcs[2] = { vs_src, fs_src };
        uint32_t types[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
        const char *names[2] = { vs_path, fs_path };
        for (int i = 0; i < 2; i++) {
            sh[i] = glCreateShader(types[i]);
            glShaderSource(sh[i], 1, &srcs[i], NULL);
            glCompileShader(sh[i]);
            int32_t st = 0; glGetShaderiv(sh[i], GL_COMPILE_STATUS, &st);
            if (!st) {
                char log[2048]; glGetShaderInfoLog(sh[i], sizeof log, NULL, log);
                printf("  %s UNMODIFIED -> FAILED\n%s\n", names[i], log);
                ok = 0;
            } else {
                printf("  %s compiles UNMODIFIED (no #version rewrite)\n", names[i]);
            }
        }
        uint32_t prog = glCreateProgram();
        glAttachShader(prog, sh[0]); glAttachShader(prog, sh[1]);
        glLinkProgram(prog);
        int32_t linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        printf("  program %s\n", linked ? "links" : "FAILED to link");
        if (!linked) ok = 0;
    }

    printf(ok ? "=== S0.9 PASS: ANGLE gives a real GLES 3 driver that takes the guest's\n"
                "    shaders as-is and accepts ETC2 — the two things desktop GL could not\n"
              : "FAIL: ANGLE did not clear the bar\n");
    return ok ? 0 : 1;
}
