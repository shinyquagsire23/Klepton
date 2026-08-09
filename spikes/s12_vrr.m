// S1.2 — does ANGLE rasterize through an MTLRasterizationRateMap we own?
//
// The gate for foveated guest rendering. Upstream ANGLE has no notion of a rate
// map at all (`grep rasterizationRate` in the Metal backend returns nothing), so
// angle-patches/klepton.patch adds one: a registry keyed on id<MTLTexture>
// (mtl_common.mm), a lookup in FramebufferMtl::prepareRenderPass, and a field on
// mtl::RenderPassDesc that convertToMetalDesc hands to Metal. This spike is what
// says that chain works end to end, before anything in the runtime depends on it.
//
// **Why a rate map and not GL_QCOM_framebuffer_foveated.** ANGLE's *frontend*
// already implements the QCOM foveation extensions, with a Vulkan backend behind
// them. They are the wrong shape for Metal: QCOM (and Vulkan's fragment density
// map) leave the attachment at full size and reconstruct internally, whereas
// MTLRasterizationRateMap makes the stored image physically SMALLER and leaves
// the unwarp to whoever samples it. That difference is the whole design problem
// — a foveated texture the guest samples with screen-space UVs is a wrong
// picture — and it is why the runtime foveates only targets it consumes itself.
//
// Four questions, in the order a failure would matter:
//
//   Q1  does this GPU support rate maps at all? (Apple silicon only; a fallback
//       to "no foveation" is the correct answer on anything else, and it should
//       be decided here rather than discovered as a Metal validation abort)
//   Q2  what does a map with a plausible falloff actually cost — screen size vs
//       physical size? This is the number the whole feature is FOR.
//   Q3  with the map registered, does a full-screen draw through ANGLE land in
//       the PHYSICAL rect rather than filling the texture? And with it
//       unregistered, does the same draw fill the texture again? The A/B is the
//       point: a bounding box that happens to match physical size proves
//       nothing unless removing the map changes it.
//   Q4  does the measured warp agree with the map's OWN screen->physical
//       mapping? This is the arithmetic the compositor's unwarp will do, so
//       getting it wrong here is getting the picture wrong later.
//
// Build/run: make vrr
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#import <Metal/Metal.h>

// The vendored build, always — ours is the patched one, and a different ANGLE on
// the machine would not have the entry point this spike exists to exercise.
#define ANGLE_VENDORED_DIR "vendor/out/Debug"

