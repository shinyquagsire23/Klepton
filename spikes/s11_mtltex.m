// The compositor interop primitive: does ANGLE render into an MTLTexture we own?
//
// The one mechanism the whole visionOS renderer rests on. Compositor Services
// hands out drawables as MTLTextures; Unity
// renders into GL textures our ovrp_SetupEyeTexture2 gives storage to. If ANGLE
// can be told "this GL texture's storage IS that MTLTexture", the two halves
// meet with no copy and the compositor is bookkeeping. If it cannot, it needs a blit pass
// and a second full-eye allocation, which is a different (and worse) design.
//
// Answered on the host first, deliberately — rung 1 of the development ladder,
// and the same code then goes to the device probe, where the open
// question is AMFI rather than ANGLE.
//
// eglCreatePbufferFromClientBuffer is the obvious primitive; the extension
// spec (vendor/extensions/EGL_ANGLE_metal_texture_client_buffer.txt) is written
// against eglCreateImageKHR instead, which is the better primitive for us: an
// EGLImage-backed texture is a normal GL texture and so is FBO-renderable,
// whereas a pbuffer is a surface you would have to make current. Unity hands us
// a texture *name* and expects us to give it storage — an EGLImage target is
// exactly that shape, so the shipping change is one call:
//
//     glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, h, w)     // today
//     glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image)
//
// Four questions, in the order a failure would matter:
//
//   Q1  are the three extensions actually exposed by this build's Metal display?
//   Q2  can we get ANGLE's own MTLDevice? (the spec REQUIRES the texture come
//       from that device, so guessing MTLCreateSystemDefaultDevice() is wrong)
//   Q3  does a 2-slice array texture give two independently renderable eyes?
//       Verified from BOTH sides: glReadPixels says GL wrote something,
//       [tex getBytes:slice:] says it landed in *our* texture. And the two
//       slices must differ — if EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE were
//       ignored, both eyes would carry the same image and it would present as a
//       compositor bug rather than as an EGL one.
//   Q4  does the guest's real eye format and size work — RGBA16F at 2198x2304
//       (KL_OVRP_TEXFMT_EYE, and note the h,w transposition kl_ovrp.c explains)?
//
// Build/run: make mtltex
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#import <Metal/Metal.h>

// The vendored build (vendor/out/Debug, `make angle-debug`), always: ours is
// patched (angle-patches/), so silently measuring a different ANGLE on the
// machine would report on something the runtime never loads. KL_ANGLE_DIR
// overrides deliberately.
#define ANGLE_VENDORED_DIR "vendor/out/Debug"

static const char *angle_dir(void) {
    const char *dir = getenv("KL_ANGLE_DIR");
    if (dir) return dir;
    return ANGLE_VENDORED_DIR;
}

// ---- EGL/GLES constants, spelled out so there is nothing to include ----
#define EGL_DEFAULT_DISPLAY      ((void *)0)
#define EGL_NO_CONTEXT           ((void *)0)
#define EGL_NO_IMAGE             ((void *)0)
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
#define EGL_EXTENSIONS           0x3055
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_PLATFORM_ANGLE_ANGLE            0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE       0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define EGL_DEVICE_EXT                      0x322C
#define EGL_METAL_DEVICE_ANGLE              0x34A6
#define EGL_METAL_TEXTURE_ANGLE             0x34A7
#define EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE 0x34DD
#define EGL_TEXTURE_INTERNAL_FORMAT_ANGLE   0x345D

#define GL_NO_ERROR              0
#define GL_TEXTURE_2D            0x0DE1
#define GL_TEXTURE_MIN_FILTER    0x2801
#define GL_TEXTURE_MAG_FILTER    0x2800
#define GL_NEAREST               0x2600
#define GL_FRAMEBUFFER           0x8D40
#define GL_COLOR_ATTACHMENT0     0x8CE0
#define GL_FRAMEBUFFER_COMPLETE  0x8CD5
#define GL_RGBA                  0x1908
#define GL_UNSIGNED_BYTE         0x1401
#define GL_FLOAT                 0x1406
#define GL_HALF_FLOAT            0x140B
#define GL_RGBA16F               0x881A
#define GL_SRGB8_ALPHA8          0x8C43
#define GL_COLOR_BUFFER_BIT      0x00004000
#define GL_IMPLEMENTATION_COLOR_READ_FORMAT 0x8B9B
#define GL_IMPLEMENTATION_COLOR_READ_TYPE   0x8B9A
#define GL_VERTEX_SHADER         0x8B31
#define GL_FRAGMENT_SHADER       0x8B30
#define GL_COMPILE_STATUS        0x8B81
#define GL_LINK_STATUS           0x8B82
#define GL_ARRAY_BUFFER          0x8892
#define GL_STATIC_DRAW           0x88E4
#define GL_TRIANGLES             0x0004
#define GL_RENDERER              0x1F01