static const char *angle_dir(void) {
    const char *dir = getenv("KL_ANGLE_DIR");
    return dir ? dir : ANGLE_VENDORED_DIR;
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
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_PLATFORM_ANGLE_ANGLE            0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE       0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define EGL_DEVICE_EXT                      0x322C
#define EGL_METAL_DEVICE_ANGLE              0x34A6
#define EGL_METAL_TEXTURE_ANGLE             0x34A7
#define EGL_TEXTURE_INTERNAL_FORMAT_ANGLE   0x345D

#define GL_NO_ERROR              0
#define GL_TEXTURE_2D            0x0DE1
#define GL_TEXTURE_MIN_FILTER    0x2801
#define GL_TEXTURE_MAG_FILTER    0x2800
#define GL_NEAREST               0x2600
#define GL_FRAMEBUFFER           0x8D40
#define GL_COLOR_ATTACHMENT0     0x8CE0
#define GL_FRAMEBUFFER_COMPLETE  0x8CD5
#define GL_RGBA8                 0x8058
#define GL_COLOR_BUFFER_BIT      0x00004000
#define GL_VERTEX_SHADER         0x8B31
#define GL_FRAGMENT_SHADER       0x8B30
#define GL_COMPILE_STATUS        0x8B81
#define GL_LINK_STATUS           0x8B82
#define GL_ARRAY_BUFFER          0x8892
#define GL_STREAM_DRAW           0x88E0
#define GL_TRIANGLE_STRIP        0x0005
#define GL_FLOAT                 0x1406
#define GL_RENDERER              0x1F01
#define GL_RENDERBUFFER          0x8D41
#define GL_READ_FRAMEBUFFER      0x8CA8
#define GL_DRAW_FRAMEBUFFER      0x8CA9

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

// ---- resolved entry points ----
static void *(*eglGetPlatformDisplayEXT)(uint32_t, void *, const int32_t *);
static unsigned (*eglInitialize)(void *, int32_t *, int32_t *);
static unsigned (*eglChooseConfig)(void *, const int32_t *, void **, int32_t, int32_t *);
static void *(*eglCreateContext)(void *, void *, void *, const int32_t *);
static void *(*eglCreatePbufferSurface)(void *, void *, const int32_t *);
static unsigned (*eglMakeCurrent)(void *, void *, void *, void *);
static unsigned (*eglQueryDisplayAttribEXT)(void *, int32_t, intptr_t *);
static unsigned (*eglQueryDeviceAttribEXT)(void *, int32_t, intptr_t *);
static void *(*eglCreateImageKHR)(void *, void *, uint32_t, void *, const int32_t *);
static uint32_t (*eglGetError)(void);
static void (*glEGLImageTargetTexture2DOES)(uint32_t, void *);

static void (*glGenTextures)(int32_t, uint32_t *);
static void (*glBindTexture)(uint32_t, uint32_t);
static void (*glTexParameteri)(uint32_t, uint32_t, int32_t);
static void (*glGenFramebuffers)(int32_t, uint32_t *);
static void (*glBindFramebuffer)(uint32_t, uint32_t);
static void (*glFramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
static uint32_t (*glCheckFramebufferStatus)(uint32_t);
static void (*glClearColor)(float, float, float, float);
static void (*glClear)(uint32_t);
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
static void (*glGenRenderbuffers)(int32_t, uint32_t *);
static void (*glBindRenderbuffer)(uint32_t, uint32_t);
static void (*glRenderbufferStorageMultisample)(uint32_t, int32_t, uint32_t, int32_t, int32_t);
static void (*glFramebufferRenderbuffer)(uint32_t, uint32_t, uint32_t, uint32_t);
static void (*glBlitFramebuffer)(int32_t, int32_t, int32_t, int32_t,
                                 int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t);

// The patched-in entry point. Its ABSENCE is the most likely failure of this
// whole spike and it is worth naming: it means the loaded ANGLE is not the
// patched one (a stale out/Debug, or KL_ANGLE_DIR pointing elsewhere).
static void (*ANGLEMetalSetRasterizationRateMap)(void *texture, void *rateMap);
static void (*ANGLEMetalSetRasterizationRateMapForSize)(uint32_t w, uint32_t h,
                                                        uint32_t minSamples, void *rateMap);

static int resolve_all(void) {
    eglGetPlatformDisplayEXT = sym("eglGetPlatformDisplayEXT");
    eglInitialize            = sym("eglInitialize");
    eglChooseConfig          = sym("eglChooseConfig");
    eglCreateContext         = sym("eglCreateContext");
    eglCreatePbufferSurface  = sym("eglCreatePbufferSurface");
    eglMakeCurrent           = sym("eglMakeCurrent");
    eglQueryDisplayAttribEXT = sym("eglQueryDisplayAttribEXT");
    eglQueryDeviceAttribEXT  = sym("eglQueryDeviceAttribEXT");
    eglCreateImageKHR        = sym("eglCreateImageKHR");
    eglGetError              = sym("eglGetError");
    glEGLImageTargetTexture2DOES = sym("glEGLImageTargetTexture2DOES");

    glGenTextures            = sym("glGenTextures");
    glBindTexture            = sym("glBindTexture");
    glTexParameteri          = sym("glTexParameteri");
    glGenFramebuffers        = sym("glGenFramebuffers");
    glBindFramebuffer        = sym("glBindFramebuffer");
    glFramebufferTexture2D   = sym("glFramebufferTexture2D");
    glCheckFramebufferStatus = sym("glCheckFramebufferStatus");
    glClearColor             = sym("glClearColor");
    glClear                  = sym("glClear");
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

    glGenRenderbuffers       = sym("glGenRenderbuffers");
    glBindRenderbuffer       = sym("glBindRenderbuffer");
    glRenderbufferStorageMultisample = sym("glRenderbufferStorageMultisample");
    glFramebufferRenderbuffer = sym("glFramebufferRenderbuffer");
    glBlitFramebuffer        = sym("glBlitFramebuffer");

    ANGLEMetalSetRasterizationRateMap = sym("ANGLEMetalSetRasterizationRateMap");
    ANGLEMetalSetRasterizationRateMapForSize = sym("ANGLEMetalSetRasterizationRateMapForSize");

    return eglInitialize && eglCreateImageKHR && glEGLImageTargetTexture2DOES
        && eglQueryDeviceAttribEXT && glDrawArrays;
}

// A separable falloff: full rate in the middle, `edge` at the borders, linear in
// between. Deliberately crude — the SHAPE of a production map is a tuning
// question and this spike is about whether the mechanism engages at all.
static void fill_quality(float *q, int n, float edge) {
    for (int i = 0; i < n; i++) {
        // -1 at the left border, 0 at the centre, +1 at the right.
        float t = n > 1 ? (2.f * ((float)i + 0.5f) / (float)n - 1.f) : 0.f;
        if (t < 0) t = -t;
        q[i] = 1.f - (1.f - edge) * t;
    }
}

enum { VRR_ZONES = 16 };

static id<MTLRasterizationRateMap> make_map(id<MTLDevice> dev, int w, int h, float edge) {
    float qx[VRR_ZONES], qy[VRR_ZONES];
    fill_quality(qx, VRR_ZONES, edge);
    fill_quality(qy, VRR_ZONES, edge);
    // The float* initializer, not the subscript accessors: those take NSNumber,
    // which would box 32 objects to say the same thing.
    MTLRasterizationRateLayerDescriptor *layer =
        [[MTLRasterizationRateLayerDescriptor alloc]
            initWithSampleCount:MTLSizeMake((NSUInteger)VRR_ZONES, (NSUInteger)VRR_ZONES, 0)
                     horizontal:qx
                       vertical:qy];
    MTLRasterizationRateMapDescriptor *desc = [MTLRasterizationRateMapDescriptor
        rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 0)
                                               layer:layer];
    desc.label = @"klepton s12 probe";
    return [dev newRasterizationRateMapWithDescriptor:desc];
}

// Draw a solid quad covering [x0,x1] in NDC x and all of NDC y. Returns 0 on a
// GL error. The quad, rather than a clear: a clear can be serviced by a load
// action, which is NOT affected by the rate map and would silently pass.
static int draw_quad(float x0, float x1) {
    static uint32_t prog, vbo;
    if (!prog) {
        const char *vs = "#version 300 es\nin vec2 p;void main(){gl_Position=vec4(p,0.,1.);}\n";
        const char *fs = "#version 300 es\nprecision mediump float;out vec4 o;"
                         "void main(){o=vec4(1.,1.,1.,1.);}\n";
        uint32_t v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
        int32_t ok = 0;
        glShaderSource(v, 1, &vs, NULL); glCompileShader(v);
        glGetShaderiv(v, GL_COMPILE_STATUS, &ok); if (!ok) return 0;
        glShaderSource(f, 1, &fs, NULL); glCompileShader(f);
        glGetShaderiv(f, GL_COMPILE_STATUS, &ok); if (!ok) return 0;
        prog = glCreateProgram();
        glAttachShader(prog, v); glAttachShader(prog, f); glLinkProgram(prog);
        glGetProgramiv(prog, GL_LINK_STATUS, &ok); if (!ok) { prog = 0; return 0; }
        glGenBuffers(1, &vbo);
    }
    const float quad[] = { x0, -1.f,  x1, -1.f,  x0, 1.f,  x1, 1.f };
    glUseProgram(prog);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (intptr_t)sizeof quad, quad, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, NULL);
    glEnableVertexAttribArray(0);
    while (glGetError() != GL_NO_ERROR) {}
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return glGetError() == GL_NO_ERROR;
}