static void *g_egl, *g_gles;
static void *sym(const char *n) {
    void *p = g_gles ? dlsym(g_gles, n) : NULL;
    if (!p && g_egl) p = dlsym(g_egl, n);
    return p;
}

static int g_fail;
static void check(int ok, const char *what) {
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) g_fail = 1;
}

// half -> float, for reading RGBA16F back through Metal. Enough of the format
// for a "did the clear colour land" comparison; not a general converter.
static float half_to_float(uint16_t h) {
    int s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    if (e == 0)  return (s ? -1.f : 1.f) * (float)m * (1.f / 16777216.f);
    if (e == 31) return (s ? -1.f : 1.f) * (m ? 0.f / 0.f : 1.f / 0.f);
    union { uint32_t u; float f; } u;
    u.u = (uint32_t)(s << 31) | (uint32_t)((e - 15 + 127) << 23) | (uint32_t)(m << 13);
    return u.f;
}

// ---- resolved entry points ----
static void *(*eglGetPlatformDisplayEXT)(uint32_t, void *, const int32_t *);
static unsigned (*eglInitialize)(void *, int32_t *, int32_t *);
static unsigned (*eglChooseConfig)(void *, const int32_t *, void **, int32_t, int32_t *);
static void *(*eglCreateContext)(void *, void *, void *, const int32_t *);
static void *(*eglCreatePbufferSurface)(void *, void *, const int32_t *);
static unsigned (*eglMakeCurrent)(void *, void *, void *, void *);
static const char *(*eglQueryString)(void *, int32_t);
static unsigned (*eglQueryDisplayAttribEXT)(void *, int32_t, intptr_t *);
static unsigned (*eglQueryDeviceAttribEXT)(void *, int32_t, intptr_t *);
static void *(*eglCreateImageKHR)(void *, void *, uint32_t, void *, const int32_t *);
static unsigned (*eglDestroyImageKHR)(void *, void *);
static uint32_t (*eglGetError)(void);
static void (*glEGLImageTargetTexture2DOES)(uint32_t, void *);

static void (*glGenTextures)(int32_t, uint32_t *);
static void (*glBindTexture)(uint32_t, uint32_t);
static void (*glTexParameteri)(uint32_t, uint32_t, int32_t);
static void (*glTexStorage2D)(uint32_t, int32_t, uint32_t, int32_t, int32_t);
static void (*glGenFramebuffers)(int32_t, uint32_t *);
static void (*glBindFramebuffer)(uint32_t, uint32_t);
static void (*glFramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
static uint32_t (*glCheckFramebufferStatus)(uint32_t);
static void (*glClearColor)(float, float, float, float);
static void (*glClear)(uint32_t);
static void (*glReadPixels)(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void *);
static void (*glGetIntegerv)(uint32_t, int32_t *);
static void (*glFinish)(void);
static uint32_t (*glGetError)(void);
static void (*glViewport)(int32_t, int32_t, int32_t, int32_t);
static const uint8_t *(*glGetString)(uint32_t);
static uint32_t (*glCreateShader)(uint32_t);
static void (*glShaderSource)(uint32_t, int32_t, const char *const *, const int32_t *);
static void (*glCompileShader)(uint32_t);
static void (*glGetShaderiv)(uint32_t, uint32_t, int32_t *);
static uint32_t (*glCreateProgram)(void);
static void (*glAttachShader)(uint32_t, uint32_t);
static void (*glLinkProgram)(uint32_t);
static void (*glGetProgramiv)(uint32_t, uint32_t, int32_t *);
static void (*glUseProgram)(uint32_t);
static void (*glGenBuffers)(int32_t, uint32_t *);
static void (*glBindBuffer)(uint32_t, uint32_t);
static void (*glBufferData)(uint32_t, intptr_t, const void *, uint32_t);
static void (*glVertexAttribPointer)(uint32_t, int32_t, uint32_t, uint8_t, int32_t, const void *);
static void (*glEnableVertexAttribArray)(uint32_t);
static void (*glDrawArrays)(uint32_t, int32_t, int32_t);

static int resolve_all(void) {
    eglGetPlatformDisplayEXT = sym("eglGetPlatformDisplayEXT");
    eglInitialize            = sym("eglInitialize");
    eglChooseConfig          = sym("eglChooseConfig");
    eglCreateContext         = sym("eglCreateContext");
    eglCreatePbufferSurface  = sym("eglCreatePbufferSurface");
    eglMakeCurrent           = sym("eglMakeCurrent");
    eglQueryString           = sym("eglQueryString");
    eglQueryDisplayAttribEXT = sym("eglQueryDisplayAttribEXT");
    eglQueryDeviceAttribEXT  = sym("eglQueryDeviceAttribEXT");
    eglCreateImageKHR        = sym("eglCreateImageKHR");
    eglDestroyImageKHR       = sym("eglDestroyImageKHR");
    eglGetError              = sym("eglGetError");
    glEGLImageTargetTexture2DOES = sym("glEGLImageTargetTexture2DOES");

    glGenTextures            = sym("glGenTextures");
    glBindTexture            = sym("glBindTexture");
    glTexParameteri          = sym("glTexParameteri");
    glTexStorage2D           = sym("glTexStorage2D");
    glGenFramebuffers        = sym("glGenFramebuffers");
    glBindFramebuffer        = sym("glBindFramebuffer");
    glFramebufferTexture2D   = sym("glFramebufferTexture2D");
    glCheckFramebufferStatus = sym("glCheckFramebufferStatus");
    glClearColor             = sym("glClearColor");
    glClear                  = sym("glClear");
    glReadPixels             = sym("glReadPixels");
    glGetIntegerv            = sym("glGetIntegerv");
    glFinish                 = sym("glFinish");
    glGetError               = sym("glGetError");
    glViewport               = sym("glViewport");
    glGetString              = sym("glGetString");
    glCreateShader           = sym("glCreateShader");
    glShaderSource           = sym("glShaderSource");
    glCompileShader          = sym("glCompileShader");
    glGetShaderiv            = sym("glGetShaderiv");
    glCreateProgram          = sym("glCreateProgram");
    glAttachShader           = sym("glAttachShader");
    glLinkProgram            = sym("glLinkProgram");
    glGetProgramiv           = sym("glGetProgramiv");
    glUseProgram             = sym("glUseProgram");
    glGenBuffers             = sym("glGenBuffers");
    glBindBuffer             = sym("glBindBuffer");
    glBufferData             = sym("glBufferData");
    glVertexAttribPointer    = sym("glVertexAttribPointer");
    glEnableVertexAttribArray = sym("glEnableVertexAttribArray");
    glDrawArrays             = sym("glDrawArrays");

    return eglInitialize && eglCreateImageKHR && glEGLImageTargetTexture2DOES
        && eglQueryDisplayAttribEXT && eglQueryDeviceAttribEXT && glFramebufferTexture2D;
}

// A GL texture whose storage is `mtl`'s slice `slice`. Returns the texture name,
// or 0 with the reason printed. `tex` names an EXISTING texture to re-point, or
// 0 to make a fresh one — see Q5 for why re-pointing matters.
static uint32_t bind_mtl_slice_to(void *dpy, id<MTLTexture> mtl, int slice,
                                  uint32_t internal_fmt, uint32_t tex,
                                  void **out_image) {
    const int32_t img_attrs[] = {
        EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE, slice,
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE,   (int32_t)internal_fmt,
        EGL_NONE,
    };
    void *img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_METAL_TEXTURE_ANGLE,
                                 (__bridge void *)mtl, img_attrs);
    if (img == EGL_NO_IMAGE) {
        printf("    eglCreateImageKHR(slice=%d) failed, EGL error 0x%x\n",
               slice, eglGetError ? eglGetError() : 0);
        return 0;
    }
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    while (glGetError() != GL_NO_ERROR) {}
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
    uint32_t e = glGetError();
    if (e != GL_NO_ERROR) {
        printf("    glEGLImageTargetTexture2DOES(slice=%d) -> GL error 0x%x\n", slice, e);
        return 0;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    *out_image = img;
    return tex;
}

static uint32_t bind_mtl_slice(void *dpy, id<MTLTexture> mtl, int slice,
                               uint32_t internal_fmt, void **out_image) {
    return bind_mtl_slice_to(dpy, mtl, slice, internal_fmt, 0, out_image);
}

// Draw a full-viewport-ish triangle in `rgb`, to prove the path carries real
// rendering and not only a clear. (A clear can be serviced by a load-action;
// a draw needs the whole pipeline pointed at our texture.)
static int draw_triangle(float r, float g, float b) {
    static uint32_t prog;
    if (!prog) {
        const char *vs = "#version 300 es\nin vec2 p;void main(){gl_Position=vec4(p,0.,1.);}\n";
        const char *fs = "#version 300 es\nprecision mediump float;uniform vec3 c;"
                         "out vec4 o;void main(){o=vec4(c,1.);}\n";
        uint32_t v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
        int32_t ok = 0;
        glShaderSource(v, 1, &vs, NULL); glCompileShader(v);
        glGetShaderiv(v, GL_COMPILE_STATUS, &ok); if (!ok) return 0;
        glShaderSource(f, 1, &fs, NULL); glCompileShader(f);
        glGetShaderiv(f, GL_COMPILE_STATUS, &ok); if (!ok) return 0;
        prog = glCreateProgram();
        glAttachShader(prog, v); glAttachShader(prog, f); glLinkProgram(prog);
        glGetProgramiv(prog, GL_LINK_STATUS, &ok); if (!ok) { prog = 0; return 0; }
        uint32_t vbo = 0;
        // A triangle that covers the whole clip square, so any sampled pixel is lit.
        const float tri[] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, (intptr_t)sizeof tri, tri, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, NULL);
        glEnableVertexAttribArray(0);
    }
    void (*glUniform3f)(int32_t, float, float, float) = sym("glUniform3f");
    int32_t (*glGetUniformLocation)(uint32_t, const char *) = sym("glGetUniformLocation");
    glUseProgram(prog);
    if (glUniform3f && glGetUniformLocation)
        glUniform3f(glGetUniformLocation(prog, "c"), r, g, b);
    while (glGetError() != GL_NO_ERROR) {}
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return glGetError() == GL_NO_ERROR;
}