// A pure-Metal render of a full-screen quad through `map`, with no ANGLE
// anywhere. The control: if THIS does not warp either, the problem is our model
// of rasterization rate maps rather than anything in the ANGLE patch, and every
// minute spent in FramebufferMtl would be wasted. Returns the lit bounds.
static void metal_control(id<MTLDevice> dev, id<MTLRasterizationRateMap> map,
                          int texw, int texh, int vpw, int vph,
                          int *bw, int *bh, long *count);
static void metal_control_ex(id<MTLDevice> dev, id<MTLRasterizationRateMap> map,
                             int texw, int texh, int vpw, int vph, int use_view,
                             int *bw, int *bh, long *count);

// The bounding box of non-black texels, read from the MTLTexture directly —
// NOT through glReadPixels, which would go back through ANGLE and could not
// distinguish "the map warped the write" from "the map warped the read".
static void lit_bounds(id<MTLTexture> tex, int w, int h, int *bw, int *bh, long *count) {
    size_t stride = (size_t)w * 4;
    uint8_t *px = calloc(1, stride * (size_t)h);
    [tex getBytes:px
      bytesPerRow:stride
       fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
      mipmapLevel:0];
    int maxx = -1, maxy = -1;
    long n = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (px[(size_t)y * stride + (size_t)x * 4] > 127) {
                if (x > maxx) maxx = x;
                if (y > maxy) maxy = y;
                n++;
            }
        }
    }
    free(px);
    *bw = maxx + 1; *bh = maxy + 1; *count = n;
}