int main(void) {
    const char *dir = angle_dir();
    char p[1024];
    snprintf(p, sizeof p, "%s/libEGL.dylib", dir);
    g_egl = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    snprintf(p, sizeof p, "%s/libGLESv2.dylib", dir);
    g_gles = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    if (!g_egl || !g_gles) {
        fprintf(stderr, "FAIL: dlopen ANGLE: %s\n  (set KL_ANGLE_DIR)\n", dlerror());
        return 1;
    }
    printf("=== ANGLE renders into an MTLTexture we own ===\n");
    printf("  ANGLE from %s\n", dir);
    if (!resolve_all()) {
        // A missing entry point here is a real answer, not a setup problem: it
        // says this ANGLE build cannot do the interop at all.
        printf("  eglCreateImageKHR            %s\n", eglCreateImageKHR ? "present" : "MISSING");
        printf("  glEGLImageTargetTexture2DOES %s\n", glEGLImageTargetTexture2DOES ? "present" : "MISSING");
        printf("  eglQueryDeviceAttribEXT      %s\n", eglQueryDeviceAttribEXT ? "present" : "MISSING");
        fprintf(stderr, "FAIL: this ANGLE is missing an interop entry point\n");
        return 1;
    }

    // ---- Metal display, explicitly. The GL backend has no MTLDevice to share.
    const int32_t dpy_attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE };
    void *dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, dpy_attrs);
    int32_t major = 0, minor = 0;
    if (!dpy || !eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "FAIL: eglInitialize on the Metal backend\n"); return 1;
    }

    // ---- Q1: the extensions this design assumes
    const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
    printf("\n[Q1] display extensions\n");
    // EGL_ANGLE_device_metal is deliberately NOT in this list: it is a *device*
    // extension (Caps.cpp's DeviceExtensions, not DisplayExtensions), so it is
    // advertised by eglQueryDeviceStringEXT and never appears here. Checking the
    // display string for it reports a missing capability that is present and
    // working, which is the kind of false negative that sends you looking for a
    // build problem. It is checked in Q2, against the device.
    static const char *want[] = {
        "EGL_ANGLE_metal_texture_client_buffer",
        "EGL_KHR_image_base",
        "EGL_ANGLE_metal_shared_event_sync",   // the GPU-side ordering
    };
    for (size_t i = 0; i < sizeof want / sizeof *want; i++)
        check(exts && strstr(exts, want[i]) != NULL, want[i]);

    // ---- a context to work in. Surfaceless would do; a small pbuffer is less
    // to explain and matches what kl_glfb does today.
    const int32_t cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    void *cfg = NULL; int32_t ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "FAIL: eglChooseConfig\n"); return 1;
    }
    const int32_t surf_attrs[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    void *surf = eglCreatePbufferSurface(dpy, cfg, surf_attrs);
    const int32_t ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    void *ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (!ctx || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "FAIL: no current ES3 context\n"); return 1;
    }
    printf("  GL_RENDERER = %s\n", glGetString(GL_RENDERER));

    // ---- Q2: ANGLE's own MTLDevice. The spec requires the texture come from
    // this device, so MTLCreateSystemDefaultDevice() is not interchangeable —
    // on a multi-GPU Mac it would be a different object and the create would
    // fail with EGL_BAD_PARAMETER for a reason nothing else would explain.
    printf("\n[Q2] ANGLE's MTLDevice\n");
    intptr_t egl_dev = 0, mtl_dev_ptr = 0;
    check(eglQueryDisplayAttribEXT(dpy, EGL_DEVICE_EXT, &egl_dev) && egl_dev,
          "eglQueryDisplayAttribEXT(EGL_DEVICE_EXT)");
    if (!egl_dev) return 1;
    const char *(*eglQueryDeviceStringEXT)(void *, int32_t) = sym("eglQueryDeviceStringEXT");
    const char *dexts = eglQueryDeviceStringEXT
                      ? eglQueryDeviceStringEXT((void *)egl_dev, EGL_EXTENSIONS) : NULL;
    check(dexts && strstr(dexts, "EGL_ANGLE_device_metal") != NULL,
          "EGL_ANGLE_device_metal (a device extension, not a display one)");
    check(eglQueryDeviceAttribEXT((void *)egl_dev, EGL_METAL_DEVICE_ANGLE, &mtl_dev_ptr)
          && mtl_dev_ptr, "eglQueryDeviceAttribEXT(EGL_METAL_DEVICE_ANGLE)");
    if (!mtl_dev_ptr) return 1;
    id<MTLDevice> mtl = (__bridge id<MTLDevice>)(void *)mtl_dev_ptr;
    printf("      MTLDevice = %s\n", [[mtl name] UTF8String]);
    id<MTLDevice> sysdev = MTLCreateSystemDefaultDevice();
    printf("      (system default is %s object)\n", sysdev == mtl ? "the same" : "a DIFFERENT");

    // ---- Q3: two eyes as two slices of one array texture, both directions
    printf("\n[Q3] a 2-slice RGBA8 array texture — one slice per eye\n");
    const int W = 64, H = 32;                     // small: this test is about pixels
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureType2DArray;
    d.pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.width = W; d.height = H; d.arrayLength = 2;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;         // so getBytes: can read it directly
    id<MTLTexture> arr = [mtl newTextureWithDescriptor:d];
    check(arr != nil, "newTextureWithDescriptor (2DArray, 2 slices, shared)");
    if (!arr) return 1;

    // Distinct colours per slice, so an ignored slice attribute is visible as
    // "both eyes are the same" rather than as nothing at all.
    const uint8_t expect[2][3] = { { 255, 0, 0 }, { 0, 255, 0 } };
    uint32_t fbo = 0; glGenFramebuffers(1, &fbo);
    for (int slice = 0; slice < 2; slice++) {
        void *img = NULL;
        uint32_t tex = bind_mtl_slice(dpy, arr, slice, GL_RGBA, &img);
        char label[96];
        snprintf(label, sizeof label, "slice %d: EGLImage -> GL texture", slice);
        check(tex != 0, label);
        if (!tex) { g_fail = 1; continue; }

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        uint32_t st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        snprintf(label, sizeof label, "slice %d: FBO complete (status 0x%x)", slice, st);
        check(st == GL_FRAMEBUFFER_COMPLETE, label);

        glViewport(0, 0, W, H);
        glClearColor(0.f, 0.f, 1.f, 1.f);         // blue, then overdrawn — so a
        glClear(GL_COLOR_BUFFER_BIT);             // stale clear cannot pass as a draw
        snprintf(label, sizeof label, "slice %d: draw a triangle over it", slice);
        check(draw_triangle(expect[slice][0] / 255.f, expect[slice][1] / 255.f,
                            expect[slice][2] / 255.f), label);
        glFinish();

        // (a) GL's own view of what it just wrote.
        uint8_t px[4] = { 0 };
        glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        snprintf(label, sizeof label, "slice %d: glReadPixels = %u,%u,%u",
                 slice, px[0], px[1], px[2]);
        check(px[0] == expect[slice][0] && px[1] == expect[slice][1]
              && px[2] == expect[slice][2], label);

        // (b) the claim that actually matters: it landed in OUR MTLTexture.
        uint8_t *rows = calloc((size_t)W * H, 4);
        [arr getBytes:rows bytesPerRow:(NSUInteger)W * 4 bytesPerImage:(NSUInteger)W * H * 4
           fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)W, (NSUInteger)H)
          mipmapLevel:0 slice:(NSUInteger)slice];
        uint8_t *c = rows + ((size_t)(H / 2) * W + W / 2) * 4;
        snprintf(label, sizeof label, "slice %d: MTLTexture getBytes = %u,%u,%u",
                 slice, c[0], c[1], c[2]);
        check(c[0] == expect[slice][0] && c[1] == expect[slice][1]
              && c[2] == expect[slice][2], label);
        free(rows);
        if (img) eglDestroyImageKHR(dpy, img);
    }

    // The cross-check: slice 1 must not have overwritten slice 0. If
    // EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE were silently ignored, every check
    // above would still pass (each read happens right after its own draw) and
    // only this one would fail.
    {
        uint8_t *rows = calloc((size_t)W * H, 4);
        [arr getBytes:rows bytesPerRow:(NSUInteger)W * 4 bytesPerImage:(NSUInteger)W * H * 4
           fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)W, (NSUInteger)H)
          mipmapLevel:0 slice:0];
        uint8_t *c = rows + ((size_t)(H / 2) * W + W / 2) * 4;
        char label[96];
        snprintf(label, sizeof label,
                 "slice 0 survived slice 1 (%u,%u,%u — eyes are independent)",
                 c[0], c[1], c[2]);
        check(c[0] == 255 && c[1] == 0, label);
        free(rows);
    }

    // ---- Q4: the guest's real eye texture — RGBA16F, eye-sized, transposed
    // exactly as kl_ovrp.c's SetupEyeTexture2 allocates it (h wide, w tall).
    printf("\n[Q4] the guest's real eye format: RGBA16F 2198x2304\n");
    const int EW = 2198, EH = 2304;
    MTLTextureDescriptor *ed = [MTLTextureDescriptor new];
    ed.textureType = MTLTextureType2DArray;
    ed.pixelFormat = MTLPixelFormatRGBA16Float;
    ed.width = EW; ed.height = EH; ed.arrayLength = 2;
    ed.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    ed.storageMode = MTLStorageModeShared;
    id<MTLTexture> eye = [mtl newTextureWithDescriptor:ed];
    check(eye != nil, "newTextureWithDescriptor (RGBA16F 2198x2304 x2)");
    if (eye) {
        void *img = NULL;
        uint32_t tex = bind_mtl_slice(dpy, eye, 1, GL_RGBA16F, &img);
        check(tex != 0, "eye slice 1: EGLImage -> GL texture");
        if (tex) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            uint32_t st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            char label[96];
            snprintf(label, sizeof label, "eye slice 1: FBO complete (status 0x%x)", st);
            check(st == GL_FRAMEBUFFER_COMPLETE, label);
            glViewport(0, 0, EW, EH);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            // 2.0 — above 1.0 on purpose: a float target must carry it, and a
            // silently-unorm one clamps. That distinguishes "RGBA16F" from
            // "something that accepted the enum".
            check(draw_triangle(2.f, 0.5f, 0.25f), "eye slice 1: draw (HDR value 2.0)");
            glFinish();
            uint16_t h4[4] = { 0 };
            [eye getBytes:h4 bytesPerRow:8 bytesPerImage:8
               fromRegion:MTLRegionMake2D((NSUInteger)EW / 2, (NSUInteger)EH / 2, 1, 1)
              mipmapLevel:0 slice:1];
            float rr = half_to_float(h4[0]), gg = half_to_float(h4[1]);
            snprintf(label, sizeof label, "eye slice 1: MTLTexture half = %.3f, %.3f", rr, gg);
            check(rr > 1.9f && rr < 2.1f && gg > 0.45f && gg < 0.55f, label);
            if (img) eglDestroyImageKHR(dpy, img);
        }
    }

    // ---- Q5: the OTHER guest's eye texture — the OpenXR swapchain's shape.
    //
    // Two assumptions the Steam Link eye path rests on, neither of which the
    // OVRPlugin path could ever have exercised, and both of which fail
    // *silently* in the direction that matters (a declined bind leaves the
    // guest rendering happily into GL storage nothing composites):
    //
    //   a. the format is the guest's, not ours. Steam Link's swapchains are
    //      SRGB8_ALPHA8, and ANGLE compares the internal format we claim
    //      against the MTLTexture's pixel format and refuses a mismatch
    //      ("Incompatible format"). So a provider hardwired to RGBA16F cannot
    //      back this guest at all.
    //   b. the texture ALREADY HAS STORAGE. An OpenXR runtime creates the
    //      swapchain images before anything knows which of them is an eye —
    //      that is only asserted at xrEndFrame — so the eye textures are given
    //      glTexStorage2D storage first and re-pointed at an MTLTexture
    //      afterwards. glTexStorage2D makes a texture IMMUTABLE, and whether
    //      glEGLImageTargetTexture2DOES may respecify one is a question about
    //      ANGLE rather than about the spec.
    printf("\n[Q5] the OpenXR guest's eye swapchain: SRGB8_ALPHA8, re-pointed "
           "after glTexStorage2D\n");
    const int SW = 2290, SH = 2400;         // measured, steamlink-vr
    MTLTextureDescriptor *sd = [MTLTextureDescriptor new];
    sd.textureType = MTLTextureType2DArray;
    sd.pixelFormat = MTLPixelFormatRGBA8Unorm_sRGB;
    sd.width = SW; sd.height = SH; sd.arrayLength = 2;
    sd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    sd.storageMode = MTLStorageModeShared;
    id<MTLTexture> sw = [mtl newTextureWithDescriptor:sd];
    check(sw != nil, "newTextureWithDescriptor (RGBA8Unorm_sRGB 2290x2400 x2)");
    if (sw) {
        // The image as the swapchain first made it: an ordinary immutable GL
        // texture, which the guest may already have rendered a frame into.
        uint32_t tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        while (glGetError() != GL_NO_ERROR) {}
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, SW, SH);
        check(glGetError() == GL_NO_ERROR, "glTexStorage2D SRGB8_ALPHA8 (immutable)");

        void *img = NULL;
        uint32_t re = bind_mtl_slice_to(dpy, sw, 0, GL_SRGB8_ALPHA8, tex, &img);
        check(re == tex, "the SAME texture name re-pointed at an MTLTexture slice");
        if (re == tex) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            uint32_t st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            char label[96];
            snprintf(label, sizeof label, "re-pointed: FBO complete (status 0x%x)", st);
            check(st == GL_FRAMEBUFFER_COMPLETE, label);
            glViewport(0, 0, SW, SH);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            check(draw_triangle(0.5f, 0.25f, 0.75f), "re-pointed: draw");
            glFinish();
            // Read the METAL side. The point is not that GL is consistent with
            // itself — it is that the pixels landed in the texture the
            // compositor will sample, through a name that had other storage a
            // moment ago. sRGB encodes on write, so 0.5 linear comes back as
            // ~188, not 128; the window is wide enough not to depend on the
            // exact transfer function and narrow enough to fail on black.
            uint8_t px[4] = { 0 };
            [sw getBytes:px bytesPerRow:4 bytesPerImage:4
              fromRegion:MTLRegionMake2D((NSUInteger)SW / 2, (NSUInteger)SH / 2, 1, 1)
             mipmapLevel:0 slice:0];
            snprintf(label, sizeof label,
                     "re-pointed: MTLTexture sRGB bytes = %u, %u, %u", px[0], px[1], px[2]);
            check(px[0] > 150 && px[0] < 220 && px[2] > px[0], label);
            if (img) eglDestroyImageKHR(dpy, img);
        }
    }

    printf("\n=== %s ===\n", g_fail
        ? "FAILED — the interop primitive does not hold; the compositor needs a blit design"
        : "PASSED — ANGLE renders into MTLTextures we own, per eye slice");
    return g_fail;
}