static void metal_control(id<MTLDevice> dev, id<MTLRasterizationRateMap> map,
                          int texw, int texh, int vpw, int vph,
                          int *bw, int *bh, long *count) {
    metal_control_ex(dev, map, texw, texh, vpw, vph, 0, bw, bh, count);
}

// `use_view` renders into a texture VIEW of the target rather than the target
// itself — which is what ANGLE does with an EGLImage-backed texture, and the one
// structural difference between this control and the ANGLE path.
static void metal_control_ex(id<MTLDevice> dev, id<MTLRasterizationRateMap> map,
                             int texw, int texh, int vpw, int vph, int use_view,
                             int *bw, int *bh, long *count) {
    *bw = *bh = -1; *count = -1;
    static const char *src =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "vertex float4 v(uint i [[vertex_id]]) {\n"
        "  float2 p[4] = {float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1)};\n"
        "  return float4(p[i], 0, 1);\n"
        "}\n"
        "fragment float4 f() { return float4(1,1,1,1); }\n";
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(src) options:nil error:&err];
    if (!lib) { printf("      control: library failed: %s\n",
                       [[err localizedDescription] UTF8String]); return; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction   = [lib newFunctionWithName:@"v"];
    pd.fragmentFunction = [lib newFunctionWithName:@"f"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pso) { printf("      control: pipeline failed: %s\n",
                       [[err localizedDescription] UTF8String]); return; }

    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = MTLTextureType2D;
    td.pixelFormat = MTLPixelFormatRGBA8Unorm;
    td.width = (NSUInteger)texw; td.height = (NSUInteger)texh;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];

    id<MTLTexture> attach = t;
    if (use_view) {
        attach = [t newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                      textureType:MTLTextureType2D
                                           levels:NSMakeRange(0, 1)
                                           slices:NSMakeRange(0, 1)];
    }

    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture     = attach;
    rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 1);
    rp.rasterizationRateMap            = map;

    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (!enc) { printf("      control: encoder failed (Metal refused the descriptor)\n"); return; }
    [enc setRenderPipelineState:pso];
    [enc setViewport:(MTLViewport){0, 0, (double)vpw, (double)vph, 0, 1}];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    lit_bounds(t, texw, texh, bw, bh, count);
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
    printf("=== S1.2 — ANGLE rasterizes through an MTLRasterizationRateMap ===\n");
    printf("  ANGLE from %s\n", dir);
    if (!resolve_all()) {
        fprintf(stderr, "FAIL: this ANGLE is missing an interop entry point\n");
        return 1;
    }
    // Named separately from the rest: this one is OURS, and its absence means
    // the loaded ANGLE predates angle-patches/klepton.patch rather than being
    // broken. Everything below would then measure the unpatched behaviour and
    // report it as a failure of the design.
    if (!ANGLEMetalSetRasterizationRateMap) {
        fprintf(stderr, "FAIL: ANGLEMetalSetRasterizationRateMap is missing from %s\n"
                        "  This ANGLE is not the patched build — run `make angle-debug`.\n", dir);
        return 1;
    }

    const int32_t dpy_attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE };
    void *dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, dpy_attrs);
    int32_t major = 0, minor = 0;
    if (!dpy || !eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "FAIL: eglInitialize on the Metal backend\n"); return 1;
    }
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

    intptr_t egl_dev = 0, mtl_dev_ptr = 0;
    if (!eglQueryDisplayAttribEXT(dpy, EGL_DEVICE_EXT, &egl_dev) || !egl_dev ||
        !eglQueryDeviceAttribEXT((void *)egl_dev, EGL_METAL_DEVICE_ANGLE, &mtl_dev_ptr) ||
        !mtl_dev_ptr) {
        fprintf(stderr, "FAIL: could not get ANGLE's MTLDevice\n"); return 1;
    }
    id<MTLDevice> dev = (__bridge id<MTLDevice>)(void *)mtl_dev_ptr;
    printf("  MTLDevice   = %s\n", [[dev name] UTF8String]);

    // ---- Q1: can this GPU do it at all?
    printf("\n[Q1] rasterization rate map support\n");
    BOOL supported = [dev supportsRasterizationRateMapWithLayerCount:1];
    check(supported, "supportsRasterizationRateMapWithLayerCount:1");
    if (!supported) {
        // Not a bug — it is the answer. Foveation is then simply off, which is
        // exactly what the runtime must do on such a device.
        printf("\n  This GPU has no variable rasterization rate. Foveation is\n"
               "  unavailable here and the runtime must fall back to uniform\n"
               "  rendering; that is a correct outcome, not a failure.\n");
        return 1;
    }

    // ---- Q2: what does the map cost?
    printf("\n[Q2] the map\n");
    const int W = 512, H = 512;
    const float EDGE = 0.3f;             // periphery rasterized at 30% rate
    id<MTLRasterizationRateMap> map = make_map(dev, W, H, EDGE);
    check(map != nil, "newRasterizationRateMapWithDescriptor");
    if (!map) return 1;
    MTLSize phys = [map physicalSizeForLayer:0];
    MTLSize scr  = [map screenSize];
    MTLSize gran = [map physicalGranularity];
    printf("      screen   %lux%lu\n", (unsigned long)scr.width, (unsigned long)scr.height);
    printf("      physical %lux%lu  (granularity %lux%lu)\n",
           (unsigned long)phys.width, (unsigned long)phys.height,
           (unsigned long)gran.width, (unsigned long)gran.height);
    printf("      fragments %.1f%% of uniform\n",
           100.0 * (double)(phys.width * phys.height) / (double)(scr.width * scr.height));
    // The runtime allocates eye textures at SCREEN size and lets Metal write the
    // physical sub-rect, so this must hold or the design does not work.
    check(phys.width <= (NSUInteger)W && phys.height <= (NSUInteger)H,
          "physical size fits inside the screen-sized texture");

    // ---- Q2b: the control. Metal only, no ANGLE, same map.
    printf("\n[Q2b] pure-Metal control — is the map honoured with no ANGLE involved?\n");
    int cw = 0, ch = 0; long cn = 0;
    metal_control(dev, map, W, H, W, H, &cw, &ch, &cn);
    printf("      screen-sized target (%dx%d), viewport %dx%d: lit %dx%d, %ld texels\n",
           W, H, W, H, cw, ch, cn);
    // One texel of slack: the quad's far edge lands on the boundary between the
    // last two granules, so the inclusive bounding box comes back one short.
    check(cw >= (int)phys.width - 1 && cw <= (int)phys.width &&
          ch >= (int)phys.height - 1 && ch <= (int)phys.height,
          "pure Metal warps a full-screen quad into the physical rect");
    // ANGLE attaches a texture VIEW of the EGLImage's texture, never the texture
    // itself. If a view defeats the map, that is the answer and it is a Metal
    // property, not an ANGLE bug.
    int vw = 0, vh = 0; long vn = 0;
    metal_control_ex(dev, map, W, H, W, H, 1, &vw, &vh, &vn);
    printf("      same, rendering into a texture VIEW:      lit %dx%d, %ld texels\n",
           vw, vh, vn);

    // ---- the render target: an MTLTexture we own, at SCREEN size
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = MTLTextureType2D;
    td.pixelFormat = MTLPixelFormatRGBA8Unorm;
    td.width = W; td.height = H;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;      // so getBytes: can read it back
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    if (!tex) { fprintf(stderr, "FAIL: newTextureWithDescriptor\n"); return 1; }

    const int32_t img_attrs[] = { EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, GL_RGBA8, EGL_NONE };
    void *img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_METAL_TEXTURE_ANGLE,
                                  (__bridge void *)tex, img_attrs);
    if (img == EGL_NO_IMAGE) {
        fprintf(stderr, "FAIL: eglCreateImageKHR, EGL error 0x%x\n",
                eglGetError ? eglGetError() : 0);
        return 1;
    }
    uint32_t gltex = 0, fbo = 0;
    glGenTextures(1, &gltex);
    glBindTexture(GL_TEXTURE_2D, gltex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gltex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FAIL: FBO incomplete\n"); return 1;
    }
    glViewport(0, 0, W, H);              // SCREEN space, always

    // ---- Q3: the A/B. Same draw, map registered and not.
    printf("\n[Q3] a full-screen draw, with the map and without it\n");
    int uw = 0, uh = 0, fw = 0, fh = 0;
    long un = 0, fn = 0;

    // Unfoveated first, so a failure to register cannot masquerade as success.
    ANGLEMetalSetRasterizationRateMap((__bridge void *)tex, NULL);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    if (!draw_quad(-1.f, 1.f)) { fprintf(stderr, "FAIL: draw (unfoveated)\n"); return 1; }
    glFinish();
    lit_bounds(tex, W, H, &uw, &uh, &un);
    printf("      no map:   lit bounds %dx%d, %ld texels\n", uw, uh, un);

    ANGLEMetalSetRasterizationRateMap((__bridge void *)tex, (__bridge void *)map);
    // Re-attach so ANGLE re-syncs the framebuffer. Registering a map is invisible
    // to GL state tracking, and FramebufferMtl caches its render pass descriptor
    // — without a dirty bit the hoist in prepareRenderPass never runs again.
    // Whether this is NEEDED is exactly what the run below measures.
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gltex, 0);
    // Clear ALONE first, and read it back. If the foveated pass silently does
    // nothing, the texture still holds the previous draw and every measurement
    // below is of stale content — which reads exactly like "the map was
    // ignored". Distinguishing those two is worth one extra readback.
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    int zw = 0, zh = 0; long zn = 0;
    lit_bounds(tex, W, H, &zw, &zh, &zn);
    printf("      after a black clear with the map bound: %ld lit texels%s\n", zn,
           zn ? "  <-- STALE, the pass did not run" : "");
    if (!draw_quad(-1.f, 1.f)) { fprintf(stderr, "FAIL: draw (foveated)\n"); return 1; }
    glFinish();
    lit_bounds(tex, W, H, &fw, &fh, &fn);
    printf("      with map: lit bounds %dx%d, %ld texels\n", fw, fh, fn);

    check(uw == W && uh == H, "without a map the draw fills the whole texture");
    check(fw == (int)phys.width && fh == (int)phys.height,
          "with a map the draw fills exactly the physical rect");
    check(fn < un, "the foveated draw shades strictly fewer fragments");

    // ---- Q4: does the warp match the map's own mapping?
    printf("\n[Q4] the measured warp vs the map's screen->physical mapping\n");
    // Half the screen in NDC x is [-1,0], i.e. screen x in [0, W/2).
    ANGLEMetalSetRasterizationRateMap((__bridge void *)tex, (__bridge void *)map);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    if (!draw_quad(-1.f, 0.f)) { fprintf(stderr, "FAIL: draw (half)\n"); return 1; }
    glFinish();
    int hw = 0, hh = 0; long hn = 0;
    lit_bounds(tex, W, H, &hw, &hh, &hn);
    MTLSamplePosition predicted =
        [map mapScreenToPhysicalCoordinates:MTLSamplePositionMake((float)W / 2.f, 0.f)
                                 forLayer:0];
    printf("      screen x=%d maps to physical x=%.1f; measured lit width %d\n",
           W / 2, predicted.x, hw);
    // One physical texel of slack: the boundary lands between granules and the
    // bounding box is inclusive.
    check(hw >= (int)predicted.x - 1 && hw <= (int)predicted.x + 1,
          "the half-screen quad ends where the map says it should");

    // ---- Q5: the shape the RUNTIME actually renders.
    //
    // Q3 only got its answer once a glFinish and a readback sat between the
    // clear and the draw, and "an extra glFinish fixed it" is not a mechanism.
    // A frame loop does clear-then-draw inside ONE pass, over and over, with the
    // map bound throughout — so that is what has to be measured. If only the
    // first iteration is wrong, this is a warm-up problem; if all of them are,
    // the map does not survive a load-action clear and Route A is in trouble.
    printf("\n[Q5] clear+draw in one pass, repeated — the frame-loop shape\n");
    ANGLEMetalSetRasterizationRateMap((__bridge void *)tex, (__bridge void *)map);
    int bad = 0, firstbad = -1;
    for (int i = 0; i < 5; i++) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!draw_quad(-1.f, 1.f)) { fprintf(stderr, "FAIL: draw (loop %d)\n", i); return 1; }
        glFinish();
        int lw = 0, lh = 0; long ln = 0;
        lit_bounds(tex, W, H, &lw, &lh, &ln);
        int ok = lw == (int)phys.width && lh == (int)phys.height;
        printf("      iter %d: lit %dx%d, %ld texels%s\n", i, lw, lh, ln, ok ? "" : "  <-- WRONG");
        if (!ok) { bad++; if (firstbad < 0) firstbad = i; }
    }
    check(bad == 0, "every iteration rasterizes into the physical rect");
    if (bad) {
        printf("      %d of 5 wrong, first at iteration %d\n", bad, firstbad);
    }

    // ---- Q6: the guest's ACTUAL shape — a multisampled renderbuffer resolved
    // into the eye texture by glBlitFramebuffer, with BOTH sides carrying the
    // same map.
    //
    // Measured on a real run (KL_GLFB_BLIT_PROBE=1): every write to an eye
    // texture is a blit from an RGBA16F multisampled renderbuffer, and no draw
    // ever targets an eye texture directly. So the scene target is what has to
    // be foveated, and this resolve is the seam its content has to cross.
    //
    // The whole matched-map design rests on that resolve being a
    // PHYSICAL-to-PHYSICAL copy. It is not obvious that it is: if ANGLE
    // implements the blit as a full-screen draw sampling with screen-space UVs,
    // the source's warped content gets warped a second time.
    //
    // The two outcomes are cleanly separable, which is why the half-screen quad
    // is the payload — a full-screen one would look identical either way:
    //
    //     lit width ~= 189  the physical content came across intact  (GOOD)
    //     lit width ~= 142  the map was applied twice                (BAD)
    //
    // The renderbuffer is registered by SIZE, because ANGLE allocates it and
    // there is no MTLTexture for us to name — so this also exercises the size
    // rule that exists for exactly that reason.
    printf("\n[Q6] MSAA renderbuffer -> eye texture, both foveated (the guest's shape)\n");
    if (!glRenderbufferStorageMultisample || !glBlitFramebuffer ||
        !ANGLEMetalSetRasterizationRateMapForSize) {
        printf("      SKIPPED — missing entry points (is this the patched ANGLE?)\n");
        g_fail = 1;
    } else {
        // minSamples=2: the rule must hit the MULTISAMPLED scene target and
        // nothing else. The guest allocates single-sampled post-processing
        // intermediates at the eye size too, and foveating one of those is a
        // wrong picture — it is sampled with screen-space coordinates that know
        // nothing about the warp.
        ANGLEMetalSetRasterizationRateMapForSize((uint32_t)W, (uint32_t)H, 2,
                                                 (__bridge void *)map);
        uint32_t rb = 0, fbo_ms = 0;
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, W, H);
        glGenFramebuffers(1, &fbo_ms);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_ms);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        uint32_t st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            printf("      MSAA FBO incomplete (0x%x)\n", st);
            g_fail = 1;
        } else {
            glViewport(0, 0, W, H);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            if (!draw_quad(-1.f, 0.f)) { fprintf(stderr, "FAIL: draw into MSAA rb\n"); return 1; }

            // Wipe the destination so nothing below can be stale content.
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glFinish();

            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_ms);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
            while (glGetError() != GL_NO_ERROR) {}
            glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            uint32_t be = glGetError();
            glFinish();
            int rw = 0, rh = 0; long rn = 0;
            lit_bounds(tex, W, H, &rw, &rh, &rn);
            printf("      resolved: lit %dx%d, %ld texels (blit GL error 0x%x)\n",
                   rw, rh, rn, be);
            printf("      expected %d if the physical content came across, "
                   "~%d if the map was applied twice\n",
                   (int)predicted.x,
                   (int)[map mapScreenToPhysicalCoordinates:
                             MTLSamplePositionMake(predicted.x, 0.f) forLayer:0].x);
            check(be == GL_NO_ERROR, "glBlitFramebuffer between two foveated targets succeeds");
            // Reported, NOT failed. This measures something deliberately not
            // implemented yet: ANGLE's blit is unconditionally a draw sampling
            // with screen-space UVs (`// Use blit with draw` in
            // FramebufferMtl::blit), so it warps content that is already warped.
            // The gate's job is to say the rate-map mechanism works; this line's
            // job is to say what the next piece of work is, and to go green by
            // itself the moment that work lands.
            int passthrough = rw >= (int)predicted.x - 2 && rw <= (int)predicted.x + 2;
            printf("  %-58s %s\n", "the resolve is a physical-to-physical copy",
                   passthrough ? "ok" : "OPEN");
            if (!passthrough) {
                printf("      ^ the map is applied TWICE. Matched-map foveation needs a\n"
                       "        physical-passthrough mode in ANGLE's blit: sample the source\n"
                       "        at the fragment's own physical position (input.position.xy in\n"
                       "        blit.metal) instead of the interpolated texCoords, whenever the\n"
                       "        source and destination carry the same rate map.\n");
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        ANGLEMetalSetRasterizationRateMapForSize((uint32_t)W, (uint32_t)H, 2, NULL);
    }

    // Unbind before the texture dies — the registry retains both sides, and the
    // runtime has the same obligation (kl_glfb's release path).
    ANGLEMetalSetRasterizationRateMap((__bridge void *)tex, NULL);

    printf("\n%s\n", g_fail ? "FAIL — see above" : "PASS — ANGLE honours the rate map");
    return g_fail;
}
