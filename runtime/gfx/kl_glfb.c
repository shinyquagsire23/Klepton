// The one-eye reference renderer, on ANGLE's Metal backend. It produces the
// known-good frame a real backend is diffed against; it is not the shipping
// backend. Opt in with KL_GLFB=1 — the null driver in kl_egl.c stays the
// default, so `make check` is unaffected by anything here.
//
// The Metal backend must be asked for by name. eglGetDisplay(EGL_DEFAULT_DISPLAY)
// selects ANGLE's OpenGL backend on macOS, which inherits desktop GL's limits
// and rejects ETC2 0x9279 — a format mandatory in the GLES 3.0 the guest speaks
// (`make angle`). EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE accepts it with shaders
// unmodified, so there is no shader rewrite, no format substitution, and no
// framebuffer impersonation: the guest's default framebuffer is a genuine EGL
// pbuffer, binding 0 means what it says, and GL_BACK is a valid buffer name.
//
// ABI hazard, about Darwin rather than about GL: the guest is AAPCS64 and ANGLE
// is a Darwin arm64 dylib. They agree on x0-x7 and disagree on the stack —
// AAPCS64 gives every stack argument an 8-byte slot, Darwin packs each to its
// natural size — so the first stack argument lands in the same place and
// everything after it can diverge. glBlitFramebuffer(8 ints, mask, filter) is
// the live case: the guest writes filter at sp+8, a Darwin callee reads sp+4.
// The wrappers below declare those parameters 64-bit so Darwin reproduces the
// AAPCS64 layout, then truncate. The int64_t parameters holding obviously
// 32-bit values are load-bearing; removing the widening silently corrupts the
// argument after it.
#include <dlfcn.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>          // powf — the debug tone map in the capture path
#include <float.h>         // FLT_MIN — subnormal literals
#include <ctype.h>
#include <zlib.h>
// The decoded-video image path — an AHardwareBuffer here is a
// CVPixelBuffer, and what ANGLE can take is the IOSurface behind it.
#include <CoreVideo/CoreVideo.h>
// The flat door's zero-readback storage. IOSurfaceRef.h by name: the umbrella
// IOSurface.h is macOS-only, and visionOS ships only the Ref/Types split.
#include <IOSurface/IOSurfaceRef.h>
#include "kl_glfb.h"
#include "kl_cvmtl.h"      // native-format video frames as MTLTextures
#include "kl_present.h"
#include "kl_egl.h"        // kl_gl_cap_* — the capability tables
#include "kl_reproject.h"  // kl_reproject_set_srgb_decode — see klfb_srgb_settle
#include "kl_fault.h"      // kl_fault_print_frames — who issued a GL call
#include "kl_target.h"     // kl_guest_kind — defaults that hold for one engine
#include "klepton.h"       // kl_trace_stub, for the per-name GL call trace
#include "kl_key.h"
#include "kl_guestpoke.h"  // kl_texunit_mask_active — the GL half of unity-texunit-mask

// ---- the EGL/GLES constants used here, so there is nothing to include ----
#define EGL_DEFAULT_DISPLAY  ((void *)0)
#define EGL_NO_CONTEXT       ((void *)0)
#define EGL_NONE             0x3038
#define EGL_WIDTH            0x3057
#define EGL_HEIGHT           0x3056
#define EGL_SURFACE_TYPE     0x3033
#define EGL_PBUFFER_BIT      0x0001
#define EGL_RENDERABLE_TYPE  0x3040
#define EGL_OPENGL_ES3_BIT   0x0040
#define EGL_RED_SIZE         0x3024
#define EGL_GREEN_SIZE       0x3023
#define EGL_BLUE_SIZE        0x3022
#define EGL_ALPHA_SIZE       0x3021
#define EGL_DEPTH_SIZE       0x3025
#define EGL_STENCIL_SIZE     0x3026
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_PLATFORM_ANGLE_ANGLE             0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE        0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE  0x3489

#define GL_NO_ERROR      0
#define GL_RGBA          0x1908
#define GL_TEXTURE_2D    0x0DE1
#define GL_SRGB8_ALPHA8  0x8C43
#define GL_RGBA8         0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT         0x1406
#define GL_RENDERER      0x1F01
#define GL_VERSION       0x1F02
#define GL_EXTENSIONS    0x1F03

// We use the VENDORED ANGLE, always — `make angle-debug` (which pulls and
// patches the checkout first). It is not interchangeable with a stock build:
// angle-patches/klepton.patch raises the Metal backend's kMaxShaderSamplers
// from 16 to 32 because Unity's HLSLCC-baked sampler bindings reach unit 35 on
// the post-processing passes, and a stock ANGLE fails GL-side validation there.
//
// So there is deliberately NO silent fallback to some other ANGLE on the
// machine. Loading Chrome's prebuilt because ours was missing would trade a
// clear "you have not built ANGLE yet" for a rendering bug hunted somewhere
// else entirely. KL_ANGLE_DIR still overrides, for pointing at a different
// build ON PURPOSE.
#define ANGLE_VENDORED_DIR "vendor/out/Debug"

static const char *kl_angle_dir(void) {
    const char *dir = kl_env_str("KL_ANGLE_DIR", NULL);
    if (dir) return dir;
    return ANGLE_VENDORED_DIR;
}

// ANGLE arrives in two shapes and the port needs both: a bare dylib on macOS,
// and a .framework bundle on visionOS (ANGLE emits ios_framework_bundle for
// iOS-family targets, and an app may only load code from inside its bundle
// anyway). Try both rather than making the caller know which.
//
// Note libEGL then finds libGLESv2 *itself* on iOS-family platforms — it dlopens
// <exec dir>/Frameworks/libGLESv2.framework/libGLESv2 (ANGLE's
// system_utils_posix.cpp) — so on the device both frameworks must be embedded,
// and this loader's own libGLESv2 open resolves to that same image.
static void *angle_dlopen(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.dylib", dir, name);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h) return h;
    snprintf(path, sizeof path, "%s/%s.framework/%s", dir, name, name);
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static void *g_egl_lib, *g_gles_lib;
static void *g_dpy, *g_surf, *g_ctx, *g_cfg;
// Kept so per-thread contexts can be created later, after init has chosen a config.
static void *(*a_eglCreateContext)(void *, void *, void *, const int32_t *);
static void *(*a_eglCreatePbufferSurface)(void *, void *, const int32_t *);
static int   g_on = -1, g_ready;
static int   g_w = 4000, g_h = 3200;      // one eye, Quest 2 per-eye default

// The flat door's zero-readback storage: when requested (before init), the
// root surface is a pbuffer over an IOSurface WE allocate, so the guest's
// default framebuffer IS shareable storage — the viewer (and one day the app)
// wraps the same IOSurface in an MTLTexture and samples it, and the display
// path stops calling glReadPixels entirely. Single-buffered by construction
// (an EGL surface has one storage), so a compositor sampling mid-frame can see
// a partial 2D UI — accepted for the pairing shell, where the alternative was
// a full-frame readback and a pipeline stall every swap.
static int          g_flat_request;
static IOSurfaceRef g_flat_ios;
static unsigned g_presented;

int kl_glfb_enabled(void) {
    // Value-aware, not presence-aware. Default off, which keeps every host loop
    // and `make check` exactly as they were — but the visionOS app now turns
    // this on for itself (kl_app_configure), so `KL_GLFB=0` has to be a real way
    // back to the null driver rather than a second way to say "on".
    if (g_on < 0) g_on = kl_env_on("KL_GLFB", 0);
    return g_on;
}

void kl_glfb_request_flat_surface(void) {
    if (g_ready) {
        fprintf(stderr, "  [glfb] flat-surface request arrived after ANGLE was "
                        "up — too late, the root surface exists\n");
        return;
    }
    g_flat_request = 1;
}

void *kl_glfb_flat_surface(int *w, int *h) {
    if (!g_flat_ios) return NULL;
    if (w) *w = g_w;
    if (h) *h = g_h;
    return (void *)g_flat_ios;
}

void kl_glfb_set_size(int w, int h) {
    // KL_GLFB_SIZE=WxH overrides the eye size the guest asked for. Every spike that
    // failed to reproduce the AGX abort used a 256x256 pbuffer while the guest uses
    // 1832x1920, and each thread gets its own with depth and stencil — so surface
    // size is the one resource axis none of them varied. Shrinking it here tests
    // that without touching anything else.
    const char *env = kl_env_str("KL_GLFB_SIZE", NULL);
    if (env) {
        int ew = 0, eh = 0;
        if (sscanf(env, "%dx%d", &ew, &eh) == 2 && ew > 0 && eh > 0) { w = ew; h = eh; }
    }
    if (w > 0 && h > 0 && !g_ready) { g_w = w; g_h = h; }
}

// Whether ANGLE offers EXT/OES_copy_image (set at init, read by the
// glCopyImageSubData thunk further down).
static int g_angle_copy_image;

// A name counts only if ANGLE itself defines it. dlsym on a handle also searches
// what that image links against, and on macOS ANGLE links the system's
// OpenGL.framework: a guest that probes for a desktop-era entry point
// (Xash's GLES renderer asks for glGenBuffersARB and takes glGenBuffers when it
// is absent) was handed Apple's, which has no context here and faults on its
// first use. A headset has no such framework, so the answer there was always
// "absent" — this makes the Mac give the same one.
static void *asym_in_angle(void *lib, const char *n) {
    void *p = lib ? dlsym(lib, n) : NULL;
    if (!p) return NULL;
    Dl_info di;
    if (dladdr(p, &di) && di.dli_fname &&
        !strstr(di.dli_fname, "libGLESv2") && !strstr(di.dli_fname, "libEGL"))
        return NULL;
    return p;
}
static void *asym(const char *n) {
    void *p = asym_in_angle(g_gles_lib, n);
    if (!p) p = asym_in_angle(g_egl_lib, n);
    return p;
}

// The few ANGLE entry points this file drives itself, as opposed to the ones it
// hands straight to the guest.
static uint32_t (*a_glGetError)(void);
static void     (*a_glReadPixels)(int32_t, int32_t, int32_t, int32_t, uint32_t,
                                  uint32_t, void *);
static void     (*a_glFinish)(void);
static void     (*a_glGetIntegerv)(uint32_t, int32_t *);
// ERRSCAN's framebuffer report (below): what GL itself thinks of the target a
// draw just failed against.
static uint32_t (*a_glCheckFramebufferStatus)(uint32_t);
static void     (*a_glGetFramebufferAttachmentParameteriv)(uint32_t, uint32_t,
                                                           uint32_t, int32_t *);
static const uint8_t *(*a_glGetString)(uint32_t);
static unsigned (*a_eglMakeCurrent)(void *, void *, void *, void *);
// ANGLE resolves GLES *extension* entry points (e.g. glTextureViewOES) through its own
// eglGetProcAddress, not as dlsym-exported symbols — so asym() misses them. Used as a fallback
// in kl_glfb_sym so those functions reach ANGLE instead of aborting on the null driver.
static void *(*a_eglGetProcAddress)(const char *);

static void klfb_selftest(void);

// --- glTextureViewOES emulation (ANGLE-Metal has no GL_OES/EXT_texture_view entry point) ---
// hl2's multiview world path views its stereo array RTs (e.g. the DEPTH24_STENCIL8 2064x2208x2
// array, and per-eye 2D slices). We give the view its OWN storage matching origtexture's WxH:
// not a true alias, but the multiview RT is rendered fresh each frame, so a complete texture of
// the right shape keeps the framebuffer valid (an empty view crashes ANGLE's Metal encoder on the
// next draw). origtexture's dims can't be re-queried on ES 3.0 (no glGetTexLevelParameteriv, and
// its target differs from the view's), so we read them from the existing glTexStorage-fed table
// (klfb_tex_info). The allocation saves/restores the binding so it never disturbs guest GL state.
static int  klfb_tex_info(uint32_t name, uint32_t *fmt, int32_t *w, int32_t *h);
static void klfb_note_tex_storage(uint32_t name, uint32_t fmt, int32_t w, int32_t h);
static int  klfb_block_geom(uint32_t fmt, int *bw, int *bh, int *bytes);
static void klfb_TextureViewOES(uint32_t texture, uint32_t target, uint32_t origtexture,
                                uint32_t internalformat, uint32_t minlevel, uint32_t numlevels,
                                uint32_t minlayer, uint32_t numlayers)
{
    (void)minlevel; (void)minlayer;
    enum { T_2D_ARRAY = 0x8C1A, B_2D = 0x8069, B_2D_ARRAY = 0x8C1D };
    static void (*bindTex)(uint32_t, uint32_t);
    static void (*ts2D)(uint32_t, int32_t, uint32_t, int32_t, int32_t);
    static void (*ts3D)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t);
    static int loaded;
    if (!loaded) { loaded = 1; bindTex = asym("glBindTexture");
                   ts2D = asym("glTexStorage2D"); ts3D = asym("glTexStorage3D"); }
    uint32_t f = 0; int32_t w = 0, h = 0;
    klfb_tex_info(origtexture, &f, &w, &h);
    if (w <= 0 && g_w > 0) { w = g_w; h = g_h; }   // fall back to the eye size
    if (bindTex && w > 0) {
        int32_t prev = 0;
        if (a_glGetIntegerv) a_glGetIntegerv(target == T_2D_ARRAY ? B_2D_ARRAY : B_2D, &prev);
        bindTex(target, texture);
        int levels = numlevels ? (int)numlevels : 1;
        if (target == T_2D_ARRAY && ts3D)
            ts3D(target, levels, internalformat, w, h, numlayers ? (int)numlayers : 1);
        else if (ts2D)
            ts2D(target, levels, internalformat, w, h);
        klfb_note_tex_storage(texture, internalformat, w, h);   // the view is now a known texture
        bindTex(target, (uint32_t)prev);                        // restore guest binding
    }
    fprintf(stderr, "  [glfb] glTextureViewOES emulated: tex=%u<-orig=%u target=0x%x fmt=0x%x "
                    "%dx%d layers=%u levels=%u\n",
            texture, origtexture, target, internalformat, w, h, numlayers, numlevels);
}

// KL_GLFB_DEBUG_CB=1 registers this with glDebugMessageCallback and prints
// every message. The vendored debug ANGLE speaks KHR_debug fluently, so a
// guest call failing silently (the "0 lit" frame was one) comes with its
// reason here instead of needing an error-code probe per call site.
static void klfb_debug_cb(uint32_t source, uint32_t type, uint32_t id,
                          uint32_t severity, int32_t length, const char *msg,
                          const void *user) {
    (void)source; (void)type; (void)id; (void)severity; (void)user;
    fprintf(stderr, "  [glcb] %.*s\n", length, msg ? msg : "");
}

int kl_glfb_init(void) {
    if (!kl_glfb_enabled() || g_ready) return g_ready;

    // Applied here rather than only in kl_glfb_set_size, which this path never
    // calls — the eye size stays at its 1832x1920 default unless OVRPlugin says
    // otherwise, so a setter-only override silently does nothing.
    const char *size_env = kl_env_str("KL_GLFB_SIZE", NULL);
    if (size_env) {
        int ew = 0, eh = 0;
        if (sscanf(size_env, "%dx%d", &ew, &eh) == 2 && ew > 0 && eh > 0) {
            g_w = ew; g_h = eh;
        }
    }

    const char *dir = kl_angle_dir();
    g_egl_lib  = angle_dlopen(dir, "libEGL");
    g_gles_lib = angle_dlopen(dir, "libGLESv2");
    if (!g_egl_lib || !g_gles_lib) {
        fprintf(stderr, "  [glfb] cannot load ANGLE from %s\n         (%s)\n"
                        "         build it with 'make angle-debug' — that pulls and "
                        "patches the checkout too.\n"
                        "         (KL_ANGLE_DIR overrides, but a stock ANGLE is not "
                        "equivalent: see angle-patches/.)\n"
                        "         staying on the null driver\n", dir, dlerror());
        return 0;
    }

    void *(*eglGetPlatformDisplayEXT)(uint32_t, void *, const int32_t *) =
        asym("eglGetPlatformDisplayEXT");
    unsigned (*eglInitialize)(void *, int32_t *, int32_t *) = asym("eglInitialize");
    unsigned (*eglChooseConfig)(void *, const int32_t *, void **, int32_t, int32_t *) =
        asym("eglChooseConfig");
    a_eglCreatePbufferSurface = asym("eglCreatePbufferSurface");
    a_eglCreateContext        = asym("eglCreateContext");
    a_eglMakeCurrent = asym("eglMakeCurrent");
    if (!eglGetPlatformDisplayEXT || !eglInitialize || !eglChooseConfig ||
        !a_eglCreatePbufferSurface || !a_eglCreateContext || !a_eglMakeCurrent) {
        fprintf(stderr, "  [glfb] ANGLE is missing core EGL entry points\n");
        return 0;
    }

    // Metal by name. The default display selects ANGLE's OpenGL backend, and with
    // it every limitation this move exists to escape.
    const int32_t dpy_attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
        EGL_NONE,
    };
    g_dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY,
                                     dpy_attrs);
    int32_t major = 0, minor = 0;
    if (!g_dpy || !eglInitialize(g_dpy, &major, &minor)) {
        fprintf(stderr, "  [glfb] eglInitialize failed\n");
        return 0;
    }

    // The eye. A real pbuffer, so the guest's framebuffer 0 is a real default
    // framebuffer — no FBO impersonation, and GL_BACK means what it says.
    const int32_t cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE,
    };
    void *cfg = NULL; int32_t ncfg = 0;
    if (!eglChooseConfig(g_dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "  [glfb] no ES3 pbuffer config\n");
        return 0;
    }
    g_cfg = cfg;
    const int32_t surf_attrs[] = { EGL_WIDTH, g_w, EGL_HEIGHT, g_h, EGL_NONE };
    // The flat door's surface first, when asked for: a pbuffer over an
    // IOSurface we allocate. Every failure falls through to the plain pbuffer
    // — the accessor answering NULL is how the caller knows to keep the
    // readback path — and MakeCurrent gets its own retry below because a
    // surface that CREATES but cannot go current would otherwise take the
    // whole renderer down for what is only a display optimisation.
    if (g_flat_request) {
        void *(*mkclient)(void *, uint32_t, void *, void *, const int32_t *) =
            asym("eglCreatePbufferFromClientBuffer");
        if (mkclient) {
            // Zeroed at birth (lock + memset) so the first composited frame is
            // black rather than whatever the allocator held.
            CFMutableDictionaryRef props = CFDictionaryCreateMutable(
                kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks,
                &kCFTypeDictionaryValueCallBacks);
            int32_t iw = g_w, ih = g_h, bpe = 4, pfmt = 'BGRA';
            CFNumberRef n;
            n = CFNumberCreate(NULL, kCFNumberSInt32Type, &iw);
            CFDictionarySetValue(props, kIOSurfaceWidth, n); CFRelease(n);
            n = CFNumberCreate(NULL, kCFNumberSInt32Type, &ih);
            CFDictionarySetValue(props, kIOSurfaceHeight, n); CFRelease(n);
            n = CFNumberCreate(NULL, kCFNumberSInt32Type, &bpe);
            CFDictionarySetValue(props, kIOSurfaceBytesPerElement, n); CFRelease(n);
            n = CFNumberCreate(NULL, kCFNumberSInt32Type, &pfmt);
            CFDictionarySetValue(props, kIOSurfacePixelFormat, n); CFRelease(n);
            g_flat_ios = IOSurfaceCreate(props);
            CFRelease(props);
            if (g_flat_ios) {
                IOSurfaceLock(g_flat_ios, 0, NULL);
                memset(IOSurfaceGetBaseAddress(g_flat_ios), 0,
                       IOSurfaceGetAllocSize(g_flat_ios));
                IOSurfaceUnlock(g_flat_ios, 0, NULL);
                // The same six-attribute contract the video image states, with
                // the width and height this surface will be rendered at.
                const int32_t io_attrs[] = {
                    EGL_WIDTH,  g_w,
                    EGL_HEIGHT, g_h,
                    0x345A /* EGL_IOSURFACE_PLANE_ANGLE */,           0,
                    0x3081 /* EGL_TEXTURE_TARGET */,                  0x305F /* EGL_TEXTURE_2D */,
                    0x3080 /* EGL_TEXTURE_FORMAT */,                  0x305E /* EGL_TEXTURE_RGBA */,
                    0x345C /* EGL_TEXTURE_TYPE_ANGLE */,              0x1401 /* GL_UNSIGNED_BYTE */,
                    0x345D /* EGL_TEXTURE_INTERNAL_FORMAT_ANGLE */,   0x80E1 /* GL_BGRA_EXT */,
                    EGL_NONE,
                };
                g_surf = mkclient(g_dpy, 0x3454 /* EGL_IOSURFACE_ANGLE */,
                                  (void *)g_flat_ios, cfg, io_attrs);
                if (!g_surf) {
                    fprintf(stderr, "  [glfb] IOSurface pbuffer refused — the flat "
                                    "door keeps the readback path\n");
                    CFRelease(g_flat_ios);
                    g_flat_ios = NULL;
                }
            }
        }
        if (g_surf)
            fprintf(stderr, "  [glfb] flat surface is a %dx%d IOSurface pbuffer — "
                            "the picture is shareable storage, nothing reads back\n",
                    g_w, g_h);
    }
    if (!g_surf) g_surf = a_eglCreatePbufferSurface(g_dpy, cfg, surf_attrs);
    const int32_t ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    g_ctx  = a_eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (g_surf && g_ctx && g_flat_ios && !a_eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
        // The IOSurface surface created but cannot go current: retire it and
        // retry on an ordinary pbuffer before declaring the renderer down.
        fprintf(stderr, "  [glfb] the IOSurface pbuffer cannot go current — "
                        "falling back to a plain pbuffer (readback path)\n");
        unsigned (*dsurf)(void *, void *) = asym("eglDestroySurface");
        if (dsurf) dsurf(g_dpy, g_surf);
        CFRelease(g_flat_ios);
        g_flat_ios = NULL;
        g_surf = a_eglCreatePbufferSurface(g_dpy, cfg, surf_attrs);
    }
    if (!g_surf || !g_ctx || !a_eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
        fprintf(stderr, "  [glfb] could not make an ES3 context current\n");
        return 0;
    }

    a_eglGetProcAddress = asym("eglGetProcAddress");
    a_glGetError   = asym("glGetError");
    a_glReadPixels = asym("glReadPixels");
    a_glFinish     = asym("glFinish");
    a_glGetIntegerv = asym("glGetIntegerv");
    a_glCheckFramebufferStatus = asym("glCheckFramebufferStatus");
    a_glGetFramebufferAttachmentParameteriv =
        asym("glGetFramebufferAttachmentParameteriv");
    a_glGetString  = asym("glGetString");
    g_ready = 1;

    // KL_GLFB_DEBUG_CB=1: register a KHR_debug callback and print every
    // message. The vendored debug ANGLE speaks KHR_debug fluently, so a guest
    // call failing silently (the "0 lit" frame was one) comes with its reason
    // here instead of needing an error-code probe per call site.
    if (kl_env_on("KL_GLFB_DEBUG_CB", 0)) {
        void (*glDebugMessageCallback)(const void *, const void *) =
            asym("glDebugMessageCallback");
        void (*glEnable)(uint32_t) = asym("glEnable");
        if (glDebugMessageCallback) {
            glDebugMessageCallback((const void *)klfb_debug_cb, NULL);
            // GL_DEBUG_OUTPUT 0x92E0 — synchronous delivery.
            if (glEnable) glEnable(0x92E0);
            fprintf(stderr, "  [glfb] KHR_debug callback registered\n");
        } else {
            fprintf(stderr, "  [glfb] no glDebugMessageCallback in this ANGLE\n");
        }
    }
    fprintf(stderr, "  [glfb] ANGLE %dx%d — %s\n", g_w, g_h,
            a_glGetString ? (const char *)a_glGetString(GL_RENDERER) : "?");
    fprintf(stderr, "  [glfb] %s\n",
            a_glGetString ? (const char *)a_glGetString(GL_VERSION) : "?");
    // Whether a half-float eye texture can be RENDERED to, which on an ES 3.0
    // context is an extension question and not a version one: RGBA16F is
    // texture-filterable in core ES 3.0 but only colour-renderable with
    // EXT_color_buffer_(half_)float. Printed because the failure mode is a
    // single engine-side line ("RenderTexture.Create failed: format
    // unsupported - RGBA16 SFloat") a long way from anything GL, and the
    // engine's own answer depends on which of the two it looks for -- Unity
    // 2019.4 infers it from the version it is told, 2018.4 asks for the string.
    if (a_glGetString) {
        const char *ext = (const char *)a_glGetString(GL_EXTENSIONS);
        if (!ext) ext = "";
        fprintf(stderr, "  [glfb] renderable float: EXT_color_buffer_float=%s "
                        "EXT_color_buffer_half_float=%s\n",
                strstr(ext, "GL_EXT_color_buffer_float") ? "yes" : "NO",
                strstr(ext, "GL_EXT_color_buffer_half_float") ? "yes" : "NO");
        // The ES 3.2 core calls served through their extension names (see
        // g_es32_alias) work only if these are on.
        g_angle_copy_image = strstr(ext, "GL_EXT_copy_image") ||
                             strstr(ext, "GL_OES_copy_image");
        fprintf(stderr, "  [glfb] ES 3.2 by extension: copy_image=%s "
                        "draw_buffers_indexed=%s\n",
                (strstr(ext, "GL_EXT_copy_image") || strstr(ext, "GL_OES_copy_image"))
                    ? "yes" : "NO",
                (strstr(ext, "GL_OES_draw_buffers_indexed") ||
                 strstr(ext, "GL_EXT_draw_buffers_indexed")) ? "yes" : "NO");
        static const char *const want[] = {
            "GL_OES_draw_buffers_indexed", "GL_OES_copy_image", "GL_EXT_copy_image",
            "GL_OES_draw_elements_base_vertex", "GL_EXT_texture_buffer",
            "GL_OVR_multiview2", "GL_ANGLE_multiview_multisample",
            "GL_EXT_clip_control", "GL_OES_texture_border_clamp",
        };
        fprintf(stderr, "  [glfb] beyond ES 3.0:");
        for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
            fprintf(stderr, " %s=%s", want[i] + 3, strstr(ext, want[i]) ? "yes" : "NO");
        fprintf(stderr, "\n");

        // ...and the extensions the description may REPEAT rather than assert
        // (kl_egl.c): adopted only if this string names them, and only where
        // KL_GL_BACKEND_EXT asks for it — so the line is printed only then.
        kl_gl_cap_adopt_backend_extensions(ext);
        int adopted = kl_gl_cap_adopted_extension_count();
        if (adopted || kl_env_on("KL_GL_BACKEND_EXT", 0)) {
            fprintf(stderr, "  [glfb] backend extensions adopted: %d%s", adopted,
                    adopted ? " —" : " (none of the candidates are present)");
            for (int i = 0; i < adopted; i++)
                fprintf(stderr, " %s", kl_gl_cap_adopted_extension(i));
            fprintf(stderr, "\n");
        }
    }

    // Release it. EGL binds a context to one thread at a time and *refuses* to
    // migrate it — unlike CGL, which silently allows it. Leaving it current on
    // whichever thread happened to resolve the first GL symbol meant the guest's
    // render thread could never claim it (EGL_BAD_ACCESS), so every guest GL call
    // ran with no context: silently, since that is not an error, just nothing.
    // Whoever asks next through kl_glfb_make_current gets it.
    a_eglMakeCurrent(g_dpy, NULL, NULL, NULL);
    klfb_selftest();
    return 1;
}

// A GL context is current per *thread*, and Unity drives GL from its own render
// thread rather than the one that created the context. Calling in from a thread
// with no current context dereferences a null dispatch table — which presented as
// a SIGSEGV at 0x2d8 inside glFlush when this was first wired up. eglMakeCurrent
// is the guest telling us which thread now owns the context, so it is the hook.
//
// Default mode: ONE context that migrates. Unity plays by the EGL rules — it
// releases the context on one thread (eglMakeCurrent(NULL)) before taking it on
// the next — so the root context simply follows ownership: whichever thread took
// it most recently owns it, and a release frees it for the next. This is the
// correct shape, and it is what makes the guest's FBOs work: measured by
// KL_GLFB_TRACE_FBO, Unity creates its framebuffer objects during setup and
// draws with them on the render thread, and FBOs are container objects — never
// shared — so per-thread contexts (KL_GLFB_SHARED) see the setup thread's FBOs
// as empty objects ("incomplete: no attachments", every draw -> 0x506).
//
// KL_GLFB_SHARED=1: a context per thread, all sharing objects. Textures,
// buffers and programs are shared; FBOs and VAOs are not — so this mode renders
// nothing meaningful with this guest, and it stays only because the instruments
// built on it (the draw census, the lock trampolines) still answer questions.
//
// Each thread also gets its own eye-sized pbuffer in SHARED mode, so whichever
// thread draws has a real default framebuffer of the right size — and the
// capture, which runs on that same thread, reads exactly what that thread drew.
static pthread_key_t g_tls_key;
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;

typedef struct { void *ctx, *surf; int probed; int debug_cb; } klfb_thread_gl;

static void klfb_tls_free(void *p) { free(p); }
static int g_tls_ready;
static void klfb_tls_init(void) {
    kl_key_create_or_die(&g_tls_key, klfb_tls_free, "the per-thread GL contexts");
    g_tls_ready = 1;
}

// ---- emulation state, kept PER CONTEXT -------------------------------------
//
// Several of the ES 3.1 emulations below remember what the guest has bound —
// the buffer on each target, the 2D texture on each unit, the vertex layout of
// each VAO, the CPU shadow of a mapped buffer. That is context state, and it
// was kept once for the process, on the reasoning that GL is serialised on one
// thread. Unreal runs TWO contexts at once (its shared context on the game
// thread for uploads, its rendering context on the render thread), each on its
// own ANGLE context here, so each thread overwrote what the other had recorded:
// a restore put the other thread's texture back on a unit, an unmap flushed the
// other thread's shadow into the wrong buffer, a draw applied the other
// context's VAO layout (Maestro: exploded geometry, black textures, refused
// buffer uploads).
//
// The state follows the ANGLE context the calling thread holds, not the thread:
// in migration mode the one root context moves between threads and its state
// has to move with it. A thread with no context yet shares the NULL entry.
#define KLFB_CTX_SLOT_TB    0
#define KLFB_CTX_SLOT_EMAPS 1
#define KLFB_CTX_SLOT_VAO   2
#define KLFB_CTX_SLOT_FBO   3
#define KLFB_CTX_SLOTS      4
#define KLFB_CTXS           16
static struct { void *ctx; int used; void *slot[KLFB_CTX_SLOTS]; } g_ctx_state[KLFB_CTXS];
static pthread_mutex_t g_ctx_state_lock = PTHREAD_MUTEX_INITIALIZER;
static void *klfb_ctx_slot(int which, size_t size) {
    static _Thread_local void *cached_ctx;
    static _Thread_local int   cached_valid;
    static _Thread_local void *cached[KLFB_CTX_SLOTS];
    void *ctx = NULL;
    if (g_tls_ready) {
        klfb_thread_gl *t = pthread_getspecific(g_tls_key);
        if (t) ctx = t->ctx;
    }
    if (cached_valid && cached_ctx == ctx && cached[which]) return cached[which];
    if (!cached_valid || cached_ctx != ctx) {
        memset(cached, 0, sizeof cached);
        cached_ctx = ctx; cached_valid = 1;
    }
    void *p = NULL;
    pthread_mutex_lock(&g_ctx_state_lock);
    int i = 0, free_i = -1;
    for (; i < KLFB_CTXS; i++) {
        if (g_ctx_state[i].used && g_ctx_state[i].ctx == ctx) break;
        if (!g_ctx_state[i].used && free_i < 0) free_i = i;
    }
    if (i == KLFB_CTXS) {
        // More contexts than rows: the last row is shared rather than refused.
        i = free_i >= 0 ? free_i : KLFB_CTXS - 1;
        if (!g_ctx_state[i].used) { g_ctx_state[i].used = 1; g_ctx_state[i].ctx = ctx; }
    }
    if (!g_ctx_state[i].slot[which]) g_ctx_state[i].slot[which] = calloc(1, size);
    p = g_ctx_state[i].slot[which];
    pthread_mutex_unlock(&g_ctx_state_lock);
    cached[which] = p;
    return p;
}

// Owner of the root context in migration mode: the thread id that last took it
// and has not released it, 0 when free.
static uint64_t g_root_owner;

// Shared contexts the guest has RELEASED, waiting for the thread that takes its
// context next. The handoff below gives a second thread its own context in the
// root's share group; when the guest then moves THAT context on — UE4 replaces
// its render thread after the startup movie: eglMakeCurrent(NULL) on the old
// thread, eglMakeCurrent(ctx) on the new one — a fresh context per thread loses
// every FBO and VAO the guest made on the old one. Saints & Sinners
// (x2897337400373711) drew its whole menu into those stale names: every draw
// GL_INVALID_FRAMEBUFFER_OPERATION, FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT,
// black eyes and black panels at a healthy 90 Hz. So a released shared context
// is parked here and the next claimant takes it over, as EGL itself would let
// the guest's one context move. Tales from the Galaxy's Edge
// (x3484270071659289, UE 4.24) restarts its render thread the same way and drew
// only empty eye frames on the fresh context (fix 15 and fix 16, 2026-10-11).
// The Walking Dead: Saints & Sinners - Chapter 2: Retribution
// (x5190288877703616, UE 4.27.2) replaces its render thread the same way: the
// first render thread releases its context, two new threads in turn were each
// given a fresh one and asked for framebuffer names from 1 again, and the view
// stayed black with flickers of texture at 90 Hz while its sound played (on
// device, 2026-10-17).
// Vader Immortal I (x2108775495884888, UE 4.19) as well: its render thread
// made the framebuffer for one or two of the three eye stages, released its
// context, and the next thread's fresh context had no such framebuffer. Those
// stages were never drawn — all zero in the eye dumps while the third held the
// picture — so the eyes showed black on two frames of three: lights and
// holograms flickered, worse at 50 Hz, and screenshots came out black now
// and then (on device, 2026-10-19). Episodes II and III (x2223615721048141,
// x2426206484098337) are the same engine build and are named with it, not yet
// run here.
//
// The restart is the ENGINE's, not either title's, so this is a candidate for
// every Unreal guest on GLES — but it is on for the titles it was measured with
// and nothing else: every other guest keeps the handoff it was brought up
// with until the migration has been run against it. It is also an
// approximation. The guest's EGLContext handle never reaches this file (every
// guest context is the one root, or a handoff sibling of it), so "the context
// that was released" is taken to be "the context the next thread means", which
// holds while the guest moves ONE context around and not beyond that.
// KL_GLFB_SHARED_MIGRATE=0/1 overrides the per-target default.
#define KLFB_PARKED_MAX 4
static struct { void *ctx, *surf; } g_parked[KLFB_PARKED_MAX];
static int g_nparked;
static pthread_mutex_t g_parked_lock = PTHREAD_MUTEX_INITIALIZER;

static int klfb_shared_migrate(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_GLFB_SHARED_MIGRATE",
                       t && (strcmp(t, "x2897337400373711") == 0 ||
                             strcmp(t, "x3484270071659289") == 0 ||
                             strcmp(t, "x5190288877703616") == 0 ||
                             strcmp(t, "x2108775495884888") == 0 ||
                             strcmp(t, "x2223615721048141") == 0 ||
                             strcmp(t, "x2426206484098337") == 0));
    }
    return on;
}

// Pop a parked shared context into `t`. 1 if one was there.
static int klfb_take_parked(klfb_thread_gl *t, uint64_t tid) {
    if (!klfb_shared_migrate()) return 0;
    pthread_mutex_lock(&g_parked_lock);
    int ok = g_nparked > 0;
    if (ok) {
        g_nparked--;
        t->ctx  = g_parked[g_nparked].ctx;
        t->surf = g_parked[g_nparked].surf;
    }
    pthread_mutex_unlock(&g_parked_lock);
    if (ok)
        fprintf(stderr, "  [glfb] thread %llu takes over the released shared context "
                        "%p (its FBOs and VAOs come with it)\n",
                (unsigned long long)tid, t->ctx);
    return ok;
}

void kl_glfb_make_current(void) {
    if (!g_ready || !a_eglMakeCurrent) return;
    pthread_once(&g_tls_once, klfb_tls_init);

    klfb_thread_gl *t = pthread_getspecific(g_tls_key);
    uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
    if (!t) {
        t = calloc(1, sizeof *t);
        if (!t) return;
        if (!kl_env_on("KL_GLFB_SHARED", 0)) {
            // Migration mode: take the root context if it is free or already
            // ours; if another thread is holding it, this thread gets nothing —
            // same as EGL, which would answer EGL_BAD_ACCESS.
            if (!g_root_owner || g_root_owner == tid) {
                t->ctx = g_ctx; t->surf = g_surf;
                g_root_owner = tid;
            } else if (klfb_take_parked(t, tid)) {
                // The context the guest released elsewhere, moved here.
            } else {
                // Another thread nominally owns the root context, but THIS call
                // is the guest's own eglMakeCurrent (klegl_MakeCurrent is the
                // only caller) — an explicit "GL happens on this thread now".
                // Denying it left the thread context-less: its glMapBufferRange
                // returns NULL and the engine memmoves from it (TWD2 died there,
                // on RenderThread 0, when UE4 handed GL from its setup thread to
                // the render thread without an intervening release). UE4's GLES
                // RHI renders single-threaded — sequential ownership, never
                // concurrent. Stealing the ROOT context is illegal, though:
                // it is still bound on the owning thread, and eglMakeCurrent on
                // this one returns EGL_BAD_ACCESS (0x3002) — a context cannot be
                // current on two threads, and it can only be released from the
                // thread that holds it, which is not this one. So this thread
                // gets its OWN context in the same SHARE GROUP: textures,
                // buffers and programs (everything the guest loaded from the
                // paks) are shared, and only the container objects (FBOs, VAOs)
                // are per-context — which UE4 creates on the render thread
                // anyway. This is the EGL-legal handoff, and it replaces the
                // BAD_ACCESS-then-NULL-map crash TWD2 hit on its render thread.
                static int migrated;
                if (!migrated) {
                    migrated = 1;
                    fprintf(stderr, "  [glfb] context handoff: thread %llu gets a "
                                    "shared context (thread %llu holds the root; "
                                    "UE4 render-thread handoff)\n",
                            (unsigned long long)tid, (unsigned long long)g_root_owner);
                }
                const int32_t surf_attrs[] = { EGL_WIDTH, g_w, EGL_HEIGHT, g_h, EGL_NONE };
                const int32_t ctx_attrs[]  = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
                t->surf = a_eglCreatePbufferSurface(g_dpy, g_cfg, surf_attrs);
                t->ctx  = a_eglCreateContext(g_dpy, g_cfg, g_ctx, ctx_attrs);
                // NOT the root owner — this is a second, shared context, so
                // g_root_owner is left with the original holder.
            }
        } else {
            // SHARED: per-thread contexts — see the mode comment above for why
            // the guest's FBOs break here.
            const int32_t surf_attrs[] = { EGL_WIDTH, g_w, EGL_HEIGHT, g_h, EGL_NONE };
            const int32_t ctx_attrs[]  = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
            t->surf = a_eglCreatePbufferSurface(g_dpy, g_cfg, surf_attrs);
            // KL_GLFB_NOSHARE_OBJ=1 gives the thread its own context that shares
            // *nothing*. The guest then renders with objects it cannot see, so the
            // picture is meaningless — but it answers one question cleanly: whether
            // the AGX abort needs object sharing at all, or merely a second thread
            // issuing GL. Rendering correctness is not the point of this knob.
            void *share = kl_env_on("KL_GLFB_NOSHARE_OBJ", 0) ? EGL_NO_CONTEXT : g_ctx;
            t->ctx  = a_eglCreateContext(g_dpy, g_cfg, share, ctx_attrs);
        }
        pthread_setspecific(g_tls_key, t);
        fprintf(stderr, "  [glfb] thread %llu gets its own %s context\n",
                (unsigned long long)tid, t->ctx == g_ctx ? "root" : "shared");
    } else if (!kl_env_on("KL_GLFB_SHARED", 0) && !t->ctx &&
               (!g_root_owner || g_root_owner == tid)) {
        // Released earlier (by us or never taken) and now free again: retake.
        t->ctx = g_ctx; t->surf = g_surf;
        g_root_owner = tid;
    } else if (!kl_env_on("KL_GLFB_SHARED", 0) && !t->ctx && klfb_shared_migrate()) {
        // A shared context this thread released, or one another thread did.
        // If a third thread got to it first there is nothing parked, and this
        // thread — which had a context until it released one — would be left
        // without any: the guest's eglMakeCurrent answered with no GL at all.
        // It gets a fresh context in the share group instead, the same handoff
        // a thread new to GL gets above. Its container objects are gone either
        // way; its textures, buffers and programs are not.
        if (!klfb_take_parked(t, tid)) {
            const int32_t surf_attrs[] = { EGL_WIDTH, g_w, EGL_HEIGHT, g_h, EGL_NONE };
            const int32_t ctx_attrs[]  = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
            t->surf = a_eglCreatePbufferSurface(g_dpy, g_cfg, surf_attrs);
            t->ctx  = a_eglCreateContext(g_dpy, g_cfg, g_ctx, ctx_attrs);
            fprintf(stderr, "  [glfb] thread %llu released its shared context and "
                            "another thread took it over — it gets a fresh one "
                            "(%p)\n", (unsigned long long)tid, t->ctx);
        }
    }
    if (!t->ctx || !t->surf) return;
    // KL_GLFB_PROBE=1 runs the self-test's trivial clear+readback on each GUEST
    // thread as it takes a context, once. The host-thread self-test already passes
    // in this same process, so this is the discriminator that remains: if the same
    // three calls fail here, the guest's *thread environment* is at fault (its TLS
    // slot, its x18 veneers, its stack) and the elaborate GL sequence is a red
    // herring; if they pass, the environment is fine and it really is the sequence.
    // It clears the guest's framebuffer, so it is a probe and not a mode.
    if (kl_env_on("KL_GLFB_PROBE", 0) && !t->probed) {
        t->probed = 1;
        if (a_eglMakeCurrent(g_dpy, t->surf, t->surf, t->ctx)) {
            uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
            void (*cc)(float, float, float, float) = asym("glClearColor");
            void (*cl)(uint32_t) = asym("glClear");
            fprintf(stderr, "  [glfb] probe: guest thread %llu about to glClear\n",
                    (unsigned long long)tid);
            if (cc) cc(0.25f, 0.5f, 0.75f, 1.0f);
            if (cl) cl(0x4000);
            if (a_glFinish) a_glFinish();
            unsigned char px[16] = {0};
            if (a_glReadPixels) a_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            fprintf(stderr, "  [glfb] probe: guest thread %llu readback %u,%u,%u,%u"
                            " — trivial GL works on a guest thread\n",
                    (unsigned long long)tid, px[0], px[1], px[2], px[3]);
        }
    }
    if (!a_eglMakeCurrent(g_dpy, t->surf, t->surf, t->ctx)) {
        uint32_t (*eglGetError)(void) = asym("eglGetError");
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [glfb] eglMakeCurrent failed even with a per-thread "
                            "context (egl error 0x%x)\n", eglGetError ? eglGetError() : 0);
        }
    } else if (kl_env_on("KL_GLFB_DEBUG_CB", 0) && !t->debug_cb) {
        // The debug callback is per-context state, so the root-context
        // registration in kl_glfb_init does not cover this thread's context.
        t->debug_cb = 1;
        void (*glDebugMessageCallback)(const void *, const void *) =
            asym("glDebugMessageCallback");
        void (*glEnable)(uint32_t) = asym("glEnable");
        if (glDebugMessageCallback) {
            glDebugMessageCallback((const void *)klfb_debug_cb, NULL);
            if (glEnable) glEnable(0x92E0);
        }
    }
}

// The guest's eglMakeCurrent(NULL) — the release half of migration mode.
// Frees the root context for the next thread and detaches it here, so the
// next claimant's make_current can retake it. Called from klegl_MakeCurrent
// when the guest makes no context current.
// A context for a runtime call that has to make a GL object on a thread holding
// none — the guest's OVRPlugin asks ovrp_GetLayerTexture2 for an overlay's
// storage from whichever thread it is on, and Sniper Elite VR asks from one
// with no context at all, so the glGenTextures there produced no name and the
// game's video/briefing overlay was never created (a black scene under its
// narration). Only when NOTHING is current on this thread: then binding a
// context of our own is EGL-legal and cannot disturb the guest's ownership of
// its own. The context is in the guest's share group, so the texture made under
// it is visible to the guest's context; one per thread, kept, and unbound again
// by kl_glfb_return_context. Returns 1 if a context was borrowed.
static __thread void *g_borrow_ctx, *g_borrow_surf;
int kl_glfb_borrow_context(void) {
    if (!g_ready || !a_eglMakeCurrent || !a_eglCreateContext) return 0;
    static void *(*r_GetCurrentContext)(void);
    if (!r_GetCurrentContext) r_GetCurrentContext = asym("eglGetCurrentContext");
    if (!r_GetCurrentContext || r_GetCurrentContext()) return 0;   // something is current: leave it
    if (!g_borrow_ctx) {
        const int32_t surf_attrs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
        const int32_t ctx_attrs[]  = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        g_borrow_surf = a_eglCreatePbufferSurface(g_dpy, g_cfg, surf_attrs);
        g_borrow_ctx  = a_eglCreateContext(g_dpy, g_cfg, g_ctx, ctx_attrs);
        uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
        fprintf(stderr, "  [glfb] thread %llu had no context for a runtime GL call — "
                        "borrowing a shared one (%p)\n", (unsigned long long)tid, g_borrow_ctx);
    }
    if (!g_borrow_ctx || !g_borrow_surf) return 0;
    return a_eglMakeCurrent(g_dpy, g_borrow_surf, g_borrow_surf, g_borrow_ctx) ? 1 : 0;
}
void kl_glfb_return_context(void) {
    if (!a_eglMakeCurrent) return;
    static void (*r_Flush)(void);
    if (!r_Flush) r_Flush = asym("glFlush");
    if (r_Flush) r_Flush();          // the objects made here are visible to the share group
    a_eglMakeCurrent(g_dpy, NULL, NULL, NULL);
}

void kl_glfb_release_current(void) {
    if (!g_ready || !a_eglMakeCurrent) return;
    pthread_once(&g_tls_once, klfb_tls_init);
    klfb_thread_gl *t = pthread_getspecific(g_tls_key);
    if (!t) return;
    uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
    if (t->ctx == g_ctx && g_root_owner == tid) {
        a_eglMakeCurrent(g_dpy, NULL, NULL, NULL);
        g_root_owner = 0;
        t->ctx = NULL;
        t->surf = NULL;
    } else if (t->ctx && t->ctx != g_ctx && klfb_shared_migrate() &&
               !kl_env_on("KL_GLFB_SHARED", 0)) {
        // A handoff context (see g_parked): detach it here so the guest's next
        // eglMakeCurrent, on whichever thread, gets the same objects back.
        pthread_mutex_lock(&g_parked_lock);
        int room = g_nparked < KLFB_PARKED_MAX;
        if (room) {
            g_parked[g_nparked].ctx  = t->ctx;
            g_parked[g_nparked].surf = t->surf;
            g_nparked++;
        }
        pthread_mutex_unlock(&g_parked_lock);
        if (room) {
            a_eglMakeCurrent(g_dpy, NULL, NULL, NULL);
            t->ctx = NULL;
            t->surf = NULL;
        }
    }
}

// ------------------------------------------------- the shared-context compile lock
//
// A MITIGATION, not a fix. This build of ANGLE is not thread-safe across shared
// contexts; spikes/s10_shared.c reproduces it with no guest at all — three host
// threads, shared ANGLE contexts, nothing of Klepton linked:
//
//   no lock                        10/30 runs crash
//   lock around compile + link      2/60      <- ~10x better, not a cure
//   lock around the draws only      1/30
//   serialised entirely             0/40
//   one thread only                 0/40
//   concurrency without sharing     1/20      <- not concurrency as such
//
// Neither sharing nor concurrency alone: the combination. The race presents two
// ways from one code path — SIGSEGV inside libGLESv2, and a libmalloc "pointer
// being freed was not allocated" abort (heap corruption in ANGLE's own state).
// Both arrive via GL_DrawArrays and share interior frames.
//
// Compile and link carry most of the risk, ANGLE doing its heavy shared-object
// work at link time, and the improvement survives making the draws overlap
// afterwards (30 draw iterations per thread: 7/15 crashes without, 0/15 with).
// The only configurations clean at 40 runs are the ones that never use shared
// contexts concurrently, which is what the renderer must do — so the lock
// guards a real latent race without making the sharing path safe. Cheap,
// compilation not being hot.
//
// A workaround for the host driver, not a translation decision: nothing about
// the guest needs it, and a real backend does not inherit it.
static pthread_mutex_t g_compile_lock = PTHREAD_MUTEX_INITIALIZER;

static void (*g_real_CompileShader)(uint32_t);
static void (*g_real_LinkProgram)(uint32_t);
static void (*g_real_ShaderSource)(uint32_t, int32_t, const char *const *,
                                   const int32_t *);
static void (*g_real_GetShaderiv)(uint32_t, uint32_t, int32_t *);
static void (*g_real_GetShaderInfoLog)(uint32_t, int32_t, int32_t *, char *);

// The shader capture, and the version rewrite it forced.
//
// Every glShaderSource is kept, keyed by shader name; on a failed compile the
// info log and the source that produced it are printed, and written to
// KL_DUMP_SHADERS/<dir> if one is set.
//
// The gap it exposed: kl_egl describes the device as GLES 3.2 / GLSL ES 3.20 —
// a deliberate group answer, since Unity cross-checks the description against
// itself and gates B10G11R11 renderability on the version number, so describing
// the 3.0 the driver actually is kills post-processing anyway. Unity therefore
// emits "#version 320 es" variants, and the ANGLE context here is ES 3.0 (its
// Metal backend caps there), which accepts only #version 100/300 es. Every 320
// es source failed: 7016 compiles in one run over two distinct texts, retried
// forever.
//
// The rewrite repairs the text at the boundary between what is described and
// what ANGLE implements. Three mechanical moves, verified against the vendored
// ANGLE via build/s09_angle:
//
//   1. "#version 3xx es" with xx > 00 -> "#version 300 es". Nothing in the
//      failing corpus uses a real 3.1/3.2 feature; a later shader that does
//      fails here with the true error, which the failure reporter prints.
//   2. drop "binding = <tok>" from layout qualifiers. layout(binding=N) on
//      uniform blocks is an ES 3.1 feature ("only valid when used with
//      pixel local storage" on this ANGLE). Dropping it is safe because
//      Unity associates UBOs with glUniformBlockBinding/glBindBufferBase
//      explicitly — it must, the same HLSLCC output also targets devices
//      whose GLSL has no binding qualifier at all. <tok> is not always a
//      digit: Unity's own macro is `layout(binding = x, std140)`.
//   3. drop explicit uniform locations ("layout(location = N) uniform" and
//      the UNITY_LOCATION(N) spelling) — likewise ES 3.1-only on a uniform,
//      and a semantic no-op for Unity, which resolves sampler locations
//      through glGetUniformLocation either way. in/out declarations keep
//      their location qualifiers: those are program inputs/outputs, where
//      the qualifier is legal ES 3.0.
//
// Both rewrite the captured copy, so the failure reporter and the
// KL_DUMP_SHADERS output show what ANGLE actually compiled.
#define KLFB_MAX_SHADERS 1024
static struct { uint32_t name; char *src; } g_fb_shaders[KLFB_MAX_SHADERS];
static unsigned g_fb_nshaders;

// A GLSL identifier character — the test that keeps a rename inside whole words.
#define KLFB_IDENT(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || \
                       ((c) >= '0' && (c) <= '9') || (c) == '_')

// ---- explicit uniform locations: remembered, not merely thrown away --------
//
// The strip below is required — layout(location=N) on a *uniform* is ES 3.1
// syntax and the ANGLE context behind this renderer is ES 3.0 — and for Unity
// it is a semantic no-op, because Unity resolves every uniform by name through
// glGetUniformLocation. Steam Link does not. Measured: it does not call
// glGetUniformLocation ONCE in a whole run, on either driver. It writes to the
// numbers it pinned in the shader text.
//
// So stripping the pins silently re-points every uniform the guest sets. Its
// pointer shader pins `length` at 1 and the linker's own order puts a mat4
// there, which at least raises GL_INVALID_OPERATION (~12 a frame, glutils.cpp:
// 205, and that error is what led here). The video shader is the one that
// matters and it fails without a sound: it pins
// `uniform samplerExternalOES tex0` at 2, a sampler written with glUniform1i
// takes any integer without complaint, and a sampler that never receives its
// texture unit reads unit 0 — so the quad that should show the decoded stream
// samples something else entirely, and every eye texture reads back black.
//
// The pin is therefore recorded here, resolved to the linker's own location at
// glLinkProgram, and applied to every glUniform* call. The guest keeps its own
// numbering throughout and never learns ours.
#define KLFB_PIN_SHADERS 64
#define KLFB_PIN_SHADERS_BOUND 1024
#define KLFB_PINS        32
static struct { uint32_t shader; unsigned n;
                struct { int32_t at; char name[40]; } p[KLFB_PINS]; }
    g_shader_pins[KLFB_PIN_SHADERS_BOUND];
static unsigned g_shader_pins_n;

// KL_GLFB_BIND_PINS: give the linker the pins instead of translating them.
//
// A Unity built for ES 3.1 writes `UNITY_LOCATION(n) uniform sampler2D _Tex`,
// asks the driver where the sampler went, and uses THAT NUMBER AS THE TEXTURE
// UNIT (glUniform1i(loc, loc)). On a Quest the answer is the n in the text, a
// unit below the engine's cap of 32. Here the qualifier is stripped (ES 3.0),
// the linker numbers the uniforms in its own order, and a sampler declared
// after thirty-odd other uniforms is "at" 35: the engine takes 35 for the unit
// and refuses it — "OpenGL Error: Invalid texture unit!" — so the sampler reads
// whatever is on unit 0. Premium Bowling (Unity 6000.3.22f1): 105k of those in
// 26 seconds and every glyph of its menus a filled box (TextMeshPro's shader
// has the uniforms to get there).
//
// ANGLE can be told where a uniform goes before the link
// (GL_CHROMIUM_bind_uniform_location), which is the ES 3.0 spelling of the
// qualifier that was taken out. With that the linker's numbering IS the
// guest's: the by-name answer is the pinned number, the remap below is the
// identity whichever way the guest addresses a uniform, and the uniforms that
// pin nothing are placed around the pins by the linker itself.
//
// Tied to the title it was measured on. Every Unity title that works was
// verified with its samplers on the linker's units, and the two fixed-size
// tables here meet a guest's whole shader set for the first time this way.
// Real VR Fishing (x2582932495064035) is the second, on its sender's word: the
// fix that turned the pins on for it names texture faults among its reasons
// and was run on a headset that way (fix 37, 2026-10-14; not run here).
// Synth Riders (x2436558143118760, Unity 6000.3.13f1) the third: samplers at
// 169..177, 190k "Invalid texture unit!" a run, its hand-calibration orbs
// missing and stray shapes in the menu (headset, 2026-10-15).
// LES MILLS BODYCOMBAT (x4015163475201433) the fourth: the same 466k
// "Invalid texture unit!" in one session and every TextMeshPro glyph a block.
// Gun Club VR (x2285803881494677) the fifth: black gun materials and repeated
// "Invalid texture unit!", surface detail on the gun with the pins on (fix 44,
// 2026-10-15, its sender's screenshots; not run here).
// Surviving Mars: Pioneer (x9863010250399155, Unity 2022.3.62f1) the sixth:
// samplers at 99..104, 42k "Invalid texture unit!" a session and the button
// text unreadable.
// VRChat from Steam (x438100, Unity 6000.0.67f1) the seventh: 169k
// "Invalid texture unit!" in a 40-second session and every menu letter a
// wrong shape (headset, 2026-10-17).
// Garden of the Sea (x3684804704932159, Unity 2019.4.13f1) the eighth: once
// its storage-buffer shaders linked (KL_GLFB_SSBO_TEX), 227k "Invalid texture
// unit!" in a session and the soil it is played on drawn black.
// ...and every Unity title under the profile for untested games (kl_driver.c):
// seven of the eight Unity fixes for this error were these pins, and a guest
// without the error has its samplers where its own shader text puts them, which
// is where a Quest puts them too. Two guards come with it (klfb_pins_bind): a
// pin whose number another uniform of the same program already holds is left
// out rather than handed to the linker twice, and a full pin table is said.
static int klfb_bind_pins(void) {
    static int v = -1;
    if (v < 0) {
        extern const char *kl_driver_target_name(void);
        extern kl_guest_kind kl_driver_kind(void);
        extern int kl_untested_profile(void);
        const char *t = kl_driver_target_name();
        v = kl_env_on("KL_GLFB_BIND_PINS", (t && (strcmp(t, "x2773034772778845") == 0 ||
                                                  strcmp(t, "x2582932495064035") == 0 ||
                                                  strcmp(t, "x2436558143118760") == 0 ||
                                                  strcmp(t, "x4015163475201433") == 0 ||
                                                  strcmp(t, "x2285803881494677") == 0 ||
                                                  strcmp(t, "x9863010250399155") == 0 ||
                                                  strcmp(t, "x438100") == 0 ||
                                                  strcmp(t, "x3684804704932159") == 0)) ||
                                           (kl_untested_profile() && kl_driver_kind() == KL_GUEST_UNITY));
    }
    return v;
}

// A slot whose shader was deleted (shader 0, see klfb_pin_forget) is taken
// again before the table grows: a guest that loads shaders as it goes (VRChat,
// a world and every avatar in it) would otherwise fill the table with names it
// has long deleted, and every shader after that would lose its pins.
static int klfb_pin_slot(uint32_t shader) {
    int free_slot = -1;
    for (unsigned i = 0; i < g_shader_pins_n; i++) {
        if (g_shader_pins[i].shader == shader) return (int)i;
        if (free_slot < 0 && g_shader_pins[i].shader == 0) free_slot = (int)i;
    }
    if (free_slot >= 0) {
        g_shader_pins[free_slot].shader = shader;
        g_shader_pins[free_slot].n = 0;
        return free_slot;
    }
    if (g_shader_pins_n >= (klfb_bind_pins() ? KLFB_PIN_SHADERS_BOUND : KLFB_PIN_SHADERS)) {
        // Said once: from here on a shader's pins are stripped and not kept, so
        // under KL_GLFB_BIND_PINS its samplers go back to the linker's units.
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] the uniform-pin table is full (%u shaders) — later shaders "
                            "keep no pins; a sampler among them may read the wrong texture\n",
                    g_shader_pins_n);
        return -1;
    }
    int i = (int)g_shader_pins_n++;
    g_shader_pins[i].shader = shader;
    g_shader_pins[i].n = 0;
    return i;
}

// A shader name can be re-sourced, and the rewrite runs once per
// glShaderSource — so the second source must replace the first's pins, not
// append to them.
static void klfb_pin_reset(uint32_t shader) {
    for (unsigned i = 0; i < g_shader_pins_n; i++)
        if (g_shader_pins[i].shader == shader) g_shader_pins[i].n = 0;
}

// glDeleteShader: the name may come back for another shader, and the slot is
// free for the next one. A program linked from it keeps its locations.
static void klfb_pin_forget(uint32_t shader) {
    if (!shader) return;
    for (unsigned i = 0; i < g_shader_pins_n; i++)
        if (g_shader_pins[i].shader == shader) {
            g_shader_pins[i].shader = 0;
            g_shader_pins[i].n = 0;
        }
}

static void klfb_pin_note(uint32_t shader, int32_t at, const char *name) {
    int i = klfb_pin_slot(shader);
    if (i < 0 || g_shader_pins[i].n >= KLFB_PINS) return;
    unsigned k = g_shader_pins[i].n++;
    g_shader_pins[i].p[k].at = at;
    snprintf(g_shader_pins[i].p[k].name, sizeof g_shader_pins[i].p[k].name,
             "%s", name);
}

// If the layout qualifier starting at `p` ("layout(...)") directly precedes a
// `uniform` declaration, record the pin, then remove the qualifier (and the
// spaces between) in place and return the new scan position; otherwise return
// NULL. in/out declarations keep their qualifiers — those ARE program
// inputs/outputs and ES 3.0 wants them, so they never reach the strip.
static char *klfb_strip_uniform_layout(char *p, uint32_t shader) {
    char *close = strchr(p, ')');
    if (!close) return NULL;
    char *q = close + 1;
    while (*q == ' ' || *q == '\t') q++;
    if (strncmp(q, "uniform", 7) != 0) return NULL;

    // The number, before it goes: "layout(location = N)" and Unity's
    // "UNITY_LOCATION(N)" both put it between the parens.
    int32_t at = -1;
    {
        const char *loc = NULL;
        for (const char *d = p; d + 8 <= close; d++)
            if (strncmp(d, "location", 8) == 0) { loc = d + 8; break; }
        if (!loc && strncmp(p, "UNITY_LOCATION(", 15) == 0) loc = p + 14;
        while (loc && loc < close && (*loc == ' ' || *loc == '=' || *loc == '('))
            loc++;
        if (loc && loc < close && *loc >= '0' && *loc <= '9')
            at = (int32_t)strtol(loc, NULL, 10);
    }
    // ...and the name it pins: the identifier that ends the declaration, before
    // any array subscript. Read backwards from the ';' rather than counting
    // type tokens forwards, because the type may carry a precision qualifier
    // ("uniform mediump sampler2D _MainTex;").
    const char *semi = at >= 0 && shader ? strchr(q, ';') : NULL;
    if (semi) {
        const char *e = semi;
        while (e > q && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if (e > q && e[-1] == ']') {                  // an array: back over "[n]"
            const char *b = e;
            while (b > q && b[-1] != '[') b--;
            if (b > q) {
                e = b - 1;
                while (e > q && (e[-1] == ' ' || e[-1] == '\t')) e--;
            }
        }
        const char *s = e;
        while (s > q && KLFB_IDENT(s[-1])) s--;
        if (e > s && (size_t)(e - s) < 40) {
            char name[40];
            memcpy(name, s, (size_t)(e - s));
            name[e - s] = 0;
            klfb_pin_note(shader, at, name);
        }
    }
    memmove(p, q, strlen(q) + 1);
    return p;
}

// GLSL built-in function names that this guest also declares as variables, with
// the replacement each gets. The replacements are the same width as the names
// they replace, so the rename stays an in-place edit like every other rule in
// klfb_rewrite_glsl. Shared with klfb_GetUniformLocation, which is what keeps a
// renamed uniform findable under the name the guest wrote.
static const struct { const char *from, *to; } g_glsl_builtin_vars[] = {
    {"length", "kl_len"},
};

// ...and the other end of the pin: what the linker actually chose.
//
// Built at glLinkProgram, because that is the first moment a name has a
// location at all, and read by every glUniform* thunk. A program the guest
// pinned nothing in gets no entry, and g_prog_pins_n == 0 is the fast out for
// a guest that pins nothing anywhere.
//
// `byname` is the half this shipped without, and it cost Beat Saber's menu UI.
// The remap is only ever correct for a guest that WRITES TO THE NUMBER IT
// PINNED; a guest that asks the driver where a uniform went is being told our
// linker's answer, and putting that answer back through the pin table is the
// same bug in the mirror. Unity does both — it emits UNITY_LOCATION(n) pins AND
// resolves every uniform by name — so on Beat Saber the remap re-pointed the
// locations the driver itself had just handed out. The reason it was not caught:
// the by-name door was assumed to be glGetUniformLocation, and UNITY 2019.4 DOES
// NOT USE IT. It goes through the ES 3.1 program-interface family
// (glGetProgramResourceLocation), which is emulated further down this file and
// resolves to exactly the same driver call. Both doors set the flag now.
#define KLFB_PIN_PROGS 64
static struct { uint32_t prog; unsigned n; int byname;
                struct { int32_t pinned, actual; } m[KLFB_PINS * 2]; }
    g_prog_pins[KLFB_PIN_PROGS];
static unsigned g_prog_pins_n;

// Counted so the choice is visible in the end-of-run report rather than
// inferred from a picture: a pin table that never fires and one that re-points
// half the guest's uniforms look identical from outside.
static unsigned g_pin_byname_progs, g_pin_remap_hits, g_pin_remap_changed;

// The guest asked the driver where `program`'s uniforms are. Its pins are a
// statement about the shader text, not about what it is going to write to.
static void klfb_pins_byname(uint32_t program) {
    for (unsigned i = 0; i < g_prog_pins_n; i++)
        if (g_prog_pins[i].prog == program) {
            if (!g_prog_pins[i].byname) {
                g_prog_pins[i].byname = 1;
                g_pin_byname_progs++;
            }
            return;
        }
}

static int32_t (*g_real_GetUniformLocation)(uint32_t, const char *);

// The location ANGLE gave `name`, allowing for the built-in rename above — a
// uniform called `length` is in the program as `kl_len`, and the pin still
// names it the way the shader author wrote it.
static int32_t klfb_actual_loc(uint32_t prog, const char *name) {
    if (!g_real_GetUniformLocation) return -1;
    int32_t loc = g_real_GetUniformLocation(prog, name);
    if (loc >= 0) return loc;
    for (size_t i = 0; i < sizeof g_glsl_builtin_vars / sizeof g_glsl_builtin_vars[0]; i++)
        if (strcmp(name, g_glsl_builtin_vars[i].from) == 0)
            return g_real_GetUniformLocation(prog, g_glsl_builtin_vars[i].to);
    return -1;
}

// Before the link: hand each pin of the attached shaders to the linker
// (KL_GLFB_BIND_PINS). A name the program does not use binds nothing.
static void klfb_pins_bind(uint32_t program) {
    if (!g_shader_pins_n || !klfb_bind_pins()) return;
    static void (*r_GetAttachedShaders)(uint32_t, int32_t, int32_t *, uint32_t *);
    static void (*r_Bind)(uint32_t, int32_t, const char *);
    static int said;
    if (!r_GetAttachedShaders) r_GetAttachedShaders = asym("glGetAttachedShaders");
    if (!r_Bind) r_Bind = asym("glBindUniformLocationCHROMIUM");
    if (!r_GetAttachedShaders || !r_Bind) {
        if (!said) { said = 1;
            fprintf(stderr, "  [glfb] KL_GLFB_BIND_PINS: this GL has no "
                            "glBindUniformLocationCHROMIUM — pins are translated as before\n"); }
        return;
    }
    uint32_t sh[8]; int32_t ns = 0;
    r_GetAttachedShaders(program, 8, &ns, sh);
    unsigned bound = 0, clashed = 0;
    // One number, one uniform: the vertex and the fragment shader may pin the
    // same name (it is one uniform, bound once) but never two names at one
    // number — handed to the linker, that is a failed link and a program that
    // draws nothing. The second such pin is left to the linker's own numbering.
    struct { int32_t at; const char *name; } seen[KLFB_PINS * 8];
    unsigned nseen = 0;
    for (int32_t i = 0; i < ns && i < 8; i++)
        for (unsigned s = 0; s < g_shader_pins_n; s++) {
            if (g_shader_pins[s].shader != sh[i]) continue;
            for (unsigned k = 0; k < g_shader_pins[s].n; k++) {
                const char *name = g_shader_pins[s].p[k].name;
                for (size_t b = 0; b < sizeof g_glsl_builtin_vars / sizeof g_glsl_builtin_vars[0]; b++)
                    if (strcmp(name, g_glsl_builtin_vars[b].from) == 0)
                        name = g_glsl_builtin_vars[b].to;
                int32_t at = g_shader_pins[s].p[k].at;
                int dup = 0, clash = 0;
                for (unsigned q = 0; q < nseen; q++) {
                    int same_name = strcmp(seen[q].name, name) == 0;
                    if (same_name && seen[q].at == at) dup = 1;
                    else if (same_name || seen[q].at == at) clash = 1;
                }
                if (dup) continue;
                if (clash) { clashed++; continue; }
                if (nseen < sizeof seen / sizeof seen[0]) seen[nseen++] = (typeof(seen[0])){at, name};
                r_Bind(program, at, name);
                bound++;
            }
        }
    if (clashed) {
        static int said_clash;
        if (said_clash++ < 8)
            fprintf(stderr, "  [glfb] program %u: %u uniform pin%s left to the linker — the same "
                            "number for two uniforms, or two numbers for one\n",
                    program, clashed, clashed == 1 ? "" : "s");
    }
    if (bound && !said) { said = 1;
        fprintf(stderr, "  [glfb] uniform pins are bound at link "
                        "(KL_GLFB_BIND_PINS): program %u, %u of them\n", program, bound); }
}

static void klfb_pins_link(uint32_t program) {
    if (!g_shader_pins_n) return;
    static void (*r_GetAttachedShaders)(uint32_t, int32_t, int32_t *, uint32_t *);
    if (!r_GetAttachedShaders) r_GetAttachedShaders = asym("glGetAttachedShaders");
    // The guest may never resolve glGetUniformLocation — this one does not —
    // so the thunk's real slot can still be empty here. Resolve it ourselves.
    if (!g_real_GetUniformLocation)
        g_real_GetUniformLocation = (void *)asym("glGetUniformLocation");
    if (!r_GetAttachedShaders || !g_real_GetUniformLocation) return;

    uint32_t sh[8]; int32_t ns = 0;
    r_GetAttachedShaders(program, 8, &ns, sh);
    if (ns <= 0) return;

    unsigned slot = 0;
    for (; slot < g_prog_pins_n; slot++)
        if (g_prog_pins[slot].prog == program) break;
    if (slot == g_prog_pins_n) {
        if (g_prog_pins_n >= KLFB_PIN_PROGS) return;
        g_prog_pins_n++;
    }
    g_prog_pins[slot].prog = program;
    g_prog_pins[slot].n = 0;

    unsigned lost = 0;
    for (int32_t i = 0; i < ns && i < 8; i++)
        for (unsigned s = 0; s < g_shader_pins_n; s++) {
            if (g_shader_pins[s].shader != sh[i]) continue;
            for (unsigned k = 0; k < g_shader_pins[s].n; k++) {
                const char *name = g_shader_pins[s].p[k].name;
                int32_t at = g_shader_pins[s].p[k].at;
                int32_t actual = klfb_actual_loc(program, name);
                if (actual < 0) { lost++; continue; }   // optimised away
                if (g_prog_pins[slot].n < KLFB_PINS * 2) {
                    g_prog_pins[slot].m[g_prog_pins[slot].n].pinned = at;
                    g_prog_pins[slot].m[g_prog_pins[slot].n].actual = actual;
                    g_prog_pins[slot].n++;
                }
                // An array pinned at N occupies N, N+1, ... and the guest may
                // write any of them; the linker's own elements are found the
                // same way the spec says the guest would.
                for (int e = 1; e < 16; e++) {
                    char el[48];
                    snprintf(el, sizeof el, "%s[%d]", name, e);
                    int32_t a = klfb_actual_loc(program, el);
                    if (a < 0) break;
                    if (g_prog_pins[slot].n >= KLFB_PINS * 2) break;
                    g_prog_pins[slot].m[g_prog_pins[slot].n].pinned = at + e;
                    g_prog_pins[slot].m[g_prog_pins[slot].n].actual = a;
                    g_prog_pins[slot].n++;
                }
            }
        }
    if (g_prog_pins[slot].n || lost)
        fprintf(stderr, "  [glfb] program %u pins %u uniform location%s "
                        "explicitly%s — honoured\n", program,
                g_prog_pins[slot].n, g_prog_pins[slot].n == 1 ? "" : "s",
                lost ? " (plus some the linker optimised away)" : "");
}

// Which program is current, per thread — glUniform* writes into it, and the
// remap is program-scoped. Maintained by klfb_UseProgram rather than queried,
// because a glGetIntegerv under every uniform call is not free and this is on
// the guest's per-draw path.
static __thread uint32_t g_cur_prog;

// The guest's location -> the linker's. Unpinned locations pass through: a
// guest that pins some uniforms and looks up others is asking about ours in
// the second case, and rewriting those would be the same bug in the mirror.
static int32_t klfb_remap_loc(int32_t loc) {
    if (!g_prog_pins_n || loc < 0 || !g_cur_prog) return loc;
    for (unsigned i = 0; i < g_prog_pins_n; i++) {
        if (g_prog_pins[i].prog != g_cur_prog) continue;
        if (g_prog_pins[i].byname) return loc;   // it is quoting our own answer
        for (unsigned j = 0; j < g_prog_pins[i].n; j++)
            if (g_prog_pins[i].m[j].pinned == loc) {
                g_pin_remap_hits++;
                if (g_prog_pins[i].m[j].actual != loc) g_pin_remap_changed++;
                return g_prog_pins[i].m[j].actual;
            }
        return loc;
    }
    return loc;
}

// Returns buf rewritten in place (it only ever shrinks), or NULL if no rule
// applied.
// ---- texture buffers (samplerBuffer), emulated on ES 3.0 as 2D textures ------
//
// UE4's GLES RHI uses GL_TEXTURE_BUFFER for GPU skinning: a Buffer<float4> of
// bone matrices, declared `samplerBuffer` and read with the one-index form
// texelFetch(s, i). Texture buffers are ES 3.1 / EXT_texture_buffer, and the
// ANGLE-on-Metal context here is a hard ES 3.0 (DisplayMtl caps at 3.0, no
// texture-buffer extension), so the shader will not even compile — "'samplerBuffer'
// : Illegal use of reserved word" — and UE4 fatals the render thread the moment
// RHICreateBoundShaderState fails. That is the whole reason TWD2 dies after its
// paks mount.
//
// The emulation lays the buffer out as a 2D texture KLFB_TB_W texels wide (a
// power of two, so the index splits by mask/shift), and rewrites the shader to
// match: samplerBuffer -> sampler2D, and texelFetch(s, i) -> texelFetch(s,
// ivec2(i & (W-1), i >> log2W), 0). The host half (klfb_TexBuffer) uploads the
// buffer's bytes into that 2D texture with the same width. This pass is the
// shader half; both halves must agree on KLFB_TB_W.
#define KLFB_TB_W      2048
#define KLFB_TB_MASK  "2047"
#define KLFB_TB_SHIFT "11"

// Replace [at, at+oldlen) with `ins`, shifting the tail. Returns the position
// just past the inserted text, or NULL if it would overflow cap.
static char *klfb_splice(char *buf, size_t cap, char *at, size_t oldlen, const char *ins) {
    size_t inslen = strlen(ins);
    size_t used   = strlen(buf) + 1;                 // include NUL
    size_t tail   = (size_t)(at + oldlen - buf);
    if (used - oldlen + inslen > cap) return NULL;
    memmove(at + inslen, at + oldlen, strlen(at + oldlen) + 1);
    memcpy(at, ins, inslen);
    (void)tail;
    return at + inslen;
}

static int klfb_ident_run(const char *s) {
    int n = 0; while (KLFB_IDENT(s[n])) n++; return n;
}

static int klfb_rewrite_texel_buffers(char *buf, size_t cap) {
    // Collect the names declared with a *Buffer sampler type, longest keyword
    // first so "samplerBuffer" does not match inside "isamplerBuffer".
    char names[8][64]; int nn = 0;
    static const char *kw[] = { "isamplerBuffer", "usamplerBuffer", "samplerBuffer" };
    for (unsigned k = 0; k < sizeof kw / sizeof kw[0]; k++) {
        size_t L = strlen(kw[k]);
        for (char *p = buf; (p = strstr(p, kw[k])); ) {
            char before = p == buf ? 0 : p[-1];
            if (KLFB_IDENT(before) || KLFB_IDENT(p[L])) { p += L; continue; }
            char *q = p + L;
            while (*q == ' ' || *q == '\t') q++;
            int idn = klfb_ident_run(q);
            if (idn > 0 && nn < 8 && idn < 64) {
                memcpy(names[nn], q, (size_t)idn); names[nn][idn] = 0; nn++;
            }
            p += L;
        }
    }
    if (!nn) return 0;

    // samplerBuffer -> sampler2D (shrink, in place), all three widths.
    static const struct { const char *from, *to; } tymap[] = {
        { "isamplerBuffer", "isampler2D" }, { "usamplerBuffer", "usampler2D" },
        { "samplerBuffer",  "sampler2D"  },
    };
    for (unsigned k = 0; k < sizeof tymap / sizeof tymap[0]; k++) {
        size_t fl = strlen(tymap[k].from), tl = strlen(tymap[k].to);
        for (char *p = buf; (p = strstr(p, tymap[k].from)); ) {
            char before = p == buf ? 0 : p[-1];
            if (KLFB_IDENT(before) || KLFB_IDENT(p[fl])) { p += fl; continue; }
            memcpy(p, tymap[k].to, tl);
            memmove(p + tl, p + fl, strlen(p + fl) + 1);   // close the gap
            p += tl;
        }
    }

    // texelFetch(NAME, EXPR) -> texelFetch(NAME, ivec2((EXPR)&W-1,(EXPR)>>log2W), 0)
    // for each buffer sampler NAME. The one-index form is the buffer form; a real
    // sampler2D texelFetch already passes an ivec2 and a lod, so it is left alone
    // because its NAME is not in the list.
    for (char *p = buf; (p = strstr(p, "texelFetch(")); ) {
        char *a0 = p + strlen("texelFetch(");
        while (*a0 == ' ' || *a0 == '\t') a0++;
        int idn = klfb_ident_run(a0);
        int match = 0;
        for (int i = 0; i < nn; i++)
            if ((int)strlen(names[i]) == idn && strncmp(names[i], a0, (size_t)idn) == 0) { match = 1; break; }
        if (!match) { p += strlen("texelFetch("); continue; }
        // find the comma separating sampler and index (depth 1 inside the call)
        char *c = a0 + idn;
        while (*c == ' ' || *c == '\t') c++;
        if (*c != ',') { p += strlen("texelFetch("); continue; }
        char *estart = c + 1;
        // scan the index expr to the matching close paren (depth back to 0)
        int depth = 1; char *e = estart;
        for (; *e; e++) {
            if (*e == '(') depth++;
            else if (*e == ')') { if (--depth == 0) break; }
        }
        if (*e != ')') break;                         // malformed; stop
        size_t exprlen = (size_t)(e - estart);
        char expr[512];
        if (exprlen >= sizeof expr) { p = e + 1; continue; }
        memcpy(expr, estart, exprlen); expr[exprlen] = 0;
        char ins[1200];
        snprintf(ins, sizeof ins, "ivec2((%s)&" KLFB_TB_MASK ",(%s)>>" KLFB_TB_SHIFT "),0",
                 expr, expr);
        char *after = klfb_splice(buf, cap, estart, exprlen, ins);
        if (!after) break;                            // out of room; leave the rest
        p = after;
    }

    fprintf(stderr, "  [glfb] texture-buffer shader: %d sampler(s) emulated as 2D "
                    "(width %d)\n", nn, KLFB_TB_W);
    return 1;
}

// ---- read-only storage buffers, emulated on ES 3.0 as 2D textures -----------
//
// Unity's HLSLcc writes a StructuredBuffer<T> a shader only reads as
//
//   struct NAME_type { uint[K] value; };
//   layout(std430, binding = N) readonly buffer NAME { NAME_type NAME_buf[]; };
//   ... uintBitsToFloat(NAME_buf[I].value[J]) ...
//
// Storage buffers are ES 3.1, and the ANGLE-on-Metal context here is a hard ES
// 3.0, so such a shader does not compile ("'std430' : invalid layout qualifier"),
// its program never links and every draw with it is skipped. Garden of the Sea
// (x3684804704932159) lights its whole world that way (EnvironmentLightDataBuffer,
// SoilDataBuffer, RenderDataBuffer): past its logo it compiled some 1,600 of
// them, all failing, and never drew a frame of the game.
//
// The same trick as the texel buffers above: the buffer becomes an R32UI 2D
// texture KLFB_TB_W words wide, word w at (w & W-1, w >> log2W), and the shader
// reads word I*K + J of it. The block is replaced by `uniform highp usampler2D
// kl_sbN`, named after its binding point, so the host half (klfb_ssbo_apply)
// can tell which buffer the guest bound for it. Only that exact shape is
// rewritten — a buffer the shader writes, a member that is not a uint array,
// or any other use of NAME_buf leaves the shader as it was.
#define KLFB_SB_MAXB 8
// Skip whitespace, then match `lit`. Returns the position after it, or NULL.
static char *klfb_sb_lit(char *q, const char *lit) {
    while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
    size_t n = strlen(lit);
    return strncmp(q, lit, n) == 0 ? q + n : NULL;
}
// `open` points at a '['; returns its matching ']' or NULL.
static char *klfb_sb_close(char *open) {
    int depth = 0;
    for (char *e = open; *e; e++) {
        if (*e == '[') depth++;
        else if (*e == ']' && --depth == 0) return e;
    }
    return NULL;
}
// One block declaration at `p` ("layout(std430, binding = "): its binding,
// name, words per element and where the declaration ends. 0 if not our shape.
static int klfb_sb_parse(char *buf, char *p, int *binding, char name[64], int *words, char **end) {
    int n = 0;
    *binding = -1;
    if (sscanf(p, "layout(std430, binding = %d) readonly buffer %n", binding, &n) != 1 || !n) return 0;
    if (*binding < 0 || *binding >= KLFB_SB_MAXB) return 0;
    char *q = p + n;
    int idn = klfb_ident_run(q);
    if (idn <= 0 || idn > 40) return 0;
    memcpy(name, q, (size_t)idn); name[idn] = 0;
    q += idn;
    char want[160];
    if (!(q = klfb_sb_lit(q, "{"))) return 0;
    snprintf(want, sizeof want, "%s_type", name);
    if (!(q = klfb_sb_lit(q, want))) return 0;
    snprintf(want, sizeof want, "%s_buf[]", name);
    if (!(q = klfb_sb_lit(q, want))) return 0;
    if (!(q = klfb_sb_lit(q, ";")) || !(q = klfb_sb_lit(q, "}")) || !(q = klfb_sb_lit(q, ";"))) return 0;
    *end = q;
    // The element type: one uint array named value, and nothing else.
    snprintf(want, sizeof want, "struct %s_type {", name);
    char *s = strstr(buf, want);
    if (!s || !(s = klfb_sb_lit(s + strlen(want), "uint["))) return 0;
    int k = 0, m = 0;
    if (sscanf(s, "%d] value;%n", &k, &m) != 1 || !m || k <= 0 || k > 4096) return 0;
    if (!klfb_sb_lit(s + m, "};")) return 0;
    *words = k;
    return 1;
}
// Every use of NAME_buf other than its declaration must be NAME_buf[I].value[J].
// With `rewrite` 0 only counts them (-1: some other use); with 1 also replaces them.
static int klfb_sb_uses(char *buf, size_t cap, const char *name, int binding, int words, int rewrite) {
    char head[80];
    snprintf(head, sizeof head, "%s_buf", name);
    size_t hl = strlen(head);
    int n = 0;
    for (char *p = buf; (p = strstr(p, head)); ) {
        if ((p > buf && KLFB_IDENT(p[-1])) || KLFB_IDENT(p[hl])) { p += hl; continue; }
        if (strncmp(p + hl, "[]", 2) == 0) { p += hl + 2; continue; }      // the declaration
        if (p[hl] != '[') return -1;
        char *c1 = klfb_sb_close(p + hl);
        if (!c1 || strncmp(c1 + 1, ".value[", 7) != 0) return -1;
        char *c2 = klfb_sb_close(c1 + 7);
        if (!c2) return -1;
        n++;
        if (!rewrite) { p = c2 + 1; continue; }
        size_t l1 = (size_t)(c1 - (p + hl + 1)), l2 = (size_t)(c2 - (c1 + 8));
        size_t need = l1 + l2 + 96;
        char *ins = malloc(need);
        if (!ins) return -1;
        snprintf(ins, need, "kl_sbf(kl_sb%d, int(%.*s) * %d + int(%.*s))",
                 binding, (int)l1, p + hl + 1, words, (int)l2, c1 + 8);
        char *after = klfb_splice(buf, cap, p, (size_t)(c2 + 1 - p), ins);
        free(ins);
        if (!after) return -1;
        p = after;
    }
    return n;
}
// Returns the number of blocks rewritten; 0 leaves `buf` exactly as it was
// (every check is made before the first change).
static int klfb_rewrite_ro_ssbo(char *buf, size_t cap) {
    if (!strstr(buf, "layout(std430")) return 0;
    struct { char name[64]; int binding, words; } b[KLFB_SB_MAXB];
    int nb = 0;
    size_t grow = 256;
    for (char *p = buf; (p = strstr(p, "layout(std430")); ) {
        char *end;
        if (nb == KLFB_SB_MAXB || !klfb_sb_parse(buf, p, &b[nb].binding, b[nb].name, &b[nb].words, &end))
            return 0;
        for (int i = 0; i < nb; i++) if (b[i].binding == b[nb].binding) return 0;
        int uses = klfb_sb_uses(buf, cap, b[nb].name, b[nb].binding, b[nb].words, 0);
        if (uses < 0) return 0;
        grow += (size_t)uses * 48 + 64;
        nb++;
        p = end;
    }
    if (!nb || strlen(buf) + grow >= cap) return 0;
    for (int i = 0; i < nb; i++) {
        char *p = strstr(buf, "layout(std430"), *end;
        int binding, words; char name[64];
        if (!p || !klfb_sb_parse(buf, p, &binding, name, &words, &end)) return 0;   // cannot happen
        char decl[256];
        snprintf(decl, sizeof decl, "uniform highp usampler2D kl_sb%d;\n%s", binding,
                 i ? "" : "uint kl_sbf(highp usampler2D s, int i) { return texelFetch(s, "
                          "ivec2(i & " KLFB_TB_MASK ", i >> " KLFB_TB_SHIFT "), 0).x; }\n");
        if (!klfb_splice(buf, cap, p, (size_t)(end - p), decl)) return 0;
        if (klfb_sb_uses(buf, cap, name, binding, words, 1) < 0) return 0;
    }
    return nb;
}
// Warhammer 40,000: Battle Sister's renderer (PixelToys' BLAST) lights its
// levels through reflection probes held in a samplerCubeArray, with each
// probe's HDR decode in a read-only storage buffer — ES 3.2 and 3.1, both
// refused by this ES 3.0 backend ("'BLAST_ReflectionProbes' : syntax error"),
// so every surface of a level failed to compile and drew black while only its
// particles showed (2026-10-28). The array is declared a plain samplerCube so
// the shader compiles, and every sample of it becomes kl_probe_env (below), so
// the sampler is unused and holds no texture unit that a 2D array of the same
// shader could be refused for sharing. The block becomes a constant of the same
// element type giving an HDR decode that leaves the colour as it is. Lightmaps,
// albedo, normals and the main light are untouched. Only these exact names, and only
// where KL_GLFB_PROBE_CUBE_ARRAY is on (default for x3231001163584154).
static int klfb_probe_cube_array_on(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_GLFB_PROBE_CUBE_ARRAY", t && strcmp(t, "x3231001163584154") == 0);
    }
    return on;
}
static int klfb_rewrite_probe_cube_array(char *buf, size_t cap) {
    static const char decl[] = "samplerCubeArray BLAST_ReflectionProbes;";
    // The layout list still carries its `binding = N` here (klfb_rewrite_glsl
    // drops it later), so the block is found by what follows the list.
    static const char blk[]  = "readonly buffer BLAST_ReflectionProbeHDRData {";
    // A constant, not a uniform: Unity refuses a struct uniform outside a block
    // ("struct parameter ... resides outside of a uniform block").
    static const char uni[]  = "const BLAST_ReflectionProbeHDRData_type BLAST_ReflectionProbeHDRData_buf[1] = "
                               "BLAST_ReflectionProbeHDRData_type[1](BLAST_ReflectionProbeHDRData_type("
                               "uint[4](0x3f800000u, 0x3f800000u, 0u, 0u)));";
    // The probe read as the scene's ambient light (its spherical harmonics) in
    // the reflected direction, not as black: a metal has no diffuse colour, so
    // with no reflection at all every metal surface came out black (2026-10-28).
    // With the decode words above (multiplier 1, exponent 1, w 0) the shader
    // takes this colour as it is.
    static const char env_sh[] =
        "vec4 kl_probe_env(vec3 d) { vec4 n = vec4(normalize(d), 1.0);\n"
        "    return vec4(max(vec3(dot(unity_SHAr, n), dot(unity_SHAg, n), dot(unity_SHAb, n)), vec3(0.0)), 1.0); }\n";
    static const char env_flat[] = "vec4 kl_probe_env(vec3 d) { return vec4(0.25, 0.25, 0.25, 1.0); }\n";
    int n = 0, sampled = 0;
    char *d = strstr(buf, decl);
    if (d) {
        memcpy(d, "samplerCube     ", 16);           // same length as samplerCubeArray
        n++;
        static const char *const calls[] = { "textureLod(BLAST_ReflectionProbes, ",
                                             "texture(BLAST_ReflectionProbes, " };
        for (int c = 0; c < 2; c++) {
            for (char *p = buf; (p = strstr(p, calls[c])); ) {
                char *a = p + strlen(calls[c]), *e = a;
                int depth = 0;
                for (; *e; e++) {
                    if (*e == '(' || *e == '[') depth++;
                    else if ((*e == ')' || *e == ']') && depth) depth--;
                    else if ((*e == ',' || *e == ')') && !depth) break;
                }
                if (!*e || e == a) return n;
                char *close = e;                   // the call's own ')', past a lod argument
                if (*close == ',') {
                    depth = 0;
                    for (close++; *close; close++) {
                        if (*close == '(' || *close == '[') depth++;
                        else if ((*close == ')' || *close == ']') && depth) depth--;
                        else if (*close == ')' && !depth) break;
                    }
                    if (!*close) return n;
                }
                char ins[256];
                if (snprintf(ins, sizeof ins, "kl_probe_env((%.*s).xyz)", (int)(e - a), a) >= (int)sizeof ins) return n;
                char *after = klfb_splice(buf, cap, p, (size_t)(close + 1 - p), ins);
                if (!after) return n;
                if (!sampled++) {
                    char *m = strstr(buf, "void main(");
                    const char *env = strstr(buf, "unity_SHAr") && strstr(buf, "unity_SHAg") &&
                                      strstr(buf, "unity_SHAb") ? env_sh : env_flat;
                    if (!m || m > p || !klfb_splice(buf, cap, m, 0, env)) return n;
                    after += strlen(env);             // the call sits after main
                }
                p = after;
                n++;
            }
        }
    }
    char *b = strstr(buf, blk);
    if (b) {
        char *l = b;
        while (l > buf && l[-1] != '\n') l--;
        if (strncmp(l, "layout(std430", 13) != 0) return n;
        b = l;
        char *e = strstr(b, "};");
        if (!e || !klfb_splice(buf, cap, b, (size_t)(e + 2 - b), uni)) return n;
        n++;
    }
    return n;
}
// ...and the same shaders' last step: the colour is looked up in _GradingLut, a
// 3D colour-grading table the post-processing stack fills with a compute kernel
// (BlendLUTs). This ES 3.0 context has no compute stage (glCreateShader(COMPUTE)
// is 0), the table stays black, and so did every graded surface — the level
// black, its particles alone in colour (2026-10-28). The lookup becomes the
// identity, `vec4(coordinate, 1.0)`: the shader takes the square root before
// it and squares after, so the colour comes out as it went in, without the
// game's grading. KL_GLFB_NO_GRADING_LUT (default for x3231001163584154).
static int klfb_no_grading_lut_on(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_GLFB_NO_GRADING_LUT", t && strcmp(t, "x3231001163584154") == 0);
    }
    return on;
}
static int klfb_rewrite_grading_lut(char *buf, size_t cap) {
    static const char *const calls[] = { "textureLod(_GradingLut, ", "texture(_GradingLut, " };
    int n = 0;
    for (int c = 0; c < 2; c++) {
        for (char *p = buf; (p = strstr(p, calls[c])); ) {
            char *a = p + strlen(calls[c]), *e = a;
            int depth = 0;
            for (; *e; e++) {
                if (*e == '(' || *e == '[') depth++;
                else if ((*e == ')' || *e == ']') && depth) depth--;
                else if ((*e == ',' || *e == ')') && !depth) break;
            }
            if (!*e || e == a) return n;
            char *close = e;                       // the call's own ')', past a lod argument
            if (*close == ',') {
                depth = 0;
                for (close++; *close; close++) {
                    if (*close == '(' || *close == '[') depth++;
                    else if ((*close == ')' || *close == ']') && depth) depth--;
                    else if (*close == ')' && !depth) break;
                }
                if (!*close) return n;
            }
            char ins[256];
            if (snprintf(ins, sizeof ins, "vec4(%.*s, 1.0)", (int)(e - a), a) >= (int)sizeof ins) return n;
            char *after = klfb_splice(buf, cap, p, (size_t)(close + 1 - p), ins);
            if (!after) return n;
            p = after;
            n++;
        }
    }
    return n;
}
// bitfieldExtract is ES 3.1 as well, and the same title's soil shaders unpack
// bytes with it ("'bitfieldExtract' : no matching overloaded function found").
// Every call becomes kl_bfe, defined just ahead of main for the scalar types
// (the only ones seen); a shader that uses it before main is left alone.
static int klfb_rewrite_bfe(char *buf, size_t cap) {
    static const char call[] = "bitfieldExtract(";
    if (!strstr(buf, call)) return 0;
    char *m = strstr(buf, "void main(");
    if (!m) return 0;
    char *first = strstr(buf, call);
    if (first < m || strstr(m + 1, "void main(")) return 0;
    static const char defs[] =
        "uint kl_bfe(uint v, int o, int b) { return b == 0 ? 0u : (v >> uint(o)) & (0xffffffffu >> uint(32 - b)); }\n"
        "int kl_bfe(int v, int o, int b) { return b == 0 ? 0 : (v << uint(32 - o - b)) >> uint(32 - b); }\n";
    if (strlen(buf) + sizeof defs >= cap) return 0;
    int n = 0;
    for (char *p = m; (p = strstr(p, call)); n++) {
        if (p > buf && KLFB_IDENT(p[-1])) { p += sizeof call - 1; continue; }
        memcpy(p, "kl_bfe(", 7);
        memmove(p + 7, p + sizeof call - 1, strlen(p + sizeof call - 1) + 1);
        p += 7;
    }
    if (!klfb_splice(buf, cap, m, 0, defs)) return 0;
    return n;
}

// Shader stage, remembered from glCreateShader so the varying-location strip
// below knows which of in/out is the inter-stage varying. GL_VERTEX_SHADER is
// 0x8B31, GL_FRAGMENT_SHADER 0x8B30.
static struct { uint32_t name, type; } g_fb_type[KLFB_MAX_SHADERS];
static unsigned g_fb_ntype;
static void klfb_note_type(uint32_t name, uint32_t type) {
    for (unsigned i = 0; i < g_fb_ntype; i++)
        if (g_fb_type[i].name == name) { g_fb_type[i].type = type; return; }
    if (g_fb_ntype < KLFB_MAX_SHADERS) {
        g_fb_type[g_fb_ntype].name = name;
        g_fb_type[g_fb_ntype].type = type;
        g_fb_ntype++;
    }
}
static uint32_t klfb_shader_type(uint32_t name) {
    for (unsigned i = 0; i < g_fb_ntype; i++)
        if (g_fb_type[i].name == name) return g_fb_type[i].type;
    return 0;
}

// Drop an explicit location qualifier from an INTER-STAGE VARYING, which ES 3.0
// forbids there — the qualifier is legal only on vertex inputs and fragment
// outputs. HLSLCC (UE4) writes "layout(location=N)" on every in/out, varyings
// included, and at the 300 es this renderer runs (kl_egl advertises 3.2 but the
// ANGLE/Metal context is 3.0) ANGLE rejects the ones on vertex OUT / fragment IN
// with "'location' : ... only valid on program inputs and outputs". Varyings
// match across stages by NAME in ES 3.0 and HLSLCC names them identically
// (var_TEXCOORD0 ...), so dropping only the number is safe. Vertex inputs
// (attributes) and fragment outputs (draw buffers) KEEP their location — the
// engine binds attribute arrays by those indices. `p` points at "layout(" of a
// "layout(location..."; is_frag selects the varying qualifier (frag IN / vert
// OUT). Returns the new scan position, or NULL to keep the qualifier.
// Rename all whole-word occurrences of `from` to `to` in place. `to` must be no
// longer than `from` (we only shrink), which holds for kl_var<N> vs the engine's
// var_TEXCOORD*/in_TEXCOORD* names.
static void klfb_rename_ident(char *buf, const char *from, const char *to) {
    size_t fl = strlen(from), tl = strlen(to);
    if (!fl || tl > fl) return;
    for (char *p = buf; (p = strstr(p, from)); ) {
        char before = p == buf ? 0 : p[-1];
        if (KLFB_IDENT(before) || KLFB_IDENT(p[fl])) { p += fl; continue; }
        memcpy(p, to, tl);
        if (tl < fl) memmove(p + tl, p + fl, strlen(p + fl) + 1);
        p += tl;
    }
}

// `p` at "layout(location". If it qualifies an INTER-STAGE VARYING (a vertex
// output or a fragment input), rename that varying to kl_var<location> across the
// whole shader and drop the qualifier. HLSLCC names its varyings differently in
// the two stages (`var_TEXCOORDn` out of the vertex shader, `in_TEXCOORDn` into
// the fragment shader) and relies on the explicit LOCATION to pair them; ES 3.0
// has no location on varyings and pairs by NAME, so a plain strip leaves the
// names mismatched and the program fails to link ("FRAGMENT varying in_TEXCOORD1
// does not match any VERTEX varying"). Canonicalising both ends to the location
// number restores the pairing. Vertex INPUTS (attributes) and fragment OUTPUTS
// (draw buffers) keep their location — those are program I/O, legal on ES 3.0,
// and the engine binds them by index. Returns 1 if it rewrote, else 0.
static int klfb_rename_varying(char *buf, char *p, int is_frag) {
    char *close = strchr(p, ')');
    if (!close) return 0;
    const char *loc = NULL;
    for (const char *d = p; d + 8 <= close; d++)
        if (strncmp(d, "location", 8) == 0) { loc = d + 8; break; }
    while (loc && loc < close && (*loc == ' ' || *loc == '=' || *loc == '\t')) loc++;
    if (!loc || loc >= close || *loc < '0' || *loc > '9') return 0;
    long L = strtol(loc, NULL, 10);
    char *after = close + 1;
    while (*after == ' ' || *after == '\t') after++;
    char *q = after;
    int saw_flat = 0;
    for (;;) {
        while (*q == ' ' || *q == '\t') q++;
        static const char *skip[] = { "flat", "smooth", "noperspective", "centroid",
                                      "invariant", "highp", "mediump", "lowp" };
        int did = 0;
        for (unsigned i = 0; i < sizeof skip / sizeof skip[0]; i++) {
            size_t l = strlen(skip[i]);
            if (strncmp(q, skip[i], l) == 0 && !KLFB_IDENT(q[l])) {
                if (i == 0) saw_flat = 1;
                q += l; did = 1; break;
            }
        }
        if (!did) break;
    }
    int is_out = (strncmp(q, "out", 3) == 0 && !KLFB_IDENT(q[3]));
    int is_in  = (strncmp(q, "in",  2) == 0 && !KLFB_IDENT(q[2]));
    int varying = is_frag ? is_in : is_out;
    if (!varying) return 0;
    // An INTEGER varying must be `flat` in GLSL ES — the language says so and
    // ANGLE holds to it ("'out' : must use 'flat' interpolation here"), where a
    // Quest's driver lets it pass. Unreal's cross-compiler writes
    // `layout(location=0) out uint var_...;` for the view index it hands from a
    // multiview vertex shader to its fragment shader (Maestro: one shader of
    // forty, and whatever it draws was missing). The qualifier goes in where the
    // layout came out, so the text still only shrinks.
    int need_flat = 0;
    if (!saw_flat) {
        const char *ty = q + (is_out ? 3 : 2);
        for (;;) {
            while (*ty == ' ' || *ty == '\t') ty++;
            static const char *prec[] = { "highp", "mediump", "lowp" };
            int did = 0;
            for (unsigned i = 0; i < 3; i++) {
                size_t l = strlen(prec[i]);
                if (strncmp(ty, prec[i], l) == 0 && !KLFB_IDENT(ty[l])) { ty += l; did = 1; break; }
            }
            if (!did) break;
        }
        static const char *ints[] = { "int", "uint", "ivec2", "ivec3", "ivec4", "uvec2", "uvec3", "uvec4" };
        for (unsigned i = 0; i < sizeof ints / sizeof ints[0]; i++) {
            size_t l = strlen(ints[i]);
            if (strncmp(ty, ints[i], l) == 0 && !KLFB_IDENT(ty[l])) { need_flat = 1; break; }
        }
    }
    // The declared identifier: the last name before ';' (or before a '[N]').
    char *semi = strchr(q, ';');
    if (!semi) return 0;
    char *e = semi;
    while (e > q && (e[-1] == ' ' || e[-1] == '\t')) e--;
    if (e > q && e[-1] == ']') {
        char *b = e; while (b > q && b[-1] != '[') b--;
        if (b > q) { e = b - 1; while (e > q && (e[-1] == ' ' || e[-1] == '\t')) e--; }
    }
    char *nm = e;
    while (nm > q && KLFB_IDENT(nm[-1])) nm--;
    if (e <= nm || (size_t)(e - nm) >= 60) return 0;
    char name[64]; memcpy(name, nm, (size_t)(e - nm)); name[e - nm] = 0;
    char canon[24]; snprintf(canon, sizeof canon, "kl_var%ld", L);
    if (strlen(canon) > strlen(name)) return 0;         // only shrink, for in-place
    memmove(p, after, strlen(after) + 1);               // drop "layout(...) "
    if (need_flat && (size_t)(after - p) >= 5) {        // ...and "flat " fits in its place
        memmove(p + 5, p, strlen(p) + 1);
        memcpy(p, "flat ", 5);
    }
    klfb_rename_ident(buf, name, canon);                // pair by canonical name
    return 1;
}

// ESSL's default INT precision is per-STAGE, and a uniform declared in both
// stages without one is therefore two different types.
//
// The spec: a vertex shader defaults `int` to highp, a fragment shader defaults
// it to mediump. A uniform (or a struct member) that states no precision picks
// up its stage's default, and a strict linker then refuses the program because
// the two declarations disagree. ANGLE is strict; Quest drivers are not, which
// is why a Quest title can ship shaders that do not link here. The Simpsons
// Hit & Run VR port is the case that found it — six lit programs failed with
//
//   Precisions of uniform 'lights' member 'lights.enabled' differ between
//   VERTEX and FRAGMENT shaders.
//
// against `struct LightParams { int enabled; vec4 position; ... }`, and the
// engine then dereferenced the null program it got back for one of them on the
// first level load.
//
// The repair states the vertex stage's default OUT LOUD in both stages, which
// is a no-op in the vertex shader (it already defaults to highp int) and a
// PROMOTION in the fragment shader (mediump -> highp). Nothing is narrowed, so
// no value that fitted before stops fitting; pinning both to mediump instead
// would demote vertex-stage loop counters and index arithmetic.
//
// Placement is directly after the source's FIRST precision statement, which is
// guaranteed to follow #version and #extension. A source that states an int
// precision already has no mismatch to fix, and a source with no precision
// statement at all is left alone. KL_GLFB_NO_INT_PRECISION=1 disables it.
static int klfb_rewrite_int_precision(char *buf, size_t cap) {
    static int off = -1;
    if (off < 0) off = kl_env_on("KL_GLFB_NO_INT_PRECISION", 0);
    if (off) return 0;

    static const char *const q[] = { "precision highp int;", "precision mediump int;",
                                     "precision lowp int;" };
    for (unsigned i = 0; i < sizeof q / sizeof q[0]; i++)
        if (strstr(buf, q[i])) return 0;

    char *p = strstr(buf, "precision ");
    if (!p) return 0;
    char *semi = strchr(p, ';');
    if (!semi) return 0;
    char *at = semi + 1;                       // insert directly after it

    static const char ins[] = "\nprecision highp int;";
    size_t n = sizeof ins - 1;
    if (strlen(buf) + n + 1 > cap) return 0;   // no slack: leave the text alone
    memmove(at + n, at, strlen(at) + 1);
    memcpy(at, ins, n);
    return 1;
}

// UE 4.19's HLSLCC passes every varying through an interface block when the
// device names GL_EXT_shader_io_blocks (which its ES 3.1 path requires — see
// klgl_ext_extra in kl_egl.c, Vader Immortal I):
//   #define INTERFACE_BLOCK(Pos, Interp, Modifiers, Semantic, PreType, PostType) \
//       layout(location=Pos) Modifiers Semantic { PreType PostType; }
//   INTERFACE_BLOCK(0, noperspective , out , TEXCOORD0, highp vec2, Data) out_TEXCOORD0;
//   ... out_TEXCOORD0.Data.xy = ...;
// An in/out block is ES 3.1; at 300 es ANGLE says "interface blocks must be
// uniform". Each block holds one member, so it is a plain varying in a wrapper:
// the use becomes "layout(location=0) out highp vec2 out_TEXCOORD0;" and
// "out_TEXCOORD0.Data" becomes "out_TEXCOORD0", after which the varying pass
// pairs the two stages by location as it does for every HLSLCC shader.
// noperspective is dropped (ES 3.0 has no such qualifier; the shaders that ask
// for it are screen-space passes, where it changes nothing); flat and centroid
// stay. Everything shrinks, so the edit is in place.
static int klfb_flatten_interface_blocks(char *buf) {
    if (!strstr(buf, "#define INTERFACE_BLOCK(Pos, Interp, Modifiers, Semantic, PreType, PostType)"))
        return 0;
    int changed = 0;
    for (char *p = buf; (p = strstr(p, "INTERFACE_BLOCK(")); ) {
        char *a = p + 16;
        if (*a < '0' || *a > '9') { p = a; continue; }   // the #define itself
        char *close = strchr(a, ')');
        char *semi = close ? strchr(close, ';') : NULL;
        if (!close || !semi) break;
        // Six comma-separated arguments, none of which holds a comma or ')'.
        char arg[6][64];
        int na = 0;
        for (char *s = a; s <= close && na < 6; na++) {
            char *e = s;
            while (e < close && *e != ',') e++;
            while (s < e && (*s == ' ' || *s == '\t')) s++;
            char *t = e;
            while (t > s && (t[-1] == ' ' || t[-1] == '\t')) t--;
            if ((size_t)(t - s) >= sizeof arg[0]) { na = -1; break; }
            memcpy(arg[na], s, (size_t)(t - s)); arg[na][t - s] = 0;
            s = e + 1;
        }
        char inst[64];
        char *n = close + 1, *ne;
        while (*n == ' ' || *n == '\t') n++;
        for (ne = n; KLFB_IDENT(*ne); ne++) {}
        if (na != 6 || ne == n || (size_t)(ne - n) >= sizeof inst) { p = close; continue; }
        memcpy(inst, n, (size_t)(ne - n)); inst[ne - n] = 0;
        // PostType may carry an array size ("Data[2]"): it moves to the name.
        char member[64], dims[32] = "";
        snprintf(member, sizeof member, "%s", arg[5]);
        char *br = strchr(member, '[');
        if (br) { snprintf(dims, sizeof dims, "%s", br); *br = 0; }
        const char *interp = strcmp(arg[1], "noperspective") == 0 ? "" : arg[1];
        char rep[256];
        int rl = snprintf(rep, sizeof rep, "layout(location=%s) %s%s%s %s %s%s;",
                          arg[0], interp, *interp ? " " : "", arg[2], arg[4], inst, dims);
        if (rl <= 0 || rl > semi + 1 - p) { p = close; continue; }
        memcpy(p, rep, (size_t)rl);
        memmove(p + rl, semi + 1, strlen(semi + 1) + 1);
        // "<inst>.<member>" -> "<inst>" everywhere after the declaration.
        char from[160];
        snprintf(from, sizeof from, "%s.%s", inst, member);
        size_t fl = strlen(from), il = strlen(inst);
        for (char *u = p + rl; (u = strstr(u, from)); ) {
            if ((u > buf && KLFB_IDENT(u[-1])) || KLFB_IDENT(u[fl])) { u += fl; continue; }
            memmove(u + il, u + fl, strlen(u + fl) + 1);
            u += il;
        }
        p += rl;
        changed = 1;
    }
    return changed;
}

static char *klfb_rewrite_glsl(char *buf, size_t cap, uint32_t shader) {
    int changed = 0;
    klfb_pin_reset(shader);
    // First, because it inserts a line and the rules below either shrink or
    // preserve length.
    if (klfb_rewrite_int_precision(buf, cap)) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] `precision highp int;` stated in shaders that "
                            "left it to the stage default — ESSL defaults int to "
                            "highp in a vertex shader and mediump in a fragment "
                            "one, so a uniform declared in both is two types and "
                            "the link fails (KL_GLFB_NO_INT_PRECISION=1 disables)\n");
        changed = 1;
    }
    if (strncmp(buf, "#version 3", 10) == 0 &&
        buf[10] >= '0' && buf[10] <= '9' && buf[11] >= '0' && buf[11] <= '9' &&
        (buf[10] > '0' || buf[11] > '0') && strncmp(buf + 12, " es", 3) == 0) {
        memcpy(buf + 10, "00", 2);          // "#version 3xx es" -> 300 es
        changed = 1;
    }
    // "binding = <tok>" inside a layout list, in either comma position:
    // "layout(binding = x, std140)" or "layout(std140, binding = 0)".
    // Token ends at the first ',' or ')'.
    char *p = buf;
    while ((p = strstr(p, "binding = "))) {
        char *tok = p + strlen("binding = ");
        char *end = strpbrk(tok, ",)");
        if (!end) break;
        if (*end == ',') {                       // leading entry: drop "b = t, "
            char *q = end + 1;
            while (*q == ' ') q++;
            memmove(p, q, strlen(q) + 1);
        } else {                                 // trailing: drop ", b = t"
            char *q = p;
            while (q > buf && q[-1] == ' ') q--;
            if (q > buf && q[-1] == ',') q--;
            memmove(q, end, strlen(end) + 1);
        }
        changed = 1;
    }
    // Explicit uniform locations, both spellings Unity emits: the
    // UNITY_LOCATION(x) macro use ("UNITY_LOCATION(0) uniform mediump
    // sampler2D _MainTex;") and the direct layout form. HLSLCC spells every
    // in/out qualifier as a literal "layout(location = ...)", so the macro
    // form is unambiguous; the literal form goes through the uniform check.
    p = buf;
    while ((p = strstr(p, "UNITY_LOCATION("))) {
        char *q = klfb_strip_uniform_layout(p, shader);
        if (q) { p = q; changed = 1; } else p += 5;
    }
    p = buf;
    while ((p = strstr(p, "layout(location = "))) {
        char *q = klfb_strip_uniform_layout(p, shader);
        if (q) { p = q; changed = 1; } else p += 8;
    }

    // Older HLSLCC (gnustl-era UE4 — Tales from the Galaxy's Edge) does not
    // spell the qualifier out: it emits
    //   #define INTERFACE_LOCATION(Pos) layout(location=Pos)
    // and then "INTERFACE_LOCATION(0) out highp vec2 var_TEXCOORD0;". The
    // varying pass below only sees "layout(location=Pos)" in the #define, finds
    // no number and leaves every use alone, so the 300 es compile fails with
    // "'location' : invalid layout qualifier". Expand each NUMERIC use to the
    // macro's own body — identical meaning, and shorter, so in place — and
    // only when the shader defines the macro exactly that way.
    if (strstr(buf, "#define INTERFACE_LOCATION(Pos) layout(location=Pos)")) {
        p = buf;
        while ((p = strstr(p, "INTERFACE_LOCATION("))) {
            char *d = p + 19, *e = d;
            while (*e >= '0' && *e <= '9') e++;
            if (e == d || *e != ')' || e - d > 2) { p += 19; continue; }
            char rep[32];
            int rl = snprintf(rep, sizeof rep, "layout(location=%.*s)", (int)(e - d), d);
            memcpy(p, rep, (size_t)rl);
            memmove(p + rl, e + 1, strlen(e + 1) + 1);
            p += rl;
            changed = 1;
        }
    }

    // Before the varying pass, which then sees ordinary located varyings.
    if (klfb_flatten_interface_blocks(buf)) changed = 1;

    // Varying locations (both spellings): ES 3.1 syntax on a vertex output or a
    // fragment input, which the 300 es context rejects. Needs the stage; fall
    // back to "writes gl_Position => vertex" if glCreateShader was not seen.
    {
        uint32_t t = klfb_shader_type(shader);
        int is_frag = t == 0x8B30 ? 1
                    : t == 0x8B31 ? 0
                    : (strstr(buf, "gl_Position") == NULL);
        p = buf;
        while ((p = strstr(p, "layout(location"))) {
            if (klfb_rename_varying(buf, p, is_frag)) { changed = 1; p = buf; }
            else p += 15;
        }
    }

    // External images, which ANGLE's Metal backend does not have.
    //
    // Steam Link's video shader is the ordinary Android one — it declares
    // `#extension GL_OES_EGL_image_external_essl3` and a `samplerExternalOES`,
    // because on Android the decoded frame arrives as an external image and the
    // sampler is what performs the YUV->RGB conversion. ANGLE hard-disables
    // both extensions on Metal (DisplayMtl.mm sets EGLImageExternalOES and
    // EGLImageExternalEssl3OES to false, with an upstream "NOTE(hqle): Support
    // ..." beside them), so the shader does not compile at all and the app
    // carries on with a broken program — measured, and it is the second half of
    // why there is no picture.
    //
    // An external sampler is a 2D sampler plus three promises: the conversion
    // has happened, there are no mipmaps, and the texture cannot be redefined
    // by glTexImage2D. We keep all three. kl_vtdec asks VideoToolbox for BGRA
    // rather than NV12 precisely so the conversion is already done (see
    // kl_vtdec.h), and the other two are restrictions, so a 2D texture is a
    // strict superset. The retarget is therefore exact HERE — it is not a
    // general claim that external images are 2D images.
    //
    // The bind target has to move with it (klfb_BindTexture below): a shader
    // that samples `sampler2D` while the guest binds GL_TEXTURE_EXTERNAL_OES
    // reads nothing, and would be a worse failure than the compile error
    // because it is silent.
    //
    // Both edits are length-preserving, which is what lets them run in place
    // alongside the shrinking rules above.
    p = buf;
    while ((p = strstr(p, "#extension GL_OES_EGL_image_external"))) {
        char *e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        memset(p, ' ', (size_t)(e - p));   // blank the directive, keep the line
        changed = 1;
    }
    p = buf;
    while ((p = strstr(p, "samplerExternalOES"))) {
        memcpy(p, "sampler2D", 9);
        memset(p + 9, ' ', strlen("samplerExternalOES") - 9);
        p += strlen("samplerExternalOES");
        changed = 1;
    }

    // A variable named for a built-in function.
    //
    // Steam Link's pointer shader declares `uniform float length;` and also
    // calls the built-in `length(v)` three times. ESSL keeps variables and
    // functions in ONE namespace, so the declaration hides the function and
    // every later call is "'length' : function name expected". The vendor GLES
    // driver this shader shipped against evidently keeps two namespaces, C-style,
    // and accepts it. Three compile errors, then the program fails to LINK — and
    // that program belongs to XRConstruct, which is what draws the decoded video,
    // so the whole stream scene comes up with "[SceneStream] Failed to initialize
    // construct" and there is no surface for a frame to land on.
    //
    // The split is exact rather than a heuristic, because it is the compiler's
    // own: an occurrence followed by `(` IS the function call it is complaining
    // about, and every other occurrence is the variable. Renaming the second
    // group and leaving the first restores exactly the reading the shader was
    // written for. klfb_GetUniformLocation maps the guest's name back, so a
    // uniform this renames is still findable by the name it was declared with.
    for (size_t i = 0; i < sizeof g_glsl_builtin_vars / sizeof g_glsl_builtin_vars[0]; i++) {
        const char *from = g_glsl_builtin_vars[i].from, *to = g_glsl_builtin_vars[i].to;
        size_t n = strlen(from);
        if (strlen(to) != n || strstr(buf, to)) continue;   // never shadow a name already here
        int renamed = 0;
        for (p = buf; (p = strstr(p, from)); p += n) {
            char before = p == buf ? 0 : p[-1];
            if (KLFB_IDENT(before) || KLFB_IDENT(p[n])) continue;   // part of a longer name
            const char *q = p + n;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '(') continue;                                // the function call
            memcpy(p, to, n);
            renamed++;
        }
        if (renamed) {
            fprintf(stderr, "  [glfb] shader declares `%s`, which is a built-in "
                            "function name — renamed %d use%s to `%s`\n",
                    from, renamed, renamed == 1 ? "" : "s", to);
            changed = 1;
        }
    }
    // Texture buffers (samplerBuffer): ES 3.1 feature the ES 3.0 context rejects;
    // emulate as a 2D texture (host half in klfb_TexBuffer).
    if (klfb_rewrite_texel_buffers(buf, cap)) changed = 1;

    // gl_ClipDistance through GL_EXT_clip_cull_distance: ANGLE on Metal does
    // not expose that extension, but does expose GL_APPLE_clip_distance, which
    // provides the same gl_ClipDistance (redeclarable, up to 8). Tea For God's
    // vertex shaders enable the EXT one and clip with six planes; every one of
    // them failed to compile ("GL_APPLE_clip_distance : extension is disabled")
    // and the game's library load then crashed on a missing store. Only when
    // the shader does not use gl_CullDistance, which APPLE has no equivalent
    // for. Same length after padding, so the rewrite is in place.
    if (!strstr(buf, "gl_CullDistance")) {
        static const char from[] = "GL_EXT_clip_cull_distance";
        static const char to[]   = "GL_APPLE_clip_distance   ";
        int n = 0;
        for (p = buf; (p = strstr(p, from)); p += sizeof from - 1) {
            memcpy(p, to, sizeof to - 1);
            n++;
        }
        if (n) {
            static int said;
            if (!said) {
                said = 1;
                fprintf(stderr, "  [glfb] shader enables GL_EXT_clip_cull_distance "
                                "— served as GL_APPLE_clip_distance (clip only)\n");
            }
            changed = 1;
        }
    }

    // Unreal without multiview emits "#define gl_ViewID_OVR 0" and then compares
    // it with uints ("gl_ViewID_OVR==0u", "gl_ViewID_OVR==uint(i0)"). The
    // built-in it stands for is a uint; a Quest's driver lets int == uint pass,
    // ANGLE does not ("no operation '==' ... 'const int' and ... 'highp uint'"),
    // and the program fails to link. "0u" is the same value with the type the
    // name promises. Seen in Tales from the Galaxy's Edge (UE 4.24).
    {
        static const char vid[] = "#define gl_ViewID_OVR 0";
        size_t vl = sizeof vid - 1, len = strlen(buf);
        p = strstr(buf, vid);
        if (p && (p[vl] == '\n' || p[vl] == ' ' || p[vl] == '\r' || p[vl] == '\t') &&
            len + 2 <= cap) {
            memmove(p + vl + 1, p + vl, strlen(p + vl) + 1);
            p[vl] = 'u';
            changed = 1;
        }
    }

    // fma() is ESSL 3.20 / GL_EXT_gpu_shader5; a "#version 300 es" shader that
    // calls it compiles on Adreno and not on ANGLE ("'fma' : no matching
    // overloaded function found"). Tea For God's material shaders all do, so
    // every material failed and the game went black after its splash. Define
    // it as the unfused multiply-add right after the #version line — it is a
    // precision hint, not a semantic difference.
    if (strncmp(buf, "#version 300 es", 15) == 0 && !strstr(buf, "gpu_shader5")) {
        int uses = 0;
        for (p = buf; (p = strstr(p, "fma")); p += 3) {
            char before = p == buf ? 0 : p[-1];
            const char *q = p + 3;
            while (*q == ' ' || *q == '\t') q++;
            if (!KLFB_IDENT(before) && *q == '(') { uses = 1; break; }
        }
        static const char def[] = "#define fma(a, b, c) ((a) * (b) + (c))\n";
        char *nl = strchr(buf, '\n');
        size_t len = strlen(buf);
        if (uses && nl && len + sizeof def <= cap) {
            nl++;
            memmove(nl + sizeof def - 1, nl, strlen(nl) + 1);
            memcpy(nl, def, sizeof def - 1);
            static int said;
            if (!said) {
                said = 1;
                fprintf(stderr, "  [glfb] ESSL 3.00 shader calls fma() — defined "
                                "as a*b+c (ESSL 3.20 built-in)\n");
            }
            changed = 1;
        }
    }

    return changed ? buf : NULL;
}

static uint32_t (*g_real_CreateShader)(uint32_t);
static void (*g_real_DeleteShader)(uint32_t);
static void (*g_real_BindBuffer)(uint32_t, uint32_t);
// ----------------------------------------- compute kernels as transform feedback
//
// Beat Saber 1.45 builds the quads of its tube lights and lasers in a compute
// kernel (FillMeshData): one invocation per light reads 144 bytes of
// `_LightData` and writes 4 vertices straight into the vertex buffer of the mesh
// the bloom pre-pass then draws. 1.40 did that on the CPU; 1.45 has no such path
// left, and ANGLE's Metal backend is ES 3.0 — no compute stage, no shader
// storage. Refused, the kernel leaves the mesh empty and the pre-pass (the
// source of the fog's colour) without its lights.
//
// A kernel of that shape — every invocation reads only its own element of one
// read-only buffer and writes only its own stride of one output buffer — is a
// vertex shader with transform feedback, which ES 3.0 has:
//
//   compute shader object   a GL_VERTEX_SHADER, its source rewritten
//   invocation i            vertex i of one GL_POINTS draw
//   readonly buffer X       X as the vertex buffer: element i is vertex i's
//                           attributes, a uvec4 per 16 bytes (nothing Unity's
//                           reflection of the program's uniforms can trip over)
//   buffer Y, written       uvec4 varyings captured into Y (one each: ANGLE
//                           does not capture arrays)
//   glDispatchCompute       that draw, under RASTERIZER_DISCARD
//
// The game's own GLSL does the arithmetic; only the storage access is rewritten.
// What it cannot express is refused at glShaderSource (the shader then fails to
// compile, as it did before). KL_GLFB_COMPUTE_TF=0/1; on for Beat Saber from
// 1.45 (kl_guest_beatsaber_openxr), the one guest it was seen with.
#define KLFB_GL_COMPUTE_SHADER 0x91B9
#define KLFB_GL_SSBO           0x90D2
#define KLFB_CS_MAX            128
#define KLFB_CS_BINDINGS       8
typedef struct {
    uint32_t name;                       // shader, then (in g_cs_progs) program
    uint32_t fs;                         // program only: the fragment stage we add
    int group_x, group_y, group_z;       // local_size_x, y, z
    int group, in_words, out_words;      // local_size_x; uints per element read / written
    int in_binding, out_binding;         // the kernel's shader-storage binding points
    int ready;                           // program only
    int is_tf;                           // 1 if transform feedback kernel
    int sb;                              // 1: inputs read as textures (klfb_cs_rewrite_sb)
    // ...and that kernel's storage blocks as it declared them, which is what a
    // guest asking the program about GL_SHADER_STORAGE_BLOCK is told
    int sbn;
    struct { char name[48]; int binding, words; } sbb[KLFB_CS_BINDINGS];
} klfb_cs_obj;
static klfb_cs_obj g_cs_shaders[KLFB_CS_MAX], g_cs_progs[KLFB_CS_MAX];
static struct { uint32_t buf; intptr_t off, size; } g_cs_ssbo[KLFB_CS_BINDINGS];
static uint32_t g_cs_generic;            // what the guest has bound to GL_SHADER_STORAGE_BUFFER
static struct { void *ctx; uint32_t vao; } g_cs_vaos[4];   // a VAO belongs to one context

int kl_glfb_compute_tf(void) {
    static int on = -1;
    if (!kl_glfb_enabled()) return 0;
    if (on < 0) {
        extern int kl_guest_beatsaber_openxr(void);
        int env = kl_env_int("KL_GLFB_COMPUTE_TF", -1);
        if (env >= 0) on = env != 0;
        else {
            // The guest's answer is only good once its library directory is
            // set (see kl_guest_beatsaber_openxr); a yes is, and so is any
            // answer given after GL came up. Until then: no, and ask again.
            // Arizona Sunshine's are Unity's skinning kernels, served by the
            // texture-reading form (klfb_cs_rewrite_sb, with KL_GLFB_SSBO_TEX).
            extern const char *kl_driver_target_name(void);
            const char *t = kl_driver_target_name();
            int g = kl_guest_beatsaber_openxr() || (t && strcmp(t, "x2190353671014400") == 0);
            if (!g && !g_ready) return 0;
            on = g;
        }
    }
    return on;
}
// Compute kernels that cannot run here, stubbed (community fixes 62/64): a
// guest whose compute shaders ANGLE's ES 3.0 refuses (glCreateShader(0x91b9)
// -> 0) and whose Unity shader compiler then reads metadata off the missing
// object. The kernel becomes a dummy vertex shader that keeps its local size
// for COMPUTE_WORK_GROUP_SIZE, and its dispatches do nothing. Warplanes: WW1
// Fighters (x3922378427824210) and Warplanes: Battles over Pacific
// (x3984056454948095) crashed while their scenes loaded without it.
//   KL_GLFB_COMPUTE_STUB=0|1 — default 1 for those two titles, else 0.
int kl_glfb_compute_stub(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_GLFB_COMPUTE_STUB", t && (strcmp(t, "x3922378427824210") == 0 ||
                                                     strcmp(t, "x3984056454948095") == 0));
    }
    return on;
}
// Read-only storage buffers as textures (klfb_rewrite_ro_ssbo, klfb_ssbo_apply).
// KL_GLFB_SSBO_TEX=0/1; on for Garden of the Sea (x3684804704932159), the one
// title it was written for, and Arizona Sunshine (x2190353671014400): told the
// ES 3.1 storage limits are zero, much of its world was Unity's magenta error
// shader; told the minimums without this, every one of its multiview shaders
// failed on "'std430' : invalid layout qualifier" and it hung on its logo
// (2026-10-31). While it is on, the shader-storage binding points are kept
// here as they are for the compute kernels above.
int kl_glfb_ssbo_tex(void) {
    static int on = -1;
    if (!kl_glfb_enabled()) return 0;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_GLFB_SSBO_TEX", t && (strcmp(t, "x3684804704932159") == 0 ||
                                                 strcmp(t, "x2190353671014400") == 0));
    }
    return on;
}
static void klfb_tbm_want(uint32_t buf);            // the kept bytes, below
static void klfb_ssbo_forget(uint32_t prog);        // the program table, below
static klfb_cs_obj *klfb_cs_find(klfb_cs_obj *tab, uint32_t name) {
    if (!name) return NULL;
    for (int i = 0; i < KLFB_CS_MAX; i++) if (tab[i].name == name) return &tab[i];
    return NULL;
}
static klfb_cs_obj *klfb_cs_add(klfb_cs_obj *tab, uint32_t name) {
    klfb_cs_obj *o = klfb_cs_find(tab, name);
    for (int i = 0; !o && i < KLFB_CS_MAX; i++) if (!tab[i].name) o = &tab[i];
    if (!o) { fprintf(stderr, "  [glfb] compute-tf: table full, object %u not tracked\n", name); return NULL; }
    memset(o, 0, sizeof *o);
    o->name = name;
    return o;
}

// Replace every `<head>EXPR]` with `<pre>EXPR<post>` in place. EXPR is whatever
// stands before the closing bracket; HLSLcc writes these indices with
// parentheses only. Returns the number of replacements, -1 when out of room.
static int klfb_cs_reindex(char *buf, size_t cap, const char *head, const char *pre, const char *post) {
    size_t hl = strlen(head), prl = strlen(pre), pol = strlen(post);
    int n = 0;
    for (char *p = buf; (p = strstr(p, head)) != NULL; n++) {
        char *e = strchr(p + hl, ']');
        if (!e) return -1;
        size_t len = strlen(buf);
        if (len + prl + pol + 1 > cap + hl) return -1;
        size_t el = (size_t)(e - (p + hl)), tl = strlen(e + 1) + 1;
        // whichever of the two moves away from the other goes first
        if (prl <= hl) { memmove(p + prl, p + hl, el); memmove(p + prl + el + pol, e + 1, tl); }
        else           { memmove(p + prl + el + pol, e + 1, tl); memmove(p + prl, p + hl, el); }
        memcpy(p, pre, prl);
        memcpy(p + prl + el, post, pol);
        p += prl + el + pol;
    }
    return n;
}
// Cut [from, the end of the first `stop` at or after it). 0 if either is missing.
static int klfb_cs_cut(char *from, const char *stop) {
    char *e = from ? strstr(from, stop) : NULL;
    if (!e) return 0;
    e += strlen(stop);
    memmove(from, e, strlen(e) + 1);
    return 1;
}
// The rewrite. `buf` holds the kernel as HLSLcc wrote it; on success it holds
// the vertex shader and `cs` the numbers the dispatch needs.
static int klfb_cs_rewrite(char *buf, size_t cap, klfb_cs_obj *cs) {
    char in[64] = "", out[64] = "", t[256];
    const char *why = NULL;
    char *p, *ro = strstr(buf, ") readonly buffer ");
    // the two storage blocks: `layout(std430, binding = N) [readonly] buffer NAME {`
    if (!ro || sscanf(ro + 18, "%63[A-Za-z0-9_]", in) != 1) why = "no read-only buffer block";
    for (p = buf; !why && (p = strstr(p, ") buffer ")) != NULL; p += 9) {
        if (out[0]) { why = "more than one written buffer"; break; }
        sscanf(p + 9, "%63[A-Za-z0-9_]", out);
    }
    if (!why && !out[0]) why = "no written buffer block";
    if (!why && strstr(ro + 18, ") readonly buffer ")) why = "more than one read-only buffer";
    if (!why) {
        char *b;
        snprintf(t, sizeof t, ") readonly buffer %s {", in);
        for (b = strstr(buf, t); b && b > buf && *b != '\n'; b--) {}
        if (!b || sscanf(b, " layout(std430, binding = %d", &cs->in_binding) != 1) why = "read-only block binding";
        snprintf(t, sizeof t, ") buffer %s {", out);
        for (b = strstr(buf, t); b && b > buf && *b != '\n'; b--) {}
        if (!why && (!b || sscanf(b, " layout(std430, binding = %d", &cs->out_binding) != 1)) why = "written block binding";
    }
    // element size read, work group size, bytes written per invocation
    if (!why) {
        snprintf(t, sizeof t, "struct %s_type {", in);
        p = strstr(buf, t);
        if (!p || sscanf(p + strlen(t), " uint[%d] value;", &cs->in_words) != 1) why = "element is not a uint[N]";
    }
    if (!why) {
        int y = 0, z = 0, stride = 0;
        p = strstr(buf, "layout(local_size_x = ");
        if (!p || sscanf(p, "layout(local_size_x = %d, local_size_y = %d, local_size_z = %d) in;",
                         &cs->group, &y, &z) != 3 || y != 1 || z != 1) why = "work group is not N x 1 x 1";
        p = strstr(buf, "gl_GlobalInvocationID.x) * ");
        if (!why && (!p || sscanf(p + 27, "%d", &stride) != 1)) why = "no output stride";
        cs->out_words = stride / 4;
        if (!why && (stride % 16 || cs->out_words < 4 || cs->out_words > 56)) why = "output stride does not fit the varyings";
        if (!why && (cs->in_words % 4 || cs->in_words < 4 || cs->in_words > 48 || cs->group < 1))
            why = "input does not fit the vertex attributes";
        if (!why && (cs->in_binding < 0 || cs->in_binding >= KLFB_CS_BINDINGS ||
                     cs->out_binding < 0 || cs->out_binding >= KLFB_CS_BINDINGS)) why = "binding point out of range";
    }
    // the declarations go, ours take their place
    if (!why) {
        char decl[1024];
        snprintf(t, sizeof t, "struct %s_type {", in);
        p = strstr(buf, t);
        int cut = klfb_cs_cut(p, "};\n");
        snprintf(t, sizeof t, ") readonly buffer %s {", in);
        for (p = strstr(buf, t); p && p > buf && p[-1] != '\n'; p--) {}
        cut += klfb_cs_cut(p, "};\n");
        snprintf(t, sizeof t, ") buffer %s {", out);
        for (p = strstr(buf, t); p && p > buf && p[-1] != '\n'; p--) {}
        cut += klfb_cs_cut(p, "};\n");
        char *at = p;                      // where the written block stood
        cut += klfb_cs_cut(strstr(buf, "layout(local_size_x = "), "\n");
        if (cut != 4) why = "declarations not where expected";
        int n = why ? 0 : snprintf(decl, sizeof decl,
            "uvec4 kl_in_v[%d];\n"
            "uint kl_out[%d];\n"
            "uvec3 kl_gid;\n"
            "int kl_first;\n"
            "uint kl_in(int w) { return kl_in_v[w >> 2][w & 3]; }\n",
            cs->in_words / 4, cs->out_words);
        for (int i = 0; !why && i < cs->in_words / 4; i++)
            n += snprintf(decl + n, sizeof decl - (size_t)n, "in uvec4 kl_a%d;\n", i);
        for (int i = 0; !why && i < cs->out_words / 4; i++)
            n += snprintf(decl + n, sizeof decl - (size_t)n, "flat out uvec4 kl_o%d;\n", i);
        if (!why && strlen(buf) + (size_t)n + 1 > cap) why = "no room";
        if (!why) klfb_splice(buf, cap, at, 0, decl);
    }
    if (!why) {
        snprintf(t, sizeof t, "%s_buf[gl_GlobalInvocationID.x].value[", in);
        int r = klfb_cs_reindex(buf, cap, t, "kl_in(int(", "))");
        snprintf(t, sizeof t, "%s_buf[", out);
        int w = klfb_cs_reindex(buf, cap, t, "kl_out[int(", ") - kl_first]");
        if (r <= 0 || w <= 0) why = "no storage access to rewrite";
        snprintf(t, sizeof t, "%s_buf", in);
        if (!why && strstr(buf, t)) why = "read-only buffer indexed by something else";
    }
    if (!why) {
        for (p = buf; (p = strstr(p, "gl_GlobalInvocationID")) != NULL; )
            klfb_splice(buf, cap, p, strlen("gl_GlobalInvocationID"), "kl_gid");
        p = strstr(buf, "void main()");
        if (!p) why = "no main";
        else klfb_splice(buf, cap, p, strlen("void main()"), "void kl_cs_main()");
        if (strstr(buf, "gl_LocalInvocation") || strstr(buf, "gl_WorkGroupID") || strstr(buf, "gl_NumWorkGroups") ||
            strstr(buf, "imageStore") || strstr(buf, "imageLoad") || strstr(buf, "atomic"))
            why = "uses something a vertex shader has not";
    }
    if (!why) {
        char tail[2048];
        int n = snprintf(tail, sizeof tail,
            "\nvoid main() {\n"
            "    kl_gid = uvec3(uint(gl_VertexID), 0u, 0u);\n"
            "    kl_first = gl_VertexID * %d;\n"
            "    for (int i = 0; i < %d; i++) kl_out[i] = 0u;\n", cs->out_words, cs->out_words);
        for (int i = 0; i < cs->in_words / 4; i++)
            n += snprintf(tail + n, sizeof tail - (size_t)n, "    kl_in_v[%d] = kl_a%d;\n", i, i);
        n += snprintf(tail + n, sizeof tail - (size_t)n, "    kl_cs_main();\n");
        for (int i = 0; i < cs->out_words / 4; i++)
            n += snprintf(tail + n, sizeof tail - (size_t)n,
                          "    kl_o%d = uvec4(kl_out[%d], kl_out[%d], kl_out[%d], kl_out[%d]);\n",
                          i, i * 4, i * 4 + 1, i * 4 + 2, i * 4 + 3);
        n += snprintf(tail + n, sizeof tail - (size_t)n,
                      "    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n    gl_PointSize = 1.0;\n}\n");
        size_t len = strlen(buf);
        if (len + (size_t)n + 1 > cap) why = "no room";
        else memcpy(buf + len, tail, (size_t)n + 1);
    }
    if (why) {
        fprintf(stderr, "  [glfb] compute-tf: shader %u NOT rewritten (%s); it will fail to compile\n", cs->name, why);
        return 0;
    }
    fprintf(stderr, "  [glfb] compute-tf: shader %u is a kernel reading %s (binding %d, %d bytes each) and writing %s "
                    "(binding %d, %d bytes each), %d per group — now a vertex shader\n", cs->name, in, cs->in_binding,
            cs->in_words * 4, out, cs->out_binding, cs->out_words * 4, cs->group);
    return 1;
}

// The other kernel shape: SEVERAL read-only buffers, read at any element, and one
// written buffer, written only at the invocation's own element. Unity's
// Internal-Skinning kernels are that — each vertex reads its own entry of
// inVertices and inSkin, then the bone matrices of inMatrices its weights name,
// and writes its own entry of outVertices — and a Unity title described ES 3.2
// skins every skinned mesh with them. Refused, its hands and characters were
// never drawn (Arizona Sunshine, x2190353671014400, 2026-10-31).
//
// The reads are left for klfb_rewrite_ro_ssbo, which runs on this source later
// and turns every read-only block into a texture read (KL_GLFB_SSBO_TEX); here
// only the written block becomes transform feedback — one `flat out uint` per
// word of its element, captured interleaved, so the element needs no rounding
// to 16 bytes — and the invocation becomes the vertex, as above. A read-only
// block read at the invocation's own element, if there is one, is recorded:
// the dispatch runs no more invocations than it has elements. Only with
// KL_GLFB_SSBO_TEX on, since nothing else serves the reads.
static int klfb_cs_rewrite_sb(char *buf, size_t cap, klfb_cs_obj *cs) {
    char out[64] = "", t[256], h[192];
    const char *why = NULL;
    char *p, *blk = NULL;
    int words = 0;
    for (p = buf; (p = strstr(p, ") buffer ")) != NULL; p += 9) {
        if (out[0]) { why = "more than one written buffer"; break; }
        sscanf(p + 9, "%63[A-Za-z0-9_]", out);
    }
    if (!why && !out[0]) why = "no written buffer block";
    // every storage block, before any is rewritten away (klfb_cs_reflect)
    cs->sbn = 0;
    for (p = buf; !why && (p = strstr(p, "layout(std430, binding = ")) != NULL; p++) {
        int bb = -1, n = 0, ww = 0;
        char nm[48];
        if (sscanf(p, "layout(std430, binding = %d) readonly buffer %47[A-Za-z0-9_]%n", &bb, nm, &n) < 2 &&
            sscanf(p, "layout(std430, binding = %d) buffer %47[A-Za-z0-9_]%n", &bb, nm, &n) < 2) continue;
        if (cs->sbn == KLFB_CS_BINDINGS) { why = "too many storage blocks"; break; }
        snprintf(t, sizeof t, "struct %s_type {", nm);
        char *st = strstr(buf, t);
        if (st) sscanf(st + strlen(t), " uint[%d] value;", &ww);
        snprintf(cs->sbb[cs->sbn].name, sizeof cs->sbb[0].name, "%s", nm);
        cs->sbb[cs->sbn].binding = bb;
        cs->sbb[cs->sbn].words = ww;
        cs->sbn++;
    }
    if (!why && (strstr(buf, "gl_LocalInvocation") || strstr(buf, "gl_WorkGroupID") ||
                 strstr(buf, "gl_NumWorkGroups") || strstr(buf, "barrier(") || strstr(buf, "shared ") ||
                 strstr(buf, "imageStore") || strstr(buf, "imageLoad") || strstr(buf, "atomic")))
        why = "uses something a vertex shader has not";
    if (!why) {
        snprintf(t, sizeof t, ") buffer %s {", out);
        for (blk = strstr(buf, t); blk && blk > buf && blk[-1] != '\n'; blk--) {}
        if (!blk || sscanf(blk, " layout(std430, binding = %d", &cs->out_binding) != 1) why = "written block binding";
        else if (cs->out_binding < 0 || cs->out_binding >= KLFB_CS_BINDINGS) why = "binding point out of range";
    }
    if (!why) {
        snprintf(t, sizeof t, "struct %s_type {", out);
        p = strstr(buf, t);
        if (!p || sscanf(p + strlen(t), " uint[%d] value;", &words) != 1 || words < 1 || words > 32)
            why = "written element is not a uint[1..32]";
    }
    if (!why) {
        int y = 0, z = 0;
        p = strstr(buf, "layout(local_size_x = ");
        if (!p || sscanf(p, "layout(local_size_x = %d, local_size_y = %d, local_size_z = %d) in;",
                         &cs->group, &y, &z) != 3 || y != 1 || z != 1 || cs->group < 1)
            why = "work group is not N x 1 x 1";
    }
    if (!why) {
        // every use of the written block but its declaration is a write of the
        // invocation's own element
        int all = 0, own = 0;
        snprintf(t, sizeof t, "%s_buf[", out);
        snprintf(h, sizeof h, "%s_buf[gl_GlobalInvocationID.x].value[", out);
        for (p = buf; (p = strstr(p, t)) != NULL; p++) all++;
        for (p = buf; (p = strstr(p, h)) != NULL; p++) own++;
        if (!own || all != own + 1) why = "written buffer indexed by something else";
    }
    cs->in_binding = -1; cs->in_words = 0;
    for (p = buf; !why && (p = strstr(p, ") readonly buffer ")) != NULL; p += 18) {
        char nm[64], *b, *st;
        int bb = -1, ww = 0;
        if (sscanf(p + 18, "%63[A-Za-z0-9_]", nm) != 1) continue;
        snprintf(h, sizeof h, "%s_buf[gl_GlobalInvocationID.x]", nm);
        if (!strstr(buf, h)) continue;
        for (b = p; b > buf && b[-1] != '\n'; b--) {}
        snprintf(t, sizeof t, "struct %s_type {", nm);
        st = strstr(buf, t);
        if (sscanf(b, " layout(std430, binding = %d", &bb) == 1 && st &&
            sscanf(st + strlen(t), " uint[%d] value;", &ww) == 1 &&
            bb >= 0 && bb < KLFB_CS_BINDINGS && ww > 0) { cs->in_binding = bb; cs->in_words = ww; break; }
    }
    if (!why) {
        char decl[1024];
        int cut = 0;
        snprintf(t, sizeof t, "struct %s_type {", out);
        cut += klfb_cs_cut(strstr(buf, t), "};\n");
        snprintf(t, sizeof t, ") buffer %s {", out);
        for (p = strstr(buf, t); p && p > buf && p[-1] != '\n'; p--) {}
        cut += klfb_cs_cut(p, "};\n");
        char *at = p;
        cut += klfb_cs_cut(strstr(buf, "layout(local_size_x = "), "\n");
        if (cut != 3) why = "declarations not where expected";
        int n = why ? 0 : snprintf(decl, sizeof decl, "uint kl_out[%d];\nuvec3 kl_gid;\n", words);
        for (int i = 0; !why && i < words; i++)
            n += snprintf(decl + n, sizeof decl - (size_t)n, "flat out uint kl_o%d;\n", i);
        if (!why && !klfb_splice(buf, cap, at, 0, decl)) why = "no room";
    }
    if (!why) {
        snprintf(h, sizeof h, "%s_buf[gl_GlobalInvocationID.x].value[", out);
        if (klfb_cs_reindex(buf, cap, h, "kl_out[int(", ")]") <= 0) why = "no storage write to rewrite";
        snprintf(t, sizeof t, "%s_buf", out);
        if (!why && strstr(buf, t)) why = "written buffer used some other way";
    }
    if (!why) {
        for (p = buf; (p = strstr(p, "gl_GlobalInvocationID")) != NULL; )
            klfb_splice(buf, cap, p, strlen("gl_GlobalInvocationID"), "kl_gid");
        p = strstr(buf, "void main()");
        if (!p) why = "no main";
        else klfb_splice(buf, cap, p, strlen("void main()"), "void kl_cs_main()");
    }
    if (!why) {
        char tail[2048];
        int n = snprintf(tail, sizeof tail,
            "\nvoid main() {\n"
            "    kl_gid = uvec3(uint(gl_VertexID), 0u, 0u);\n"
            "    for (int i = 0; i < %d; i++) kl_out[i] = 0u;\n"
            "    kl_cs_main();\n", words);
        for (int i = 0; i < words; i++)
            n += snprintf(tail + n, sizeof tail - (size_t)n, "    kl_o%d = kl_out[%d];\n", i, i);
        n += snprintf(tail + n, sizeof tail - (size_t)n,
                      "    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n    gl_PointSize = 1.0;\n}\n");
        if (strlen(buf) + (size_t)n + 1 > cap) why = "no room";
        else memcpy(buf + strlen(buf), tail, (size_t)n + 1);
    }
    if (why) {
        fprintf(stderr, "  [glfb] compute-tf: shader %u NOT rewritten as a texture-reading kernel (%s); "
                        "it will fail to compile\n", cs->name, why);
        return 0;
    }
    cs->out_words = words;
    cs->sb = 1;
    fprintf(stderr, "  [glfb] compute-tf: shader %u is a kernel writing %s (binding %d, %d bytes each), "
                    "%d per group, its storage reads served as textures — now a vertex shader\n",
            cs->name, out, cs->out_binding, words * 4, cs->group);
    return 1;
}

// glAttachShader: a program that gets one of our kernels gets a fragment stage
// and its capture list as well — a vertex shader alone does not link.
static void (*g_real_AttachShader)(uint32_t, uint32_t);
// The storage blocks of the graphics shaders klfb_rewrite_ro_ssbo turns into
// texture reads, by shader and then by program — the same reflection the
// kernels get (klfb_cs_reflect). Told no block, Unity binds no buffer for the
// draw, and the texture then reads whatever was left at that binding: in
// Arizona Sunshine the vertices the last skinning kernel wrote, under a zombie's
// head that flickered and showed through itself (2026-10-31).
#define KLFB_SB_REFL 512
typedef struct {
    uint32_t name;
    int n;
    struct { char name[48]; int binding, words; } b[KLFB_SB_MAXB];
} klfb_sb_refl;
static klfb_sb_refl g_sh_sb[KLFB_SB_REFL], g_pg_sb[KLFB_SB_REFL];
static unsigned g_sh_sb_next, g_pg_sb_next;
static klfb_sb_refl *klfb_sb_refl_get(klfb_sb_refl *tab, unsigned *next, uint32_t name, int make) {
    for (unsigned i = 0; i < KLFB_SB_REFL; i++) if (tab[i].name == name) return &tab[i];
    if (!make) return NULL;
    klfb_sb_refl *r = &tab[(*next)++ % KLFB_SB_REFL];     // the oldest goes
    memset(r, 0, sizeof *r);
    r->name = name;
    return r;
}
// At glShaderSource, before the rewrite: the read-only blocks as declared.
static void klfb_sb_refl_note(uint32_t shader, const char *buf) {
    klfb_sb_refl *r = NULL;
    for (const char *p = buf; (p = strstr(p, "layout(std430, binding = ")) != NULL; p++) {
        int bb = -1, ww = 0;
        char nm[48], t[96];
        if (sscanf(p, "layout(std430, binding = %d) readonly buffer %47[A-Za-z0-9_]", &bb, nm) != 2) continue;
        if (!r) { r = klfb_sb_refl_get(g_sh_sb, &g_sh_sb_next, shader, 1); r->n = 0; }
        if (r->n == KLFB_SB_MAXB) break;
        snprintf(t, sizeof t, "struct %s_type {", nm);
        const char *st = strstr(buf, t);
        if (st) sscanf(st + strlen(t), " uint[%d] value;", &ww);
        snprintf(r->b[r->n].name, sizeof r->b[0].name, "%s", nm);
        r->b[r->n].binding = bb; r->b[r->n].words = ww;
        r->n++;
    }
}
// At glAttachShader: the program has every block its shaders declared.
static void klfb_sb_refl_attach(uint32_t program, uint32_t shader) {
    klfb_sb_refl *sh = klfb_sb_refl_get(g_sh_sb, &g_sh_sb_next, shader, 0);
    if (!sh || !sh->n) return;
    klfb_sb_refl *pg = klfb_sb_refl_get(g_pg_sb, &g_pg_sb_next, program, 1);
    for (int i = 0; i < sh->n; i++) {
        int k = 0;
        while (k < pg->n && strcmp(pg->b[k].name, sh->b[i].name)) k++;
        if (k == pg->n && pg->n < KLFB_SB_MAXB) pg->b[pg->n++] = sh->b[i];
    }
}

static void klfb_AttachShader(uint32_t program, uint32_t shader) {
    if (g_real_AttachShader) g_real_AttachShader(program, shader);
    if (kl_glfb_ssbo_tex()) klfb_sb_refl_attach(program, shader);
    klfb_cs_obj *cs = klfb_cs_find(g_cs_shaders, shader);
    if (!cs || !cs->group) return;
    klfb_cs_obj *pr = klfb_cs_add(g_cs_progs, program);
    if (!pr) return;
    *pr = *cs;
    pr->name = program;
    static const char *const fsrc =
        "#version 300 es\nprecision mediump float;\nout vec4 kl_c;\nvoid main() { kl_c = vec4(0.0); }\n";
    void (*compile)(uint32_t) = asym("glCompileShader");
    void (*source)(uint32_t, int32_t, const char *const *, const int32_t *) = asym("glShaderSource");
    pr->fs = g_real_CreateShader ? g_real_CreateShader(0x8B30 /* FRAGMENT_SHADER */) : 0;
    if (pr->fs && source && compile && g_real_AttachShader) {
        source(pr->fs, 1, &fsrc, NULL);
        compile(pr->fs);
        g_real_AttachShader(program, pr->fs);
        if (pr->is_tf && pr->sb && pr->out_words > 0) {
            // one uint varying per word of the element (klfb_cs_rewrite_sb)
            void (*tfv)(uint32_t, int32_t, const char *const *, uint32_t) = asym("glTransformFeedbackVaryings");
            char names[32][8];
            const char *cap[32];
            int rows = pr->out_words < 32 ? pr->out_words : 32;
            for (int i = 0; i < rows; i++) { snprintf(names[i], sizeof names[i], "kl_o%d", i); cap[i] = names[i]; }
            if (tfv) tfv(program, rows, cap, 0x8C8C /* INTERLEAVED_ATTRIBS */);
        } else if (pr->is_tf && pr->out_words > 0) {
            void (*tfv)(uint32_t, int32_t, const char *const *, uint32_t) = asym("glTransformFeedbackVaryings");
            char names[14][8];
            const char *cap[14];
            int rows = pr->out_words / 4;
            for (int i = 0; i < rows && i < 14; i++) { snprintf(names[i], sizeof names[i], "kl_o%d", i); cap[i] = names[i]; }
            if (tfv) tfv(program, rows, cap, 0x8C8C /* INTERLEAVED_ATTRIBS */);
            void (*bindattr)(uint32_t, uint32_t, const char *) = asym("glBindAttribLocation");
            for (int i = 0; bindattr && i < pr->in_words / 4; i++) {
                char a[8];
                snprintf(a, sizeof a, "kl_a%d", i);
                bindattr(program, (uint32_t)i, a);
            }
        }
    }
}
static void klfb_cs_selftest(klfb_cs_obj *pr);
static void klfb_DispatchCompute(uint32_t gx, uint32_t gy, uint32_t gz);
// ...and after the link, whether it took.
static void klfb_cs_linked(uint32_t program) {
    klfb_cs_obj *pr = klfb_cs_find(g_cs_progs, program);
    if (!pr) return;
    void (*piv)(uint32_t, uint32_t, int32_t *) = asym("glGetProgramiv");
    int32_t ok = 0;
    if (piv) piv(program, 0x8B82 /* LINK_STATUS */, &ok);
    pr->ready = ok;
    if (!ok) {
        char log[1024]; log[0] = 0;
        void (*ilog)(uint32_t, int32_t, int32_t *, char *) = asym("glGetProgramInfoLog");
        if (ilog) ilog(program, sizeof log, NULL, log);
        fprintf(stderr, "  [glfb] compute program %u did NOT link: %s\n", program, log);
    } else {
        fprintf(stderr, "  [glfb] compute program %u linked (tf=%d)\n", program, pr->is_tf);
        if (pr->is_tf && kl_env_on("KL_GLFB_COMPUTE_TF_SELFTEST", 0)) klfb_cs_selftest(pr);
    }
}
// glGetProgramiv: ANGLE's answer, except the one question only a compute
// program is asked — its work group size, which Unity hands the game
// (ComputeShader.GetKernelThreadGroupSizes) and the game divides by.
static void (*g_real_GetProgramiv)(uint32_t, uint32_t, int32_t *);
static void klfb_GetProgramiv(uint32_t program, uint32_t pname, int32_t *params) {
    if (pname == 0x8267 /* COMPUTE_WORK_GROUP_SIZE */) {
        klfb_cs_obj *pr = klfb_cs_find(g_cs_progs, program);
        if (pr && params) {
            params[0] = pr->group_x ? pr->group_x : (pr->group ? pr->group : 1);
            params[1] = pr->group_y ? pr->group_y : 1;
            params[2] = pr->group_z ? pr->group_z : 1;
            return;
        }
    }
    if (g_real_GetProgramiv) g_real_GetProgramiv(program, pname, params);
}
static void klfb_cs_forget(klfb_cs_obj *tab, uint32_t name) {
    klfb_cs_obj *o = klfb_cs_find(tab, name);
    if (!o) return;
    if (o->fs && g_real_DeleteShader) g_real_DeleteShader(o->fs);
    memset(o, 0, sizeof *o);
}

// The shader-storage binding points are ours: ANGLE has no such target. The
// indexed ones are what a dispatch reads; the generic one is where the guest
// uploads, and those calls borrow GL_COPY_WRITE_BUFFER for their duration
// (borrow, not take: the guest caches what it bound there).
static void (*g_real_BindBufferBase)(uint32_t, uint32_t, uint32_t);
static void (*g_real_BindBufferRange)(uint32_t, uint32_t, uint32_t, intptr_t, intptr_t);
static void klfb_BindBufferBase(uint32_t target, uint32_t index, uint32_t buffer) {
    if (target == KLFB_GL_SSBO && (kl_glfb_compute_tf() || kl_glfb_ssbo_tex() || kl_glfb_compute_stub())) {
        g_cs_generic = buffer;
        if (kl_glfb_ssbo_tex()) klfb_tbm_want(buffer);
        if (index < KLFB_CS_BINDINGS) { g_cs_ssbo[index].buf = buffer; g_cs_ssbo[index].off = 0; g_cs_ssbo[index].size = -1; }
        return;
    }
    if (g_real_BindBufferBase) g_real_BindBufferBase(target, index, buffer);
}
static void klfb_BindBufferRange(uint32_t target, uint32_t index, uint32_t buffer, intptr_t off, intptr_t size) {
    if (target == KLFB_GL_SSBO && (kl_glfb_compute_tf() || kl_glfb_ssbo_tex() || kl_glfb_compute_stub())) {
        g_cs_generic = buffer;
        if (kl_glfb_ssbo_tex()) klfb_tbm_want(buffer);
        if (index < KLFB_CS_BINDINGS) { g_cs_ssbo[index].buf = buffer; g_cs_ssbo[index].off = off; g_cs_ssbo[index].size = size; }
        return;
    }
    if (g_real_BindBufferRange) g_real_BindBufferRange(target, index, buffer, off, size);
}
#define KLFB_GL_COPY_WRITE 0x8F37
// Bind `buffer` to GL_COPY_WRITE_BUFFER and return what was there.
static uint32_t klfb_cs_borrow(uint32_t buffer) {
    int32_t was = 0;
    if (!g_real_BindBuffer) g_real_BindBuffer = asym("glBindBuffer");
    if (a_glGetIntegerv) a_glGetIntegerv(0x8F37 /* COPY_WRITE_BUFFER_BINDING */, &was);
    if (g_real_BindBuffer) g_real_BindBuffer(KLFB_GL_COPY_WRITE, buffer);
    return (uint32_t)was;
}
static void klfb_cs_giveback(uint32_t was) {
    if (g_real_BindBuffer) g_real_BindBuffer(KLFB_GL_COPY_WRITE, was);
}

// The dispatch of a klfb_cs_rewrite_sb kernel: the same GL_POINTS draw under
// RASTERIZER_DISCARD, with no vertex attributes — its storage reads are bound
// as textures by klfb_ssbo_apply, as for any draw — captured into the written
// buffer. As many invocations as were asked for, the written range holds and
// the per-invocation read-only buffer (if the kernel has one) has elements; a
// kernel's own bounds check (Unity's `id >= g_VertCount`) covers the rest, its
// words left zero.
static void klfb_ssbo_apply(void);
static void klfb_tb_gpu_refresh(uint32_t buffer, intptr_t written);
static void klfb_tbm_drop(uint32_t buf);
static void klfb_cs_dispatch_sb(klfb_cs_obj *pr, int32_t cur, uint32_t gx) {
    static int said;
    static void (*bufpar)(uint32_t, uint32_t, int32_t *);
    static void (*geti)(uint32_t, uint32_t, int32_t *);
    static void (*begin)(uint32_t), (*end)(void), (*draw)(uint32_t, int32_t, int32_t);
    static void (*enable)(uint32_t), (*disable)(uint32_t), (*bindvao)(uint32_t), (*genvao)(int32_t, uint32_t *);
    static void (*bindtf)(uint32_t, uint32_t);
    static uint8_t (*isenabled)(uint32_t);
    static uint32_t (*geterr)(void);
    static void *(*curctx)(void);
    static struct { void *ctx; uint32_t vao; } vaos[4];
    if (!draw) {
        bufpar = asym("glGetBufferParameteriv"); geti = asym("glGetIntegeri_v");
        begin = asym("glBeginTransformFeedback"); end = asym("glEndTransformFeedback");
        enable = asym("glEnable"); disable = asym("glDisable"); isenabled = asym("glIsEnabled");
        bindvao = asym("glBindVertexArray"); genvao = asym("glGenVertexArrays");
        bindtf = asym("glBindTransformFeedback"); geterr = asym("glGetError");
        curctx = asym("eglGetCurrentContext");
        if (!g_real_BindBufferRange) g_real_BindBufferRange = asym("glBindBufferRange");
        if (!g_real_BindBufferBase) g_real_BindBufferBase = asym("glBindBufferBase");
        if (!g_real_BindBuffer) g_real_BindBuffer = asym("glBindBuffer");
        draw = asym("glDrawArrays");
    }
    uint32_t out = g_cs_ssbo[pr->out_binding].buf;
    intptr_t out_off = g_cs_ssbo[pr->out_binding].off, out_range = g_cs_ssbo[pr->out_binding].size;
    if (!out || !draw || !begin || !end || !bufpar || !geti || !bindvao || !genvao ||
        !g_real_BindBufferRange || !g_real_BindBufferBase || !g_real_BindBuffer || !a_glGetIntegerv) {
        if (said++ < 4)
            fprintf(stderr, "  [glfb] compute-tf: dispatch of program %d without its written buffer — nothing done\n", cur);
        return;
    }
    intptr_t out_el = (intptr_t)pr->out_words * 4;
    int32_t out_size = 0, in_size = 0;
    uint32_t was = klfb_cs_borrow(out);
    bufpar(KLFB_GL_COPY_WRITE, 0x8764 /* BUFFER_SIZE */, &out_size);
    uint32_t in = pr->in_binding >= 0 ? g_cs_ssbo[pr->in_binding].buf : 0;
    if (in) {
        g_real_BindBuffer(KLFB_GL_COPY_WRITE, in);
        bufpar(KLFB_GL_COPY_WRITE, 0x8764, &in_size);
    }
    klfb_cs_giveback(was);
    intptr_t count = (intptr_t)gx * pr->group;
    intptr_t room = out_range > 0 ? out_range : out_size - out_off;
    if (room / out_el < count) count = room / out_el;
    if (in && pr->in_words > 0) {
        intptr_t in_off = g_cs_ssbo[pr->in_binding].off, in_range = g_cs_ssbo[pr->in_binding].size;
        intptr_t have = (in_range > 0 ? in_range : in_size - in_off) / ((intptr_t)pr->in_words * 4);
        if (have < count) count = have;
    }
    if (count <= 0) return;
    int32_t tfb = 0, tfb_i = 0, tfb_start = 0, tfb_size = 0, vao = 0, tfo = 0;
    a_glGetIntegerv(0x8E25 /* TRANSFORM_FEEDBACK_BINDING */, &tfo);
    if (tfo && bindtf) bindtf(0x8E22 /* TRANSFORM_FEEDBACK */, 0);
    a_glGetIntegerv(0x8C8F /* TRANSFORM_FEEDBACK_BUFFER_BINDING */, &tfb);
    geti(0x8C8F, 0, &tfb_i);
    geti(0x8C84 /* TRANSFORM_FEEDBACK_BUFFER_START */, 0, &tfb_start);
    geti(0x8C85 /* TRANSFORM_FEEDBACK_BUFFER_SIZE */, 0, &tfb_size);
    a_glGetIntegerv(0x85B5 /* VERTEX_ARRAY_BINDING */, &vao);
    int discard = isenabled ? isenabled(0x8C89 /* RASTERIZER_DISCARD */) : 0;
    // an empty vertex array of our own, one per context
    void *ctx = curctx ? curctx() : NULL;
    int vi = 0;
    while (vi < 3 && vaos[vi].vao && vaos[vi].ctx != ctx) vi++;
    if (vaos[vi].ctx != ctx || !vaos[vi].vao) { vaos[vi].ctx = ctx; genvao(1, &vaos[vi].vao); }
    bindvao(vaos[vi].vao);
    klfb_ssbo_apply();
    if (!discard && enable) enable(0x8C89);
    if (geterr) while (geterr()) {}
    g_real_BindBufferRange(0x8C8E /* TRANSFORM_FEEDBACK_BUFFER */, 0, out, out_off, count * out_el);
    begin(0x0000 /* POINTS */);
    draw(0x0000, 0, (int32_t)count);
    end();
    uint32_t err = geterr ? geterr() : 0;
    if (!discard && disable) disable(0x8C89);
    bindvao((uint32_t)vao);
    if (tfb_i && tfb_size) g_real_BindBufferRange(0x8C8E, 0, (uint32_t)tfb_i, tfb_start, tfb_size);
    else g_real_BindBufferBase(0x8C8E, 0, (uint32_t)tfb_i);
    g_real_BindBuffer(0x8C8E, (uint32_t)tfb);
    if (tfo && bindtf) bindtf(0x8E22, (uint32_t)tfo);
    // What the CPU last saw of the written buffer is no longer its contents:
    // a texture standing in for it is refreshed from the GPU, and the kept
    // bytes are dropped so no later refresh brings the old ones back.
    klfb_tbm_drop(out);
    klfb_tb_gpu_refresh(out, out_off + count * out_el);
    static unsigned long runs;
    runs++;
    if (err || runs == 1 || runs % 3000 == 0)
        fprintf(stderr, "  [glfb] compute-tf: texture-reading dispatch #%lu, program %d, %ld invocation(s) "
                        "-> buffer %u%s", runs, cur, (long)count, out, err ? "" : "\n");
    if (err) fprintf(stderr, " — GL error 0x%x\n", err);
}

// glDispatchCompute: one GL_POINTS draw, captured into the kernel's output
// buffer. Never forwarded — on ES 3.0 it is only an error.
static void klfb_DispatchCompute(uint32_t gx, uint32_t gy, uint32_t gz) {
    static int said;
    int32_t cur = 0;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &cur);
    klfb_cs_obj *pr = klfb_cs_find(g_cs_progs, (uint32_t)cur);
    if (!pr || !pr->ready || !pr->is_tf || gy != 1 || gz != 1) {
        if (said++ < 8)
            fprintf(stderr, "  [glfb] glDispatchCompute(%u,%u,%u): program %d (tf=%d) — nothing done\n",
                    gx, gy, gz, cur, pr ? pr->is_tf : 0);
        return;
    }
    if (pr->sb) { klfb_cs_dispatch_sb(pr, cur, gx); return; }
    uint32_t in = g_cs_ssbo[pr->in_binding].buf, out = g_cs_ssbo[pr->out_binding].buf;
    intptr_t in_off = g_cs_ssbo[pr->in_binding].off, out_off = g_cs_ssbo[pr->out_binding].off;
    static void (*bufpar)(uint32_t, uint32_t, int32_t *);
    static void (*geti)(uint32_t, uint32_t, int32_t *);
    static void (*begin)(uint32_t), (*end)(void), (*draw)(uint32_t, int32_t, int32_t);
    static void (*enable)(uint32_t), (*disable)(uint32_t), (*bindvao)(uint32_t), (*genvao)(int32_t, uint32_t *);
    static void (*bindtf)(uint32_t, uint32_t), (*enattr)(uint32_t);
    static void (*attri)(uint32_t, int32_t, uint32_t, int32_t, const void *);
    static uint8_t (*isenabled)(uint32_t);
    static uint32_t (*geterr)(void);
    static void *(*curctx)(void);
    if (!draw) {
        bufpar = asym("glGetBufferParameteriv"); geti = asym("glGetIntegeri_v");
        begin = asym("glBeginTransformFeedback"); end = asym("glEndTransformFeedback");
        enable = asym("glEnable"); disable = asym("glDisable"); isenabled = asym("glIsEnabled");
        bindvao = asym("glBindVertexArray"); genvao = asym("glGenVertexArrays");
        enattr = asym("glEnableVertexAttribArray"); attri = asym("glVertexAttribIPointer");
        bindtf = asym("glBindTransformFeedback"); geterr = asym("glGetError");
        curctx = asym("eglGetCurrentContext");
        if (!g_real_BindBufferRange) g_real_BindBufferRange = asym("glBindBufferRange");
        if (!g_real_BindBufferBase) g_real_BindBufferBase = asym("glBindBufferBase");
        if (!g_real_BindBuffer) g_real_BindBuffer = asym("glBindBuffer");
        draw = asym("glDrawArrays");
    }
    if (!in || !out || !draw || !begin || !end || !bufpar || !geti || !bindvao || !genvao || !enattr || !attri ||
        !g_real_BindBufferRange || !g_real_BindBufferBase || !g_real_BindBuffer || !a_glGetIntegerv) {
        if (said++ < 4)
            fprintf(stderr, "  [glfb] compute-tf: dispatch without its buffers (in %u, out %u) — nothing done\n", in, out);
        return;
    }
    // as many invocations as were asked for and both buffers hold
    intptr_t in_el = (intptr_t)pr->in_words * 4, out_el = (intptr_t)pr->out_words * 4;
    int32_t in_size = 0, out_size = 0;
    uint32_t was = klfb_cs_borrow(in);
    bufpar(KLFB_GL_COPY_WRITE, 0x8764 /* BUFFER_SIZE */, &in_size);
    g_real_BindBuffer(KLFB_GL_COPY_WRITE, out);
    bufpar(KLFB_GL_COPY_WRITE, 0x8764, &out_size);
    klfb_cs_giveback(was);
    intptr_t count = (intptr_t)gx * pr->group;
    if ((in_size - in_off) / in_el < count) count = (in_size - in_off) / in_el;
    if ((out_size - out_off) / out_el < count) count = (out_size - out_off) / out_el;
    if (count < (intptr_t)gx * pr->group && said++ < 4)
        fprintf(stderr, "  [glfb] compute-tf: %u groups of %d asked, buffers hold %ld elements (in %d bytes, out %d bytes)\n",
                gx, pr->group, (long)count, in_size, out_size);
    if (count <= 0) return;
    // what we touch, as the guest left it
    int32_t arr = 0, tfb = 0, tfb_i = 0, tfb_start = 0, tfb_size = 0, vao = 0, tfo = 0;
    a_glGetIntegerv(0x8894 /* ARRAY_BUFFER_BINDING */, &arr);
    a_glGetIntegerv(0x8E25 /* TRANSFORM_FEEDBACK_BINDING */, &tfo);
    if (tfo && bindtf) bindtf(0x8E22 /* TRANSFORM_FEEDBACK */, 0);
    a_glGetIntegerv(0x8C8F /* TRANSFORM_FEEDBACK_BUFFER_BINDING */, &tfb);
    geti(0x8C8F, 0, &tfb_i);
    geti(0x8C84 /* TRANSFORM_FEEDBACK_BUFFER_START */, 0, &tfb_start);
    geti(0x8C85 /* TRANSFORM_FEEDBACK_BUFFER_SIZE */, 0, &tfb_size);
    a_glGetIntegerv(0x85B5 /* VERTEX_ARRAY_BINDING */, &vao);
    int discard = isenabled ? isenabled(0x8C89 /* RASTERIZER_DISCARD */) : 0;
    // our own vertex array, one per context: element i of the input is vertex i
    void *ctx = curctx ? curctx() : NULL;
    int vi = 0;
    while (vi < 3 && g_cs_vaos[vi].vao && g_cs_vaos[vi].ctx != ctx) vi++;
    if (g_cs_vaos[vi].ctx != ctx || !g_cs_vaos[vi].vao) {
        g_cs_vaos[vi].ctx = ctx;
        genvao(1, &g_cs_vaos[vi].vao);
    }
    bindvao(g_cs_vaos[vi].vao);
    g_real_BindBuffer(0x8892 /* ARRAY_BUFFER */, in);
    for (int i = 0; i < pr->in_words / 4; i++) {
        enattr((uint32_t)i);
        attri((uint32_t)i, 4, 0x1405 /* UNSIGNED_INT */, (int32_t)in_el, (const void *)(in_off + i * 16));
    }
    if (!discard && enable) enable(0x8C89);
    if (geterr) while (geterr()) {}
    g_real_BindBufferRange(0x8C8E /* TRANSFORM_FEEDBACK_BUFFER */, 0, out, out_off, count * out_el);
    begin(0x0000 /* POINTS */);
    draw(0x0000, 0, (int32_t)count);
    end();
    uint32_t err = geterr ? geterr() : 0;
    if (!discard && disable) disable(0x8C89);
    g_real_BindBuffer(0x8892, (uint32_t)arr);
    bindvao((uint32_t)vao);
    if (tfb_i && tfb_size) g_real_BindBufferRange(0x8C8E, 0, (uint32_t)tfb_i, tfb_start, tfb_size);
    else g_real_BindBufferBase(0x8C8E, 0, (uint32_t)tfb_i);
    g_real_BindBuffer(0x8C8E, (uint32_t)tfb);
    if (tfo && bindtf) bindtf(0x8E22, (uint32_t)tfo);
    static unsigned long runs;
    runs++;
    static int log_every = -1;
    if (log_every < 0) log_every = kl_env_on("KL_GLFB_COMPUTE_TF_LOG", 0);
    if (err || runs == 1 || (log_every && runs % 300 == 0))
        fprintf(stderr, "  [glfb] compute-tf: dispatch #%lu, program %d, %ld invocation(s), buffer %u -> %u%s",
                runs, cur, (long)count, in, out, err ? "" : "\n");
    if (err) fprintf(stderr, " — GL error 0x%x\n", err);
}
// KL_GLFB_COMPUTE_TF_SELFTEST=1: run a freshly linked kernel once on made-up
// input and print what it wrote — the dispatch above, start to end, on a
// screen that has no light to dispatch for (the Mac run ends at the language
// screen). The input is Beat Saber's: 144 bytes a light, its transform at byte
// 80; three lights 5 m ahead of a camera at the origin, the fourth element
// left zero. What comes back is not checked against anything — the kernel is
// the game's — only shown: four vertices a light, or nothing arrived.
static void klfb_cs_selftest(klfb_cs_obj *pr) {
    void (*gen)(int32_t, uint32_t *) = asym("glGenBuffers");
    void (*del)(int32_t, const uint32_t *) = asym("glDeleteBuffers");
    void (*data)(uint32_t, intptr_t, const void *, uint32_t) = asym("glBufferData");
    void *(*map)(uint32_t, intptr_t, intptr_t, uint32_t) = asym("glMapBufferRange");
    uint8_t (*unmap)(uint32_t) = asym("glUnmapBuffer");
    void (*use)(uint32_t) = asym("glUseProgram");
    void (*geti)(uint32_t, uint32_t, int32_t *) = asym("glGetIntegeri_v");
    void (*blockiv)(uint32_t, uint32_t, uint32_t, int32_t *) = asym("glGetActiveUniformBlockiv");
    uint32_t (*bidx)(uint32_t, const char *) = asym("glGetUniformBlockIndex");
    if (!g_real_BindBufferBase) g_real_BindBufferBase = asym("glBindBufferBase");
    if (!g_real_BindBufferRange) g_real_BindBufferRange = asym("glBindBufferRange");
    if (!g_real_BindBuffer) g_real_BindBuffer = asym("glBindBuffer");
    if (!gen || !del || !data || !map || !unmap || !use || !geti || !blockiv || !bidx || !a_glGetIntegerv ||
        !g_real_BindBufferBase || !g_real_BindBufferRange || !g_real_BindBuffer || pr->in_words != 36) return;
    enum { N = 4 };
    float in[N][36], globals[40];
    memset(in, 0, sizeof in); memset(globals, 0, sizeof globals);
    for (int l = 0; l < 3; l++) {
        for (int w = 0; w < 20; w++) in[l][w] = 1.0f;                 // colour, sizes, multipliers
        in[l][10] = 0.5f;                                             // byte 40: where along its length the light is centred
        in[l][20] = in[l][25] = in[l][30] = in[l][35] = 1.0f;         // byte 80: identity...
        in[l][32] = (float)(l - 1) * 2.0f; in[l][34] = 5.0f;          // ...moved to x = -2, 0, 2 and z = 5
    }
    // CGlobals, std140: uint count; mat4 view at 16; mat4 projection at 80; float width at 144
    uint32_t count = 3;
    memcpy(&globals[0], &count, 4);
    globals[4] = 1.0f; globals[9] = 1.0f; globals[14] = -1.0f; globals[19] = 1.0f;      // view: looking down +z
    globals[20] = 1.0f; globals[25] = 1.0f; globals[30] = -1.002f; globals[31] = -1.0f; globals[34] = -0.2f;
    globals[36] = 0.05f;
    uint32_t b[3] = {0, 0, 0}, bi = bidx(pr->name, "CGlobals");
    int32_t point = 0, cur = 0, ubo = 0, ubo_i = 0, ubo_start = 0, ubo_size = 0;
    if (bi == 0xFFFFFFFFu) { fprintf(stderr, "  [glfb] compute-tf selftest: no CGlobals block\n"); return; }
    blockiv(pr->name, bi, 0x8A3F /* UNIFORM_BLOCK_BINDING */, &point);
    a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &cur);
    a_glGetIntegerv(0x8A28 /* UNIFORM_BUFFER_BINDING */, &ubo);
    geti(0x8A28, (uint32_t)point, &ubo_i);
    geti(0x8A29, (uint32_t)point, &ubo_start);
    geti(0x8A2A, (uint32_t)point, &ubo_size);
    gen(3, b);
    uint32_t was = klfb_cs_borrow(b[0]);
    data(KLFB_GL_COPY_WRITE, sizeof in, in, 0x88E8 /* DYNAMIC_DRAW */);
    g_real_BindBuffer(KLFB_GL_COPY_WRITE, b[1]);
    data(KLFB_GL_COPY_WRITE, (intptr_t)N * pr->out_words * 4, NULL, 0x88E8);
    g_real_BindBuffer(KLFB_GL_COPY_WRITE, b[2]);
    data(KLFB_GL_COPY_WRITE, sizeof globals, globals, 0x88E8);
    g_real_BindBufferBase(0x8A11 /* UNIFORM_BUFFER */, (uint32_t)point, b[2]);
    typeof(g_cs_ssbo[0]) keep_in = g_cs_ssbo[pr->in_binding], keep_out = g_cs_ssbo[pr->out_binding];
    g_cs_ssbo[pr->in_binding].buf = b[0]; g_cs_ssbo[pr->in_binding].off = 0;
    g_cs_ssbo[pr->out_binding].buf = b[1]; g_cs_ssbo[pr->out_binding].off = 0;
    use(pr->name);
    klfb_DispatchCompute(1, 1, 1);
    use((uint32_t)cur);
    g_cs_ssbo[pr->in_binding] = keep_in; g_cs_ssbo[pr->out_binding] = keep_out;
    g_real_BindBuffer(KLFB_GL_COPY_WRITE, b[1]);
    const float *o = map(KLFB_GL_COPY_WRITE, 0, (intptr_t)N * pr->out_words * 4, 0x0001 /* MAP_READ_BIT */);
    if (!o) fprintf(stderr, "  [glfb] compute-tf selftest: output buffer could not be read back\n");
    for (int l = 0; o && l < N; l++)
        for (int v = 0; v < 4; v++) {
            const float *f = o + (l * pr->out_words) + v * (pr->out_words / 4);
            fprintf(stderr, "  [glfb] compute-tf selftest: light %d vertex %d:", l, v);
            for (int k = 0; k < pr->out_words / 4; k++) fprintf(stderr, " %.3g", (double)f[k]);
            fprintf(stderr, "\n");
        }
    if (o) unmap(KLFB_GL_COPY_WRITE);
    klfb_cs_giveback(was);
    if (ubo_i && ubo_size) g_real_BindBufferRange(0x8A11, (uint32_t)point, (uint32_t)ubo_i, ubo_start, ubo_size);
    else g_real_BindBufferBase(0x8A11, (uint32_t)point, (uint32_t)ubo_i);
    g_real_BindBuffer(0x8A11, (uint32_t)ubo);
    del(3, b);
}
static void klfb_MemoryBarrier(uint32_t barriers) {
    static void (*real)(uint32_t);
    if (kl_glfb_compute_tf()) return;        // the draw above is in order with everything after it
    if (kl_glfb_ssbo_tex()) return;          // no storage writes: ES 3.0 only errors on it
    if (!real) real = asym("glMemoryBarrier");
    if (real) real(barriers);
}

// The guest's own upload calls on GL_SHADER_STORAGE_BUFFER, re-aimed: the
// handler runs once more on GL_COPY_WRITE_BUFFER with the guest's buffer bound
// there (and the bookkeeping slot saying so), then both are put back.
#define KLFB_CS_VIA_COPY_WRITE(call)                                              \
    do {                                                                          \
        uint32_t was_ = klfb_cs_borrow(g_cs_generic);                             \
        uint32_t slot_ = g_tb_bound_buf[5];                                       \
        g_tb_bound_buf[5] = g_cs_generic;                                         \
        call;                                                                     \
        g_tb_bound_buf[5] = slot_;                                                \
        klfb_cs_giveback(was_);                                                   \
    } while (0)
#define KLFB_CS_GENERIC(target) ((target) == KLFB_GL_SSBO && (kl_glfb_compute_tf() || kl_glfb_ssbo_tex() || kl_glfb_compute_stub()))

// Subnormal float literals, written as the bits they stand for (KL_GLFB_DENORM).
//
// Unity's HLSLcc keeps integer temporaries in float registers and moves them
// with floatBitsToInt/intBitsToFloat. A small integer constant then appears as
// a float literal whose BITS are the integer: `u_xlat3.x = 2.80259693e-45;` is
// the int 2, the exit value of a loop counter that is compared by
// `floatBitsToInt(u_xlat9.x) >= 2`. Metal compiles ANGLE's shaders with fast
// math, which flushes subnormals to zero, so that constant arrives as 0, the
// counter is reset to 0 instead of 2 on the way out, and `while(true)` never
// ends: a GPU hang. Elixir's potion-glass shader (parallax loop, Potionary) hung
// the Mac's GPU hard enough to take the WindowServer down (2026-10-24) and left
// the headset's picture black: every later command buffer was discarded.
//
// Each such literal becomes intBitsToFloat(<bits> + kl_denorm_zero_), with
// kl_denorm_zero_ an int uniform nobody sets (so 0, as every uniform starts):
// a value the compiler cannot fold back into a constant to flush. A literal is
// rewritten only when it parses to a nonzero float below FLT_MIN; GLSL ES 1.00
// sources (no bit casts) are left alone. KL_GLFB_DENORM=0 turns it off.
static int klfb_rewrite_denorm_literals(char *buf, size_t *off, size_t cap) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_DENORM", 1);
    if (!on || strncmp(buf, "#version 3", 10) != 0) return 0;
    int n = 0;
    for (char *p = buf; *p; p++) {
        if (!(*p >= '0' && *p <= '9')) continue;
        if (p > buf && (isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '.')) {
            while (isalnum((unsigned char)p[1]) || p[1] == '_' || p[1] == '.') p++;
            continue;
        }
        char *end;
        float v = strtof(p, &end);
        // Only literals with an exponent can be this small.
        int has_e = 0;
        for (char *q = p; q < end; q++) if (*q == 'e' || *q == 'E') has_e = 1;
        if (!has_e || v == 0.0f || fabsf(v) >= FLT_MIN) { p = end - 1; continue; }
        int32_t bits;
        memcpy(&bits, &v, sizeof bits);
        char rep[64];
        int rl = snprintf(rep, sizeof rep, "intBitsToFloat(%d + kl_denorm_zero_)", bits);
        size_t ol = (size_t)(end - p);
        if (*off + (size_t)rl - ol + 1 >= cap) break;
        memmove(p + rl, end, strlen(end) + 1);
        memcpy(p, rep, (size_t)rl);
        *off += (size_t)rl - ol;
        p += rl - 1;
        n++;
    }
    if (!n) return 0;
    // The uniform: after the last #extension / precision line at the top, or
    // after #version.
    static const char decl[] = "uniform highp int kl_denorm_zero_;\n";
    char *at = strchr(buf, '\n');
    if (!at) return n;
    at++;
    for (char *q = at; *q; ) {
        if (strncmp(q, "#extension", 10) == 0 || strncmp(q, "precision ", 10) == 0 ||
            strncmp(q, "#ifdef GL_EXT", 13) == 0 || strncmp(q, "#endif", 6) == 0 ||
            *q == '\n') {
            char *nl = strchr(q, '\n');
            if (!nl) break;
            if (*q != '\n') at = nl + 1;
            q = nl + 1;
        } else break;
    }
    size_t dl = sizeof decl - 1;
    if (*off + dl + 1 >= cap) return n;
    memmove(at + dl, at, strlen(at) + 1);
    memcpy(at, decl, dl);
    *off += dl;
    return n;
}

static void klfb_ShaderSource(uint32_t shader, int32_t count,
                              const char *const *strings, const int32_t *lengths) {
    if (strings && count > 0) {
        // The buffer is built for the rewrite, NOT tied to the capture: the
        // capture table holds 1024 entries and a long run sources thousands,
        // so a shared buffer stops rewriting the moment it fills and the 320
        // es failures return with nothing reporting it.
        size_t total = 0;
        for (int32_t i = 0; i < count; i++)
            total += lengths && lengths[i] >= 0 ? (size_t)lengths[i]
                                                : (strings[i] ? strlen(strings[i]) : 0);
        // Slack for the one GROWING rewrite: the texture-buffer texelFetch
        // remap (see klfb_rewrite_texel_buffers) expands each fetch. 2x plus a
        // fixed pad covers the skinning shaders that use it many times over.
        size_t cap = total * 2 + 8192;
        char *buf = malloc(cap);
        if (buf) {
            size_t off = 0;
            for (int32_t i = 0; i < count; i++) {
                size_t len = lengths && lengths[i] >= 0 ? (size_t)lengths[i]
                                                        : (strings[i] ? strlen(strings[i]) : 0);
                if (strings[i] && len) { memcpy(buf + off, strings[i], len); off += len; }
            }
            buf[off] = 0;
            // KL_GLFB_DUMP_MATCH=<text>: print the guest's own source of every
            // shader that contains <text> to the log, as it arrived (at most 8,
            // each once). For reading one shader on the headset where there is
            // no shader dump: Synth Riders' rail shader, by a property only it
            // has (_UnspunPosition). A shader loaded from the guest's program
            // binary cache never comes through here — KL_GLFB_PROGRAM_BINARY=0
            // makes it compile from source.
            {
                static const char *match; static int init, dumped;
                if (!init) { init = 1; match = kl_env_str("KL_GLFB_DUMP_MATCH", NULL); }
                if (match && *match && dumped < 8 && strstr(buf, match)) {
                    dumped++;
                    fprintf(stderr, "  [glfb] ===== shader %u (type 0x%x) contains \"%s\", %zu bytes =====\n",
                            shader, klfb_shader_type(shader), match, off);
                    const char *ln = buf;
                    while (ln && *ln) {
                        const char *nl = strchr(ln, '\n');
                        int len = nl ? (int)(nl - ln) : (int)strlen(ln);
                        fprintf(stderr, "  [glsl %u] %.*s\n", shader, len, ln);
                        ln = nl ? nl + 1 : NULL;
                    }
                    fprintf(stderr, "  [glfb] ===== end of shader %u =====\n", shader);
                }
            }
            // Source for shader 0: glCreateShader gave the guest nothing (see
            // klfb_CreateShader), so this source, its compile and the program it
            // is attached to go nowhere, and the link then fails with "No
            // compiled shaders." and no source to show. The head of it says
            // what the guest was building. A few, once each.
            if (!shader) {
                static int said;
                if (said < 4) {
                    said++;
                    fprintf(stderr, "  [glfb] glShaderSource for shader 0 (%zu bytes) — "
                                    "its first lines:\n", off);
                    const char *ln = buf; int lines = 0;
                    while (ln && *ln && lines++ < 40) {
                        const char *nl = strchr(ln, '\n');
                        int len = nl ? (int)(nl - ln) : (int)strlen(ln);
                        fprintf(stderr, "  [glsl 0] %.*s\n", len > 160 ? 160 : len, ln);
                        ln = nl ? nl + 1 : NULL;
                    }
                }
            }
            // `#version` must be the FIRST thing in the source: ANGLE rejects a
            // shader that opens with a blank line ("#version directive must occur
            // on the first line"), where the Android drivers these guests were
            // written against skip leading whitespace. Doom3Quest's programs all
            // start "\n#version 300 es". Drop only whitespace, and only when the
            // directive is what follows it.
            int trimmed = 0;
            {
                size_t ws = 0;
                while (buf[ws] == ' ' || buf[ws] == '\t' || buf[ws] == '\r' || buf[ws] == '\n') ws++;
                if (ws && strncmp(buf + ws, "#version", 8) == 0) {
                    memmove(buf, buf + ws, off - ws + 1);
                    off -= ws;
                    trimmed = 1;
                }
            }
            // Either flag sends `buf` to ANGLE instead of the guest's strings.
            int dbg_modified = trimmed;
            {
                int dn = klfb_rewrite_denorm_literals(buf, &off, cap);
                if (dn) {
                    dbg_modified = 1;
                    static int said;
                    if (said++ < 4)
                        fprintf(stderr, "  [glfb] shader %u: %d subnormal float literal(s) written as "
                                        "their bits — fast math would flush them to 0 (an HLSLcc "
                                        "integer constant; KL_GLFB_DENORM=0 disables)\n", shader, dn);
                }
            }
            // KL_GLFB_VS_FINITE: a vertex whose clip position is not finite
            // (an exponent of all ones: infinity or NaN) is moved outside the
            // view. Synth Riders shows its pooled rail LineRenderers for two
            // seconds at song start with every point the same; the line
            // geometry of a zero-length segment is not finite, and where a
            // Quest's GPU drops such a triangle Apple's draws it — a straight
            // green line out to the screen's edge and dark wedges across the
            // scene (2026-10-15). The rail's fragment shader hides nothing, so
            // geometry is all there is. The guest's main becomes kvm_ (same
            // length, an in-place edit) and a new main checks gl_Position by
            // its bits, which no fast-math compile can fold away.
            {
                static int on = -1;
                if (on < 0) {
                    extern const char *kl_driver_target_name(void);
                    const char *t = kl_driver_target_name();
                    // Default off since the headset run of 2026-10-15: on, the line
                    // was still there — the rails' geometry is finite after all.
                    (void)t;
                    on = kl_env_on("KL_GLFB_VS_FINITE", 0);
                }
                char *m;
                if (on && klfb_shader_type(shader) == 0x8B31 /* VERTEX */ &&
                    strncmp(buf, "#version 3", 10) == 0 && (m = strstr(buf, "void main()")) &&
                    !strstr(m + 1, "void main()") && !strstr(buf, "kvm_")) {
                    static const char tail[] =
                        "\nvoid main()\n{\n    kvm_();\n"
                        "    uvec4 kl_e = floatBitsToUint(gl_Position) & uvec4(0x7f800000u);\n"
                        "    if (any(equal(kl_e, uvec4(0x7f800000u)))) gl_Position = vec4(0.0, 0.0, 2.0, 1.0);\n}\n";
                    if (off + sizeof tail < cap) {
                        memcpy(m, "void kvm_()", 11);
                        memcpy(buf + off, tail, sizeof tail);
                        off += sizeof tail - 1;
                        dbg_modified = 1;
                        static int said;
                        if (!said++) fprintf(stderr, "  [glfb] vertex shaders: non-finite positions are put "
                                                     "out of view (KL_GLFB_VS_FINITE)\n");
                    }
                }
            }
            // A compute kernel becomes the vertex shader its object already is
            // (see "compute kernels as transform feedback" above).
            {
                klfb_cs_obj *cs = klfb_cs_find(g_cs_shaders, shader);
                if (cs) {
                    char *orig = kl_glfb_compute_tf() && kl_glfb_ssbo_tex() ? strdup(buf) : NULL;
                    int rewritten = kl_glfb_compute_tf() && klfb_cs_rewrite(buf, cap, cs);
                    if (!rewritten && orig) {
                        // the first rewrite may have stopped halfway
                        snprintf(buf, cap, "%s", orig);
                        rewritten = klfb_cs_rewrite_sb(buf, cap, cs);
                    }
                    free(orig);
                    if (rewritten) {
                        cs->is_tf = 1;
                        cs->group_x = cs->group;
                        cs->group_y = 1;
                        cs->group_z = 1;
                    } else if (!kl_glfb_compute_stub()) {
                        cs->group = 0;
                    } else {
                        cs->is_tf = 0;
                        cs->group_x = 1; cs->group_y = 1; cs->group_z = 1;
                        char *p = strstr(buf, "layout(local_size_x = ");
                        if (p) sscanf(p, "layout(local_size_x = %d, local_size_y = %d, local_size_z = %d)",
                                      &cs->group_x, &cs->group_y, &cs->group_z);
                        else {
                            p = strstr(buf, "local_size_x = ");
                            if (p) sscanf(p, "local_size_x = %d", &cs->group_x);
                        }
                        cs->group = cs->group_x;
                        static const char dummy_vs[] =
                            "#version 300 es\n"
                            "void main()\n"
                            "{\n"
                            "    gl_Position = vec4(0.0);\n"
                            "}\n";
                        strcpy(buf, dummy_vs);
                        fprintf(stderr, "  [glfb] compute shader %u (group %d,%d,%d) stubbed as dummy vertex shader\n",
                                shader, cs->group_x, cs->group_y, cs->group_z);
                    }
                    off = strlen(buf);
                    dbg_modified = 1;
                }
            }
            // `invariant gl_Position;` for an engine that renders in passes that
            // must agree on depth to the last bit. id Tech 4 fills depth with one
            // program and then lights with others under GL_EQUAL. On Metal a
            // vertex function is compiled with fast-math unless it declares
            // invariance, and an Apple GPU runs the position computation of a
            // deferred-shaded tile separately from the binning one — so two
            // programs, or two evaluations of one, can differ in the last bit of
            // z. Seen as whole 32x32 TILES of the world going black (every
            // fragment in them failing EQUAL) plus a stipple where they nearly
            // agree. Declaring it makes ANGLE emit [[invariant]] and compile with
            // preserveInvariance. Vertex shaders only, and only for the guest
            // that needs it; KL_GLFB_INVARIANT=0/1 is the A/B.
            {
                static int inv = -1;
                if (inv < 0) {
                    extern const char *kl_driver_target_name(void);
                    const char *t = kl_driver_target_name();
                    inv = kl_env_on("KL_GLFB_INVARIANT", t && strcmp(t, "doom3quest") == 0);
                }
                if (inv && klfb_shader_type(shader) == 0x8B31 /* GL_VERTEX_SHADER */ &&
                    strncmp(buf, "#version 3", 10) == 0 && !strstr(buf, "invariant gl_Position")) {
                    static const char decl[] = "invariant gl_Position;\n";
                    // After the last #extension line, or else after #version:
                    // the declaration must follow every directive and precede
                    // any use of gl_Position.
                    char *at = buf, *q = buf;
                    while ((q = strstr(q, "#extension")) != NULL) {
                        char *nl = strchr(q, '\n');
                        if (!nl) break;
                        at = nl + 1; q = nl + 1;
                    }
                    if (at == buf) { char *nl = strchr(buf, '\n'); at = nl ? nl + 1 : NULL; }
                    if (at && off + sizeof decl < cap) {
                        memmove(at + sizeof decl - 1, at, off - (size_t)(at - buf) + 1);
                        memcpy(at, decl, sizeof decl - 1);
                        off += sizeof decl - 1;
                        dbg_modified = 1;
                    }
                }
            }
            // KL_HL2_DEBUG_AMBIENT=1: for the VertexLitGeneric model PS, force the output
            // to the ambient-cube constant so we can see whether pc[5..10] reach the METAL
            // shader (glGetUniformfv only proves ANGLE's CPU cache). Prop turns grey =>
            // the constant reaches Metal, so the black is the normal/weights; prop stays
            // black => the default-uniform-block upload never lands the ambient on Metal.
            {
                static int dbg_amb = -2;
                if (dbg_amb == -2) dbg_amb = kl_env_int("KL_HL2_DEBUG_AMBIENT", 0);
                if (dbg_amb && strstr(buf, "vertexlit_and_unlit_generic")) {
                    char *hit = strstr(buf, "gl_FragData[0] = r0;");
                    if (hit) {
                        // 1=ambient cube constant (bypass all lighting+albedo),
                        // 2=albedo texture only, 3=normal varying, 4=computed ambient*albedo(r0).
                        const char *rep =
                            dbg_amb == 2 ? "gl_FragData[0] = texture(sampler0, oT0.xy);" :
                            dbg_amb == 3 ? "gl_FragData[0] = vec4(oT4.xyz*0.5+0.5,1.0);" :
                            dbg_amb == 4 ? "gl_FragData[0] = vec4(r0.xyz,1.0);" :
                                           "gl_FragData[0] = vec4(pc[5].xyz+pc[8].xyz,1.0);";
                        size_t oldlen = strlen("gl_FragData[0] = r0;");
                        size_t newlen = strlen(rep);
                        size_t tail   = strlen(hit + oldlen);
                        if (off + (newlen - oldlen) + 1 < cap) {
                            memmove(hit + newlen, hit + oldlen, tail + 1);
                            memcpy(hit, rep, newlen);
                            off += newlen - oldlen;
                            dbg_modified = 1;
                            fprintf(stderr, "  [glfb] DEBUG_AMBIENT: forced model PS "
                                            "(shader %u) output to the ambient cube\n", shader);
                        }
                    }
                }
            }
            // Read-only storage buffers and bitfieldExtract (ES 3.1) for the title
            // that needs them; before klfb_rewrite_glsl, which drops the
            // `binding =` the storage rewrite names its texture after.
            if (kl_glfb_ssbo_tex()) {
                if (!klfb_cs_find(g_cs_shaders, shader)) klfb_sb_refl_note(shader, buf);
                int nsb = klfb_rewrite_ro_ssbo(buf, cap);
                int nbf = klfb_rewrite_bfe(buf, cap);
                if (nsb > 0 || nbf > 0) {
                    off = strlen(buf);
                    dbg_modified = 1;
                    static int said;
                    if (said < 4) { said++;
                        fprintf(stderr, "  [glfb] shader %u: %d read-only storage buffer(s) read as "
                                        "textures, %d bitfieldExtract call(s) replaced\n", shader, nsb, nbf); }
                }
            }
            if (klfb_probe_cube_array_on() && klfb_rewrite_probe_cube_array(buf, cap) > 0) {
                off = strlen(buf);
                dbg_modified = 1;
                static int said;
                if (said < 4) { said++;
                    fprintf(stderr, "  [glfb] shader %u: BLAST reflection probes read as ambient light "
                                    "(cube-map array and storage buffer, KL_GLFB_PROBE_CUBE_ARRAY)\n", shader); }
            }
            if (klfb_no_grading_lut_on() && klfb_rewrite_grading_lut(buf, cap) > 0) {
                off = strlen(buf);
                dbg_modified = 1;
                static int said;
                if (said < 4) { said++;
                    fprintf(stderr, "  [glfb] shader %u: _GradingLut read as the identity "
                                    "(no compute stage to fill it, KL_GLFB_NO_GRADING_LUT)\n", shader); }
            }
            char *rewritten = klfb_rewrite_glsl(buf, cap, shader);
            int stored = 0;
            if (g_fb_nshaders < KLFB_MAX_SHADERS) {
                pthread_mutex_lock(&g_compile_lock);
                g_fb_shaders[g_fb_nshaders].name = shader;
                g_fb_shaders[g_fb_nshaders].src  = buf;
                g_fb_nshaders++;
                pthread_mutex_unlock(&g_compile_lock);
                stored = 1;
            }
            if (rewritten || dbg_modified) {
                if (g_real_ShaderSource) {
                    const char *s = buf;
                    g_real_ShaderSource(shader, 1, &s, NULL);
                }
                if (!stored) free(buf);
                return;
            }
            if (!stored) free(buf);
        }
    }
    if (g_real_ShaderSource) g_real_ShaderSource(shader, count, strings, lengths);
}

static const char *klfb_shader_src(uint32_t shader) {
    // Latest wins: a name can be re-sourced, and shared contexts share the
    // name space, so the most recent source for a name is the text the next
    // compile actually saw.
    for (unsigned i = g_fb_nshaders; i-- > 0; )
        if (g_fb_shaders[i].name == shader) return g_fb_shaders[i].src;
    return NULL;
}

#define KLFB_GL_COMPILE_STATUS 0x8B81

// KL_GLFB_SLOW_MS=<ms> — name every shader compile, program link and draw that
// takes at least this long, and every few seconds what the shader work of
// that stretch added up to. A hitch "now and then" in a GL guest is, more
// often than anything else, ANGLE turning a program into Metal the first time
// it is linked or — for the pipeline state — the first time it is drawn with;
// this says so with the program's number and the time. 0 (the default) costs
// nothing: no clock is read.
static int klfb_slow_ms(void) {
    static int ms = -1;
    if (ms < 0) ms = kl_env_int("KL_GLFB_SLOW_MS", 0);
    return ms;
}
static uint64_t klfb_slow_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static struct {
    pthread_mutex_t mu;
    uint64_t since_ns;
    unsigned compiles, links, slow_draws;
    double compile_ms, link_ms, slow_draw_ms, worst_ms;
} g_slow = { .mu = PTHREAD_MUTEX_INITIALIZER };
// kind: 0 compile, 1 link, 2 draw
static void klfb_slow_note(int kind, uint32_t name, double ms, int verts) {
    int limit = klfb_slow_ms();
    pthread_mutex_lock(&g_slow.mu);
    uint64_t now = klfb_slow_now();
    if (!g_slow.since_ns) g_slow.since_ns = now;
    if (kind == 0) { g_slow.compiles++; g_slow.compile_ms += ms; }
    if (kind == 1) { g_slow.links++; g_slow.link_ms += ms; }
    if (kind == 2) { g_slow.slow_draws++; g_slow.slow_draw_ms += ms; }
    if (ms > g_slow.worst_ms) g_slow.worst_ms = ms;
    int report = (now - g_slow.since_ns) > 5000000000ull &&
                 (g_slow.compiles || g_slow.links || g_slow.slow_draws);
    unsigned c = g_slow.compiles, l = g_slow.links, d = g_slow.slow_draws;
    double cm = g_slow.compile_ms, lm = g_slow.link_ms, dm = g_slow.slow_draw_ms, w = g_slow.worst_ms;
    if (report) {
        g_slow.since_ns = now;
        g_slow.compiles = g_slow.links = g_slow.slow_draws = 0;
        g_slow.compile_ms = g_slow.link_ms = g_slow.slow_draw_ms = g_slow.worst_ms = 0;
    }
    pthread_mutex_unlock(&g_slow.mu);
    if (ms >= (double)limit) {
        if (kind == 2) {
            int32_t prog = 0;
            if (a_glGetIntegerv) a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &prog);
            fprintf(stderr, "  [glfb] slow draw: %.1f ms, program %d, %d vertices\n", ms, prog, verts);
        } else {
            fprintf(stderr, "  [glfb] slow %s: %.1f ms, %s %u\n", kind ? "link" : "compile", ms,
                    kind ? "program" : "shader", name);
        }
    }
    if (report)
        fprintf(stderr, "  [glfb] shader work since the last report: %u compiled (%.0f ms), %u linked "
                        "(%.0f ms), %u slow draws (%.0f ms); the longest single one %.1f ms\n",
                c, cm, l, lm, d, dm, w);
}
static __thread uint64_t g_draw_t0;

static void klfb_CompileShader(uint32_t shader) {
    if (!g_real_CompileShader) return;
    // The status/info-log readers are not thunks (the guest must get ANGLE's
    // real answers), so they resolve lazily here rather than through g_thunks,
    // whose entries hand their thunk pointer to the guest.
    if (!g_real_GetShaderiv) g_real_GetShaderiv = (void *)asym("glGetShaderiv");
    if (!g_real_GetShaderInfoLog)
        g_real_GetShaderInfoLog = (void *)asym("glGetShaderInfoLog");
    pthread_mutex_lock(&g_compile_lock);
    uint64_t slow_t0 = klfb_slow_ms() ? klfb_slow_now() : 0;
    g_real_CompileShader(shader);
    if (slow_t0) klfb_slow_note(0, shader, (double)(klfb_slow_now() - slow_t0) / 1.0e6, 0);
    int32_t ok = 1;
    if (g_real_GetShaderiv)
        g_real_GetShaderiv(shader, KLFB_GL_COMPILE_STATUS, &ok);
    // PROBE (hl2 multiview): the HL2Q3VR multiview generator builds a shader with
    // num_views/gl_ViewID_OVR and its own compile reports "unknown error", yet no
    // klfb compile FAILURE is logged. Log EVERY multiview shader that reaches here,
    // success or fail, with ANGLE's real status + info log + source, so we learn
    // whether it even goes through klfb and what ANGLE actually says.
    {
        const char *msrc = klfb_shader_src(shader);
        if (msrc && (strstr(msrc, "num_views") || strstr(msrc, "gl_ViewID_OVR"))) {
            static int said;
            if (said < 40) { said++;
                char ilog[2048]; ilog[0] = 0;
                if (g_real_GetShaderInfoLog)
                    g_real_GetShaderInfoLog(shader, sizeof ilog, NULL, ilog);
                fprintf(stderr, "  [glfb] MULTIVIEW shader %u reached klfb: ANGLE "
                        "COMPILE_STATUS=%d\n  [glfb] mv info log: %s\n"
                        "  [glfb] ---- mv source ----\n%s\n  [glfb] ---- end mv ----\n",
                        shader, ok, ilog, msrc);
            }
        }
    }
    if (!ok) {
        char log[4096];
        log[0] = 0;
        if (g_real_GetShaderInfoLog)
            g_real_GetShaderInfoLog(shader, sizeof log, NULL, log);
        fprintf(stderr, "  [glfb] glCompileShader(%u) FAILED — info log:\n%s\n",
                shader, log);
        const char *src = klfb_shader_src(shader);
        if (src)
            fprintf(stderr, "  [glfb] ---- source of failed shader %u ----\n%s\n"
                            "  [glfb] ---- end source ----\n", shader, src);
        const char *dir = kl_env_str("KL_DUMP_SHADERS", NULL);
        if (dir && src) {
            char path[600];
            snprintf(path, sizeof path, "%s/failed_shader_%u.glsl", dir, shader);
            FILE *f = fopen(path, "w");
            if (f) { fputs(src, f); fclose(f); }
        }
    }
    pthread_mutex_unlock(&g_compile_lock);
}

// The guest's own glGetShaderiv, passed through. Catan VR (Unity 2018.4) logs
// "GLSL compilation failed, no infolog provided" twelve times on the headset —
// libunity 0x781e2c reads COMPILE_STATUS, wants exactly 1, and finds an empty
// info log — while klfb_CompileShader saw every compile succeed. So when the
// guest is told anything but 1, say which shader, on which thread, whether that
// thread holds a context of ours, and what ANGLE answers when asked again here.
static void (*g_real_GetShaderiv_g)(uint32_t, uint32_t, int32_t *);
static void klfb_GetShaderiv(uint32_t shader, uint32_t pname, int32_t *out) {
    if (!g_real_GetShaderiv_g) { if (out) *out = 0; return; }
    g_real_GetShaderiv_g(shader, pname, out);
    if (pname != KLFB_GL_COMPILE_STATUS || !out || *out == 1) return;
    static int said;
    if (said >= 16) return;
    said++;
    pthread_once(&g_tls_once, klfb_tls_init);
    klfb_thread_gl *t = pthread_getspecific(g_tls_key);
    static void *(*r_cur)(void);
    if (!r_cur) r_cur = asym("eglGetCurrentContext");
    static uint32_t (*r_err)(void);
    if (!r_err) r_err = asym("glGetError");
    uint32_t err = r_err ? r_err() : 0;
    int32_t again = -1;
    pthread_mutex_lock(&g_compile_lock);
    g_real_GetShaderiv_g(shader, pname, &again);
    pthread_mutex_unlock(&g_compile_lock);
    uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
    const char *src = klfb_shader_src(shader);
    fprintf(stderr, "  [glfb] guest glGetShaderiv(%u, COMPILE_STATUS) -> %d (again under the "
                    "compile lock: %d), glGetError 0x%x, thread %llu, our context %p, ANGLE current %p\n",
            shader, *out, again, err, (unsigned long long)tid, t ? t->ctx : NULL,
            r_cur ? r_cur() : NULL);
    if (src && said <= 4)
        fprintf(stderr, "  [glfb] ---- source of shader %u ----\n%.6000s\n  [glfb] ---- end ----\n",
                shader, src);
}

// ----------------------------------------------------- the OTHER way to link
//
// glLinkProgram reports a failed link (below); glProgramBinary was passed
// straight through, so a program loaded from a cached binary that FAILS to link
// left no line anywhere. The guest then uses it, every glUseProgram answers
// GL_INVALID_OPERATION ("program has not been successfully linked" — the same
// error as "not a program object", so it reads like a name bug), and UE4 turns
// its RHI's Fatal into a ForceQuit storm and, eventually, a crash.
//
// That is exactly what a stale ProgramBinaryCache does on the first launch
// after an OS update; kl_app.c's shader_cache_guard is the cure, and this is
// the alarm that names it in one line.
//
// KL_GLFB_PROGRAM_BINARY=0 refuses the binary path outright: the program is
// left unlinked, which is precisely what the spec says a rejected binary looks
// like, so a guest that checks GL_LINK_STATUS falls back to its shader source.
static void klfb_vfx_mark_names(uint32_t program);   // KL_GLFB_SKIP_VFX, below
static void (*g_real_ProgramBinary)(uint32_t, uint32_t, const void *, int32_t);
static _Atomic unsigned g_pb_loads, g_pb_failed, g_pb_refused;
static void klfb_ProgramBinary(uint32_t program, uint32_t format,
                               const void *binary, int32_t length) {
    static int allow = -1;
    if (allow < 0) allow = kl_env_on("KL_GLFB_PROGRAM_BINARY", 1);
    g_pb_loads++;
    if (!allow) {
        if (g_pb_refused++ == 0)
            fprintf(stderr, "  [glfb] glProgramBinary REFUSED "
                            "(KL_GLFB_PROGRAM_BINARY=0) — every cached program "
                            "is left unlinked so the guest recompiles from "
                            "source\n");
        return;
    }
    if (!g_real_ProgramBinary) return;
    klfb_ssbo_forget(program);
    g_real_ProgramBinary(program, format, binary, length);
    klfb_vfx_mark_names(program);          // KL_GLFB_SKIP_VFX: cached programs have no source

    static void (*r_GetProgramiv)(uint32_t, uint32_t, int32_t *);
    static void (*r_GetProgramInfoLog)(uint32_t, int32_t, int32_t *, char *);
    if (!r_GetProgramiv) r_GetProgramiv = asym("glGetProgramiv");
    if (!r_GetProgramiv) return;
    int32_t ok = 1;
    r_GetProgramiv(program, 0x8B82 /* GL_LINK_STATUS */, &ok);
    if (ok) return;

    unsigned nth = ++g_pb_failed;
    if (nth <= 4)
        fprintf(stderr, "  [glfb] glProgramBinary(%u) REJECTED — format 0x%x, "
                        "%d bytes, program left UNLINKED (failure #%u). A "
                        "compiled-program cache does not survive a driver "
                        "change; see kl_app.c shader_cache_guard.\n",
                program, format, length, nth);
    else if (nth == 5)
        fprintf(stderr, "  [glfb] glProgramBinary: further rejections counted, "
                        "not printed\n");
    // The info log is the driver's own reason and is usually empty for a
    // format mismatch, but it costs nothing to ask on the first one.
    if (nth == 1) {
        if (!r_GetProgramInfoLog) r_GetProgramInfoLog = asym("glGetProgramInfoLog");
        char log[1024]; log[0] = 0;
        if (r_GetProgramInfoLog) r_GetProgramInfoLog(program, sizeof log, NULL, log);
        if (log[0]) fprintf(stderr, "  [glfb]   info log: %s\n", log);
    }
}

void kl_glfb_program_binary_census(FILE *f) {
    unsigned n = g_pb_loads, bad = g_pb_failed, ref = g_pb_refused;
    if (!n) return;
    fprintf(f, "  [glfb] glProgramBinary: %u load(s), %u REJECTED, %u refused "
               "by KL_GLFB_PROGRAM_BINARY=0\n", n, bad, ref);
    if (bad)
        fprintf(f, "  [glfb]   a rejected binary is an UNLINKED program — every "
                   "glUseProgram on it is GL_INVALID_OPERATION\n");
}

// KL_GLFB_SKIP_VFX: Synth Riders' VFX Graph effects. VFX Graph fills its
// particle buffers with compute kernels, and those fail here ("VFXCopyBuffer.
// compute: Kernel at index (3) is invalid" — ES 3.0 ANGLE has no compute), so
// its output shaders draw from buffers nothing initialised: the stray green
// lines across the play area and the dark green triangle in the menu
// (headset, 2026-10-15). A program one of whose shaders names a VFX particle
// buffer is marked at link, and its draws are left out — what a device without
// compute shaders shows of these effects. Counted in the log.
#define KLFB_VFX_PROGS 8192
static uint8_t g_vfx_prog[KLFB_VFX_PROGS];
static unsigned g_vfx_skipped;
static int klfb_skip_vfx_on(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_GLFB_SKIP_VFX", t && strcmp(t, "x2436558143118760") == 0);
    }
    return on;
}
static const char *klfb_shader_src(uint32_t shader);
static const char *const g_vfx_marks[] = { "attributeBuffer", "deadListIn", "deadListOut",
                                           "elementToVFXBuffer", "graphValuesBuffer" };
static void klfb_vfx_say(uint32_t program, const char *mark, const char *how) {
    static int said;
    if (said++ < 16)
        fprintf(stderr, "  [glfb] program %u is a VFX Graph shader (\"%s\", %s) — its draws are "
                        "left out (KL_GLFB_SKIP_VFX)\n", program, mark, how);
}
// The same, from what the linked program reports: its active uniforms and
// uniform blocks by name. A program loaded from Unity's binary cache has no
// source here and is never linked, so this is the only way to know it.
static void klfb_vfx_mark_names(uint32_t program) {
    if (program >= KLFB_VFX_PROGS || g_vfx_prog[program] || !klfb_skip_vfx_on()) return;
    static void (*r_piv)(uint32_t, uint32_t, int32_t *);
    static void (*r_au)(uint32_t, uint32_t, int32_t, int32_t *, int32_t *, uint32_t *, char *);
    static void (*r_ubn)(uint32_t, uint32_t, int32_t, int32_t *, char *);
    if (!r_piv) r_piv = asym("glGetProgramiv");
    if (!r_au) r_au = asym("glGetActiveUniform");
    if (!r_ubn) r_ubn = asym("glGetActiveUniformBlockName");
    if (!r_piv) return;
    int32_t ok = 0, nu = 0, nb = 0;
    r_piv(program, 0x8B82 /* LINK_STATUS */, &ok);
    if (!ok) return;
    r_piv(program, 0x8B86 /* ACTIVE_UNIFORMS */, &nu);
    r_piv(program, 0x8A36 /* ACTIVE_UNIFORM_BLOCKS */, &nb);
    char name[128];
    for (int32_t i = 0; r_au && i < nu && i < 256; i++) {
        int32_t len = 0, size = 0; uint32_t type = 0; name[0] = 0;
        r_au(program, (uint32_t)i, sizeof name, &len, &size, &type, name);
        for (size_t k = 0; k < sizeof g_vfx_marks / sizeof g_vfx_marks[0]; k++)
            if (strstr(name, g_vfx_marks[k])) { g_vfx_prog[program] = 1; klfb_vfx_say(program, g_vfx_marks[k], "uniform"); return; }
    }
    for (int32_t i = 0; r_ubn && i < nb && i < 64; i++) {
        int32_t len = 0; name[0] = 0;
        r_ubn(program, (uint32_t)i, sizeof name, &len, name);
        for (size_t k = 0; k < sizeof g_vfx_marks / sizeof g_vfx_marks[0]; k++)
            if (strstr(name, g_vfx_marks[k])) { g_vfx_prog[program] = 1; klfb_vfx_say(program, g_vfx_marks[k], "uniform block"); return; }
    }
}
static void klfb_vfx_mark(uint32_t program) {
    if (program >= KLFB_VFX_PROGS || !klfb_skip_vfx_on()) return;
    static void (*r_att)(uint32_t, int32_t, int32_t *, uint32_t *);
    if (!r_att) r_att = asym("glGetAttachedShaders");
    if (!r_att) return;
    uint32_t sh[8]; int32_t n = 0;
    r_att(program, 8, &n, sh);
    for (int i = 0; i < n; i++) {
        const char *src = klfb_shader_src(sh[i]);
        if (!src) continue;
        for (size_t k = 0; k < sizeof g_vfx_marks / sizeof g_vfx_marks[0]; k++)
            if (strstr(src, g_vfx_marks[k])) {
                g_vfx_prog[program] = 1;
                klfb_vfx_say(program, g_vfx_marks[k], "source");
                return;
            }
    }
    klfb_vfx_mark_names(program);
}
// In each draw thunk: a marked program's draw does not happen.
#define KLFB_VFX_SKIP() do { \
        if (g_cur_prog < KLFB_VFX_PROGS && g_vfx_prog[g_cur_prog]) { \
            if ((g_vfx_skipped++ & 0x3fff) == 0) \
                fprintf(stderr, "  [glfb] VFX draws left out so far: %u\n", g_vfx_skipped); \
            return; } } while (0)

static void klfb_LinkProgram(uint32_t program) {
    if (!g_real_LinkProgram) return;
    klfb_ssbo_forget(program);
    pthread_mutex_lock(&g_compile_lock);
    uint64_t slow_t0 = klfb_slow_ms() ? klfb_slow_now() : 0;
    klfb_pins_bind(program);
    g_real_LinkProgram(program);
    klfb_cs_linked(program);
    klfb_vfx_mark(program);
    if (slow_t0) {
        // The link may be finished behind this call; asking for its status is
        // what waits for it, and what the guest does next anyway.
        static void (*status)(uint32_t, uint32_t, int32_t *);
        if (!status) status = asym("glGetProgramiv");
        int32_t linked = 0;
        if (status) status(program, 0x8B82 /* LINK_STATUS */, &linked);
        klfb_slow_note(1, program, (double)(klfb_slow_now() - slow_t0) / 1.0e6, 0);
    }
    // The pins the rewrite took out of the shader text become a translation
    // table here — this is the first moment a name has a location to translate
    // to. See the block above klfb_strip_uniform_layout.
    klfb_pins_link(program);

    // Report a FAILED link with its info log and the shader sources. A program
    // whose shaders each compiled but that will not link is invisible otherwise
    // — the guest reads GL_LINK_STATUS itself and (UE4) fatals the render thread
    // in RHICreateBoundShaderState with nothing on our side saying why. This is
    // the same reporting the compile path already does, moved to link time.
    {
        static void (*r_GetProgramiv)(uint32_t, uint32_t, int32_t *);
        static void (*r_GetProgramInfoLog)(uint32_t, int32_t, int32_t *, char *);
        static void (*r_GetAttachedShaders)(uint32_t, int32_t, int32_t *, uint32_t *);
        if (!r_GetProgramiv)      r_GetProgramiv      = asym("glGetProgramiv");
        if (!r_GetProgramInfoLog) r_GetProgramInfoLog = asym("glGetProgramInfoLog");
        if (!r_GetAttachedShaders) r_GetAttachedShaders = asym("glGetAttachedShaders");
        int32_t ok = 1;
        if (r_GetProgramiv) r_GetProgramiv(program, 0x8B82 /* LINK_STATUS */, &ok);
        if (!ok) {
            char log[2048]; log[0] = 0;
            if (r_GetProgramInfoLog) r_GetProgramInfoLog(program, sizeof log, NULL, log);
            fprintf(stderr, "  [glfb] glLinkProgram(%u) FAILED — info log:\n%s\n",
                    program, log);
            if (r_GetAttachedShaders) {
                uint32_t sh[8]; int32_t n = 0;
                r_GetAttachedShaders(program, 8, &n, sh);
                for (int32_t i = 0; i < n; i++) {
                    const char *src = klfb_shader_src(sh[i]);
                    fprintf(stderr, "  [glfb] ---- linked program %u shader %u ----\n%s\n"
                                    "  [glfb] ---- end ----\n", program, sh[i],
                            src ? src : "(source not captured)\n");
                }
            }
        }
    }
    // KL_GLFB_DUMP_PROGRAM=N: print the sources that were linked into program
    // N. The timeline names programs by number ("the frame's last draw is
    // program 7"); this turns the number into the shader text.
    static int dump_prog = -2;
    if (dump_prog == -2) {
        const char *d = kl_env_str("KL_GLFB_DUMP_PROGRAM", NULL);
        dump_prog = d ? atoi(d) : -1;
    }
    if (dump_prog >= 0 && (int)program == dump_prog) {
        static void (*r_GetAttachedShaders)(uint32_t, int32_t, int32_t *,
                                            uint32_t *);
        if (!r_GetAttachedShaders)
            r_GetAttachedShaders = asym("glGetAttachedShaders");
        if (r_GetAttachedShaders) {
            uint32_t sh[8]; int32_t n = 0;
            r_GetAttachedShaders(program, 8, &n, sh);
            fprintf(stderr, "  [glfb] program %u: %d attached shaders\n",
                    program, (int)n);
            for (int32_t i = 0; i < n; i++) {
                const char *src = klfb_shader_src(sh[i]);
                fprintf(stderr, "  [glfb] ---- program %u shader %u ----\n%s\n"
                                "  [glfb] ---- end ----\n", program, sh[i],
                        src ? src : "(source not captured)\n");
            }
        }
    }
    // KL_GLFB_DUMP_SHADERS=1: dump EVERY linked program's shader sources (capped),
    // marked "[shaderdump]" for grepping. Used to find the model (VertexLitGeneric)
    // shader whose ambient-cube term reads zero on ANGLE-Metal — its GLSL can then be
    // re-translated on host ANGLE to locate the bug. World LightmappedGeneric samples a
    // lightmap; the model shader computes ambient from constants + normal instead.
    static int dump_all = -2, dumped_n = 0;
    if (dump_all == -2) dump_all = kl_env_on("KL_GLFB_DUMP_SHADERS", 0);
    static int dump_max = -1;
    if (dump_max < 0) dump_max = kl_env_int("KL_GLFB_DUMP_SHADERS_MAX", 80);
    if (dump_all && dumped_n < dump_max) {
        dumped_n++;
        static void (*r_gas)(uint32_t, int32_t, int32_t *, uint32_t *);
        if (!r_gas) r_gas = asym("glGetAttachedShaders");
        if (r_gas) {
            uint32_t sh[8]; int32_t n = 0;
            r_gas(program, 8, &n, sh);
            for (int32_t i = 0; i < n; i++) {
                const char *src = klfb_shader_src(sh[i]);
                fprintf(stderr, "[shaderdump] prog %u shader %u:\n%s\n[shaderdump-end]\n",
                        program, sh[i], src ? src : "(none)");
            }
        }
    }
    pthread_mutex_unlock(&g_compile_lock);
}

// ------------------------------------------------------ texture storage census
//
// KL_GLFB_SKIP bisected the AGX abort down to this pair: dropping BOTH
// glTexStorage2D and glTexStorage3D avoids it, and so does dropping both
// glTexSubImage2D and glTexSubImage3D, while dropping any one of the four alone
// does not. That is the "allocate immutable storage, then upload into it" pattern,
// and it says the trigger is a *format*, which is what this prints.
//
// Both calls fit in registers (5 and 6 arguments), so the natural signature is
// safe here — unlike glTexSubImage3D above, whose 11 arguments spill to the stack
// where Darwin's packing and AAPCS64's 8-byte slots disagree.
static void (*g_real_TexStorage2D)(uint32_t, int32_t, uint32_t, int32_t, int32_t);
static void (*g_real_TexStorage3D)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t);

#define KLFB_MAX_FMTS 32
static struct { uint32_t fmt; unsigned n; } g_fmts[KLFB_MAX_FMTS];
static unsigned g_nfmts;

static void klfb_note_format(uint32_t fmt, int32_t w, int32_t h, int32_t d,
                             const char *who) {
    for (unsigned i = 0; i < g_nfmts; i++)
        if (g_fmts[i].fmt == fmt) { g_fmts[i].n++; return; }
    if (g_nfmts < KLFB_MAX_FMTS) {
        g_fmts[g_nfmts].fmt = fmt;
        g_fmts[g_nfmts].n = 1;
        g_nfmts++;
        // Only the first sighting of each format is printed, so the tail of a
        // crashing run names the format that was new when it died.
        fprintf(stderr, "  [glfb] %s: new internalformat 0x%04x (%dx%dx%d)\n",
                who, fmt, w, h, d);
    }
}

// ------------------------------------------------------ the GL object census
//
// KL_GL_CENSUS=<swaps> prints, every N eglSwapBuffers, how many GL objects the
// guest has created and how many it has deleted, per class, plus the bytes of
// texture and renderbuffer storage still live.
//
// Why this exists: every gen/delete goes straight to ANGLE (kl_glfb_sym hands
// the guest ANGLE's own pointers), so nothing at this seam knows whether the
// guest's objects are being reclaimed. That is fine until something grows
// across scene loads, at which point the first question — "is the leak the
// guest's, ANGLE's, or ours?" — has no instrument to answer it. A live count
// that climbs by the same amount at each loading transition names the class;
// a flat one exonerates the whole GL path in one run.
//
// The counts are exact. The bytes are an *estimate over immutable allocations
// only*: glTexStorage2D/3D and glRenderbufferStorage(Multisample), which is
// where the large targets in this title come from. Storage taken through
// glTexImage2D is counted as a call, not as bytes, and printed separately so
// the number is never quietly incomplete.
#define KLFB_CENSUS_KINDS 7
enum { KLC_TEX, KLC_FBO, KLC_RBO, KLC_BUF, KLC_VAO, KLC_SHADER, KLC_PROG };
static const char *const g_census_kind[KLFB_CENSUS_KINDS] = {
    "textures", "framebuffers", "renderbuffers", "buffers",
    "vertex arrays", "shaders", "programs",
};
static struct { unsigned long made, killed; } g_census[KLFB_CENSUS_KINDS];
static unsigned long g_census_teximage;      // allocations whose bytes we do not model

// name -> bytes, for the two immutable-storage families. Kept in one table
// because a texture name and a renderbuffer name live in different spaces and
// the kind disambiguates. Grows; entries are removed on delete, so this table
// is itself a leak detector — if it never shrinks, neither does the VRAM.
typedef struct { uint8_t kind; uint32_t name; uint64_t bytes; } klfb_vram;
static klfb_vram *g_vram;
static unsigned   g_vram_n, g_vram_cap;
static uint64_t   g_vram_bytes[KLFB_CENSUS_KINDS];
static pthread_mutex_t g_census_lock = PTHREAD_MUTEX_INITIALIZER;

// Bits per pixel for the formats this guest allocates. Anything unlisted is
// counted at zero and tallied, rather than guessed at: a wrong multiplier
// would make the total look authoritative while being fiction.
static unsigned long g_census_unknown_fmt;
static unsigned klfb_fmt_bits(uint32_t fmt) {
    // The ASTC LDR block formats are two contiguous runs (linear, then sRGB) in
    // the same block-size order, and every block is 128 bits — so the bits per
    // pixel is 128/(w*h) and the table is the block dimensions, not a case each.
    static const uint8_t astc[14][2] = {
        {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},
        {10,5},{10,6},{10,8},{10,10},{12,10},{12,12},
    };
    if (fmt >= 0x93B0 && fmt <= 0x93BD) {
        const uint8_t *b = astc[fmt - 0x93B0];
        return 128 / (unsigned)(b[0] * b[1]);
    }
    if (fmt >= 0x93D0 && fmt <= 0x93DD) {
        const uint8_t *b = astc[fmt - 0x93D0];
        return 128 / (unsigned)(b[0] * b[1]);
    }
    switch (fmt) {
    case 0x8229 /* R8 */:      case 0x8F94 /* R8_SNORM */:
    case 0x8D48 /* STENCIL_INDEX8 */:                           return 8;
    case 0x822B /* RG8 */:     case 0x822D /* R16F */:
    case 0x8D62 /* RGB565 */:  case 0x8056 /* RGBA4 */:
    case 0x8057 /* RGB5_A1 */: case 0x81A5 /* DEPTH_COMPONENT16 */: return 16;
    case 0x8051 /* RGB8 */:    case 0x8C41 /* SRGB8 */:         return 24;
    case 0x8058 /* RGBA8 */:   case 0x8C43 /* SRGB8_ALPHA8 */:
    case 0x822E /* R32F */:    case 0x822F /* RG16F */:
    case 0x8C3A /* R11F_G11F_B10F */: case 0x8C3D /* RGB9_E5 */:
    case 0x81A6 /* DEPTH_COMPONENT24 */: case 0x81A7 /* DEPTH_COMPONENT32 */:
    case 0x88F0 /* DEPTH24_STENCIL8 */:  case 0x8CAC /* DEPTH_COMPONENT32F */:
                                                                return 32;
    case 0x881B /* RGB16F */:                                   return 48;
    case 0x881A /* RGBA16F */: case 0x8230 /* RG32F */:
    case 0x8CAD /* DEPTH32F_STENCIL8 */:                        return 64;
    case 0x8815 /* RGB32F */:                                   return 96;
    case 0x8814 /* RGBA32F */:                                  return 128;
    case 0x9274 /* COMPRESSED_RGB8_ETC2 */:
    case 0x9275 /* COMPRESSED_SRGB8_ETC2 */:
    case 0x9270 /* COMPRESSED_R11_EAC */:                       return 4;
    case 0x9278 /* COMPRESSED_RGBA8_ETC2_EAC */:
    case 0x9279 /* COMPRESSED_SRGB8_ALPHA8_ETC2_EAC */:
    case 0x9272 /* COMPRESSED_RG11_EAC */:                      return 8;
    }
    g_census_unknown_fmt++;
    return 0;
}

// Bytes an immutable allocation reserves. A mip chain is the base level times
// 4/3 in the limit; levels>1 uses that bound rather than summing, which is
// within a few per cent and cannot be wrong in the direction that hides a leak.
static uint64_t klfb_storage_bytes(uint32_t fmt, int32_t w, int32_t h,
                                   int32_t d, int32_t levels) {
    if (w <= 0 || h <= 0 || d <= 0) return 0;
    uint64_t px = (uint64_t)w * (uint64_t)h * (uint64_t)d;
    uint64_t bytes = px * klfb_fmt_bits(fmt) / 8;
    if (levels > 1) bytes = bytes * 4 / 3;
    return bytes;
}

static int klfb_census_every(void) {
    static int n = -1;
    if (n < 0) {
        n = kl_env_int("KL_GL_CENSUS", 0);
        if (n < 0) n = 0;
    }
    return n;
}

// Caller must not hold g_census_lock.
static void klfb_vram_note(int kind, uint32_t name, uint64_t bytes) {
    if (!klfb_census_every() || !name) return;
    pthread_mutex_lock(&g_census_lock);
    for (unsigned i = 0; i < g_vram_n; i++)
        if (g_vram[i].kind == kind && g_vram[i].name == name) {
            // Re-specified storage replaces, it does not add.
            g_vram_bytes[kind] -= g_vram[i].bytes;
            g_vram[i].bytes = bytes;
            g_vram_bytes[kind] += bytes;
            pthread_mutex_unlock(&g_census_lock);
            return;
        }
    if (g_vram_n == g_vram_cap) {
        unsigned cap = g_vram_cap ? g_vram_cap * 2 : 256;
        klfb_vram *p = realloc(g_vram, cap * sizeof *p);
        if (!p) { pthread_mutex_unlock(&g_census_lock); return; }
        g_vram = p; g_vram_cap = cap;
    }
    g_vram[g_vram_n++] = (klfb_vram){(uint8_t)kind, name, bytes};
    g_vram_bytes[kind] += bytes;
    pthread_mutex_unlock(&g_census_lock);
}

static void klfb_vram_forget(int kind, uint32_t name) {
    uint64_t freed = 0;
    pthread_mutex_lock(&g_census_lock);
    for (unsigned i = 0; i < g_vram_n; i++)
        if (g_vram[i].kind == kind && g_vram[i].name == name) {
            freed = g_vram[i].bytes;
            g_vram_bytes[kind] -= freed;
            g_vram[i] = g_vram[--g_vram_n];
            break;
        }
    pthread_mutex_unlock(&g_census_lock);
    // The large allocations are the ones a leak is made of, and "was it ever
    // released" is not answerable from a running total. Naming each big release
    // makes the absence of one a visible fact rather than an inference.
    if (freed >= (16u << 20))
        fprintf(stderr, "  [census] released %s %u: %llu MiB\n",
                g_census_kind[kind], name, (unsigned long long)(freed >> 20));
}

static void klfb_census_made(int kind, int32_t n) {
    if (n > 0) __atomic_fetch_add(&g_census[kind].made, (unsigned long)n,
                                  __ATOMIC_RELAXED);
}
static void klfb_census_killed(int kind, int32_t n) {
    if (n > 0) __atomic_fetch_add(&g_census[kind].killed, (unsigned long)n,
                                  __ATOMIC_RELAXED);
}

void kl_glfb_gl_census(FILE *f) {
    if (!klfb_census_every()) return;
    fprintf(f, "  [census] GL objects live (made - deleted):");
    for (int k = 0; k < KLFB_CENSUS_KINDS; k++)
        fprintf(f, " %s %lu/%lu", g_census_kind[k],
                g_census[k].made - g_census[k].killed, g_census[k].made);
    // Read without the lock, deliberately. This also runs from kl_egl_report,
    // which every fatal path goes through, and a diagnostic that can block on
    // the way to reporting a crash costs the whole report — the lock is held
    // across a realloc in the allocation path, so that is not hypothetical.
    // The cost of not taking it is three scalars that may be one allocation
    // stale, which changes no conclusion this number is used for.
    uint64_t tex = g_vram_bytes[KLC_TEX], rbo = g_vram_bytes[KLC_RBO];
    unsigned tracked = g_vram_n;
    fprintf(f, "\n  [census] immutable storage live: textures %llu MiB, "
               "renderbuffers %llu MiB (%u allocations tracked)",
            (unsigned long long)(tex >> 20), (unsigned long long)(rbo >> 20),
            tracked);
    if (g_census_teximage)
        fprintf(f, ", %lu glTexImage* allocations unmeasured", g_census_teximage);
    if (g_census_unknown_fmt)
        fprintf(f, ", %lu allocations of unknown format", g_census_unknown_fmt);
    fprintf(f, "\n");
}

// glCopyImageSubData emulation (klfb_CopyImageSubData, with the compressed
// upload code): its hooks into the texture lifecycle.
static void klfb_cimg_note_storage(uint32_t target, int32_t levels, uint32_t fmt,
                                   int32_t w, int32_t h);
static void klfb_cimg_forget(int32_t n, const uint32_t *ids);

// The gen/delete pairs, as a macro: the only thing that varies is which counter
// moves, and a typo in one hand-written copy is a census that lies about exactly
// one class.
static void klfb_tb_forget(uint32_t tex, uint32_t buffer);   // texel-buffer emulation, below
#define KLFB_GENDEL(Name, KIND)                                               \
    static void (*g_real_Gen##Name)(int32_t, uint32_t *);                     \
    static void (*g_real_Delete##Name)(int32_t, const uint32_t *);            \
    static void klfb_Gen##Name(int32_t n, uint32_t *ids) {                    \
        if (g_real_Gen##Name) g_real_Gen##Name(n, ids);                       \
        klfb_census_made(KIND, n);                                            \
    }                                                                         \
    static void klfb_Delete##Name(int32_t n, const uint32_t *ids) {           \
        klfb_census_killed(KIND, n);                                          \
        if ((KIND) == KLC_TEX && ids) klfb_cimg_forget(n, ids);               \
        if (klfb_census_every() && ids &&                                     \
            ((KIND) == KLC_TEX || (KIND) == KLC_RBO))                         \
            for (int32_t i = 0; i < n; i++) klfb_vram_forget(KIND, ids[i]);   \
        if ((KIND) == KLC_TEX && ids)                                         \
            for (int32_t i = 0; i < n; i++) {                                 \
                klfb_tb_forget(ids[i], 0);                                   \
            }                                                                 \
        if ((KIND) == KLC_BUF && ids)                                         \
            for (int32_t i = 0; i < n; i++) klfb_tb_forget(0, ids[i]);        \
        if (g_real_Delete##Name) g_real_Delete##Name(n, ids);                 \
    }

KLFB_GENDEL(Textures,     KLC_TEX)
KLFB_GENDEL(Renderbuffers, KLC_RBO)
KLFB_GENDEL(Buffers,      KLC_BUF)
KLFB_GENDEL(VertexArrays, KLC_VAO)
// glGenFramebuffers already has a thunk (the FBO trace); only the delete half
// is new, and the gen half calls klfb_census_made itself.
static void klfb_forget_fbo(uint32_t fbo);           // with the stage map below
static void (*g_real_DeleteFramebuffers)(int32_t, const uint32_t *);
static void klfb_DeleteFramebuffers(int32_t n, const uint32_t *ids) {
    klfb_census_killed(KLC_FBO, n);
    // Drop the stage attribution with the object. GL recycles names, so an
    // entry kept past the delete answers for whatever inherits the name next.
    for (int32_t i = 0; ids && i < n; i++) klfb_forget_fbo(ids[i]);
    if (g_real_DeleteFramebuffers) g_real_DeleteFramebuffers(n, ids);
}

// Counted, not modelled. Mutable storage is per-level and re-specifiable, so a
// byte figure would need a level table; the count is here so that a leak taking
// this route cannot hide behind a census that only knows about glTexStorage*.
// Nine arguments, all in registers bar the last pointer, so the natural
// signature is safe (unlike glTexSubImage3D above).
static void (*g_real_TexImage2D)(uint32_t, int32_t, int32_t, int32_t, int32_t,
                                 int32_t, uint32_t, uint32_t, const void *);
// Defined in the ABI-thunks block below; the texture-allocation thunks feed it.
static void klfb_note_tex_storage(uint32_t name, uint32_t fmt, int32_t w,
                                  int32_t h);

static void klfb_TexImage2D(uint32_t target, int32_t level, int32_t ifmt,
                            int32_t w, int32_t h, int32_t border, uint32_t fmt,
                            uint32_t type, const void *pixels) {
    if (level == 0) g_census_teximage++;
    // A MUTABLE allocation is an allocation: recording only glTexStorage2D/3D
    // leaves a render target created this way invisible to every readback path
    // — the same blindness as a full table, by a different door.
    if (level == 0 && w > 0 && h > 0 && a_glGetIntegerv) {
        int32_t bound = -1;
        a_glGetIntegerv(target == 0x8C1A /* TEXTURE_2D_ARRAY */ ? 0x8C1D
                                                                : 0x8069,
                        &bound);
        if (bound > 0) klfb_note_tex_storage((uint32_t)bound, (uint32_t)ifmt, w, h);
    }
    if (g_real_TexImage2D)
        g_real_TexImage2D(target, level, ifmt, w, h, border, fmt, type, pixels);
}

static uint32_t (*g_real_CreateShader)(uint32_t);
static uint32_t klfb_CreateShader(uint32_t type) {
    klfb_census_made(KLC_SHADER, 1);
    // Ensure ANGLE's context is current on this thread. HL2Q3VR's multiview shader
    // generator (libtogl) runs glCreateShader from a code path where the guest had
    // released the GL context (klegl_MakeCurrent(NULL) -> kl_glfb_release_current),
    // so ANGLE returned 0 and the whole multiview variant silently failed (shader 0,
    // "compile failed: unknown error"). Re-taking the root context here is idempotent
    // when it is already current, and glCreateShader requires a current context
    // regardless, so this is correct for every title, not just hl2.
    kl_glfb_make_current();
    // A compute shader is a vertex shader here; its source is rewritten to match.
    uint32_t asked = type;
    int cs = type == KLFB_GL_COMPUTE_SHADER && (kl_glfb_compute_tf() || kl_glfb_compute_stub());
    if (cs) type = 0x8B31 /* GL_VERTEX_SHADER */;
    uint32_t name = g_real_CreateShader ? g_real_CreateShader(type) : 0;
    // Say so when ANGLE makes nothing: the guest goes on to source, compile
    // and attach shader 0, none of which reports anything, and the first sign
    // is a link that fails with "No compiled shaders." (Saints & Sinners -
    // Chapter 2, at the loading screen after its tutorial, 2026-10-17).
    if (!name && g_real_CreateShader) {
        static int said;
        if (said < 8) {
            said++;
            fprintf(stderr, "  [glfb] glCreateShader(0x%x) -> 0%s\n", asked,
                    asked == KLFB_GL_COMPUTE_SHADER
                        ? " — a COMPUTE shader, a stage this ES 3.0 context does not have"
                        : "");
        }
    }
    if (name) klfb_note_type(name, type);   // stage, for the varying-location strip
    if (name) { if (cs) klfb_cs_add(g_cs_shaders, name); else klfb_cs_forget(g_cs_shaders, name); }
    return name;
}
static void (*g_real_DeleteShader)(uint32_t);
static void klfb_DeleteShader(uint32_t s) {
    klfb_census_killed(KLC_SHADER, 1);
    klfb_cs_forget(g_cs_shaders, s);          // the program keeps its own copy of the numbers
    klfb_pin_forget(s);
    if (g_real_DeleteShader) g_real_DeleteShader(s);
}
static uint32_t (*g_real_CreateProgram)(void);
static uint32_t klfb_CreateProgram(void) {
    klfb_census_made(KLC_PROG, 1);
    return g_real_CreateProgram ? g_real_CreateProgram() : 0;
}
static void (*g_real_DeleteProgram)(uint32_t);
static void klfb_DeleteProgram(uint32_t p) {
    klfb_census_killed(KLC_PROG, 1);
    klfb_ssbo_forget(p);
    klfb_cs_forget(g_cs_progs, p);
    if (g_real_DeleteProgram) g_real_DeleteProgram(p);
}

// KL_GLFB_NOSRGB=1 substitutes GL_RGBA8 for GL_SRGB8_ALPHA8 at allocation. The
// census found the guest's eye-sized render target is SRGB8_ALPHA8, and sRGB is a
// case Metal's fixed-function blit cannot always service — it falls back to a
// compute blit, which is the AGX family that aborts. Swapping the format is the
// one-line way to test that without touching anything else; the two are upload- and
// attachment-compatible, so only the colour transfer function changes. A frame
// captured with this set is wrong (un-decoded sRGB), which is fine for a probe.

static uint32_t klfb_maybe_unsrgb(uint32_t fmt) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_NOSRGB", 0);
    return (on && fmt == GL_SRGB8_ALPHA8) ? GL_RGBA8 : fmt;
}

// Defined in the ABI-thunks block below; the glTexStorage2D thunk feeds it.
static void klfb_note_tex_storage(uint32_t name, uint32_t fmt, int32_t w,
                                  int32_t h);

// KL_GLFB_TRACE_TEX=1 logs every texture call rather than only first sightings, so
// the tail of a crashing run names the exact call it died on. The abort happens on
// a Metal compiler thread, so the last line is the trigger's neighbourhood rather
// than provably the trigger itself — but it bounds the search to one call.
static int klfb_trace_tex(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_TRACE_TEX", 0);
    return on;
}
static unsigned g_texcalls;

static void (*g_real_TexSubImage2D)(uint32_t, int32_t, int32_t, int32_t, int32_t,
                                    int32_t, uint32_t, uint32_t, const void *);

// The parameters set on a texture before it is allocated matter as much as the
// format: swizzle in particular (GL_TEXTURE_SWIZZLE_R..A, 0x8E42-0x8E45) is a
// state a Metal backend may have to emulate rather than set natively, and Unity
// uses it to make an R8 texture behave like the GL_LUMINANCE/GL_ALPHA formats
// GLES 3 dropped. The trace shows 156 of these calls, a burst of them immediately
// before the allocation the abort follows, so they are logged with their arguments.
static void (*g_real_TexParameteri)(uint32_t, uint32_t, int32_t);

// KL_GLFB_TRACE_FBO=1 logs the framebuffer-object lifecycle with thread ids.
// FBOs (like VAOs) are NOT shared between contexts, and under KL_GLFB_SHARED
// the guest runs two of them: an FBO generated on the setup thread's context
// and drawn with on the render thread's is a different, empty object there —
// "incomplete: no attachments and default size is zero", which is exactly
// what the blank frame's 0x506s report (caught via ErrorSet::validationError).
// This trace shows who made each FBO and who uses it.
static void (*g_real_GenFramebuffers)(int32_t, uint32_t *);
static void (*g_real_BindFramebuffer)(uint32_t, uint32_t);
static void (*g_real_FramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t,
                                           int32_t);
// ...and its array-slice sibling. glFramebufferTexture2D cannot name a layer, so
// a guest whose eye swapchain is one 2-slice array texture — every OpenXR guest
// in single-pass-instanced layout — attaches its eyes exclusively through this
// entry point. It was untraced, which is why "which FBO does the guest draw the
// eye into?" had no answer at all on that path while having a complete one on
// Beat Saber's.
static void (*g_real_FramebufferTextureLayer)(uint32_t, uint32_t, uint32_t, int32_t,
                                              int32_t);

static int klfb_tex_info(uint32_t name, uint32_t *fmt, int32_t *w, int32_t *h);

// Every FBO's current colour-0 attachment, for the draw census below. This is a
// different table from g_fbo_stage, which deliberately keeps its sixteen slots
// for EYE framebuffers only: the census's whole job is to say which of the
// guest's OTHER framebuffers the picture is landing in, so it has to know about
// exactly the ones that map refuses.
#define KLFB_FBO_COLOR 48
static struct { uint32_t fbo, tex; int layer; } g_fbo_color[KLFB_FBO_COLOR];
static unsigned g_nfbo_color;

static void klfb_note_fbo_color(uint32_t fbo, uint32_t tex, int layer) {
    if (!fbo) return;
    for (unsigned i = 0; i < g_nfbo_color; i++)
        if (g_fbo_color[i].fbo == fbo) {
            g_fbo_color[i].tex = tex; g_fbo_color[i].layer = layer; return;
        }
    if (g_nfbo_color < KLFB_FBO_COLOR) {
        g_fbo_color[g_nfbo_color].fbo = fbo;
        g_fbo_color[g_nfbo_color].tex = tex;
        g_fbo_color[g_nfbo_color].layer = layer;
        g_nfbo_color++;
    }
}

// Highest FBO name handed out so far — the census scans 1..g_fbomax. Binding a
// name glGenFramebuffers never returned is INVALID_OPERATION in ES 3.0, so the
// scan needs a real upper bound, not a guess.
static uint32_t g_fbomax;

static int klfb_trace_fbo(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_TRACE_FBO", 0);
    return on;
}
static uint64_t klfb_tid(void) { uint64_t t = 0; pthread_threadid_np(NULL, &t); return t; }

static void klfb_GenFramebuffers(int32_t n, uint32_t *ids) {
    // KL_HL2_CINEMA_CTX: the HL2Q3VR 2D-cutscene ("Point Insertion" g-man intro)
    // eye-capture path runs glGenFramebuffers with NO current GL context. The guest
    // (libsourcevr) queries eglGetCurrentContext, gets 0x0, and generates its
    // eye-copy FBOs anyway — 68 of them a run on the VR submission thread t6135541,
    // the same thread that elsewhere holds the root context. On this backend every
    // guest EGL context (its "source" and its "copy") aliases the single ANGLE root
    // g_ctx through kl_glfb_make_current, so what actually breaks is not "the wrong
    // context" but "no context at all": a framebuffer name reserved with none current
    // is not a real object, and when the guest then binds it under a live context it
    // BINDS A FRESH EMPTY FBO. The cinema eye-copy then reads/writes an incomplete
    // target and the intro geometry comes out see-through / stale per eye (eye 1 in
    // particular ran context-less, fallback=1, in the reference log). Re-taking the
    // root context here — idempotent when one is already current, and glGenFramebuffers
    // requires a context regardless — gives the names real backing, exactly as
    // klfb_CreateShader does for the multiview shader path. It fires ONLY when there
    // is genuinely no context (zero change for every frame that already has one) and
    // is gated to hl2, so no other title's GL path is touched. KL_HL2_CINEMA_CTX=0 is
    // the A/B that restores the old context-less behaviour for hl2.
    static int cinema_ctx = -1;
    if (cinema_ctx < 0) {
        extern const char *kl_driver_target_name(void);
        const char *tgt = kl_driver_target_name();
        cinema_ctx = kl_env_on("KL_HL2_CINEMA_CTX", tgt && !strcmp(tgt, "hl2"));
    }
    if (cinema_ctx && !kl_egl_current_context()) kl_glfb_make_current();
    if (g_real_GenFramebuffers) g_real_GenFramebuffers(n, ids);
    klfb_census_made(KLC_FBO, n);
    if (ids)
        for (int32_t i = 0; i < n; i++)
            if (ids[i] > g_fbomax) g_fbomax = ids[i];
    // The context is part of a framebuffer's identity, not decoration: FBOs are
    // container objects and are NOT shared between GL contexts, so a guest that
    // records the context it created one under (Unity does) will refuse to bind
    // it under any other. Reported unconditionally — a handful of lines a run,
    // and without them "which context was this name made under?" is unanswerable
    // after the fact.
    if (ids)
        for (int32_t i = 0; i < n; i++)
            fprintf(stderr, "  [glfb] t%llu glGenFramebuffers -> %u (ctx %p)\n",
                    (unsigned long long)klfb_tid(), ids[i],
                    kl_egl_current_context());
}
// KL_GLFB_TRACE_BUF=1 logs the vertex/index buffer lifecycle — glBufferData sizes
// and, crucially, the glMapBufferRange RESULT. Source's togl reports "failed to
// lock vertex buffer" when its own lock returns NULL, and the only way to tell
// whether that NULL is ANGLE handing back a null map, a zero-size buffer, or an
// invalid flag combo is to see the call and its result here. Off by default; the
// wrappers are plain pass-throughs to the real ANGLE entry points otherwise.
static void *(*g_real_MapBufferRange_t)(uint32_t, intptr_t, intptr_t, uint32_t);
static void  (*g_real_BufferData_t)(uint32_t, intptr_t, const void *, uint32_t);
static uint8_t (*g_real_UnmapBuffer_t)(uint32_t);

// ---- texel-buffer freshness (see klfb_texbuffer_upload, far below) -----------
// A GL_TEXTURE_BUFFER samples the CURRENT contents of its backing buffer. Our
// emulation copies those bytes into a 2D texture, but that copy WAS a one-shot at
// glTexBuffer time — and UE refills the backing buffer in place per frame (via
// glBufferSubData or a mapped range) WITHOUT re-issuing glTexBuffer, so the 2D
// texture froze at its first (zero / T-pose) contents and the whole texel-buffer-
// fed 3D world went black (twd2; same class as olar). We track tex<-buffer and
// re-upload whenever that buffer is written. Declared here (before the buffer
// unmap code that refreshes) though the copy itself lives with the texture code.
// GL is serialized on the context thread, so plain statics are safe.
//
// How many textures over buffers are kept fresh, and how many buffers kept: 1024
// each, which no title brought up before had reached. The Walking Dead: Saints &
// Sinners - Chapter 2: Retribution (x5190288877703616, UE 4.27.2 on GLES) passes
// 1024 while it is still loading ("more than 1024 textures over buffers") and
// then shows fire and other effects wrong and loses its hands from time to time
// (on device, 2026-10-17) — what a texel-buffer-fed mesh does when its data
// stops being refreshed. For it the tables take KLFB_TB_MAX; every other guest
// keeps 1024. KL_GLFB_TB_MANY=0|1 overrides the per-target default.
#define KLFB_TB_DEFAULT 1024
#define KLFB_TB_MAX     8192
#define KLFB_TB_ASSOC   KLFB_TB_MAX
static unsigned klfb_tb_cap(void) {
    static unsigned cap;
    if (!cap) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        cap = kl_env_on("KL_GLFB_TB_MANY", t && (strcmp(t, "x5190288877703616") == 0 ||
                                                 // Arizona Sunshine: its storage buffers
                                                 // (KLFB_SB_TEX) are texel-buffer stand-ins too
                                                 strcmp(t, "x2190353671014400") == 0))
            ? KLFB_TB_MAX : KLFB_TB_DEFAULT;
        if (cap != KLFB_TB_DEFAULT)
            fprintf(stderr, "  [glfb] texel buffers: up to %u textures over buffers and %u "
                            "buffers kept fresh (KL_GLFB_TB_MANY)\n", cap, cap);
    }
    return cap;
}
// off/len: the glTexBufferRange window (len 0 = the whole buffer, glTexBuffer).
static struct { uint32_t buffer, tex, ifmt; intptr_t off, len; } g_tb_assoc[KLFB_TB_ASSOC];
static unsigned g_tb_assoc_n;
// A deleted texture or buffer leaves the table: names are reused, and an entry
// that outlived its texture would have every later write to the buffer uploaded
// over whatever texture next got that name.
// ---- a CPU copy of every buffer that backs a texel-buffer texture ------------
//
// The 2D stand-in is filled from the buffer's bytes, and until now those were
// read back out of GL with a READ mapping each time the buffer was written.
// That read is the weak link: this ANGLE's Metal backend hands back NULL for
// maps of some dynamic buffers (see klfb_MapBufferRange), a failed read uploads
// nothing, and a bone buffer that is never uploaded is all zeros — every vertex
// of the mesh at one point, i.e. a skinned character that is simply not there
// (Maestro: its musicians and its book, with their instruments in mid-air).
//
// So the bytes are kept as they are written — glBufferData, glBufferSubData and
// both kinds of unmap all pass through here — and the upload uses them. Only
// for buffers a texture has been pointed at; the read-back stays as the way a
// copy is first seeded, and as the fallback when there is none.
#define KLFB_TBM KLFB_TB_MAX
static struct { uint32_t buf; uint8_t *bytes; size_t size; unsigned ev; } g_tbm[KLFB_TBM];
// KL_GLFB_TB_REPORT also names the first few things that happen to each kept
// buffer — where its bytes come from, and whether they are anything but zero.
static void klfb_tbm_event(int i, const char *what, size_t off, size_t len, const void *data) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_TB_REPORT", 0);
    if (!on || g_tbm[i].ev >= 5) return;
    g_tbm[i].ev++;
    size_t nz = 0;
    if (data) for (size_t b = 0; b < len; b++) nz += ((const uint8_t *)data)[b] != 0;
    uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
    fprintf(stderr, "  [glfb]   kept buffer %u: %s off %zu len %zu%s, %zu non-zero (t%llu)\n",
            g_tbm[i].buf, what, off, len, data ? "" : " (no data)", nz, (unsigned long long)tid);
}
static unsigned g_tbm_n;
static pthread_mutex_t g_tbm_mu = PTHREAD_MUTEX_INITIALIZER;
static int klfb_tbm_ix(uint32_t buf) {            // call with g_tbm_mu held
    for (unsigned i = 0; i < g_tbm_n; i++) if (g_tbm[i].buf == buf) return (int)i;
    return -1;
}
// Start keeping `buf` (glTexBuffer named it). Its size is not known yet.
static void klfb_tbm_want(uint32_t buf) {
    if (!buf) return;
    pthread_mutex_lock(&g_tbm_mu);
    if (klfb_tbm_ix(buf) < 0 && g_tbm_n < klfb_tb_cap()) {
        g_tbm[g_tbm_n].buf = buf; g_tbm[g_tbm_n].bytes = NULL; g_tbm[g_tbm_n].size = 0;
        g_tbm[g_tbm_n].ev = 0;
        g_tbm_n++;
    }
    pthread_mutex_unlock(&g_tbm_mu);
}
static void klfb_tbm_drop(uint32_t buf) {
    pthread_mutex_lock(&g_tbm_mu);
    int i = klfb_tbm_ix(buf);
    if (i >= 0) { free(g_tbm[i].bytes); g_tbm[i] = g_tbm[--g_tbm_n]; }
    pthread_mutex_unlock(&g_tbm_mu);
}
// The buffer was (re)specified: `size` bytes, from `data` or undefined (zeros).
static void klfb_tbm_respecify(uint32_t buf, size_t size, const void *data) {
    if (!buf || !g_tbm_n) return;
    pthread_mutex_lock(&g_tbm_mu);
    int i = klfb_tbm_ix(buf);
    if (i >= 0) {
        uint8_t *nb = size ? realloc(g_tbm[i].bytes, size) : NULL;
        if (size && !nb) { free(g_tbm[i].bytes); nb = NULL; size = 0; }
        if (!size) free(g_tbm[i].bytes);
        g_tbm[i].bytes = nb; g_tbm[i].size = size;
        if (nb) { if (data) memcpy(nb, data, size); else memset(nb, 0, size); }
        klfb_tbm_event(i, "respecified", 0, size, data);
    }
    pthread_mutex_unlock(&g_tbm_mu);
}
// `len` bytes were written at `off`. Grows the copy when the write reaches past
// it (a copy that was never sized starts here).
static void klfb_tbm_write(uint32_t buf, size_t off, size_t len, const void *data) {
    if (!buf || !g_tbm_n || !data || !len) return;
    pthread_mutex_lock(&g_tbm_mu);
    int i = klfb_tbm_ix(buf);
    if (i >= 0) {
        if (off + len > g_tbm[i].size) {
            uint8_t *nb = realloc(g_tbm[i].bytes, off + len);
            if (nb) {
                memset(nb + g_tbm[i].size, 0, off + len - g_tbm[i].size);
                g_tbm[i].bytes = nb; g_tbm[i].size = off + len;
            }
        }
        if (g_tbm[i].bytes && off + len <= g_tbm[i].size) memcpy(g_tbm[i].bytes + off, data, len);
        klfb_tbm_event(i, "written", off, len, data);
    }
    pthread_mutex_unlock(&g_tbm_mu);
}
// A copy of [off, off+len) of the kept bytes (len 0 = to the end), malloc'd for
// the caller; NULL when nothing is kept for `buf`.
static void *klfb_tbm_copy(uint32_t buf, size_t off, size_t len, size_t *out_len) {
    void *r = NULL;
    pthread_mutex_lock(&g_tbm_mu);
    int i = klfb_tbm_ix(buf);
    if (i >= 0 && g_tbm[i].bytes && off < g_tbm[i].size) {
        size_t n = g_tbm[i].size - off;
        if (len && len < n) n = len;
        r = malloc(n);
        if (r) { memcpy(r, g_tbm[i].bytes + off, n); *out_len = n; }
    }
    pthread_mutex_unlock(&g_tbm_mu);
    return r;
}

static void klfb_tb_forget(uint32_t tex, uint32_t buffer) {
    for (unsigned i = 0; i < g_tb_assoc_n; ) {
        if ((tex && g_tb_assoc[i].tex == tex) || (buffer && g_tb_assoc[i].buffer == buffer))
            g_tb_assoc[i] = g_tb_assoc[--g_tb_assoc_n];
        else i++;
    }
    if (buffer) klfb_tbm_drop(buffer);
}
#define KLFB_TB_UNITS 128
// What the guest has bound, per context (klfb_ctx_slot): the three names below
// read as the globals they used to be.
typedef struct {
    int      active_unit;                  // current glActiveTexture unit
    uint32_t bound2d[KLFB_TB_UNITS];       // GL_TEXTURE_2D bound per unit
    uint32_t bound_buf[10];                // bound buffer per target slot (below)
    uint32_t sampler[KLFB_TB_UNITS];       // the guest's sampler object per unit
    uint8_t  tb_unit[KLFB_TB_UNITS];       // unit currently shows a texel-buffer stand-in
} klfb_tb_state;
#define KLFB_TB_STATE    ((klfb_tb_state *)klfb_ctx_slot(KLFB_CTX_SLOT_TB, sizeof(klfb_tb_state)))
#define g_tb_active_unit (KLFB_TB_STATE->active_unit)
#define g_tb_bound2d     (KLFB_TB_STATE->bound2d)
#define g_tb_bound_buf   (KLFB_TB_STATE->bound_buf)
#define g_tb_sampler     (KLFB_TB_STATE->sampler)
#define g_tb_unit        (KLFB_TB_STATE->tb_unit)
static int klfb_tb_tgtslot(uint32_t target) {
    switch (target) {
        case 0x8892: return 0;  // ARRAY_BUFFER
        case 0x8893: return 1;  // ELEMENT_ARRAY_BUFFER
        case 0x8A11: return 2;  // UNIFORM_BUFFER
        case 0x8C2A: return 3;  // TEXTURE_BUFFER
        case 0x8F36: return 4;  // COPY_READ_BUFFER
        case 0x8F37: return 5;  // COPY_WRITE_BUFFER
        case 0x88EB: return 6;  // PIXEL_PACK_BUFFER
        case 0x88EC: return 7;  // PIXEL_UNPACK_BUFFER
        case 0x90D2: return 8;  // SHADER_STORAGE_BUFFER
        case 0x8C8E: return 9;  // TRANSFORM_FEEDBACK_BUFFER
        default:     return -1;
    }
}
static void klfb_tb_refresh_for_buffer(uint32_t buffer);   // defined with the texture code

static int klfb_trace_buf(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_TRACE_BUF", 0);
    return on;
}
#define KL_MAP_INVALIDATE_BUFFER_BIT 0x0008u
#define KL_MAP_READ_BIT              0x0001u

// glMapBufferRange CPU-shadow emulation — the ANGLE-Metal last resort. Its Metal
// backend returns NULL (no GL error) for glMapBufferRange on some dynamic buffers
// even after the store is orphaned: Source's CMeshDX8 re-locks a small
// GL_ARRAY_BUFFER every draw and the map fails, killing the run at "failed to
// lock vertex buffer". When the real map fails and the map is WRITE-only, hand
// the guest a heap buffer and push it into the real buffer with glBufferSubData at
// unmap — glBufferSubData needs no mapping and always works. READ maps are not
// emulated: GLES has no glGetBufferSubData to seed the shadow from the buffer.
#define KLFB_MAX_EMAPS 8
typedef struct {
    int      active;               // slot in use between map and unmap (2: a REAL map, watched)
    void    *real;                 // ...and for that kind, ANGLE's own pointer
    uint32_t buf, target;
    intptr_t offset, length;
    void    *shadow;               // kept and GROWN across maps, not malloc'd each time
    size_t   cap;
} klfb_emap;
// Per context too: a map and its unmap happen on one context, and two threads
// mapping at once must not be handed each other's shadow.
#define g_emaps ((klfb_emap *)klfb_ctx_slot(KLFB_CTX_SLOT_EMAPS, sizeof(klfb_emap) * KLFB_MAX_EMAPS))
// Set once ANGLE-Metal has proven it will not map these dynamic buffers, so the
// hot path stops paying for a doomed real map + glBufferData orphan on every one.
static int g_map_metal_refuses;
static void (*g_real_BufferSubData_t)(uint32_t, intptr_t, intptr_t, const void *);
static uint32_t klfb_buffer_binding_of(uint32_t target) {
    switch (target) {
        case 0x8892: return 0x8894; // ARRAY_BUFFER          -> ARRAY_BUFFER_BINDING
        case 0x8893: return 0x8895; // ELEMENT_ARRAY_BUFFER  -> ELEMENT_ARRAY_BUFFER_BINDING
        case 0x8A11: return 0x8A28; // UNIFORM_BUFFER         -> UNIFORM_BUFFER_BINDING
        // Unreal uploads textures through a mapped GL_PIXEL_UNPACK_BUFFER; a
        // shadow that does not know which buffer it stands for cannot tell two
        // of them apart.
        case 0x88EB: return 0x88ED; // PIXEL_PACK_BUFFER      -> PIXEL_PACK_BUFFER_BINDING
        case 0x88EC: return 0x88EF; // PIXEL_UNPACK_BUFFER    -> PIXEL_UNPACK_BUFFER_BINDING
        case 0x8F36: return 0x8F36; // COPY_READ_BUFFER       (binding query is the same enum)
        case 0x8F37: return 0x8F37; // COPY_WRITE_BUFFER      (likewise)
        default:     return 0;
    }
}
static void klfb_buffer_refused(const char *fn, uint32_t target, intptr_t size, uint32_t usage);
static int glfb_errscan(void);
static uint32_t klfb_bound_buffer(uint32_t target) {
    uint32_t pn = klfb_buffer_binding_of(target);
    int32_t  v  = 0;
    if (pn && a_glGetIntegerv) a_glGetIntegerv(pn, &v);
    return (uint32_t)v;
}
static void *klfb_MapBufferRange(uint32_t target, intptr_t offset, intptr_t length,
                                 uint32_t access) {
    if (KLFB_CS_GENERIC(target)) {
        void *r = NULL;
        KLFB_CS_VIA_COPY_WRITE(r = klfb_MapBufferRange(KLFB_GL_COPY_WRITE, offset, length, access));
        return r;
    }
    if (!g_real_MapBufferRange_t) return NULL;
    void *p = NULL;
    // ANGLE's Metal backend returns NULL (no GL error) for the dynamic maps
    // Source uses (WRITE|INVALIDATE_BUFFER|FLUSH_EXPLICIT etc.). The first few
    // times we try the real map, strip the INVALIDATE hint, and orphan-and-retry
    // (glBufferData(NULL)). But once it is CLEAR the Metal backend just refuses
    // them, all of that is pure waste on a path Source hits hundreds of thousands
    // of times a load — the glBufferData orphan alone reallocates GPU storage
    // every call. So latch g_map_metal_refuses and go straight to the shadow.
    if (!g_map_metal_refuses) {
        p = g_real_MapBufferRange_t(target, offset, length, access);
        if (!p && (access & KL_MAP_INVALIDATE_BUFFER_BIT)) {
            uint32_t a2 = access & ~KL_MAP_INVALIDATE_BUFFER_BIT;
            p = g_real_MapBufferRange_t(target, offset, length, a2);
            if (klfb_trace_buf())
                fprintf(stderr, "  [glfb] glMapBufferRange acc=0x%x -> NULL; retried acc=0x%x -> %p\n",
                        access, a2, p);
            if (!p && g_real_BufferData_t) {
                static void (*get_bp)(uint32_t, uint32_t, int32_t *);
                if (!get_bp) get_bp = asym("glGetBufferParameteriv");
                int32_t bsize = 0, busage = 0x88E8 /* GL_DYNAMIC_DRAW */;
                if (get_bp) {
                    get_bp(target, 0x8764 /* GL_BUFFER_SIZE */,  &bsize);
                    get_bp(target, 0x8765 /* GL_BUFFER_USAGE */, &busage);
                }
                if (bsize <= 0) bsize = (int32_t)(offset + length);
                if (a_glGetError) while (a_glGetError()) {}
                g_real_BufferData_t(target, bsize, NULL, (uint32_t)busage);
                uint32_t orphan_err = a_glGetError ? a_glGetError() : 0;
                if (!orphan_err) {
                    p = g_real_MapBufferRange_t(target, offset, length, access);
                    if (!p) p = g_real_MapBufferRange_t(target, offset, length, a2);
                }
                if (klfb_trace_buf())
                    fprintf(stderr, "  [glfb] glMapBufferRange still NULL; orphaned "
                            "buffer (size=%d usage=0x%x err=0x%x) and retried -> %p\n",
                            bsize, busage, orphan_err, p);
            }
        }
        if (!p) {
            static int fails;
            if (++fails >= 8) {
                g_map_metal_refuses = 1;
                fprintf(stderr, "  [glfb] glMapBufferRange: ANGLE-Metal refused %d dynamic "
                        "maps in a row — serving them from the CPU shadow directly "
                        "(no more real-map/orphan attempts)\n", fails);
            }
        }
    }
    // The CPU shadow. If the guest only WRITES (the dynamic-VB case), hand it a
    // heap buffer and push it with glBufferSubData at unmap. The buffer is kept
    // and grown per slot rather than malloc'd each map — Source maps constantly.
    if (!p && length > 0 && !(access & KL_MAP_READ_BIT)) {
        if (!g_real_BufferSubData_t) g_real_BufferSubData_t = asym("glBufferSubData");
        if (g_real_BufferSubData_t) {
            for (int i = 0; i < KLFB_MAX_EMAPS; i++) {
                if (g_emaps[i].active) continue;
                if (g_emaps[i].cap < (size_t)length) {
                    void *nb = realloc(g_emaps[i].shadow, (size_t)length);
                    if (!nb) break;
                    g_emaps[i].shadow = nb;
                    g_emaps[i].cap    = (size_t)length;
                }
                g_emaps[i].active = 1;
                g_emaps[i].buf    = klfb_bound_buffer(target);
                g_emaps[i].target = target;
                g_emaps[i].offset = offset;
                g_emaps[i].length = length;
                p = g_emaps[i].shadow;
                if (klfb_trace_buf())
                    fprintf(stderr, "  [glfb] glMapBufferRange emulated with a CPU "
                            "shadow (buf=%u target=0x%x off=%ld len=%ld)\n",
                            g_emaps[i].buf, target, (long)offset, (long)length);
                break;
            }
        }
    }
    // A REAL map of a buffer whose bytes are being kept (klfb_tbm): remember
    // where the guest is writing, so the unmap can take the bytes with it.
    if (p && length > 0 && (access & 0x0002 /* MAP_WRITE */) && g_tbm_n) {
        int kept = 0;
        for (int i = 0; i < KLFB_MAX_EMAPS && !kept; i++)
            kept = g_emaps[i].active && g_emaps[i].shadow == p;   // it IS a shadow
        uint32_t bound = klfb_bound_buffer(target);
        pthread_mutex_lock(&g_tbm_mu);
        int want = !kept && bound && klfb_tbm_ix(bound) >= 0;
        pthread_mutex_unlock(&g_tbm_mu);
        for (int i = 0; want && i < KLFB_MAX_EMAPS; i++) {
            if (g_emaps[i].active) continue;
            g_emaps[i].active = 2; g_emaps[i].real = p;
            g_emaps[i].buf = bound; g_emaps[i].target = target;
            g_emaps[i].offset = offset; g_emaps[i].length = length;
            break;
        }
    }
    if (klfb_trace_buf()) {
        if (!p) {
            uint32_t e = a_glGetError ? a_glGetError() : 0;
            fprintf(stderr, "  [glfb] glMapBufferRange(0x%x, off=%ld, len=%ld, acc=0x%x)"
                    " -> NULL, glGetError=0x%x\n", target, (long)offset, (long)length, access, e);
        } else {
            fprintf(stderr, "  [glfb] glMapBufferRange(0x%x, off=%ld, len=%ld, acc=0x%x) -> %p\n",
                    target, (long)offset, (long)length, access, p);
        }
    }
    return p;
}
static uint8_t klfb_UnmapBuffer(uint32_t target) {
    if (KLFB_CS_GENERIC(target)) {
        uint8_t r = 0;
        KLFB_CS_VIA_COPY_WRITE(r = klfb_UnmapBuffer(KLFB_GL_COPY_WRITE));
        return r;
    }
    // A CPU-shadow map (see klfb_MapBufferRange): push the guest's writes into the
    // real buffer with glBufferSubData and report success WITHOUT calling the real
    // glUnmapBuffer — the real buffer was never mapped.
    uint32_t buf = klfb_bound_buffer(target);
    // A watched REAL map first: take the bytes while ANGLE's pointer is valid.
    for (int i = 0; i < KLFB_MAX_EMAPS; i++) {
        if (g_emaps[i].active != 2 || g_emaps[i].target != target) continue;
        if (g_emaps[i].buf != buf && buf != 0) continue;
        klfb_tbm_write(g_emaps[i].buf, (size_t)g_emaps[i].offset, (size_t)g_emaps[i].length,
                       g_emaps[i].real);
        g_emaps[i].active = 0; g_emaps[i].real = NULL;
        break;
    }
    for (int i = 0; i < KLFB_MAX_EMAPS; i++) {
        if (g_emaps[i].active != 1 || g_emaps[i].target != target) continue;
        if (g_emaps[i].buf != buf && buf != 0) continue;
        klfb_tbm_write(g_emaps[i].buf, (size_t)g_emaps[i].offset, (size_t)g_emaps[i].length,
                       g_emaps[i].shadow);
        if (g_real_BufferSubData_t)
            g_real_BufferSubData_t(target, g_emaps[i].offset, g_emaps[i].length,
                                   g_emaps[i].shadow);
        if (glfb_errscan())
            klfb_buffer_refused("glUnmapBuffer's shadow flush", target, g_emaps[i].length, 0);
        if (klfb_trace_buf())
            fprintf(stderr, "  [glfb] glUnmapBuffer(0x%x): flushed CPU shadow "
                    "(buf=%u off=%ld len=%ld) via glBufferSubData\n", target,
                    g_emaps[i].buf, (long)g_emaps[i].offset, (long)g_emaps[i].length);
        g_emaps[i].active = 0;   // keep the buffer allocated for reuse
        klfb_tb_refresh_for_buffer(g_emaps[i].buf);   // the mapped write may back a texel buffer
        return 1;
    }
    uint8_t r = g_real_UnmapBuffer_t ? g_real_UnmapBuffer_t(target) : 1;
    if (klfb_trace_buf())
        fprintf(stderr, "  [glfb] glUnmapBuffer(0x%x) -> %u\n", target, r);
    { int s = klfb_tb_tgtslot(target); if (s >= 0) klfb_tb_refresh_for_buffer(g_tb_bound_buf[s]); }
    return r;
}
// FLUSH_EXPLICIT maps call this; for a CPU-shadow (not really mapped) it must be a
// no-op — the whole range is pushed at unmap — or the real call raises a spurious
// GL_INVALID_OPERATION on a buffer GL does not consider mapped.
static void (*g_real_FlushMappedBufferRange_t)(uint32_t, intptr_t, intptr_t);
static void klfb_FlushMappedBufferRange(uint32_t target, intptr_t offset, intptr_t length) {
    if (KLFB_CS_GENERIC(target)) {
        KLFB_CS_VIA_COPY_WRITE(klfb_FlushMappedBufferRange(KLFB_GL_COPY_WRITE, offset, length));
        return;
    }
    uint32_t buf = klfb_bound_buffer(target);
    for (int i = 0; i < KLFB_MAX_EMAPS; i++)
        if (g_emaps[i].active == 1 && g_emaps[i].target == target &&
            (g_emaps[i].buf == buf || buf == 0))
            return;   // emulated — nothing to flush now
    if (g_real_FlushMappedBufferRange_t)
        g_real_FlushMappedBufferRange_t(target, offset, length);
}
static int glfb_errscan(void);
// ERRSCAN: a refused buffer call, with what it was aimed at — the error alone
// does not say which target, and "nothing bound there" is the usual reason.
static void klfb_buffer_refused(const char *fn, uint32_t target, intptr_t size, uint32_t usage) {
    uint32_t e = a_glGetError ? a_glGetError() : 0;
    static int said;
    if (!e || said >= 16) return;
    said++;
    uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
    fprintf(stderr, "  [glfb] %s(target 0x%x, size %ld, usage 0x%x) REFUSED 0x%x — buffer "
                    "bound there: %u (tracked %u), thread %llu\n",
            fn, target, (long)size, usage, e, klfb_bound_buffer(target),
            klfb_tb_tgtslot(target) >= 0 ? g_tb_bound_buf[klfb_tb_tgtslot(target)] : 0,
            (unsigned long long)tid);
    while (a_glGetError && a_glGetError()) {}
}
static void klfb_BufferData(uint32_t target, intptr_t size, const void *data,
                            uint32_t usage) {
    if (KLFB_CS_GENERIC(target)) {
        KLFB_CS_VIA_COPY_WRITE(klfb_BufferData(KLFB_GL_COPY_WRITE, size, data, usage));
        return;
    }
    if (g_real_BufferData_t) g_real_BufferData_t(target, size, data, usage);
    if (glfb_errscan()) klfb_buffer_refused("glBufferData", target, size, usage);
    { int ks = klfb_tb_tgtslot(target);
      if (ks >= 0 && size >= 0) klfb_tbm_respecify(g_tb_bound_buf[ks], (size_t)size, data); }
    if (klfb_trace_buf())
        fprintf(stderr, "  [glfb] glBufferData(0x%x, size=%ld, %s, usage=0x%x)\n",
                target, (long)size, data ? "data" : "NULL", usage);
    // A full re-specify of a texel-buffer backing must refresh its emulated 2D copy
    // too (UE also updates SRV ring buffers this way, not only via glBufferSubData).
    { int s = klfb_tb_tgtslot(target); if (s >= 0) klfb_tb_refresh_for_buffer(g_tb_bound_buf[s]); }
}

// GL_EXT_multisampled_render_to_texture — the IMPLICIT-resolve path: the guest
// attaches a normal texture to an FBO with a sample count via glFramebufferTexture
// 2DMultisampleEXT and expects the driver to resolve the MSAA into that texture
// when the FBO is read. ANGLE's Metal backend does NOT perform that implicit
// resolve into the texture, so any guest that renders its eye this way ends up
// with a black swapchain image (the MSAA store is never resolved into it).
// (Portal/Source is NOT such a guest — its togl uses CORE glRenderbufferStorage
// Multisample + an explicit glBlitFramebuffer resolve, so these wrappers are inert
// for it; its black screen has a different cause, still under diagnosis.)
//
// Downgrade the whole implicit-MSAA-render-to-texture path to single-sample: a
// guest that uses it then renders straight into the texture (no resolve to depend
// on), which costs multisample anti-aliasing and nothing else. Both halves are
// dropped together so colour and depth keep the SAME sample count and the FBO
// stays complete. KL_GLFB_KEEP_MSAA=1 restores the pass-through for A/B.
static void (*g_real_FramebufferTexture2D_ss)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
static void (*g_real_RenderbufferStorage_ss)(uint32_t, uint32_t, int32_t, int32_t);
static void (*g_real_FramebufferTexture2DMultisampleEXT_ss)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t, int32_t);
static void (*g_real_RenderbufferStorageMultisampleEXT_ss)(uint32_t, int32_t, uint32_t, int32_t, int32_t);
static int klfb_force_ss(void) {
    static int on = -1;
    if (on < 0) on = !kl_env_on("KL_GLFB_KEEP_MSAA", 0);
    return on;
}
static void klfb_FramebufferTexture2DMultisampleEXT(uint32_t target, uint32_t att,
        uint32_t textarget, uint32_t tex, int32_t level, int32_t samples) {
    if (klfb_force_ss()) {
        if (!g_real_FramebufferTexture2D_ss) g_real_FramebufferTexture2D_ss = asym("glFramebufferTexture2D");
        if (g_real_FramebufferTexture2D_ss)
            g_real_FramebufferTexture2D_ss(target, att, textarget, tex, level);   // direct attach, no implicit MSAA
        return;
    }
    if (g_real_FramebufferTexture2DMultisampleEXT_ss)
        g_real_FramebufferTexture2DMultisampleEXT_ss(target, att, textarget, tex, level, samples);
}
static void klfb_RenderbufferStorageMultisampleEXT(uint32_t target, int32_t samples,
        uint32_t ifmt, int32_t w, int32_t h) {
    if (klfb_force_ss()) {
        if (!g_real_RenderbufferStorage_ss) g_real_RenderbufferStorage_ss = asym("glRenderbufferStorage");
        if (g_real_RenderbufferStorage_ss)
            g_real_RenderbufferStorage_ss(target, ifmt, w, h);                    // single-sample store
        return;
    }
    if (g_real_RenderbufferStorageMultisampleEXT_ss)
        g_real_RenderbufferStorageMultisampleEXT_ss(target, samples, ifmt, w, h);
}

// The stage observation, defined with the eye-texture table further down.
static int  klfb_stage_of_tex(uint32_t tex);
static int  klfb_stage_of_fbo(uint32_t fbo);
static void klfb_map_fbo(uint32_t fbo, uint32_t tex);
static void klfb_note_render_stage(int stage);
static void klfb_note_eye_fbo(uint32_t fbo, uint32_t touched);
// Which framebuffer is bound for drawing and for reading, and which of them are
// multiview — per context (klfb_ctx_slot), like the rest of what the guest has
// bound: framebuffers are container objects and their names repeat across
// contexts. The multiview entry is the VIEW COUNT (0 for an ordinary one).
#define KLFB_RESYNC_FBOS 1024
typedef struct {
    uint32_t draw_fb, read_fb;
    uint8_t  multiview[KLFB_RESYNC_FBOS];
} klfb_fbo_ctx;
#define KLFB_FBO_CTX    ((klfb_fbo_ctx *)klfb_ctx_slot(KLFB_CTX_SLOT_FBO, sizeof(klfb_fbo_ctx)))
#define g_draw_fb       (KLFB_FBO_CTX->draw_fb)
#define g_read_fb       (KLFB_FBO_CTX->read_fb)
#define g_fbo_multiview (KLFB_FBO_CTX->multiview)
// ...and the framebuffer the last draw CALL targeted, which is a different
// question from the last binding — see klfb_note_draw.
static uint32_t g_last_draw_fb;
// How many blits the guest issued, and how many of them read the DEFAULT
// framebuffer. The second number is the one worth having: a blit whose source
// is 0 is legal, error-free and silent, and it is how a whole frame goes
// missing between the texture the guest drew into and the one it presents.
static unsigned long g_blits, g_blits_read0;

// ...and the READ binding, which had no tracker at all. An attachment call
// names a TARGET, not a framebuffer, so attributing every attach to g_draw_fb
// is a guess — and a guest that configures a blit SOURCE attaches through
// GL_READ_FRAMEBUFFER, where the guess is wrong every time and the trace says
// so confidently.

// Is this target a DRAW binding? GL_FRAMEBUFFER binds both.
static int klfb_is_draw_target(uint32_t target) {
    return target == 0x8D40 /* FRAMEBUFFER */ || target == 0x8CA9 /* DRAW_FRAMEBUFFER */;
}
static int klfb_is_read_target(uint32_t target) {
    return target == 0x8D40 /* FRAMEBUFFER */ || target == 0x8CA8 /* READ_FRAMEBUFFER */;
}
// Which framebuffer an attachment call addresses. DRAW wins for GL_FRAMEBUFFER
// only because the two are then the same object.
static uint32_t klfb_fb_for_target(uint32_t target) {
    return klfb_is_draw_target(target) ? g_draw_fb : g_read_fb;
}

// The guest's own call site for a GL entry point, as "<image>+0x<off>". Two
// binds that look identical in a trace are a completely different reading
// depending on whether they come from one helper or two, and a GL trace has no
// other way to say so — the arguments are all the state there is.
static const char *klfb_caller(void *ret) {
    static __thread char buf[64];
    size_t off = 0;
    const char *img = kl_addr_image(ret, &off);
    if (img) snprintf(buf, sizeof buf, "%s+0x%zx", img, off);
    else     snprintf(buf, sizeof buf, "%p", ret);
    return buf;
}

// hl2 two-pass eye-copy RT probe. Called from the guest-log shim at "began eye
// submission", i.e. on the eye-submission thread with its GL context current, at the
// exact instant before the port reads its render target and reports "Eye copy skipped:
// Source render target is unavailable". The eye-copy's own glGetIntegerv bypasses klfb
// (libsourcevr binds it straight to ANGLE), so we sample the REAL ANGLE state here.
//   draw_fb 0 / type NONE  => engine rendered the eye into the backbuffer, no texture
//                             RT to copy => two-pass render-ORDERING problem.
//   real FBO + TEXTURE      => an RT IS bound; the material-system tracking the port
//                             reads (HL2Quest_SaveCurrentRenderTarget) is what's stale.
void kl_glfb_probe_current_rt(const char *why) {
    static int said;
    if (said >= 12) return;
    said++;
    int32_t dfb = 0, rfb = 0, atype = 0, aname = 0;
    if (a_glGetIntegerv) {
        a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &dfb);
        a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &rfb);
    }
    if (dfb != 0 && a_glGetFramebufferAttachmentParameteriv) {
        a_glGetFramebufferAttachmentParameteriv(
            0x8CA9 /* DRAW_FRAMEBUFFER */, 0x8CE0 /* COLOR_ATTACHMENT0 */,
            0x8CD0 /* ATTACHMENT_OBJECT_TYPE */, &atype);
        a_glGetFramebufferAttachmentParameteriv(
            0x8CA9, 0x8CE0, 0x8CD1 /* ATTACHMENT_OBJECT_NAME */, &aname);
    }
    fprintf(stderr, "  [rtprobe] %s: draw_fb=%d read_fb=%d color_attach type=0x%x(%s) "
            "name=%d\n", why ? why : "?", dfb, rfb, atype,
            atype == 0x1702 ? "TEXTURE" : atype == 0x8D41 ? "RENDERBUFFER" :
            atype == 0 ? "NONE/backbuffer" : "?", aname);
}

// The last few binds, kept unconditionally and cheaply, because the question
// "which name did the guest ASK for?" can only be answered after the fact — by
// the time GL reports an incomplete framebuffer the bind is thousands of calls
// back, and turning on the full FBO trace to catch it buries the answer in a
// firehose. Deliberately lock-free and racy: a torn entry is still a name, and
// this is read only by a diagnostic that is already reporting a broken state.
#define KLFB_BINDLOG 12
static struct { uint32_t target, fb; uint64_t tid; const char *site; void *ctx; } g_bindlog[KLFB_BINDLOG];
static unsigned g_bindlog_n;
// Whether the REAL glBindFramebuffer produced an error, i.e. whether GL took the
// name. This is the whole of candidate (b): a refused bind leaves the previous
// binding in place and says nothing anywhere.
static uint32_t g_bindlog_err[KLFB_BINDLOG];
static __thread unsigned g_bindlog_slot;
static int glfb_errscan(void);

static void klfb_resync_fbo(uint32_t fb);   // below, with the rate map
static void klfb_rate_rule_for_fbo(uint32_t fb);
// Which guest FBOs render both eyes at once (glFramebufferTextureMultiviewOVR
// with more than one view) is g_fbo_multiview, above. On a mirrored guest these
// are the eye swapchain's, and the only framebuffers the rate map is meant for.
static void (*g_real_FramebufferTextureMultiviewOVR)(uint32_t, uint32_t, uint32_t, int32_t,
                                                      int32_t, int32_t);
static int klfb_msaa_plain_samples(void);
static void klfb_FramebufferTextureMultiviewOVR(uint32_t target, uint32_t attachment,
                                                uint32_t texture, int32_t level,
                                                int32_t base_view, int32_t num_views) {
    // A guest that attaches its eyes as PLAIN multiview and is given edge
    // smoothing by the setting (Doom 3 — see kl_glfb_multiview_ms): colour and
    // depth-stencil both come through here, so both get the same sample count.
    if (texture && num_views > 1 &&
        (attachment == 0x8CE0 /* COLOR_ATTACHMENT0 */ || attachment == 0x8D00 /* DEPTH */ ||
         attachment == 0x8D20 /* STENCIL */ || attachment == 0x821A /* DEPTH_STENCIL */)) {
        int ms = klfb_msaa_plain_samples();
        if (ms > 1 &&
            kl_glfb_multiview_ms(target, attachment, texture, level, ms, base_view, num_views))
            return;
    }
    if (attachment == 0x8CE0 /* COLOR_ATTACHMENT0 */ && g_draw_fb < KLFB_RESYNC_FBOS &&
        target != 0x8CA8 /* READ_FRAMEBUFFER */) {
        g_fbo_multiview[g_draw_fb] =
            (texture && num_views > 1) ? (uint8_t)(num_views > 255 ? 255 : num_views) : 0;
        klfb_rate_rule_for_fbo(g_draw_fb);
    }
    if (g_real_FramebufferTextureMultiviewOVR)
        g_real_FramebufferTextureMultiviewOVR(target, attachment, texture, level, base_view,
                                              num_views);
}
// The multisampled form of the same attachment, for a guest that asks for it
// (GL_OVR_multiview_multisampled_render_to_texture): the two views are drawn
// into a multisampled array and resolved into the texture's slices when the pass
// ends. That needs an ANGLE whose Metal backend has the layered multisampled
// pass (angle-patches/klepton.patch from this change on); an older one refuses
// the call, which the first attachment finds out and remembers.
//
// Off unless asked for: the attachment stays plain multiview, as it always was.
// In the app it is a SETTING (KleptonRendering, "Edge smoothing": off, 2x, 4x;
// off unless a person turns it on — the owner's call after playing it: at the
// display's full resolution 4x dropped a song to 60 Hz halfway through, so it
// is an option for a lower resolution, not a default), handed over before the
// guest boots: 2 or 4 is the sample count, anything else is off.
// KL_GL_MSAA_MV=0|1 overrides the setting (1 without one: the guest's own
// count), KL_GL_MSAA_MV_SAMPLES the sample count (2 or 4).
// Returns 1 when the attachment was made here.
static int g_msaa_default = -1;
void kl_glfb_set_msaa_default(int samples) {
    g_msaa_default = samples == 0 || samples == 2 || samples == 4 ? samples : -1;
}
// Which guests the setting is for, and how they attach their eyes:
//   1  asks for the multisampled attachment itself — Beat Saber from 1.45 on and
//      Maestro (both ask for 4 samples; read in their logs on the headset);
//   2  attaches plain multiview and is upgraded (klfb_FramebufferTextureMultiviewOVR).
//      Nobody by default. Doom 3 was, for one release (1.1.7): on the owner's
//      headset its picture flickered black now and then with it and stopped
//      doing so with the setting off, so it is out again ("erstmal für Doom AA
//      entfernen und auf off forcen"). Why is not known — the game uses the
//      stencil buffer and reopens its eye pass every frame, which is the part
//      of the restore no other guest exercises. KL_GL_MSAA_PLAIN=1 with
//      KL_GL_MSAA_MV=1 is the upgrade again, for finding out.
static int klfb_msaa_guest(void) {
    extern int kl_guest_beatsaber_openxr(void);
    extern const char *kl_driver_target_name(void);
    const char *t = kl_driver_target_name();
    if (kl_guest_beatsaber_openxr()) return 1;
    if (t && strcmp(t, "x6963344583787383") == 0) return 1;
    // DreamSpace: Unity 6 on OpenXR, SinglePassInstanced into an array swapchain
    // like Beat Saber 1.45 (klgl_want_msrtt), so it asks for the same attachment.
    if (t && strcmp(t, "x9449316705178989") == 0) return 1;
    (void)t;
    return kl_env_on("KL_GL_MSAA_PLAIN", 0) ? 2 : 0;
}
static int g_msaa_on = -1, g_msaa_forced, g_msaa_kind;
static void (*g_msaa_real)(uint32_t, uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t);
static void klfb_msaa_init(void) {
    if (g_msaa_on >= 0) return;
    g_msaa_kind   = klfb_msaa_guest();
    // DreamSpace: on unless switched off. Its foliage is cut out by
    // alpha-to-coverage, which on a single-sampled attachment leaves the cut-out
    // parts of a leaf card as solid brown patches (2026-10-16); the start window
    // offers no setting for it, so the default is the decision.
    extern const char *kl_driver_target_name(void);
    const char *t = kl_driver_target_name();
    int msaa_dflt = (g_msaa_kind && g_msaa_default > 0) ||
                    (t && strcmp(t, "x9449316705178989") == 0);
    g_msaa_on     = kl_env_on("KL_GL_MSAA_MV", msaa_dflt);
    g_msaa_forced = kl_env_int("KL_GL_MSAA_MV_SAMPLES", g_msaa_default > 0 ? g_msaa_default : 0);
    if (g_msaa_on) g_msaa_real = asym("glFramebufferTextureMultisampleMultiviewOVR");
    // Said once, whichever way it goes: a log then answers "was the picture
    // multisampled in this run" without the picture.
    const char *env = getenv("KL_GL_MSAA_MV");
    fprintf(stderr, "  [glfb] edge smoothing: %s — setting %d (2 or 4 samples, else off), "
                    "KL_GL_MSAA_MV %s, ANGLE entry point %s, guest kind %d\n",
            !g_msaa_on ? "off" : g_msaa_real ? "on" : "off, this ANGLE has no entry point for it",
            g_msaa_default, env ? env : "not set",
            g_msaa_real ? "found" : g_msaa_on ? "missing" : "not asked", g_msaa_kind);
}
// The sample count a plain multiview attachment is upgraded to, 0 for none.
static int klfb_msaa_plain_samples(void) {
    klfb_msaa_init();
    if (!g_msaa_on || !g_msaa_real || g_msaa_kind != 2) return 0;
    return g_msaa_forced > 0 ? g_msaa_forced : 4;
}
int kl_glfb_multiview_ms(uint32_t target, uint32_t attachment, uint32_t texture, int32_t level,
                         int32_t samples, int32_t base_view, int32_t num_views) {
    static int dead, said;
    klfb_msaa_init();
    const int on = g_msaa_on, forced = g_msaa_forced;
    void (*real)(uint32_t, uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t) = g_msaa_real;
    if (!on || !real || dead || !texture || samples <= 1) return 0;
    if (forced > 0) samples = forced;
    if (samples <= 1) return 0;
    if (attachment == 0x8CE0 /* COLOR_ATTACHMENT0 */ && g_draw_fb < KLFB_RESYNC_FBOS &&
        target != 0x8CA8 /* READ_FRAMEBUFFER */) {
        g_fbo_multiview[g_draw_fb] =
            num_views > 1 ? (uint8_t)(num_views > 255 ? 255 : num_views) : 0;
        klfb_rate_rule_for_fbo(g_draw_fb);
    }
    if (!said && a_glGetError) while (a_glGetError()) {}
    real(target, attachment, texture, level, samples, base_view, num_views);
    if (!said) {
        // Asked once, on the first attachment: is this an ANGLE that takes it?
        uint32_t err = a_glGetError ? a_glGetError() : 0;
        said = 1;
        if (err) {
            dead = 1;
            fprintf(stderr, "  [glfb] multisampled multiview: this ANGLE refuses it (0x%x) — "
                            "attached as plain multiview\n", err);
            return 0;
        }
        fprintf(stderr, "  [glfb] multisampled multiview: %d samples, %d views, attachment "
                        "0x%x (KL_GL_MSAA_MV)\n", samples, num_views, attachment);
    }
    return 1;
}
static void klfb_BindFramebuffer(uint32_t target, uint32_t fb) {
    if (klfb_trace_fbo())
        fprintf(stderr, "  [glfb] t%llu glBindFramebuffer(0x%x, %u) <- %s\n",
                (unsigned long long)klfb_tid(), target, fb,
                klfb_caller(__builtin_return_address(0)));
    {
        unsigned i = __atomic_fetch_add(&g_bindlog_n, 1, __ATOMIC_RELAXED) % KLFB_BINDLOG;
        g_bindlog[i].target = target;
        g_bindlog[i].fb     = fb;
        g_bindlog[i].tid    = klfb_tid();
        // klfb_caller's buffer is per-thread and reused, so it cannot be stored;
        // the raw return address can, and kl_addr_image resolves it at print time.
        g_bindlog[i].site   = (const char *)__builtin_return_address(0);
        g_bindlog[i].ctx    = kl_egl_current_context();
        g_bindlog_err[i]    = 0xffffffffu;   // "not asked"
        g_bindlog_slot      = i;
    }
    if (klfb_is_read_target(target)) g_read_fb = fb;
    if (klfb_is_draw_target(target)) {
        g_draw_fb = fb;
        // Which stage this frame is going into. Sticky: a guest that binds an
        // eye FBO and then bounces through others (shadow maps, post) has still
        // last committed to this stage, and the next eye bind is what moves it.
        klfb_note_render_stage(klfb_stage_of_fbo(fb));
    }
    if (g_real_BindFramebuffer) g_real_BindFramebuffer(target, fb);
    if (klfb_is_draw_target(target)) { klfb_rate_rule_for_fbo(fb); klfb_resync_fbo(fb); }
    // Did GL take the name? Only asked when ERRSCAN is already draining the
    // error queue every call — otherwise this would eat exactly the errors the
    // guest's own glGetError is looking for.
    if (glfb_errscan() && a_glGetError) {
        unsigned i = g_bindlog_slot % KLFB_BINDLOG;
        g_bindlog_err[i] = a_glGetError();
    }
}
// ---- a 2D framebuffer the guest draws into with MULTIVIEW programs ----
//
// Doom3Quest compiles every shader it has with `layout(num_views = 2)`, and
// then draws its HUD and its PDA with them into a plain 2D framebuffer of its
// own (neo/renderer/framebuffer.cpp: a GL_TEXTURE_2D colour attachment and a
// depth-stencil renderbuffer), from which idImage::CopyFramebuffer lifts a
// square with glCopyTexImage2D. OVR_multiview says a draw whose program and
// framebuffer disagree about the number of views is INVALID_OPERATION; the
// Quest's driver draws anyway, ANGLE does not (validationES.cpp,
// kMultiviewMismatch), and every one of those draws is dropped. On device,
// 2026-10-08: KL_GLFB_ERRSCAN names glDrawElements with 0x502, and the PDA is a
// black rectangle with a lit frame — the cleared framebuffer, copied faithfully.
//
// So such a framebuffer is given what the programs want. When the guest
// attaches a 2D texture as colour 0 of one of its framebuffers, a two-slice
// array of the same size is attached in its place with
// glFramebufferTextureMultiviewOVR, and a two-slice depth-stencil array where
// it attaches its renderbuffer; the guest's draws are then legal and land in
// both slices. Reading is the other half: a multiview framebuffer cannot be
// read at all, so glCopyTexImage2D / glCopyTexSubImage2D are served from slice
// 0 through a read framebuffer of our own. The guest's 2D texture itself is
// never written — nothing in this guest samples it, it only copies out.
//
// KL_GLFB_MV_SHADOW, on for doom3quest only: a guest that renders ordinary
// programs into its 2D framebuffers must be left exactly as it is.
static int klfb_mv_shadow_on(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        on = kl_env_on("KL_GLFB_MV_SHADOW", t && strcmp(t, "doom3quest") == 0);
    }
    return on;
}
typedef struct { uint32_t fbo, color, depth; int w, h; } klfb_mvs;
#define KLFB_MVS_MAX 8
static klfb_mvs g_mvs[KLFB_MVS_MAX];
static klfb_mvs *klfb_mvs_find(uint32_t fbo) {
    if (!fbo) return NULL;
    for (int i = 0; i < KLFB_MVS_MAX; i++)
        if (g_mvs[i].fbo == fbo) return &g_mvs[i];
    return NULL;
}
static klfb_mvs *klfb_mvs_make(uint32_t fbo, int w, int h) {
    klfb_mvs *m = klfb_mvs_find(fbo);
    if (m && m->w == w && m->h == h) return m;
    static void (*gen)(int32_t, uint32_t *);
    static void (*del)(int32_t, const uint32_t *);
    static void (*bind)(uint32_t, uint32_t);
    if (!gen)  gen  = asym("glGenTextures");
    if (!del)  del  = asym("glDeleteTextures");
    if (!bind) bind = asym("glBindTexture");
    if (!gen || !del || !bind || !g_real_TexStorage3D || !a_glGetIntegerv) return NULL;
    if (!m) {
        for (int i = 0; i < KLFB_MVS_MAX && !m; i++)
            if (!g_mvs[i].fbo) m = &g_mvs[i];
        if (!m) return NULL;
    } else {
        uint32_t old[2] = { m->color, m->depth };
        del(2, old);
    }
    int32_t save = 0;
    a_glGetIntegerv(0x8C1D /* TEXTURE_BINDING_2D_ARRAY */, &save);
    uint32_t t[2] = { 0, 0 };
    gen(2, t);
    bind(0x8C1A, t[0]);
    g_real_TexStorage3D(0x8C1A, 1, 0x8058 /* RGBA8 */, w, h, 2);
    bind(0x8C1A, t[1]);
    g_real_TexStorage3D(0x8C1A, 1, 0x88F0 /* DEPTH24_STENCIL8 */, w, h, 2);
    bind(0x8C1A, (uint32_t)save);
    m->fbo = fbo; m->color = t[0]; m->depth = t[1]; m->w = w; m->h = h;
    fprintf(stderr, "  [glfb] framebuffer %u is a 2D target the guest draws into with "
                    "multiview programs: %dx%d two-slice colour and depth-stencil "
                    "arrays stand in for its attachments (KL_GLFB_MV_SHADOW=0 leaves "
                    "it alone)\n", fbo, w, h);
    return m;
}
static void (*g_real_FramebufferRenderbuffer)(uint32_t, uint32_t, uint32_t, uint32_t);
static void klfb_FramebufferRenderbuffer(uint32_t target, uint32_t attachment,
                                         uint32_t rbtarget, uint32_t rb) {
    klfb_mvs *m = (klfb_mv_shadow_on() && klfb_is_draw_target(target))
                      ? klfb_mvs_find(g_draw_fb) : NULL;
    if (m && rb && g_real_FramebufferTextureMultiviewOVR &&
        (attachment == 0x8D00 /* DEPTH */ || attachment == 0x8D20 /* STENCIL */ ||
         attachment == 0x821A /* DEPTH_STENCIL */)) {
        g_real_FramebufferTextureMultiviewOVR(target, attachment, m->depth, 0, 0, 2);
        return;
    }
    if (g_real_FramebufferRenderbuffer)
        g_real_FramebufferRenderbuffer(target, attachment, rbtarget, rb);
}
// Slice 0 of a stand-in, bound for reading; returns whether it was.
static int klfb_mvs_read_begin(int32_t *saved) {
    klfb_mvs *m = klfb_mv_shadow_on() ? klfb_mvs_find(g_read_fb) : NULL;
    if (!m || !g_real_BindFramebuffer || !a_glGetIntegerv) return 0;
    static void (*genfb)(int32_t, uint32_t *);
    static void (*layer)(uint32_t, uint32_t, uint32_t, int32_t, int32_t);
    static uint32_t rfb;
    if (!genfb) genfb = asym("glGenFramebuffers");
    if (!layer) layer = asym("glFramebufferTextureLayer");
    if (!genfb || !layer) return 0;
    if (!rfb) genfb(1, &rfb);
    a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, saved);
    g_real_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, rfb);
    layer(0x8CA8, 0x8CE0, m->color, 0, 0);
    return 1;
}
static void (*g_real_CopyTexImage2D)(uint32_t, int32_t, uint32_t, int32_t, int32_t,
                                     int32_t, int32_t, int32_t);
static void klfb_CopyTexImage2D(uint32_t target, int32_t level, uint32_t ifmt, int32_t x,
                                int32_t y, int32_t w, int32_t h, int32_t border) {
    int32_t saved = 0;
    int redirected = klfb_mvs_read_begin(&saved);
    if (g_real_CopyTexImage2D) g_real_CopyTexImage2D(target, level, ifmt, x, y, w, h, border);
    if (redirected) g_real_BindFramebuffer(0x8CA8, (uint32_t)saved);
}
static void (*g_real_CopyTexSubImage2D)(uint32_t, int32_t, int32_t, int32_t, int32_t,
                                        int32_t, int32_t, int32_t);
static void klfb_CopyTexSubImage2D(uint32_t target, int32_t level, int32_t xo, int32_t yo,
                                   int32_t x, int32_t y, int32_t w, int32_t h) {
    int32_t saved = 0;
    int redirected = klfb_mvs_read_begin(&saved);
    if (g_real_CopyTexSubImage2D) g_real_CopyTexSubImage2D(target, level, xo, yo, x, y, w, h);
    if (redirected) g_real_BindFramebuffer(0x8CA8, (uint32_t)saved);
}

static void klfb_FramebufferTexture2D(uint32_t target, uint32_t attachment,
                                      uint32_t textarget, uint32_t texture,
                                      int32_t level) {
    if (klfb_trace_fbo())
        fprintf(stderr, "  [glfb] t%llu glFramebufferTexture2D(target=0x%x fb=%u "
                        "att=0x%x, tex=%u)\n",
                (unsigned long long)klfb_tid(), target,
                klfb_fb_for_target(target), attachment, texture);
    // Colour attachment 0 only: the eye texture is what the picture lands in,
    // and a depth or stencil attachment says nothing about which stage it is.
    if (klfb_is_draw_target(target) && attachment == 0x8CE0 /* COLOR_ATTACHMENT0 */) {
        klfb_note_fbo_color(g_draw_fb, texture, -1);
        klfb_map_fbo(g_draw_fb, texture);
        // Unity may attach per frame rather than bind a pre-built FBO, so the
        // attachment itself has to count as committing to a stage.
        // The capture's hint, and it is taken HERE and not at the bind above:
        // this call carries the texture, where a bind only carries an FBO name
        // whose attachment we have to look up in a map that a re-created
        // swapchain has already made a liar (klfb_note_eye_fbo).
        int st = klfb_stage_of_tex(texture);
        klfb_note_eye_fbo(st >= 0 ? g_draw_fb : 0, g_draw_fb);
        klfb_note_render_stage(st);
    }
    // The stand-in for a 2D colour target drawn into with multiview programs
    // (klfb_mv_shadow_on, above).
    if (klfb_mv_shadow_on() && klfb_is_draw_target(target) && attachment == 0x8CE0 &&
        textarget == 0x0DE1 /* TEXTURE_2D */ && texture && g_draw_fb &&
        g_real_FramebufferTextureMultiviewOVR) {
        uint32_t f = 0; int32_t tw = 0, th = 0;
        klfb_tex_info(texture, &f, &tw, &th);
        klfb_mvs *m = (tw > 0 && th > 0) ? klfb_mvs_make(g_draw_fb, tw, th) : NULL;
        if (m) {
            g_real_FramebufferTextureMultiviewOVR(target, attachment, m->color, 0, 0, 2);
            return;
        }
    }
    if (g_real_FramebufferTexture2D)
        g_real_FramebufferTexture2D(target, attachment, textarget, texture, level);
}

static void klfb_FramebufferTextureLayer(uint32_t target, uint32_t attachment,
                                         uint32_t texture, int32_t level,
                                         int32_t layer) {
    if (klfb_trace_fbo()) {
        uint32_t f = 0; int32_t tw = 0, th = 0;
        klfb_tex_info(texture, &f, &tw, &th);
        fprintf(stderr, "  [glfb] t%llu glFramebufferTextureLayer(target=0x%x "
                        "fb=%u att=0x%x tex=%u level=%d layer=%d) [%dx%d fmt=0x%x]"
                        " <- %s\n",
                (unsigned long long)klfb_tid(), target,
                klfb_fb_for_target(target), attachment, texture,
                level, layer, tw, th, f,
                klfb_caller(__builtin_return_address(0)));
    }
    if (attachment == 0x8CE0 /* COLOR_ATTACHMENT0 */) {
        uint32_t fb = klfb_fb_for_target(target);
        klfb_note_fbo_color(fb, texture, layer);
        klfb_map_fbo(fb, texture);
        if (klfb_is_draw_target(target)) {
            int st = klfb_stage_of_tex(texture);
            klfb_note_eye_fbo(st >= 0 ? fb : 0, fb);
            klfb_note_render_stage(st);
        }
    }
    // An attach to the READ target while the READ binding is 0 is
    // GL_INVALID_OPERATION — you cannot give the default framebuffer an
    // attachment — and it is what VRChat's Unity does for EVERY eye copy:
    //
    //   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 4)
    //   glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, COLOR0, tex29, 0, layer)
    //   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, <swapchain image>)
    //   glBlitFramebuffer(...)
    //
    // libunity's own error handler reports the INVALID_OPERATION and carries
    // on, the read binding stays 0, and the blit copies the default framebuffer
    // — which on this path is a window nothing draws into — over the eye. The
    // picture is really there: Unity's eye array reads 3.5M of 5.5M texels lit
    // at the same instant (KL_GLFB_PROBE_TEX).
    //
    // The repair is not a guess about what the guest meant, which is the line
    // this file does not cross: the attach STATES the source completely —
    // texture, level and array layer — and the only thing missing is an object
    // that may legally carry it. So the same attachment is made on a
    // framebuffer of ours and that is bound as READ, and the guest's next read
    // bind takes it away again, which is exactly the lifetime the guest gave
    // it. The illegal call is not forwarded: all it can do is set an error.
    //
    // Its own FBO, never the probe's: klfb_read_from_texture_layer's is
    // re-pointed on every probe, and a diagnostic that silently moved the
    // guest's blit source is an instrument corrupting what it measures.
    // KL_GLFB_READ_ATTACH_FIX=0 restores the failing configuration exactly.
    static int read_fix = -1;
    if (read_fix < 0) read_fix = kl_env_on("KL_GLFB_READ_ATTACH_FIX", 1);
    if (read_fix && target == 0x8CA8 /* READ_FRAMEBUFFER */ && g_read_fb == 0 &&
        texture && attachment == 0x8CE0 /* COLOR_ATTACHMENT0 */) {
        static void (*r_Gen)(int32_t, uint32_t *);
        static void (*r_Bind)(uint32_t, uint32_t);
        static uint32_t (*r_Check)(uint32_t);
        static int resolved;
        if (!resolved) {
            resolved = 1;
            r_Gen = asym("glGenFramebuffers");
            r_Bind = asym("glBindFramebuffer");
            r_Check = asym("glCheckFramebufferStatus");
        }
        static uint32_t fix_fb;
        if (!fix_fb && r_Gen) r_Gen(1, &fix_fb);
        if (fix_fb && r_Bind && g_real_FramebufferTextureLayer) {
            r_Bind(0x8CA8, fix_fb);
            g_real_FramebufferTextureLayer(0x8CA8, attachment, texture, level, layer);
            uint32_t st = r_Check ? r_Check(0x8CA8) : 0x8CD5;
            static int said;
            if (!said++)
                fprintf(stderr, "  [glfb] READ_ATTACH_FIX: the guest attached tex %u "
                                "layer %d to the READ target with read framebuffer 0; "
                                "carried it on fb %u instead (status 0x%x)\n",
                        texture, layer, fix_fb, st);
            if (st != 0x8CD5) r_Bind(0x8CA8, 0);   // no worse than what it had
            return;
        }
    }
    if (g_real_FramebufferTextureLayer)
        g_real_FramebufferTextureLayer(target, attachment, texture, level, layer);
}

// KL_GLFB_NOSWIZZLE=1 drops the four swizzle parameters on the floor. The guest's
// single-channel textures then sample as plain red instead of as GL_ALPHA, so the
// picture is wrong — but it answers whether swizzle is *necessary* to the AGX
// abort, which a standalone repro has so far failed to settle. The abort is
// deterministic (call #312, five runs of five), so one run each way is conclusive.
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#define GL_TEXTURE_SWIZZLE_A 0x8E45

// ---- KL_GLFB_NOMIP: sample level 0 only ------------------------------------
//
// KL_GLFB_NOMIP=1 demotes every MIPMAP min filter to its non-mipmapped base
// (LINEAR_MIPMAP_* -> LINEAR, NEAREST_MIPMAP_* -> NEAREST), on textures and on
// sampler objects both, so no sample in the run can come from anywhere but
// level 0.
//
// It exists for a title that reads a mip level it should not. Tetris Effect's
// well backdrop on this path is a fine particle field drawn as one magnifying
// quad; something drives its LOD far above 0, and at its top levels the texture
// has collapsed to its own average — a flat dark-blue fill over the scene. With
// level 0 forced the backdrop renders as it does on a Quest, and the steady-
// state frame time was unchanged on device. Why a magnifying draw consults the
// minification chain at all is not settled, which is why this is a knob and not
// a rule: forcing level 0 everywhere costs minification aliasing and texture
// bandwidth across the whole title. Default off.
static int klfb_nomip(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_NOMIP", 0);
    return on;
}
static int klfb_nomip_filter(uint32_t pname, int32_t param) {
    static unsigned said;
    if (pname != 0x2801 /* TEXTURE_MIN_FILTER */ || !klfb_nomip()) return param;
    int32_t out = param;
    if (param == 0x2700 || param == 0x2702) out = 0x2600;      // NEAREST_MIPMAP_*
    if (param == 0x2701 || param == 0x2703) out = 0x2601;      // LINEAR_MIPMAP_*
    if (out != param && said++ < 3)
        fprintf(stderr, "  [glfb] NOMIP: min filter 0x%04x -> 0x%04x (level 0 only; "
                        "KL_GLFB_NOMIP=0 restores the guest's choice)\n",
                (unsigned)param, (unsigned)out);
    return out;
}

// The other spellings of the min-filter write. They forward unchanged unless
// KL_GLFB_NOMIP is on.
static void (*g_real_TexParameterf)(uint32_t, uint32_t, float);
static void klfb_TexParameterf(uint32_t target, uint32_t pname, float param) {
    if (pname == 0x2801) param = (float)klfb_nomip_filter(pname, (int32_t)param);
    if (g_real_TexParameterf) g_real_TexParameterf(target, pname, param);
}
static void (*g_real_SamplerParameteri)(uint32_t, uint32_t, int32_t);
static void klfb_SamplerParameteri(uint32_t sampler, uint32_t pname, int32_t param) {
    param = klfb_nomip_filter(pname, param);
    if (g_real_SamplerParameteri) g_real_SamplerParameteri(sampler, pname, param);
}
static void (*g_real_SamplerParameterf)(uint32_t, uint32_t, float);
static void klfb_SamplerParameterf(uint32_t sampler, uint32_t pname, float param) {
    if (pname == 0x2801) param = (float)klfb_nomip_filter(pname, (int32_t)param);
    if (g_real_SamplerParameterf) g_real_SamplerParameterf(sampler, pname, param);
}

// ---- depth textures sampled with LINEAR, which ES leaves UNDEFINED ---------
//
// A depth texture is not filterable unless the driver says so. ES 3.0 §3.8.13:
// a texture whose internal format is DEPTH_COMPONENT* is incomplete for
// sampling if its minification or magnification filter is anything but NEAREST
// **and** its compare mode is NONE — and an incomplete texture samples as
// (0,0,0,1). Hardware PCF (compare mode COMPARE_REF_TO_TEXTURE) is the one case
// where LINEAR is legal, because the filtering then happens on the comparison
// rather than on the depth value.
//
// Quest drivers sample it anyway; Metal does not. The failure is silent — no GL
// error, no incomplete-framebuffer status, no link error: the shadow lookup
// returns 0.0, every `depth > shadowSample` test passes, and the whole world is
// in shadow. The Simpsons Hit & Run VR port is the case that found it: its
// cascades are depth-only targets sampled with "manual bilinear PCF" (four
// texels, lerped in the shader), which needs NEAREST; the LINEAR filter left on
// the texture is a leftover, not a filtering choice, and the level rendered
// black.
//
// So: for a texture this runtime knows to be depth-formatted, a LINEAR filter
// written while the compare mode is NONE is rewritten to NEAREST, and the swap
// is UNDONE if the guest later turns compare mode on (the order of the two
// writes is the guest's business, and a hardware-PCF texture is entitled to its
// LINEAR). Reported once per texture either way.
//
// KL_GLFB_DEPTH_LINEAR=1 leaves the guest's write alone — and in this fork that
// is still the default (KL_GLFB_DEPTH_LINEAR=0 turns the rewrite on). Both Beat
// Saber builds write LINEAR to a depth texture with compare mode NONE at start
// (seen on the Mac, 2026-10-13), so the rewrite changes what a verified game
// samples in a level, which only a headset shows. On once it has been seen there.
#define KLFB_MAX_DEPTHTEX 32
static struct {
    uint32_t name;
    int      cmp;                  // compare mode is COMPARE_REF_TO_TEXTURE
    int      want_min, want_mag;   // what the guest asked for
    int      forced;               // we rewrote at least one filter for it
    int      said;
} g_dtex[KLFB_MAX_DEPTHTEX];
static unsigned g_ndtex;

static int klfb_fmt_is_depth(uint32_t f) {
    return f == 0x81A5 /* DEPTH_COMPONENT16 */ || f == 0x81A6 /* DEPTH_COMPONENT24 */ ||
           f == 0x81A7 /* DEPTH_COMPONENT32 */ || f == 0x8CAC /* DEPTH_COMPONENT32F */ ||
           f == 0x88F0 /* DEPTH24_STENCIL8 */  || f == 0x8CAD /* DEPTH32F_STENCIL8 */ ||
           f == 0x1902 /* DEPTH_COMPONENT */;
}

static unsigned klfb_dtex_slot(uint32_t name) {
    for (unsigned i = 0; i < g_ndtex; i++) if (g_dtex[i].name == name) return i;
    if (g_ndtex >= KLFB_MAX_DEPTHTEX) return (unsigned)-1;
    g_dtex[g_ndtex].name = name;
    g_dtex[g_ndtex].want_min = g_dtex[g_ndtex].want_mag = 0x2600 /* NEAREST */;
    return g_ndtex++;
}

// Returns the parameter to actually pass. `name` is the bound texture.
static int32_t klfb_depth_filter_fix(uint32_t name, uint32_t pname, int32_t param) {
    static int off = -1;
    if (off < 0) off = kl_env_on("KL_GLFB_DEPTH_LINEAR", 1);
    if (off || !name) return param;
    if (pname != 0x884C /* TEXTURE_COMPARE_MODE */ &&
        pname != 0x2801 /* MIN_FILTER */ && pname != 0x2800 /* MAG_FILTER */)
        return param;
    uint32_t fmt = 0;
    if (!klfb_tex_info(name, &fmt, NULL, NULL) || !klfb_fmt_is_depth(fmt)) return param;
    unsigned i = klfb_dtex_slot(name);
    if (i == (unsigned)-1) return param;

    if (pname == 0x884C /* TEXTURE_COMPARE_MODE */) {
        int now_cmp = (param == 0x884E /* COMPARE_REF_TO_TEXTURE */);
        if (now_cmp && !g_dtex[i].cmp && g_dtex[i].forced) {
            // The guest wants hardware PCF after all: give the filters back.
            if (g_real_TexParameteri) {
                g_real_TexParameteri(0x0DE1, 0x2801 /* MIN_FILTER */, g_dtex[i].want_min);
                g_real_TexParameteri(0x0DE1, 0x2800 /* MAG_FILTER */, g_dtex[i].want_mag);
            }
            fprintf(stderr, "  [glfb] depth tex %u went into COMPARE_REF_TO_TEXTURE — "
                            "its LINEAR filters are legal now and have been restored\n",
                    name);
            g_dtex[i].forced = 0;
        }
        g_dtex[i].cmp = now_cmp;
        return param;
    }
    if (pname == 0x2801) g_dtex[i].want_min = param; else g_dtex[i].want_mag = param;
    // NEAREST and NEAREST_MIPMAP_NEAREST are the two complete choices; anything
    // else filters, and filtering a depth texture with compare mode NONE is the
    // undefined case.
    if (g_dtex[i].cmp) return param;
    if (param == 0x2600 /* NEAREST */ || param == 0x2700 /* NEAREST_MIPMAP_NEAREST */)
        return param;
    if (!g_dtex[i].said++)
        fprintf(stderr, "  [glfb] depth tex %u (fmt 0x%x) asked for filter 0x%x with "
                        "compare mode NONE — ES leaves that sample UNDEFINED and Metal "
                        "returns 0. Forced to NEAREST (KL_GLFB_DEPTH_LINEAR=1 restores "
                        "the guest's write)\n", name, fmt, param);
    g_dtex[i].forced = 1;
    return 0x2600 /* NEAREST */;
}

static void klfb_TexParameteri(uint32_t target, uint32_t pname, int32_t param) {
    if (klfb_trace_tex())
        fprintf(stderr, "  [glfb] #%u glTexParameteri target=0x%04x pname=0x%04x "
                        "param=0x%04x\n", g_texcalls++, target, pname, param);
    static int noswz = -1;
    if (noswz < 0) noswz = kl_env_on("KL_GLFB_NOSWIZZLE", 0);
    if (noswz && pname >= GL_TEXTURE_SWIZZLE_R && pname <= GL_TEXTURE_SWIZZLE_A) return;
    // TEXTURE_SRGB_DECODE_EXT writes: no EXT_texture_sRGB_decode on this ANGLE,
    // so the write can only raise INVALID_ENUM — and does, about once per
    // frame, leaving a sticky error that Unity then reports attributed to
    // innocent calls (and that error-bracketing probes read as everyone's
    // fault). Dropping it is semantics-preserving here: the decode behaviour
    // it would select does not exist on this driver either way.
    if (pname == 0x8A48 /* TEXTURE_SRGB_DECODE_EXT */) return;
    // The external-image retarget again — see klfb_detarget. The guest sets
    // filtering and wrap on GL_TEXTURE_EXTERNAL_OES right after binding it, and
    // those calls have to land on the same object the bind did.
    if (target == 0x8D65 /* TEXTURE_EXTERNAL_OES */) target = 0x0DE1;
    param = klfb_nomip_filter(pname, param);
    // The depth-texture completeness rule, which needs to know WHICH texture
    // is bound rather than only what was asked for.
    if (target == 0x0DE1 /* TEXTURE_2D */ && a_glGetIntegerv &&
        (pname == 0x2801 || pname == 0x2800 || pname == 0x884C)) {
        int32_t bound = 0;
        a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &bound);
        if (bound > 0) param = klfb_depth_filter_fix((uint32_t)bound, pname, param);
    }
    if (g_real_TexParameteri) g_real_TexParameteri(target, pname, param);
    if (kl_env_on("KL_GLFB_ERRPROBE", 0) && a_glGetError) {
        uint32_t e = a_glGetError();
        if (e) fprintf(stderr, "  [glfb] glTexParameteri(0x%04x, 0x%04x) -> err 0x%x\n",
                       pname, param, e);
    }
}

// KL_GLFB_TEX_LIMIT=N performs only the first N uploads and drops the rest. The
// guest's texture stream is deterministic — every perturbation tried so far leaves
// it at exactly 327 calls — so it can be bisected by index, which is what the
// standalone replays could not do: they reproduced the calls but not the context
// around them. Binary-searching N finds the upload the abort actually needs, in
// situ. Storage allocations are always performed, so the textures still exist and
// only their contents go missing.
static int klfb_upload_budget(void) {
    static int n = -2;
    if (n == -2) {
        n = kl_env_int("KL_GLFB_TEX_LIMIT", -1);
    }
    return n;
}
static unsigned g_uploads;

static void klfb_TexSubImage2D(uint32_t target, int32_t level, int32_t xoff,
                               int32_t yoff, int32_t w, int32_t h, uint32_t format,
                               uint32_t type, const void *pixels) {
    int budget = klfb_upload_budget();
    unsigned idx = g_uploads++;
    if (budget >= 0 && (int)idx >= budget) {
        if (klfb_trace_tex())
            fprintf(stderr, "  [glfb] upload %u DROPPED (budget %d)\n", idx, budget);
        return;
    }
    if (klfb_trace_tex())
        fprintf(stderr, "  [glfb] #%u glTexSubImage2D target=0x%04x level=%d "
                        "%dx%d at %d,%d fmt=0x%04x type=0x%04x%s\n",
                g_texcalls++, target, level, w, h, xoff, yoff, format, type,
                pixels ? "" : " (NULL)");
    if (g_real_TexSubImage2D)
        g_real_TexSubImage2D(target, level, xoff, yoff, w, h, format, type, pixels);
}

static void klfb_TexStorage2D(uint32_t target, int32_t levels, uint32_t fmt,
                              int32_t w, int32_t h) {
    klfb_note_format(fmt, w, h, 1, "glTexStorage2D");
    if (klfb_trace_tex()) {
        int32_t bound = -1;
        if (a_glGetIntegerv) a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &bound);
        fprintf(stderr, "  [glfb] #%u glTexStorage2D target=0x%04x levels=%d "
                        "fmt=0x%04x %dx%d (tex=%d)\n", g_texcalls++, target,
                levels, fmt, w, h, bound);
    }
    // The guest's own format: its compressed uploads name this one.
    klfb_cimg_note_storage(target, levels, fmt, w, h);
    fmt = klfb_maybe_unsrgb(fmt);
    // Record the allocation per texture name: ES 3.0 has no
    // glGetTexLevelParameteriv (the ANGLE ext entry point rejects on this
    // context — the census's fmt=0x0 readings), so the readback path learns
    // what a texture IS from this table instead of by query.
    if (a_glGetIntegerv) {
        int32_t bound = -1;
        a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &bound);
        if (bound > 0) {
            klfb_note_tex_storage((uint32_t)bound, fmt, w, h);
            klfb_vram_note(KLC_TEX, (uint32_t)bound,
                           klfb_storage_bytes(fmt, w, h, 1, levels));
        }
    }
    if (g_real_TexStorage2D) g_real_TexStorage2D(target, levels, fmt, w, h);
}
static void klfb_TexStorage3D(uint32_t target, int32_t levels, uint32_t fmt,
                              int32_t w, int32_t h, int32_t d) {
    klfb_note_format(fmt, w, h, d, "glTexStorage3D");
    fmt = klfb_maybe_unsrgb(fmt);
    if (a_glGetIntegerv) {
        // 2D_ARRAY and 3D have their own binding points; the target says which.
        int32_t bound = -1;
        a_glGetIntegerv(target == 0x8C1A /* TEXTURE_2D_ARRAY */ ? 0x8C1D
                                                               : 0x806A /* 3D */,
                        &bound);
        if (bound > 0) {
            // The 2D thunk's record, for arrays too: an eye texture that is one
            // array with a slice per eye is the ONLY shape an OpenXR guest in
            // single-pass-instanced layout has, and without this klfb_tex_info
            // answers "never heard of it" for exactly the texture the capture
            // and every trace here are about.
            klfb_note_tex_storage((uint32_t)bound, fmt, w, h);
            if (klfb_census_every())
                klfb_vram_note(KLC_TEX, (uint32_t)bound,
                               klfb_storage_bytes(fmt, w, h, d, levels));
        }
    }
    if (g_real_TexStorage3D) g_real_TexStorage3D(target, levels, fmt, w, h, d);
}

// glTexStorage2DMultisample is ES 3.1, and the ANGLE context behind this
// renderer is ES 3.0: the core entry point refuses on the version, so the
// texture is left without storage. ANGLE serves the same allocation for an
// ES 3.0 context as GL_ANGLE_texture_multisample (the TEXTURE_2D_MULTISAMPLE
// bind and the attachment are already accepted through it), under the name
// glTexStorage2DMultisampleANGLE with identical arguments.
//
// Synth Riders (Unity 6000.3.13f1) allocates its camera colour target this way
// and draws the whole scene into it, with a multisampled depth renderbuffer:
// the framebuffer was GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT, every draw was
// GL_INVALID_FRAMEBUFFER_OPERATION, and the resolve copied an empty target
// into the eyes — black, with the music playing. KL_GLFB_TEX_MS_ANGLE=0|1,
// default 1 for x2436558143118760 only.
static void (*g_real_TexStorage2DMultisample)(uint32_t, int32_t, uint32_t, int32_t,
                                              int32_t, uint8_t);
static int klfb_tex_ms_angle(void) {
    static int v = -1;
    if (v < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        v = kl_env_on("KL_GLFB_TEX_MS_ANGLE", t && strcmp(t, "x2436558143118760") == 0);
    }
    return v;
}
static void klfb_TexStorage2DMultisample(uint32_t target, int32_t samples, uint32_t fmt,
                                         int32_t w, int32_t h, uint8_t fixed) {
    static void (*ext)(uint32_t, int32_t, uint32_t, int32_t, int32_t, uint8_t);
    static int looked, said;
    if (klfb_tex_ms_angle()) {
        if (!looked) {
            looked = 1;
            ext = asym("glTexStorage2DMultisampleANGLE");
            if (!ext && a_eglGetProcAddress)
                ext = a_eglGetProcAddress("glTexStorage2DMultisampleANGLE");
        }
        if (said < 4) {
            said++;
            fprintf(stderr, "  [glfb] glTexStorage2DMultisample fmt=0x%04x %dx%d samples=%d "
                            "-> %s (KL_GLFB_TEX_MS_ANGLE)\n", fmt, w, h, samples,
                    ext ? "glTexStorage2DMultisampleANGLE" : "no ANGLE entry point, as before");
        }
        if (ext) { ext(target, samples, fmt, w, h, fixed); return; }
    }
    if (g_real_TexStorage2DMultisample)
        g_real_TexStorage2DMultisample(target, samples, fmt, w, h, fixed);
}

void kl_glfb_report_formats(void) {
    if (!g_nfmts) return;
    fprintf(stderr, "  [glfb] immutable texture formats allocated:");
    for (unsigned i = 0; i < g_nfmts; i++)
        fprintf(stderr, " 0x%04x=%u", g_fmts[i].fmt, g_fmts[i].n);
    fprintf(stderr, "\n");
}

// ---------------------------------------------------------- ABI thunks
//
// Per-texture storage record, written by the glTexStorage2D thunk above and
// read by the capture: ES 3.0 has no glGetTexLevelParameteriv (the ANGLE
// extension entry point rejects on this context), so what a texture *is* —
// format and size — has to come from watching the allocation. Cold path, and
// appends are serialised with the compile lock because the eye-texture setup
// arrives on a different thread than some of the guest's own storage calls.
// 4096 rather than 512 because VRChat allocates over 1400 textures and the
// eye render targets are among the LAST — a table that fills drops exactly the
// entries the readback path is about, and every consumer then reads "never
// heard of it". It still has a ceiling, so it names its first drop:
// a full table is a diagnostic going quiet.
#define KLFB_MAX_TEX 4096
static struct { uint32_t name, fmt; int32_t w, h; } g_tex[KLFB_MAX_TEX];
static unsigned g_ntex;

static void klfb_note_tex_storage(uint32_t name, uint32_t fmt, int32_t w,
                                  int32_t h) {
    pthread_mutex_lock(&g_compile_lock);
    for (unsigned i = 0; i < g_ntex; i++)
        if (g_tex[i].name == name) {          // reallocation replaces
            g_tex[i].fmt = fmt; g_tex[i].w = w; g_tex[i].h = h;
            pthread_mutex_unlock(&g_compile_lock);
            return;
        }
    if (g_ntex < KLFB_MAX_TEX) {
        g_tex[g_ntex].name = name; g_tex[g_ntex].fmt = fmt;
        g_tex[g_ntex].w = w; g_tex[g_ntex].h = h;
        g_ntex++;
    } else {
        static int said_full;
        if (!said_full++)
            fprintf(stderr, "  [glfb] the texture allocation table is full at %d "
                            "entries — every texture from here on (tex %u is the "
                            "first) is unknown to the readback paths, which will "
                            "report the size rather than guess\n",
                    KLFB_MAX_TEX, name);
    }
    pthread_mutex_unlock(&g_compile_lock);
}

static int klfb_tex_info(uint32_t name, uint32_t *fmt, int32_t *w, int32_t *h) {
    int found = 0;
    pthread_mutex_lock(&g_compile_lock);
    for (unsigned i = 0; i < g_ntex; i++)
        if (g_tex[i].name == name) {
            if (fmt) *fmt = g_tex[i].fmt;
            if (w) *w = g_tex[i].w;
            if (h) *h = g_tex[i].h;
            found = 1;
            break;
        }
    pthread_mutex_unlock(&g_compile_lock);
    return found;
}

// A decoded movie frame into a guest texture — the GLES half of UE4's
// MediaPlayer14. On Android the Java player draws its SurfaceTexture into the
// texture the engine names (getVideoLastFrame(int)); here the frame is already
// plain pixels, so it is an upload. Clipped to the texture's own extent where
// that is known: a movie that changes size is re-described to the engine one
// tick later than its first frame arrives, and an upload larger than the
// storage is an error that leaves the texture untouched.
// Whether a name is a texture of the context current on this thread.
int kl_glfb_is_texture(uint32_t tex) {
    static uint8_t (*is)(uint32_t);
    if (!is) is = asym("glIsTexture");
    return is ? is(tex) != 0 : 0;
}

int kl_glfb_upload_picture(uint32_t tex, int w, int h, const void *pixels, int bgra) {
    static void (*bind)(uint32_t, uint32_t);
    static void (*sub)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                       uint32_t, uint32_t, const void *);
    static void (*store)(uint32_t, int32_t);
    static void (*bindbuf)(uint32_t, uint32_t);
    if (!bind) {
        bind = asym("glBindTexture");
        sub = asym("glTexSubImage2D");
        store = asym("glPixelStorei");
        bindbuf = asym("glBindBuffer");
    }
    if (!tex || !pixels || w <= 0 || h <= 0 || !bind || !sub || !store || !a_glGetIntegerv) return 0;

    int32_t tw = 0, th = 0, cw = w, ch = h;
    uint32_t tfmt = 0;
    if (klfb_tex_info(tex, &tfmt, &tw, &th) && tw > 0 && th > 0) {
        if (cw > tw) cw = tw;
        if (ch > th) ch = th;
    }
    // GLES takes RGBA; the decoder's frames are BGRA. Swapped into a buffer
    // kept per thread — a movie plays on one thread, and a frame a tick is a
    // megabyte or eight that should not be allocated each time.
    static __thread uint8_t *scratch;
    static __thread size_t scratch_n;
    const uint8_t *src = pixels;
    if (bgra) {
        size_t need = (size_t)w * (size_t)ch * 4;
        if (need > scratch_n) {
            uint8_t *grown = realloc(scratch, need);
            if (!grown) return 0;
            scratch = grown; scratch_n = need;
        }
        const uint8_t *s = pixels;
        for (size_t i = 0; i < need; i += 4) {
            scratch[i] = s[i + 2]; scratch[i + 1] = s[i + 1];
            scratch[i + 2] = s[i]; scratch[i + 3] = s[i + 3];
        }
        src = scratch;
    }
    int32_t prev = 0, align = 4, rowlen = 0, pbo = 0;
    a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &prev);
    a_glGetIntegerv(0x0CF5 /* UNPACK_ALIGNMENT */, &align);
    a_glGetIntegerv(0x0CF2 /* UNPACK_ROW_LENGTH */, &rowlen);
    a_glGetIntegerv(0x88EF /* PIXEL_UNPACK_BUFFER_BINDING */, &pbo);
    if (pbo && bindbuf) bindbuf(0x88EC, 0);
    store(0x0CF5, 1);
    store(0x0CF2, w);
    bind(0x0DE1, tex);
    // Said for the first few uploads only, with the error the upload itself
    // raised: a movie that plays into a texture of another format or size is
    // black with every call succeeding as far as the guest can tell.
    static int said;
    uint32_t before = (said < 4 && a_glGetError) ? a_glGetError() : 0;
    sub(0x0DE1, 0, 0, 0, cw, ch, 0x1908 /* RGBA */, 0x1401 /* UNSIGNED_BYTE */, src);
    if (said < 4) {
        said++;
        uint32_t err = a_glGetError ? a_glGetError() : 0;
        fprintf(stderr, "  [glfb] picture %dx%d -> texture %u (%s 0x%04x %dx%d), %dx%d sent, "
                        "GL error 0x%x (pending before: 0x%x), unpack buffer %d\n",
                w, h, tex, tw ? "allocated" : "not in the allocation table", tfmt, tw, th,
                cw, ch, err, before, pbo);
    }
    bind(0x0DE1, (uint32_t)prev);
    store(0x0CF2, rowlen);
    store(0x0CF5, align);
    if (pbo && bindbuf) bindbuf(0x88EC, (uint32_t)pbo);
    return 1;
}

// The eye textures, by name, as kl_ovrp's SetupEyeTexture2 allocates them
// (latest registration wins — Unity re-creates them on resize). The capture
// finds "the framebuffer with the picture" by looking for the FBO whose
// color attachment is one of these.
static uint32_t g_eye_tex[2];

// ...and by (eye, stage), which is what the stage observation needs. Each stage
// is a distinct GL texture name — Unity asks for storage per (eye, stage)
// through ovrp_SetupEyeTexture2 — so the name the guest draws into IS the
// stage, with nothing to infer.
#define KLFB_MAX_STAGES 4
static uint32_t g_eye_tex_stage[2][KLFB_MAX_STAGES];
static int      g_eye_ever;   // an eye texture was registered at some point

void kl_glfb_note_eye_texture(int eye, int stage, uint32_t tex) {
    if (eye < 0 || eye > 1 || !tex) return;
    g_eye_tex[eye] = tex;
    // An eye pair is what distinguishes a VR guest from a flat one, and it is
    // the stronger signal of the two kl_present watches: Unity creates an EGL
    // window surface as well, being an Android app, so the window alone cannot
    // tell them apart. See kl_present.h.
    kl_present_note_eye_texture();
    if ((unsigned)stage >= KLFB_MAX_STAGES) return;
    // GL recycles names, and Unity re-creates the whole eye swapchain
    // on resize. A name that meant some other (eye, stage) must stop answering
    // for it the moment it is re-registered: klfb_stage_of_tex
    // returns the FIRST match, so one stale entry files a frame's pose against
    // a different stage's picture for the rest of the run — permanently, and
    // silently, because every count still looks healthy.
    for (int e = 0; e < 2; e++)
        for (int s = 0; s < KLFB_MAX_STAGES; s++)
            if (g_eye_tex_stage[e][s] == tex) g_eye_tex_stage[e][s] = 0;
    g_eye_tex_stage[eye][stage] = tex;
    g_eye_ever = 1;
}

// Which of an eye's images the guest most recently PRESENTED.
//
// kl_glfb_note_eye_texture registers a whole swapchain, and the last call wins,
// so g_eye_tex ends up naming whichever image was registered last — image 2 of
// 3, i.e. the right picture one frame in three. That is fine for "is this an
// eye texture at all" and useless for "read the frame the guest just drew".
// The OpenXR path knows exactly which image that is (xrReleaseSwapchainImage
// says so) and says it here, so the capture reads the presented image rather
// than whichever one the rotation happens to be pointing at.
// ...and everything else the capture would otherwise have to GUESS about it.
//
// The name alone was not enough, twice over. (a) The size: kl_glfb learns a
// texture's dimensions from its own glTexStorage2D thunk, and an OpenXR
// swapchain is allocated through kl_egl_sym — the real ANGLE entry point —
// so klfb_tex_info has never heard of it. The capture then kept the pbuffer's
// size and read a 2290x2400 eye as 1832x1920, which comes back as the window.
// (b) The LAYER: this guest's eye swapchain is ONE texture with two array
// slices, so both eyes share a name and differ only by layer, and
// glFramebufferTexture2D cannot attach a slice of a 2D array at all.
//
// So the OpenXR path states the whole thing — name, size, layer — because it
// is the one place that knows it, and the capture stops searching. It is not a
// search because a GL name is a slot, not an identity: names are reissued.
static struct { uint32_t tex; int32_t w, h; int layer; } g_live_eye[2];

void kl_glfb_set_live_eye_image(int eye, uint32_t tex, int32_t w, int32_t h, int layer) {
    if (eye < 0 || eye > 1 || !tex) return;
    g_eye_tex[eye] = tex;
    g_live_eye[eye].tex = tex;
    g_live_eye[eye].w = w;
    g_live_eye[eye].h = h;
    g_live_eye[eye].layer = layer;
}

void kl_glfb_set_live_eye_texture(int eye, uint32_t tex) {
    kl_glfb_set_live_eye_image(eye, tex, 0, 0, -1);
}

// Which stage is the current draw target, and which was last drawn into.
//
// Two thunks maintain this between them: glFramebufferTexture2D says which
// texture an FBO draws into, glBindFramebuffer says which FBO is current. The
// map is tiny and the lookups are linear because there are two or three eye
// FBOs in this title and the alternative is a hash table for six entries.
//
// The map stores the TEXTURE, not the stage it resolved to at the time. The
// texture is the identity; the stage is a fact about the texture that changes
// when Unity re-creates the swapchain, and an FBO whose attachment is unchanged
// must follow it. Caching the resolved stage instead leaves entries that
// outlive their meaning and answer confidently afterwards.
#define KLFB_FBO_MAP 16
static struct { uint32_t fbo, tex; } g_fbo_stage[KLFB_FBO_MAP];
static int      g_render_stage = -1;    // the last eye stage actually bound for drawing

int kl_glfb_last_render_stage(void) {
    return __atomic_load_n(&g_render_stage, __ATOMIC_RELAXED);
}

// The stage an eye texture name belongs to, or -1.
static int klfb_stage_of_tex(uint32_t tex) {
    if (!tex) return -1;
    for (int e = 0; e < 2; e++)
        for (int s = 0; s < KLFB_MAX_STAGES; s++)
            if (g_eye_tex_stage[e][s] == tex) return s;
    return -1;
}

// How often the sixteen-slot map could not answer and the colour table did,
// how often an eye FBO found no free slot, and how many slots a released eye
// texture gave back. Printed by kl_glfb_eye_map_census.
static uint64_t g_stage_via_color;
static uint64_t g_map_full;
static uint64_t g_map_freed;
static uint64_t g_forget_refused;

static int klfb_stage_of_fbo(uint32_t fbo) {
    // A hit that RESOLVES wins; a hit that resolves to -1 does not end the
    // search. The narrow map's row can name a texture that has since stopped
    // being an eye texture while the colour table has the FBO's current
    // attachment, and returning the stale -1 there loses a frame that was
    // drawn into a perfectly good eye stage.
    for (int i = 0; i < KLFB_FBO_MAP; i++)
        if (g_fbo_stage[i].fbo == fbo) {
            int s = klfb_stage_of_tex(g_fbo_stage[i].tex);
            if (s >= 0) return s;
            break;
        }
    // The fallback. g_fbo_stage is the narrower of two tables filled from the
    // SAME two call sites: klfb_note_fbo_color takes every framebuffer into 48
    // slots, klfb_map_fbo only those whose colour attachment is a registered
    // eye texture AT THE MOMENT OF ATTACHMENT, into 16. A guest that re-creates
    // its eye swapchain gives the narrow one two ways to miss: an attachment
    // made before the texture is registered is refused a slot, and sixteen
    // slots do not go far once several generations of eye FBOs have each taken
    // one. The texture is the identity either way — klfb_stage_of_tex is still
    // the only authority on what a name means — so consulting the wider table
    // can only add resolutions the narrow one should have had.
    for (unsigned i = 0; i < g_nfbo_color; i++)
        if (g_fbo_color[i].fbo == fbo) {
            int s = klfb_stage_of_tex(g_fbo_color[i].tex);
            if (s >= 0 && __atomic_fetch_add(&g_stage_via_color, 1, __ATOMIC_RELAXED) == 0)
                fprintf(stderr, "  [glfb] fb %u resolved to eye stage %d through the "
                                "colour table — the sixteen-slot eye-FBO map did not "
                                "have it (counted in the eyemap census)\n", fbo, s);
            return s;
        }
    return -1;
}

static void klfb_map_fbo(uint32_t fbo, uint32_t tex) {
    if (!fbo) return;
    for (int i = 0; i < KLFB_FBO_MAP; i++)
        if (g_fbo_stage[i].fbo == fbo) { g_fbo_stage[i].tex = tex; return; }
    // Only eye FBOs earn one of the sixteen slots. This title has plenty of
    // other framebuffers, and remembering the first sixteen it happens to
    // create would leave no room for the ones the observation is about.
    if (klfb_stage_of_tex(tex) < 0) return;
    for (int i = 0; i < KLFB_FBO_MAP; i++)
        if (!g_fbo_stage[i].fbo) { g_fbo_stage[i].fbo = fbo;
                                   g_fbo_stage[i].tex = tex; return; }
    // Full. Not fatal — klfb_stage_of_fbo falls back to g_fbo_color — but
    // counted, because it is how a re-created swapchain used to lose every
    // frame it drew.
    if (__atomic_fetch_add(&g_map_full, 1, __ATOMIC_RELAXED) == 0)
        fprintf(stderr, "  [glfb] eye-FBO map full (%d slots) at fb %u — the stage "
                        "lookup falls back to the colour table\n", KLFB_FBO_MAP, fbo);
}

// Forgetting, which is the half that was missing.
//
// kl_glfb_note_eye_texture drops a stale (eye, stage) when a NAME is
// re-registered, but nothing dropped one when the storage was DESTROYED, and
// nothing ever gave back an FBO slot. Between ovrp_DestroyEyeTexture and the
// next ovrp_SetupEyeTexture2 there is a window six textures wide on a Unity
// swapchain reconfigure; in it, klfb_stage_of_tex still answers for names that
// no longer mean anything, and GL hands those names straight back out. And the
// sixteen FBO slots are filled once per swapchain generation and never
// reclaimed, so the map saturates after a few reconfigures and stops recording
// the eye FBOs it exists for.
//
// That second half is a visible bug, not just a skewed count. Beat Saber
// re-creates its eye swapchain at boot and twice more at its main menu
// (3072x2464 <-> 3686x2957, ~25-35 s in). After the last one the new eye FBOs
// find no slot, every frame they draw is booked as "drew into no eye stage",
// klovrp_end_frame_impl drops its record, and the stages stop being filed: the
// `[cp] stages` serial freezes for two of three stages while the guest keeps
// rendering, and the compositor warps a picture that keeps getting older — the
// room shows at the edges on a quick head turn.
static void klfb_forget_fbo(uint32_t fbo) {
    if (!fbo) return;
    for (int i = 0; i < KLFB_FBO_MAP; i++)
        if (g_fbo_stage[i].fbo == fbo) { g_fbo_stage[i].fbo = 0;
                                         g_fbo_stage[i].tex = 0; }
}

// Frees every slot whose attachment is `tex`; returns how many.
static int klfb_forget_fbo_tex(uint32_t tex) {
    int n = 0;
    if (!tex) return 0;
    for (int i = 0; i < KLFB_FBO_MAP; i++)
        if (g_fbo_stage[i].tex == tex) { g_fbo_stage[i].fbo = 0;
                                         g_fbo_stage[i].tex = 0; n++; }
    __atomic_fetch_add(&g_map_freed, (uint64_t)n, __ATOMIC_RELAXED);
    return n;
}

// `released` is the GL name whose storage is going away, or 0 when the caller
// cannot say. The registration is only dropped when it still names THAT
// texture: the "replaced with no teardown" backstop in
// kl_glfb_bind_eye_mtl_texture releases the slot's OLD texture after the new
// name has already been registered, and clearing unconditionally there would
// wipe the registration made one line earlier — every frame after it would be
// unattributable. The ordinary ovrp_DestroyEyeTexture path releases while the
// slot still holds the old name, where the guard agrees and the clear happens.
// Returns how many FBO slots the release gave back.
static int klfb_forget_eye_stage(int eye, int stage, uint32_t released) {
    if ((unsigned)eye > 1 || (unsigned)stage >= KLFB_MAX_STAGES) return 0;
    uint32_t tex = g_eye_tex_stage[eye][stage];
    if (released && tex && tex != released) {
        __atomic_fetch_add(&g_forget_refused, 1, __ATOMIC_RELAXED);
        // The FBO rows for the DEAD name are still stale, though, and leaving
        // them saturates the sixteen slots one generation at a time.
        return klfb_forget_fbo_tex(released);
    }
    g_eye_tex_stage[eye][stage] = 0;
    // An FBO whose attachment just died is unattributable, not stage-something:
    // klfb_stage_of_fbo would resolve it through a texture with no stage and
    // answer -1 anyway, so freeing the slot loses nothing and un-saturates the
    // map for the swapchain generation that follows.
    return klfb_forget_fbo_tex(tex);
}

// The association state, for the exit report and kl_ovrp's periodic report.
// One line: registered names, slots in use, and the counters above.
void kl_glfb_eye_map_census(FILE *f) {
    if (!g_eye_ever) return;     // a guest with no GL eye textures has no map
    int used = 0;
    for (int i = 0; i < KLFB_FBO_MAP; i++) used += g_fbo_stage[i].fbo != 0;
    fprintf(f, "  [eyemap] eye0[%u %u %u] eye1[%u %u %u] | fbo map %d/%d slots | "
               "%llu freed by releases, %llu full refusals, %llu colour-table "
               "lookups, %llu releases kept a re-registered slot\n",
            g_eye_tex_stage[0][0], g_eye_tex_stage[0][1], g_eye_tex_stage[0][2],
            g_eye_tex_stage[1][0], g_eye_tex_stage[1][1], g_eye_tex_stage[1][2],
            used, KLFB_FBO_MAP,
            (unsigned long long)g_map_freed, (unsigned long long)g_map_full,
            (unsigned long long)g_stage_via_color,
            (unsigned long long)g_forget_refused);
}

// --- The observation window -------------------------------------------------
//
// `g_render_stage` alone is a sticky global, and a sticky global answers even
// when nothing happened. That is the whole weakness of the association: at
// ovrp_EndFrame it reports *a* stage whether or not this frame drew into one,
// so a guest whose draws land outside the Begin..End window — a different
// thread, an entry point these thunks do not watch — gets the previous frame's
// answer with no way to tell.
//
// So the window is explicit. kl_ovrp opens one at BeginFrame and reads it at
// EndFrame, and what comes back is not just "which stage" but *how many* eye
// binds happened inside the window and on which thread. Those two numbers turn
// three different failures into three different readings:
//
//   binds == 0        nothing drew into an eye texture between Begin and End.
//                     The stage below is the PREVIOUS frame's, i.e. exactly the
//                     off-by-one that pairs a fresh pose with a stale picture.
//   two stages set    the frame committed to more than one stage, so "one
//                     stage per frame" — which the pose record assumes — is
//                     false for this title.
//   tid != EndFrame's the draws are on another thread and the window bounds
//                     nothing; ordering, not observation, is the problem.
//
// Counted rather than sampled, because the artefact this is chasing alternates:
// a probe that fires every N frames sees one phase of it and reports it as the
// steady state.
static uint32_t g_render_mask;          // bit per stage drawn into, this window
static uint32_t g_render_binds;         // eye binds this window
static uint64_t g_render_tid;           // who did the last of them
static uint64_t g_stage_binds[KLFB_MAX_STAGES];   // ...and for the whole run

// pthread_threadid_np is cheap but not free, and this sits under every
// glBindFramebuffer. Once per thread is enough — the id cannot change.
static uint64_t klfb_tid_cached(void) {
    static __thread uint64_t t;
    if (!t) pthread_threadid_np(NULL, &t);
    return t;
}

void kl_glfb_begin_render_window(void) {
    __atomic_store_n(&g_render_mask, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_render_binds, 0, __ATOMIC_RELAXED);
}

int kl_glfb_render_stages(uint32_t *mask, uint32_t *binds, uint64_t *tid) {
    if (mask)  *mask  = __atomic_load_n(&g_render_mask, __ATOMIC_RELAXED);
    if (binds) *binds = __atomic_load_n(&g_render_binds, __ATOMIC_RELAXED);
    if (tid)   *tid   = __atomic_load_n(&g_render_tid, __ATOMIC_RELAXED);
    return __atomic_load_n(&g_render_stage, __ATOMIC_RELAXED);
}

uint64_t kl_glfb_stage_draw_count(int stage) {
    if ((unsigned)stage >= KLFB_MAX_STAGES) return 0;
    return __atomic_load_n(&g_stage_binds[stage], __ATOMIC_RELAXED);
}

// The framebuffer the guest ITSELF last drove with an eye texture attached —
// and the reason the capture cannot just scan for one.
//
// A guest that re-creates its eye swapchain (1.40 does, 2290x2400 ->
// 2748x2880) deletes the old textures, and GL reissues the very same names to
// the new ones. Framebuffers the guest built for the old generation and then
// abandoned still REPORT one of those names as their colour attachment, so a
// scan matching on name finds a stale FBO first, reads the orphaned storage,
// and prints "0 lit" on a run whose eye textures are full — with the eye
// plainly attached and the size plainly right. Recency is what tells the two
// apart, and only the guest's own calls carry it.
static uint32_t g_last_eye_fbo;

// `fbo` is the framebuffer that now holds an eye texture, or 0 if `touched` no
// longer does — a guest that re-points the same FBO at something else must not
// leave the hint asserting otherwise.
static void klfb_note_eye_fbo(uint32_t fbo, uint32_t touched) {
    if (fbo) __atomic_store_n(&g_last_eye_fbo, fbo, __ATOMIC_RELAXED);
    else if (__atomic_load_n(&g_last_eye_fbo, __ATOMIC_RELAXED) == touched)
        __atomic_store_n(&g_last_eye_fbo, 0, __ATOMIC_RELAXED);
}

// Called from both thunks: a stage is now the draw target. Reported once, so a
// log says whether the observation is working at all rather than leaving the
// counter fallback to be discovered by its symptoms.
static void klfb_note_render_stage(int stage) {
    if (stage < 0) return;
    int was = __atomic_exchange_n(&g_render_stage, stage, __ATOMIC_RELAXED);
    __atomic_fetch_or(&g_render_mask, 1u << stage, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_render_binds, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_stage_binds[stage], 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_render_tid, klfb_tid_cached(), __ATOMIC_RELAXED);
    if (was < 0)
        fprintf(stderr, "  [glfb] eye stage observed from the draw target "
                        "(first: stage %d)\n", stage);
}

// ---------------------------------------------------------------------------
// The Metal interop: eglCreateImageKHR rather than a pbuffer.
//
// The extension is unconditional on ANGLE's Metal backend (DisplayMtl.mm sets
// both mtlTextureClientBuffer and EGLImageOES), so there is no capability path
// to take here — if the entry points resolve, it works, and if they do not the
// caller falls back to ordinary GL storage.
#define EGL_NO_CONTEXT_          ((void *)0)
#define EGL_NO_IMAGE_            ((void *)0)
#define EGL_NONE_                0x3038
#define EGL_DEVICE_EXT_                      0x322C
#define EGL_METAL_DEVICE_ANGLE_              0x34A6
#define EGL_METAL_TEXTURE_ANGLE_             0x34A7
#define EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE_ 0x34DD
#define EGL_TEXTURE_INTERNAL_FORMAT_ANGLE_   0x345D

static void *(*a_eglCreateImageKHR)(void *, void *, uint32_t, void *, const int32_t *);
static unsigned (*a_eglDestroyImageKHR)(void *, void *);
static unsigned (*a_eglQueryDisplayAttribEXT)(void *, int32_t, intptr_t *);
static unsigned (*a_eglQueryDeviceAttribEXT)(void *, int32_t, intptr_t *);
static void (*a_glEGLImageTargetTexture2DOES)(uint32_t, void *);
static void (*a_glBindTexture_mtl)(uint32_t, uint32_t);

// Foveation. These two are OURS — they exist only in the patched ANGLE
// (angle-patches/klepton.patch), so a NULL here means the loaded libGLESv2
// predates the patch rather than that anything is broken, and it is worth
// saying so by name rather than silently not foveating.
static void (*a_SetRateMap)(void *texture, void *map);
static void (*a_SetRateMapForSize)(uint32_t w, uint32_t h, uint32_t min_samples, void *map);

// The map in force, and the eye size it was built for. One map for both eyes
// and every stage: the two render targets in a per-eye chain have to share one
// or the resolve between them stops being a physical-to-physical copy, and
// there is no reason for the stages to differ from each other.
static void *g_eye_rate_map;
static int   g_eye_rate_w, g_eye_rate_h;
static int   g_eye_rate_zx, g_eye_rate_zy;

// The multisampled scene target is the one the size rule is for; a
// single-sampled target of the same size is a post-processing intermediate and
// must NOT be foveated. See kl_glfb.h.
#define KL_RATE_MIN_SAMPLES 2


static kl_glfb_mtl_provider g_mtl_provider;
static void *g_mtl_provider_ctx;
// One record per (eye, slot). KL_MTL_MAX_STAGES is in the header, because a
// PROVIDER has to be sized by the same number and providers live outside this
// file — a provider that holds fewer declines every slot past its own end, and
// kl_glfb then has no storage for those layers at all.
// w/h are carried so a rate map can be matched against the texture it would be
// attached to. A map is built for one screen size; attaching it to a texture of
// another is a warp against coordinates that do not exist, and nothing in Metal
// or ANGLE reports it.
// `top_left` records which way up the guest rendered — see
// kl_glfb_eye_mtl_origin_top_left. It stays 0 for everything that arrives
// through the GL path below, which is every entry this table had until the
// Vulkan seam existed.
static struct { void *tex; int slice; void *image; uint32_t gl_tex; int w, h;
                int top_left; }
    g_eye_mtl[2][KL_MTL_MAX_STAGES];

void kl_glfb_set_mtl_provider(kl_glfb_mtl_provider fn, void *ctx) {
    g_mtl_provider = fn;
    g_mtl_provider_ctx = ctx;
}
int kl_glfb_has_mtl_provider(void) { return g_mtl_provider != NULL; }

static int mtl_resolve(void) {
    if (a_eglCreateImageKHR) return 1;
    if (!g_ready) return 0;
    a_eglCreateImageKHR        = asym("eglCreateImageKHR");
    a_eglDestroyImageKHR       = asym("eglDestroyImageKHR");
    a_eglQueryDisplayAttribEXT = asym("eglQueryDisplayAttribEXT");
    a_eglQueryDeviceAttribEXT  = asym("eglQueryDeviceAttribEXT");
    a_glEGLImageTargetTexture2DOES = asym("glEGLImageTargetTexture2DOES");
    a_glBindTexture_mtl        = asym("glBindTexture");
    // Ours, from angle-patches/klepton.patch. Deliberately NOT in the return
    // below: an ANGLE without them can still do the interop, it just cannot
    // foveate, and failing the whole Metal path over that would trade a
    // performance feature for the picture.
    a_SetRateMap        = asym("ANGLEMetalSetRasterizationRateMap");
    a_SetRateMapForSize = asym("ANGLEMetalSetRasterizationRateMapForSize");
    return a_eglCreateImageKHR && a_glEGLImageTargetTexture2DOES
        && a_eglQueryDeviceAttribEXT && a_glBindTexture_mtl;
}

void *kl_glfb_mtl_device(void) {
    static void *dev;
    static int tried;
    if (tried) return dev;
    tried = 1;
    if (!kl_glfb_init() || !mtl_resolve()) return NULL;
    intptr_t egl_dev = 0, mtl = 0;
    if (!a_eglQueryDisplayAttribEXT ||
        !a_eglQueryDisplayAttribEXT(g_dpy, EGL_DEVICE_EXT_, &egl_dev) || !egl_dev) {
        fprintf(stderr, "  [glfb] eglQueryDisplayAttribEXT(EGL_DEVICE_EXT) failed — "
                        "no MTLDevice to share\n");
        return NULL;
    }
    if (!a_eglQueryDeviceAttribEXT((void *)egl_dev, EGL_METAL_DEVICE_ANGLE_, &mtl) || !mtl) {
        fprintf(stderr, "  [glfb] eglQueryDeviceAttribEXT(EGL_METAL_DEVICE_ANGLE) failed — "
                        "is this the Metal backend?\n");
        return NULL;
    }
    dev = (void *)mtl;
    return dev;
}

void *kl_glfb_eye_rate_map(void) { return g_eye_rate_map; }

void kl_glfb_eye_rate_zones(int *zones_x, int *zones_y) {
    if (zones_x) *zones_x = g_eye_rate_zx;
    if (zones_y) *zones_y = g_eye_rate_zy;
}

// The policy, not the object. Both builders read it from here so a device run
// foveates exactly as the host measurement did — see kl_glfb.h.
// ON by default since 2026-08-09, once the device leg landed: every path that
// samples an eye texture as a picture rather than as storage now unwarps it
// (both compositors, through kl_reproject's grid). What does NOT is the
// diagnostic readback — KL_GLFB_OUT and t_mtl_provider's lit-pixel count — and
// those show the squeeze on purpose, as the cheapest confirmation it engaged.
// KL_VRR=0 is the A/B.
//
// ...and the default is a variable rather than a literal, because the headset
// app carries it as a SETTING (KleptonRendering, off unless a person turns it
// on): the map is fixed for a session, sampled once before the guest boots, so
// the sharp region stays where it was put while the eyes move away from it —
// which reads as a blurry picture, not as a cheaper one. Hosts never call the
// setter and keep foveating, so the measurements above stay what they were.
static int g_foveation_default = 1;
void kl_glfb_set_foveation_default(int on) { g_foveation_default = on ? 1 : 0; }

static int g_eye_doublewide;
void kl_glfb_set_eye_doublewide(int on) {
    if (on && !g_eye_doublewide)
        fprintf(stderr, "  [glfb] eye layer is DoubleWide — guest foveation off (a "
                        "one-eye rate map cannot describe two eyes side by side)\n");
    g_eye_doublewide = on ? 1 : 0;
}

int kl_glfb_foveation_wanted(int *zones, float *edge) {
    if (g_eye_doublewide) return 0;
    if (!kl_env_on("KL_VRR", g_foveation_default)) return 0;
    int z = kl_env_int("KL_VRR_ZONES", 16);
    if (z < 1) z = 1;
    if (z > KL_FOVEATION_MAX_ZONES) z = KL_FOVEATION_MAX_ZONES;
    float e = kl_env_float("KL_VRR_EDGE", 0.35f);
    // A rate map's quality is a fraction of full rate; 0 would ask Metal for
    // zero-area zones, and >1 is not a thing. Out of range means the knob was
    // fat-fingered, so fall back rather than clamp to a silently different
    // picture.
    if (!(e > 0.05f) || !(e <= 1.0f)) e = 0.35f;
    if (zones) *zones = z;
    if (edge)  *edge  = e;
    return 1;
}

void kl_glfb_foveation_quality(float *q, int n, float edge) {
    for (int i = 0; i < n; i++) {
        // The zone centre in [-1, 1], so the curve is symmetric about the
        // middle of the screen — where the fovea is, absent eye tracking we
        // are not given.
        float t = n > 1 ? (2.f * ((float)i + 0.5f) / (float)n - 1.f) : 0.f;
        if (t < 0) t = -t;
        q[i] = 1.f - (1.f - edge) * t;
    }
}

// Set once the array-mirror path (kl_glfb_mirror_eye_layer) owns an eye. See
// kl_glfb_set_eye_rate_map for why it forbids foveation — unless the guest's own
// swapchain is given the map too (klfb_mirror_vrr).
static int g_eye_mirroring;

// Foveation for a guest whose eyes reach the compositor by copy.
//
// The refusal in kl_glfb_set_eye_rate_map is about a map on the COPY ONLY. The
// alternative it does not consider is putting the map on the guest's own
// swapchain as well: ANGLE's size rule (ANGLEMetalSetRasterizationRateMapForSize)
// with a minimum of ONE sample matches every colour target of the eye size, the
// guest's array swapchain included, so its scene is rasterized through the map;
// the copy is then between two targets carrying the same map, which the patched
// blit performs texel for texel; and the compositor unwarps what arrives exactly
// as it does for a provider-backed eye.
//
// What that costs is the caveat the size rule was given `minSamples` for: any
// OTHER colour target the guest renders at the eye size is foveated too, and one
// it then samples with screen coordinates comes out warped. Doom3Quest draws its
// scene straight into the swapchain, so it is on for that target and off
// elsewhere; KL_VRR_MIRROR is the A/B. Measured for it on device, 2026-10-08:
// 4400x4400 per eye is 32% of the fragments through the display's own map, and
// without the map that size took the game from 55 frames a second to 33.
static int klfb_mirror_vrr(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        extern int kl_guest_beatsaber_openxr(void);
        const char *t = kl_driver_target_name();
        // ...and Beat Saber from 1.45 on, whose scene also goes straight into
        // its array swapchain, one multiview pass for both eyes. Seen on
        // device, 2026-10-12: no distortion in menu or level, and nothing else
        // it draws at the eye size is read back through screen coordinates.
        on = kl_env_on("KL_VRR_MIRROR", (t && strcmp(t, "doom3quest") == 0) ||
                                        kl_guest_beatsaber_openxr());
    }
    return on;
}
// Whether a guest of the mirrored kind will be foveated this run: the setting
// (or KL_VRR) is on and the guest is one klfb_mirror_vrr names. Asked by
// kl_ovrp_set_eye_texture_size before any swapchain exists, because the size a
// foveated guest can afford is another one than an unfoveated guest's.
int kl_glfb_mirror_foveation_wanted(void) {
    return kl_glfb_foveation_wanted(NULL, NULL) && klfb_mirror_vrr();
}
// The part of an eye texture a foveated guest actually writes: the map's
// physical size, the larger of its layers'. Metal rasterizes into that corner
// (rows and columns from 0) and leaves the rest of the screen-sized texture
// untouched, so it is all the mirror copy has to bring across — at 6890x5526
// about 2840x2720 of it, a fifth, where the whole-texture copy moved 76 million
// texels a frame for the pair of eyes. Told by whoever built the map, right
// after kl_glfb_set_eye_rate_map; 0 until then, and the copy is the whole
// texture. KL_VRR_MIRROR_PHYS=0 is the whole texture again.
static int g_eye_rate_pw, g_eye_rate_ph;
void kl_glfb_set_eye_rate_physical(int pw, int ph) {
    if (pw > 0 && ph > 0 && pw <= g_eye_rate_w && ph <= g_eye_rate_h &&
        kl_env_on("KL_VRR_MIRROR_PHYS", 1)) {
        if (pw != g_eye_rate_pw || ph != g_eye_rate_ph)
            fprintf(stderr, "  [glfb] a foveated eye is copied as its physical %dx%d of %dx%d\n",
                    pw, ph, g_eye_rate_w, g_eye_rate_h);
        g_eye_rate_pw = pw; g_eye_rate_ph = ph;
    } else {
        g_eye_rate_pw = g_eye_rate_ph = 0;
    }
}
// The sample floor the size rule was installed with, so it is retired with the same.
static int g_eye_rate_min = KL_RATE_MIN_SAMPLES;

// Making a map reach framebuffers the guest has ALREADY drawn into.
//
// ANGLE's Metal framebuffer keeps the render pass descriptor it built and
// rebuilds it only when the GL framebuffer's own state is synced, so a map
// registered after a framebuffer's first pass is never picked up by it. A
// provider-backed eye gets its map before the guest can draw; a mirrored one
// cannot — which swapchain is an eye is only known at the first xrEndFrame that
// submits it (Doom3Quest's menu quad is a two-slice array as well), and by then
// the guest has drawn a frame into one of the swapchain's per-image FBOs. Left
// alone that FBO stays unfoveated while its siblings pick the map up, and the
// compositor unwarps every third frame of a picture that was never warped.
//
// So a change of map bumps a generation, and the next time the guest binds an
// FBO of an older one its draw buffers are set again to what they already are:
// a no-op to the guest, and a dirty bit to ANGLE, whose sync rebuilds the pass.
static unsigned g_rate_gen = 1;
static unsigned g_fbo_rate_gen[KLFB_RESYNC_FBOS];
// The size rule is for the eye swapchain and for nothing else of that size.
//
// ANGLE can only be told "every colour target of these dimensions", and a guest
// has others: Doom3Quest draws its HUD and its PDA into a plain 2D framebuffer
// of exactly the eye size every frame (neo/renderer/framebuffer.cpp) and copies
// a square out of it with glCopyTexImage2D — through the map that would be a
// warped HUD read back at unwarped coordinates. The two are told apart by how
// they were attached (g_fbo_multiview), so the rule is switched as the guest
// binds: on for a multiview framebuffer, off for any other. That works because
// of the same caching klfb_resync_fbo has to defeat — a framebuffer keeps the
// descriptor it was synced with, and the plain one re-attaches (and so re-syncs)
// each frame while the rule is off.
static int g_rate_rule_on;
static void klfb_rate_rule(int on) {
    if (!g_eye_rate_map || !g_eye_rate_w || !a_SetRateMapForSize) return;
    on = on ? 1 : 0;
    if (on == g_rate_rule_on) return;
    a_SetRateMapForSize((uint32_t)g_eye_rate_w, (uint32_t)g_eye_rate_h,
                        (uint32_t)g_eye_rate_min, on ? g_eye_rate_map : NULL);
    g_rate_rule_on = on;
}
static void klfb_rate_rule_for_fbo(uint32_t fb) {
    if (!g_eye_mirroring || !klfb_mirror_vrr()) return;
    klfb_rate_rule(fb < KLFB_RESYNC_FBOS && g_fbo_multiview[fb]);
}
static void (*a_glDrawBuffers)(int32_t, const uint32_t *);
static void klfb_resync_fbo(uint32_t fb) {
    if (!g_eye_mirroring || !fb || fb >= KLFB_RESYNC_FBOS || !g_fbo_multiview[fb]) return;
    if (g_fbo_rate_gen[fb] == g_rate_gen) return;
    g_fbo_rate_gen[fb] = g_rate_gen;
    if (!a_glDrawBuffers) a_glDrawBuffers = asym("glDrawBuffers");
    if (!a_glDrawBuffers || !a_glGetIntegerv) return;
    int32_t cur = 0;
    a_glGetIntegerv(0x8825 /* GL_DRAW_BUFFER0 */, &cur);
    if (cur != 0x8CE0 /* COLOR_ATTACHMENT0 */) return;   // not a plain colour FBO: leave it
    uint32_t buf = (uint32_t)cur;
    a_glDrawBuffers(1, &buf);
}

// How many layers a guest rate map needs: a mirrored guest that renders both
// eyes in one layered pass (OVR_multiview) needs one per slice.
int kl_glfb_rate_map_layers(void) { return g_eye_mirroring && klfb_mirror_vrr() ? 2 : 1; }
// ...and whether those two layers each get their own eye's curve of the
// display's map, or both the envelope of the two (KleptonCompositor's
// updateGuestRateMap has the reasons). Beat Saber from 1.45 on only.
int kl_glfb_rate_map_per_eye(void) {
    extern int kl_guest_beatsaber_openxr(void);
    return kl_guest_beatsaber_openxr();
}

void kl_glfb_set_eye_rate_map(int w, int h, int zones_x, int zones_y, void *rate_map) {
    if (!kl_glfb_init() || !mtl_resolve()) return;
    // Not available on the copy path: there is nothing there to save.
    // Foveation is a bargain with the GUEST's rasterizer: it
    // writes fewer fragments, and the compositor's grid unwarps what it wrote.
    // On the array-mirror path the guest rasterizes into a swapchain of its own
    // that carries no map (klxr_CreateSwapchain gives it plain glTexStorage3D
    // storage), and the only thing a map could attach to is the COPY — so the
    // full-resolution picture would be squeezed on the way in and stretched back
    // out on the way to the display, paying twice for a saving nobody made.
    //
    // And if the blit does not rasterize at all, the destination holds an
    // unwarped picture the compositor unwarps anyway: a magnified centre in a
    // correct-looking frame, which is the failure this file has no instrument
    // for. Refused rather than left to
    // the frontends, because both of them build a map from the display and
    // neither can know which guest is underneath.
    if (g_eye_mirroring && rate_map && !klfb_mirror_vrr()) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] foveation is not available for this guest: its "
                            "eyes reach the compositor by COPY (an array swapchain), "
                            "and a rate map on the copy costs resolution and saves "
                            "nothing — the map is dropped\n");
        rate_map = NULL;
    }
    if (!a_SetRateMapForSize || !a_SetRateMap) {
        fprintf(stderr, "  [glfb] this ANGLE has no rasterization-rate entry points — "
                        "foveation is unavailable (is it the patched build?)\n");
        return;
    }
    // Retire the previous size rule before installing another: the eye size
    // changes under us when Unity re-creates its swapchain, and a rule left
    // behind on the old size would foveate a target nothing unwarps.
    int min_samples = g_eye_mirroring && klfb_mirror_vrr() ? 1 : KL_RATE_MIN_SAMPLES;
    if (g_eye_rate_w && (g_eye_rate_w != w || g_eye_rate_h != h || !rate_map ||
                         g_eye_rate_min != min_samples))
        a_SetRateMapForSize((uint32_t)g_eye_rate_w, (uint32_t)g_eye_rate_h,
                            (uint32_t)g_eye_rate_min, NULL);

    g_eye_rate_map = rate_map;
    g_eye_rate_w   = rate_map ? w : 0;
    g_eye_rate_h   = rate_map ? h : 0;
    g_eye_rate_pw  = g_eye_rate_ph = 0;      // the new map's is told next
    g_eye_rate_zx  = rate_map ? zones_x : 0;
    g_eye_rate_zy  = rate_map ? zones_y : 0;

    g_eye_rate_min = min_samples;
    g_rate_rule_on = rate_map ? 1 : 0;
    g_rate_gen++;           // framebuffers drawn into before this must be re-synced
    if (rate_map)
        a_SetRateMapForSize((uint32_t)w, (uint32_t)h, (uint32_t)min_samples, rate_map);

    // Eye textures already bound do not come back through the bind path, so
    // re-register (or clear) each of them here — but only those the map was
    // actually built for. A frontend that re-drives this on a size change (both
    // of ours do, keyed on size) can still be holding textures of the OLD size
    // at this moment, and handing them the new map would foveate them against a
    // screen size they do not have. Clearing is always safe, so a mismatch
    // clears rather than skips.
    for (int e = 0; e < 2; e++)
        for (int s = 0; s < KL_MTL_MAX_STAGES; s++) {
            if (!g_eye_mtl[e][s].tex) continue;
            int fits = rate_map && g_eye_mtl[e][s].w == w && g_eye_mtl[e][s].h == h;
            a_SetRateMap(g_eye_mtl[e][s].tex, fits ? rate_map : NULL);
            if (rate_map && !fits)
                fprintf(stderr, "  [glfb] eye=%d stage=%d is %dx%d, not the %dx%d this "
                                "rate map was built for — left unfoveated\n",
                        e, s, g_eye_mtl[e][s].w, g_eye_mtl[e][s].h, w, h);
        }

    fprintf(stderr, "  [glfb] eye rate map %p for %dx%d, %dx%d zones (%s targets of "
                    "that size, plus every bound eye texture)\n",
            rate_map, w, h, zones_x, zones_y,
            min_samples > 1 ? "multisampled" : "ALL colour");
}

void *kl_glfb_eye_mtl_texture(int eye, int stage, int *out_slice) {
    if (eye < 0 || eye > 1 || stage < 0 || stage >= KL_MTL_MAX_STAGES) return NULL;
    if (out_slice) *out_slice = g_eye_mtl[eye][stage].slice;
    return g_eye_mtl[eye][stage].tex;
}

// The Vulkan path's way into the same table — see kl_glfb.h. There is no
// EGLImage and no GL name on that path, so those two fields stay zero and
// kl_glfb_release_eye_texture has nothing of its own to drop; the VkImage is
// kl_vulkan.c's to free.
void kl_glfb_note_eye_mtl_texture(int eye, int stage, void *texture, int slice,
                                  int w, int h) {
    if (eye < 0 || eye > 1 || stage < 0 || stage >= KL_MTL_MAX_STAGES) return;
    if (g_eye_mtl[eye][stage].tex == texture &&
        g_eye_mtl[eye][stage].slice == slice) return;
    g_eye_mtl[eye][stage].tex   = texture;
    g_eye_mtl[eye][stage].slice = slice;
    g_eye_mtl[eye][stage].image = NULL;
    g_eye_mtl[eye][stage].gl_tex = 0;
    g_eye_mtl[eye][stage].w = w;
    g_eye_mtl[eye][stage].h = h;
    // Vulkan's framebuffer origin is the top left, like Metal's and unlike GL's.
    g_eye_mtl[eye][stage].top_left = 1;
    fprintf(stderr, "  [glfb] eye %d stage %d is MTLTexture %p slice %d (%dx%d), "
                    "from Vulkan — top-left origin, the compositor can sample it\n",
            eye, stage, texture, slice, w, h);
}

// DoubleWide: one GL texture holds both eyes side by side, so there is ONE
// storage and the right eye must read the very slice the left eye's GL name is
// backed by. Binding eye 1 through kl_glfb_bind_eye_mtl_texture instead re-points
// that same GL name at the provider's slice 1: the guest then draws its whole
// wide frame into slice 1 while the compositor still reads slice 0 for the left
// eye (never written again) — one eye garbage, the other dark. So eye 1 only
// records eye 0's texture and slice; it owns no EGLImage and no GL name, and
// kl_glfb_release_eye_texture drops it together with eye 0.
int kl_glfb_share_eye_mtl_texture(int eye, int stage, int from_eye) {
    if (eye < 0 || eye > 1 || from_eye < 0 || from_eye > 1 || eye == from_eye ||
        stage < 0 || stage >= KL_MTL_MAX_STAGES || !g_eye_mtl[from_eye][stage].tex)
        return 0;
    g_eye_mtl[eye][stage] = g_eye_mtl[from_eye][stage];
    g_eye_mtl[eye][stage].image  = NULL;
    g_eye_mtl[eye][stage].gl_tex = 0;
    fprintf(stderr, "  [glfb] eye=%d stage=%d shares eye %d's MTLTexture %p slice %d "
                    "(DoubleWide: one wide image, cropped per eye)\n",
            eye, stage, from_eye, g_eye_mtl[eye][stage].tex,
            g_eye_mtl[eye][stage].slice);
    return 1;
}

int kl_glfb_eye_mtl_origin_top_left(int eye, int stage) {
    if (eye < 0 || eye > 1 || stage < 0 || stage >= KL_MTL_MAX_STAGES) return 0;
    return g_eye_mtl[eye][stage].top_left;
}

// The teardown half, called from ovrp_DestroyEyeTexture — the only thing in the
// trace that ever says an eye texture is finished with. Two references have to
// go for the storage to be reclaimed, and dropping either one alone reclaims
// nothing:
//
//   - the EGLImage, which is ANGLE's handle on the MTLTexture, and
//   - the GL texture, which took a reference of its own at
//     glEGLImageTargetTexture2DOES and keeps it until the name is deleted.
//
// The provider's own reference then goes when it replaces its cache entry, and
// only at that point does the memory actually come back. That is why the leak
// looked like the provider's: it *was* releasing on reallocation, into a
// texture ANGLE still held.
//
// The GL delete needs a current context, and this call arrives on the guest's
// render thread inside its graphics teardown, where there is one. If there
// isn't, the delete is a silent GL no-op — so it is reported rather than
// assumed.
void kl_glfb_release_eye_texture(int eye, int stage) {
    // KL_EYE_RELEASE=0 restores the old behaviour — the teardown call arrives
    // and nothing is released. The A/B is worth a knob: this is the difference
    // between a swapchain per loading transition and one swapchain, and the
    // cost of being wrong about the ownership is a texture the guest still
    // wants. Compare `frames per stage` in the OVRPlugin report either way.
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_EYE_RELEASE", 1);
    if (!on) return;
    if (eye < 0 || eye > 1 || stage < 0 || stage >= KL_MTL_MAX_STAGES) {
        fprintf(stderr, "  [glfb] release eye=%d stage=%d: out of range, "
                        "nothing released\n", eye, stage);
        return;
    }
    uint32_t gl_tex = g_eye_mtl[eye][stage].gl_tex;
    void    *img    = g_eye_mtl[eye][stage].image;
    if (!gl_tex && !img) return;                 // never bound, or already gone

    // Unbind the map first. The registry RETAINS the texture it is keyed on, so
    // a binding left behind here outlives the storage it describes — and worse,
    // the address can be recycled into an unrelated texture that would then be
    // silently foveated (mtl_common.mm says why it retains).
    if (g_eye_mtl[eye][stage].tex && a_SetRateMap)
        a_SetRateMap(g_eye_mtl[eye][stage].tex, NULL);
    if (img && a_eglDestroyImageKHR) a_eglDestroyImageKHR(g_dpy, img);
    if (gl_tex) {
        static void (*r_DeleteTextures)(int32_t, const uint32_t *);
        static void *(*r_GetCurrentContext)(void);
        if (!r_DeleteTextures) {
            r_DeleteTextures = asym("glDeleteTextures");
            r_GetCurrentContext = asym("eglGetCurrentContext");
        }
        if (r_GetCurrentContext && !r_GetCurrentContext())
            fprintf(stderr, "  [glfb] release eye=%d stage=%d: no current context, "
                            "the GL texture %u keeps its storage\n",
                    eye, stage, gl_tex);
        if (r_DeleteTextures) r_DeleteTextures(1, &gl_tex);
        klfb_vram_forget(KLC_TEX, gl_tex);
        klfb_census_killed(KLC_TEX, 1);
    }
    // A DoubleWide partner (kl_glfb_share_eye_mtl_texture) owns nothing of its
    // own and must not outlive the storage it reads.
    for (int e = 0; e < 2; e++)
        if (e != eye && !g_eye_mtl[e][stage].gl_tex && !g_eye_mtl[e][stage].image &&
            g_eye_mtl[e][stage].tex && g_eye_mtl[e][stage].tex == g_eye_mtl[eye][stage].tex &&
            g_eye_mtl[e][stage].slice == g_eye_mtl[eye][stage].slice)
            g_eye_mtl[e][stage] = (typeof(g_eye_mtl[0][0])){0};
    g_eye_mtl[eye][stage] = (typeof(g_eye_mtl[0][0])){0};
    // ...and the stage attribution that named it — guarded, see
    // klfb_forget_eye_stage.
    int freed = klfb_forget_eye_stage(eye, stage, gl_tex);
    fprintf(stderr, "  [glfb] eye=%d stage=%d released (GL texture %u, image %p)\n",
            eye, stage, gl_tex, img);
    if (freed)
        fprintf(stderr, "  [glfb] eye=%d stage=%d: %d eye-FBO map slot(s) given back "
                        "with GL texture %u\n", eye, stage, freed, gl_tex);
}

// ---------------------------------------------------------------------------
// GL_FRAMEBUFFER_SRGB, and why a cap we do not have is worth intercepting.
//
// `EXT_sRGB_write_control` lets an app turn the linear->sRGB encode off for a
// framebuffer whose attachment is an sRGB format — i.e. say "the values I am
// writing are ALREADY sRGB code values, store them as they are". Steam Link
// uses it exactly that way: it disables the encode, renders its decoded video
// into an SRGB8_ALPHA8 swapchain, and re-enables it after
// (`XRConstruct::RenderFrame`, `QSVLRendererXR::FlipFrame` — 883 of each per
// run). A Quest has the extension. ANGLE does not expose it, so both calls
// raised INVALID_ENUM and ES applied the encode anyway.
//
// **The error was never the problem; the encode was.** The stored byte becomes
// `encode(V)` where the guest wrote `V`, and sampling an sRGB texture decodes
// once — so the composite receives `V`, an sRGB code value, and treats it as
// linear. That is the picture reading too bright, and it has no error surface
// at all: every call after the two INVALID_ENUMs succeeds and the frame is
// perfectly well-formed. "An encode on write and a decode on sample cancel, so
// this is precision rather than gamma" is true about the pair and wrong about
// the conclusion: what they cancel back to is the guest's sRGB code value, and
// the composite needs
// linear.
//
// So the state is RECORDED rather than forwarded. Swallowing it also removes
// 1766 GL errors a run from a log where an unexplained error is a lead.
//
// Sticky, and deliberately: this asks "does this guest render sRGB code values
// into its eye texture", which is a property of the guest, not of the instant.
// The composite runs on another thread entirely and sampling a live GL enable
// from it would be a race with no right answer.
#define KLFB_GL_FRAMEBUFFER_SRGB 0x8DB9
static int g_srgb_write_off;        // the guest asked for the encode OFF
static int g_eye_fmt_is_srgb;       // ...and an eye texture is an sRGB format

// Both halves have to hold, and neither is knowable without the other: an eye
// texture that is not sRGB takes no encode to undo (Beat Saber's is RGBA16F),
// and a guest that never disables the encode meant the one it got.
static void klfb_srgb_settle(void) {
    kl_reproject_set_srgb_decode(g_srgb_write_off && g_eye_fmt_is_srgb);
}

// A fresh provider texture holds whatever memory it was given. The compositor
// can show a stage before the guest ever draws into it — Saints & Sinners puts
// up its title splash about three seconds before its first eye picture — and
// that showed as a full field of pink. Cleared to black once, on the calling
// (guest GL) thread, with every piece of state the clear touches put back.
static void klfb_clear_tex_black(uint32_t tex) {
    static void (*gen)(int32_t, uint32_t *), (*del)(int32_t, const uint32_t *);
    static void (*bindfb)(uint32_t, uint32_t);
    static void (*fbtex)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
    static void (*cc)(float, float, float, float), (*clr)(uint32_t);
    static void (*en)(uint32_t), (*dis)(uint32_t);
    static void (*cmask)(uint8_t, uint8_t, uint8_t, uint8_t);
    static void (*getf)(uint32_t, float *);
    static uint8_t (*isen)(uint32_t);
    static void (*getb)(uint32_t, uint8_t *);
    if (!gen) {
        gen = asym("glGenFramebuffers"); del = asym("glDeleteFramebuffers");
        bindfb = asym("glBindFramebuffer"); fbtex = asym("glFramebufferTexture2D");
        cc = asym("glClearColor"); clr = asym("glClear"); en = asym("glEnable");
        dis = asym("glDisable"); cmask = asym("glColorMask"); getf = asym("glGetFloatv");
        isen = asym("glIsEnabled"); getb = asym("glGetBooleanv");
    }
    if (!gen || !del || !bindfb || !fbtex || !cc || !clr || !en || !dis || !cmask ||
        !getf || !isen || !getb || !a_glGetIntegerv)
        return;
    int32_t dfb = 0;
    a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &dfb);
    float col[4] = {0};
    getf(0x0C22 /* COLOR_CLEAR_VALUE */, col);
    uint8_t mask[4] = {1, 1, 1, 1};
    getb(0x0C23 /* COLOR_WRITEMASK */, mask);
    uint8_t scissor = isen(0x0C11 /* SCISSOR_TEST */);
    uint32_t fb = 0;
    gen(1, &fb);
    bindfb(0x8CA9 /* DRAW_FRAMEBUFFER */, fb);
    fbtex(0x8CA9, 0x8CE0 /* COLOR_ATTACHMENT0 */, 0x0DE1, tex, 0);
    if (scissor) dis(0x0C11);
    cmask(1, 1, 1, 1);
    cc(0, 0, 0, 1);
    clr(0x4000 /* COLOR_BUFFER_BIT */);
    cc(col[0], col[1], col[2], col[3]);
    cmask(mask[0], mask[1], mask[2], mask[3]);
    if (scissor) en(0x0C11);
    bindfb(0x8CA9, (uint32_t)dfb);
    del(1, &fb);
}

int kl_glfb_bind_eye_mtl_texture(int eye, int stage, uint32_t gl_tex,
                                 int w, int h, uint32_t internal_fmt) {
    if (!g_mtl_provider || eye < 0 || eye > 1 || !gl_tex) return 0;
    if (stage < 0 || stage >= KL_MTL_MAX_STAGES) {
        fprintf(stderr, "  [glfb] eye stage %d is beyond KL_MTL_MAX_STAGES (%d)\n",
                stage, KL_MTL_MAX_STAGES);
        return 0;
    }
    if (!kl_glfb_init() || !mtl_resolve()) {
        fprintf(stderr, "  [glfb] a Metal texture provider is registered but ANGLE's "
                        "interop entry points are missing — falling back to GL storage\n");
        return 0;
    }
    kl_mtl_eye_texture t = { NULL, 0, 0, 0 };
    if (!g_mtl_provider(eye, stage, w, h, internal_fmt, &t, g_mtl_provider_ctx) || !t.texture) {
        fprintf(stderr, "  [glfb] provider declined eye=%d stage=%d %dx%d\n",
                eye, stage, w, h);
        return 0;
    }
    // The size check the extension will not do for us. eglCreateImageKHR takes
    // the dimensions from the MTLTexture and succeeds regardless of what we ask
    // for, so without this a wrong-sized texture produces a guest rendering into
    // storage smaller than it believes it has — and nothing reports it. Refuse
    // instead: ordinary GL storage of the right size is wrong in a way that
    // shows up immediately, which is strictly better than being subtly wrong.
    if (t.w != w || t.h != h) {
        fprintf(stderr, "  [glfb] provider returned a %dx%d texture for a %dx%d eye "
                        "(eye=%d stage=%d) — refusing; the guest would render into "
                        "storage it does not have\n", t.w, t.h, w, h, eye, stage);
        return 0;
    }
    const int32_t attrs[] = {
        EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE_, t.slice,
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE_,   (int32_t)internal_fmt,
        EGL_NONE_,
    };
    void *img = a_eglCreateImageKHR(g_dpy, EGL_NO_CONTEXT_, EGL_METAL_TEXTURE_ANGLE_,
                                   t.texture, attrs);
    if (img == EGL_NO_IMAGE_) {
        // Most likely cause by far: the texture was not allocated on ANGLE's own
        // MTLDevice, which the extension requires. Named because nothing else
        // about the failure points at it.
        fprintf(stderr, "  [glfb] eglCreateImageKHR(EGL_METAL_TEXTURE_ANGLE) failed for "
                        "eye=%d stage=%d — was the texture made on "
                        "kl_glfb_mtl_device()?\n", eye, stage);
        return 0;
    }
    a_glBindTexture_mtl(0x0DE1 /* GL_TEXTURE_2D */, gl_tex);
    while (a_glGetError && a_glGetError() != 0) {}
    a_glEGLImageTargetTexture2DOES(0x0DE1, img);
    uint32_t e = a_glGetError ? a_glGetError() : 0;
    if (e) {
        fprintf(stderr, "  [glfb] glEGLImageTargetTexture2DOES -> GL error 0x%x "
                        "(eye=%d stage=%d)\n", e, eye, stage);
        if (a_eglDestroyImageKHR) a_eglDestroyImageKHR(g_dpy, img);
        return 0;
    }
    // A backstop, and in this title an unexercised one: every measured
    // replacement is preceded by ovrp_DestroyEyeTexture, so this slot is empty
    // by the time a new texture arrives (24 binds, 18 teardowns — the six
    // without a teardown are the first generation, which has no predecessor).
    // It exists because the guest never calls glDeleteTextures on these names
    // itself: if a teardown ever goes missing, destroying only the EGLImage
    // leaves ANGLE's texture holding the MTLTexture and the provider's release
    // reclaims nothing. It says so when it fires — a silent backstop is how a
    // leak comes back.
    //
    // The same name re-bound to new storage is not a replacement, so it falls
    // through to the image destroy alone.
    if (g_eye_mtl[eye][stage].gl_tex && g_eye_mtl[eye][stage].gl_tex != gl_tex) {
        fprintf(stderr, "  [glfb] eye=%d stage=%d replaced with no teardown "
                        "(GL texture %u -> %u) — releasing the old one here\n",
                eye, stage, g_eye_mtl[eye][stage].gl_tex, gl_tex);
        kl_glfb_release_eye_texture(eye, stage);   // no-op under KL_EYE_RELEASE=0
    }
    if (g_eye_mtl[eye][stage].image && a_eglDestroyImageKHR)
        a_eglDestroyImageKHR(g_dpy, g_eye_mtl[eye][stage].image);
    g_eye_mtl[eye][stage].tex    = t.texture;
    g_eye_mtl[eye][stage].slice  = t.slice;
    g_eye_mtl[eye][stage].image  = img;
    g_eye_mtl[eye][stage].gl_tex = gl_tex;
    g_eye_mtl[eye][stage].w      = w;
    g_eye_mtl[eye][stage].h      = h;
    // ...and record the storage, exactly as the glTexStorage2D thunk would have
    // if this texture had got ordinary GL storage. Nothing else can: an
    // EGLImage-backed texture never passes through that thunk, and ES 3.0 has
    // no glGetTexLevelParameteriv to ask with. Without it the capture finds the
    // eye FBO by attachment and then reads it at the PBUFFER's size, which on
    // this host is 4000x3200 against a 2748x2880 eye — a black PNG on the one
    // path that has the picture, with the eye plainly attached.
    klfb_note_tex_storage(gl_tex, internal_fmt, w, h);
    // For Unreal guests, where a first frame of whatever the provider's memory
    // held was seen (fix 15). The other guests bind as they were brought up.
    {
        static int clear = -1;
        if (clear < 0) {
            extern kl_guest_kind kl_driver_kind(void);
            clear = kl_env_on("KL_GLFB_EYE_CLEAR", kl_driver_kind() == KL_GUEST_UE4);
        }
        if (clear) klfb_clear_tex_black(gl_tex);
    }
    // Foveation, if a map is in force AND it was built for this size. Before the
    // guest renders into this texture, not after: ANGLE caches a framebuffer's
    // render pass descriptor and only rebuilds it on a GL state sync, so a map
    // bound afterwards misses the first pass with nothing reporting it
    //.
    //
    // The size test is the invariant, held HERE rather than trusted to the
    // frontend. Both of ours re-drive kl_glfb_set_eye_rate_map from the provider
    // before this runs, so in practice the map is already the right one — but
    // the ordering is a convention, and the failure if it is ever broken is a
    // wrong picture with no error anywhere. Beat Saber 1.6.0 is what makes this
    // reachable: it re-creates its eye textures mid-run at a different size
    // (2400x2290 -> 2880x2748 -> back), where 2019.4 picks one and keeps it.
    if (g_eye_rate_map && a_SetRateMap) {
        if (w == g_eye_rate_w && h == g_eye_rate_h) {
            a_SetRateMap(t.texture, g_eye_rate_map);
        } else {
            fprintf(stderr, "  [glfb] eye=%d stage=%d is %dx%d but the rate map is for "
                            "%dx%d — left unfoveated (the frontend has not re-driven "
                            "kl_glfb_set_eye_rate_map for the new size)\n",
                    eye, stage, w, h, g_eye_rate_w, g_eye_rate_h);
        }
    }
    // The other half of the sRGB question (klfb_srgb_settle). The format is not
    // ours to choose — it is whatever the guest asked its swapchain for — so
    // this is where it becomes known, and it can arrive either side of the
    // guest's first glDisable.
    if (internal_fmt == GL_SRGB8_ALPHA8) g_eye_fmt_is_srgb = 1;
    klfb_srgb_settle();
    fprintf(stderr, "  [glfb] eye=%d stage=%d tex=%u is now backed by MTLTexture %p "
                    "slice %d (%dx%d fmt 0x%x)\n",
            eye, stage, gl_tex, t.texture, t.slice, w, h, internal_fmt);
    // The eye textures take their storage through the EGLImage, not through
    // glTexStorage*, so the census would otherwise be blind to the single
    // largest allocation in the process. One eye is one slice of the shared
    // array, so the per-name figure is w*h*bpp and the pair adds up to the
    // whole texture. glDeleteTextures on the old name is what removes it —
    // which makes this the measurement of whether the guest lets go of the old
    // swapchain at all.
    klfb_vram_note(KLC_TEX, gl_tex, klfb_storage_bytes(internal_fmt, w, h, 1, 1));
    // Unity re-creates the eye swapchain at every loading transition, which is
    // exactly the event a leak hunt wants a census either side of. Rare enough
    // to print unconditionally when the census is on.
    if (klfb_census_every()) kl_glfb_gl_census(stderr);
    return 1;
}

// ---------------------------------------------------------------------------
// The NON-EYE layer's storage — see kl_glfb.h. Everything here is the eye path
// above with the key widened and the eye-specific parts left out: no rate map
// (foveation is a property of the eye render, and a UI panel is not rendered
// through one), no sRGB settle (that decides how the EYE picture is decoded),
// and no vram census key of its own.
#define KL_MTL_MAX_LAYERS 4
static kl_glfb_mtl_layer_provider g_mtl_layer_provider;
static void *g_mtl_layer_provider_ctx;
// Keyed rather than indexed: a layer id is the caller's own (kl_openxr uses the
// swapchain's slot, which is 0..63), so the row carries the key it was filed
// under. `layer` is -1 in a free row, which is a valid id nowhere.
static struct { int layer, stage; void *tex; int slice; void *image;
                uint32_t gl_tex; int w, h; }
    g_layer_mtl[KL_MTL_MAX_LAYERS * KL_MTL_MAX_STAGES];
static int g_layer_mtl_init;

static void klfb_layer_rows_init(void) {
    if (g_layer_mtl_init) return;
    g_layer_mtl_init = 1;
    for (unsigned i = 0; i < sizeof g_layer_mtl / sizeof *g_layer_mtl; i++)
        g_layer_mtl[i].layer = -1;
}

// The row for (layer, stage), or the first free one when `make_room` is set.
static int klfb_layer_row(int layer, int stage, int make_room) {
    klfb_layer_rows_init();
    int free_row = -1;
    for (unsigned i = 0; i < sizeof g_layer_mtl / sizeof *g_layer_mtl; i++) {
        if (g_layer_mtl[i].layer == layer && g_layer_mtl[i].stage == stage)
            return (int)i;
        if (g_layer_mtl[i].layer < 0 && free_row < 0) free_row = (int)i;
    }
    return make_room ? free_row : -1;
}

void kl_glfb_set_mtl_layer_provider(kl_glfb_mtl_layer_provider fn, void *ctx) {
    g_mtl_layer_provider = fn;
    g_mtl_layer_provider_ctx = ctx;
}
int kl_glfb_has_mtl_layer_provider(void) { return g_mtl_layer_provider != NULL; }

// The GL a blit needs, resolved once. Split out because BOTH mirrors use it —
// the array eye's and the non-eye layer's — and two copies of an entry-point
// table is two chances for one of them to be missing a null check.
static void (*r_GenFramebuffers)(int32_t, uint32_t *);
static void (*r_BindFramebuffer)(uint32_t, uint32_t);
static void (*r_FramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
static void (*r_FramebufferTextureLayer)(uint32_t, uint32_t, uint32_t, int32_t, int32_t);
static void (*r_BlitFramebuffer)(int32_t, int32_t, int32_t, int32_t,
                                 int32_t, int32_t, int32_t, int32_t,
                                 uint32_t, uint32_t);
static uint32_t (*r_CheckFramebufferStatus)(uint32_t);
static void (*r_GenTextures)(int32_t, uint32_t *);
static void (*r_DeleteTextures)(int32_t, const uint32_t *);
static uint8_t (*r_IsEnabled)(uint32_t);
static void (*r_Enable)(uint32_t);
static void (*r_Disable)(uint32_t);

static int klfb_mirror_resolve(int need_layer) {
    static int resolved;
    if (!resolved) {
        resolved = 1;
        r_GenFramebuffers        = asym("glGenFramebuffers");
        r_BindFramebuffer        = asym("glBindFramebuffer");
        r_FramebufferTexture2D   = asym("glFramebufferTexture2D");
        r_FramebufferTextureLayer= asym("glFramebufferTextureLayer");
        r_BlitFramebuffer        = asym("glBlitFramebuffer");
        r_CheckFramebufferStatus = asym("glCheckFramebufferStatus");
        r_GenTextures            = asym("glGenTextures");
        r_DeleteTextures         = asym("glDeleteTextures");
        r_IsEnabled              = asym("glIsEnabled");
        r_Enable                 = asym("glEnable");
        r_Disable                = asym("glDisable");
    }
    if (!r_GenFramebuffers || !r_BindFramebuffer || !r_BlitFramebuffer ||
        !r_FramebufferTexture2D || !r_GenTextures) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror: ANGLE is missing a blit entry point — "
                            "the guest's picture cannot be copied\n");
        return 0;
    }
    if (need_layer && !r_FramebufferTextureLayer) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror: no glFramebufferTextureLayer — an array "
                            "slice cannot be read\n");
        return 0;
    }
    if (!a_glGetIntegerv) {
        // Before anything touches GL state, because everything below has to put
        // it back: the framebuffer bindings and the texture binding are all read
        // through this. Restoring them to 0 instead would hand the guest the
        // default framebuffer as its render target partway through its own
        // frame. Refusing costs a black picture; guessing costs the whole one.
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror: no glGetIntegerv, so the guest's GL "
                            "state cannot be restored — refusing\n");
        return 0;
    }
    return 1;
}

// One texture (or one array slice of one) into another, same size, same format.
// Returns the GL error, or 0.
//
// The guest's state is the guest's. A blit is affected by the scissor test (and
// by nothing else in the fragment pipeline — the write masks do not apply), so
// that is what has to come off, and the two framebuffer bindings are read back
// rather than assumed: this runs inside the guest's frame, between its own
// calls.
static uint32_t klfb_mirror_blit(uint32_t src_tex, int src_layer,
                                 uint32_t dst_tex, int w, int h) {
    int32_t save_read = 0, save_draw = 0;
    a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &save_read);
    a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &save_draw);
    int scissor = r_IsEnabled ? (int)r_IsEnabled(0x0C11 /* SCISSOR_TEST */) : 0;
    if (scissor && r_Disable) r_Disable(0x0C11);

    static uint32_t read_fb, draw_fb;
    if (!read_fb) r_GenFramebuffers(1, &read_fb);
    if (!draw_fb) r_GenFramebuffers(1, &draw_fb);

    r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, read_fb);
    if (src_layer >= 0)
        r_FramebufferTextureLayer(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */,
                                  src_tex, 0, src_layer);
    else
        r_FramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1 /* TEXTURE_2D */, src_tex, 0);
    r_BindFramebuffer(0x8CA9 /* DRAW_FRAMEBUFFER */, draw_fb);
    r_FramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, dst_tex, 0);

    // Both sides checked once. An incomplete framebuffer makes the blit a no-op
    // that raises INVALID_FRAMEBUFFER_OPERATION, i.e. a black picture and one
    // error in a log full of them — the failure this whole path exists to end.
    static int checked;
    if (!checked && r_CheckFramebufferStatus) {
        checked = 1;
        uint32_t rs = r_CheckFramebufferStatus(0x8CA8);
        uint32_t ds = r_CheckFramebufferStatus(0x8CA9);
        if (rs != 0x8CD5 || ds != 0x8CD5)
            fprintf(stderr, "  [glfb] mirror: read fb status 0x%x, draw fb status 0x%x "
                            "(0x8CD5 is complete) — the picture will stay black\n",
                    rs, ds);
    }

    if (a_glGetError) while (a_glGetError()) {}
    // Source and destination are the same format, so the sRGB decode on read and
    // encode on write are each other's inverse and this is a copy. NEAREST for
    // the same reason: the rectangles are identical, so no filter is consulted.
    r_BlitFramebuffer(0, 0, w, h, 0, 0, w, h,
                      0x4000 /* COLOR_BUFFER_BIT */, 0x2600 /* NEAREST */);
    uint32_t e = a_glGetError ? a_glGetError() : 0;

    // (A KL_OVRP_MIRROR_PROBE glReadPixels diagnostic lived here and was removed:
    // reading a single pixel from an Apple lossless-compressed render target
    // segfaulted inside AGX's processCompressedRegion2D — an Apple-GPU driver
    // edge case, not a bug in the mirror itself. It never produced output.)

    r_BindFramebuffer(0x8CA8, (uint32_t)save_read);
    r_BindFramebuffer(0x8CA9, (uint32_t)save_draw);
    if (scissor && r_Enable) r_Enable(0x0C11);
    return e;
}

// Blit src_tex (full sw×sh) into a SUB-RECTANGLE of dst_tex, scaling. The
// foveal-inset composite: the base eye is already in dst_tex, and this lays the
// narrow high-resolution centre image over the middle of it, where its field of
// view maps. LINEAR because the inset is scaled UP into the base (a filter IS
// consulted, unlike the identity copy klfb_mirror_blit does).
static uint32_t klfb_blit_subrect(uint32_t src_tex, int src_layer, uint32_t dst_tex,
                                  int sw, int sh, int dx0, int dy0, int dx1, int dy1) {
    int32_t save_read = 0, save_draw = 0;
    a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &save_read);
    a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &save_draw);
    int scissor = r_IsEnabled ? (int)r_IsEnabled(0x0C11 /* SCISSOR_TEST */) : 0;
    if (scissor && r_Disable) r_Disable(0x0C11);

    static uint32_t iread_fb, idraw_fb;
    if (!iread_fb) r_GenFramebuffers(1, &iread_fb);
    if (!idraw_fb) r_GenFramebuffers(1, &idraw_fb);

    r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, iread_fb);
    if (src_layer >= 0)
        r_FramebufferTextureLayer(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */,
                                  src_tex, 0, src_layer);
    else
        r_FramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1 /* TEXTURE_2D */, src_tex, 0);
    r_BindFramebuffer(0x8CA9 /* DRAW_FRAMEBUFFER */, idraw_fb);
    r_FramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, dst_tex, 0);

    if (a_glGetError) while (a_glGetError()) {}
    r_BlitFramebuffer(0, 0, sw, sh, dx0, dy0, dx1, dy1,
                      0x4000 /* COLOR_BUFFER_BIT */, 0x2601 /* LINEAR */);
    uint32_t e = a_glGetError ? a_glGetError() : 0;

    r_BindFramebuffer(0x8CA8, (uint32_t)save_read);
    r_BindFramebuffer(0x8CA9, (uint32_t)save_draw);
    if (scissor && r_Enable) r_Enable(0x0C11);
    return e;
}

// Feathered inset: draw src_tex into the dest rect of dst_tex as a textured quad
// with a RADIAL alpha falloff, blended over what is already there (the base).
// This is what turns the hard square edge of a straight blit into a soft round
// centre that fades into the periphery — the eye sees "sharper toward the middle"
// rather than "a sharp rectangle stuck on". `inner` is the fraction of the radius
// that stays fully opaque before the fade begins (0..1). Returns 0 on success;
// nonzero means the caller should fall back to the plain blit. All GL state it
// touches is saved and restored — it runs inside the guest's own frame.
//
// `copy` drops the feather and the blend: the source's own RGBA is written as
// sampled. That is how a texture glBlitFramebuffer cannot read gets copied —
// a compressed one (ETC2/ASTC) is not colour-renderable, so it cannot be a
// read framebuffer's attachment, but it can always be sampled (see
// kl_glfb_mirror_layer).
static int klfb_feather_blit(uint32_t dst_tex, int dstw, int dsth,
                             uint32_t src_tex, int dx0, int dy0, int dx1, int dy1,
                             float inner, int copy) {
    static int tried, ok_program;
    static uint32_t prog, vao, vbo;
    static int loc_tex, loc_inner, loc_copy, loc_pos, loc_uv;
    static uint32_t (*r_CreateShader)(uint32_t);
    static void (*r_ShaderSource)(uint32_t, int32_t, const char *const *, const int32_t *);
    static void (*r_CompileShader)(uint32_t);
    static void (*r_GetShaderiv)(uint32_t, uint32_t, int32_t *);
    static uint32_t (*r_CreateProgram)(void);
    static void (*r_AttachShader)(uint32_t, uint32_t);
    static void (*r_LinkProgram)(uint32_t);
    static void (*r_GetProgramiv)(uint32_t, uint32_t, int32_t *);
    static void (*r_UseProgram)(uint32_t);
    static int32_t (*r_GetUniformLocation)(uint32_t, const char *);
    static int32_t (*r_GetAttribLocation)(uint32_t, const char *);
    static void (*r_Uniform1i)(int32_t, int32_t);
    static void (*r_Uniform1f)(int32_t, float);
    static void (*r_GenVertexArrays)(int32_t, uint32_t *);
    static void (*r_BindVertexArray)(uint32_t);
    static void (*r_GenBuffers)(int32_t, uint32_t *);
    static void (*r_BindBuffer)(uint32_t, uint32_t);
    static void (*r_BufferData)(uint32_t, long, const void *, uint32_t);
    static void (*r_VertexAttribPointer)(uint32_t, int32_t, uint32_t, uint8_t,
                                         int32_t, const void *);
    static void (*r_EnableVertexAttribArray)(uint32_t);
    static void (*r_DrawArrays)(uint32_t, int32_t, int32_t);
    static void (*r_ActiveTexture)(uint32_t);
    static void (*r_BindTexture)(uint32_t, uint32_t);
    static void (*r_TexParameteri)(uint32_t, uint32_t, int32_t);
    static void (*r_GetTexParameteriv)(uint32_t, uint32_t, int32_t *);
    static void (*r_Enable)(uint32_t);
    static void (*r_Disable)(uint32_t);
    static uint8_t (*r_IsEnabled)(uint32_t);
    static void (*r_BlendFunc)(uint32_t, uint32_t);
    static void (*r_BlendFuncSeparate)(uint32_t, uint32_t, uint32_t, uint32_t);
    static void (*r_Viewport)(int32_t, int32_t, int32_t, int32_t);
    static void (*r_BindFramebuffer2)(uint32_t, uint32_t);
    static void (*r_FramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
    static uint32_t (*r_CheckFbStatus)(uint32_t);
    static void (*r_GenFramebuffers)(int32_t, uint32_t *);
    static uint32_t feather_fb;

    if (!tried) {
        tried = 1;
        r_CreateShader = asym("glCreateShader");
        r_ShaderSource = asym("glShaderSource");
        r_CompileShader = asym("glCompileShader");
        r_GetShaderiv = asym("glGetShaderiv");
        r_CreateProgram = asym("glCreateProgram");
        r_AttachShader = asym("glAttachShader");
        r_LinkProgram = asym("glLinkProgram");
        r_GetProgramiv = asym("glGetProgramiv");
        r_UseProgram = asym("glUseProgram");
        r_GetUniformLocation = asym("glGetUniformLocation");
        r_GetAttribLocation = asym("glGetAttribLocation");
        r_Uniform1i = asym("glUniform1i");
        r_Uniform1f = asym("glUniform1f");
        r_GenVertexArrays = asym("glGenVertexArrays");
        r_BindVertexArray = asym("glBindVertexArray");
        r_GenBuffers = asym("glGenBuffers");
        r_BindBuffer = asym("glBindBuffer");
        r_BufferData = asym("glBufferData");
        r_VertexAttribPointer = asym("glVertexAttribPointer");
        r_EnableVertexAttribArray = asym("glEnableVertexAttribArray");
        r_DrawArrays = asym("glDrawArrays");
        r_ActiveTexture = asym("glActiveTexture");
        r_BindTexture = asym("glBindTexture");
        r_TexParameteri = asym("glTexParameteri");
        r_GetTexParameteriv = asym("glGetTexParameteriv");
        r_Enable = asym("glEnable");
        r_Disable = asym("glDisable");
        r_IsEnabled = asym("glIsEnabled");
        r_BlendFunc = asym("glBlendFunc");
        r_BlendFuncSeparate = asym("glBlendFuncSeparate");
        r_Viewport = asym("glViewport");
        r_BindFramebuffer2 = asym("glBindFramebuffer");
        r_FramebufferTexture2D = asym("glFramebufferTexture2D");
        r_CheckFbStatus = asym("glCheckFramebufferStatus");
        r_GenFramebuffers = asym("glGenFramebuffers");
        if (r_CreateShader && r_CreateProgram && r_UseProgram && r_DrawArrays &&
            r_GenVertexArrays && r_GenBuffers && r_VertexAttribPointer &&
            r_BindFramebuffer2 && r_FramebufferTexture2D && r_GenFramebuffers) {
            const char *vs =
                "#version 300 es\n"
                "in vec2 aPos; in vec2 aUV; out vec2 vUV;\n"
                "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
            const char *fs =
                "#version 300 es\n"
                "precision highp float;\n"
                "in vec2 vUV; uniform sampler2D uTex; uniform float uInner;\n"
                "uniform int uCopy;\n"
                "out vec4 o;\n"
                "void main(){\n"
                "  if (uCopy != 0) { o = texture(uTex, vUV); return; }\n"
                "  vec3 c = texture(uTex, vUV).rgb;\n"
                "  vec2 d = (vUV - 0.5) * 2.0;\n"
                "  float r = length(d);\n"
                "  float a = 1.0 - smoothstep(uInner, 1.0, r);\n"
                "  o = vec4(c, a);\n"
                "}\n";
            uint32_t v = r_CreateShader(0x8B31 /* VERTEX_SHADER */);
            uint32_t f = r_CreateShader(0x8B30 /* FRAGMENT_SHADER */);
            int32_t okv = 0, okf = 0, okl = 0;
            r_ShaderSource(v, 1, &vs, NULL); r_CompileShader(v);
            r_ShaderSource(f, 1, &fs, NULL); r_CompileShader(f);
            if (r_GetShaderiv) { r_GetShaderiv(v, 0x8B81, &okv);
                                 r_GetShaderiv(f, 0x8B81, &okf); }
            else okv = okf = 1;
            prog = r_CreateProgram();
            r_AttachShader(prog, v); r_AttachShader(prog, f);
            r_LinkProgram(prog);
            if (r_GetProgramiv) r_GetProgramiv(prog, 0x8B82 /* LINK_STATUS */, &okl);
            else okl = 1;
            if (okv && okf && okl) {
                loc_tex   = r_GetUniformLocation(prog, "uTex");
                loc_inner = r_GetUniformLocation(prog, "uInner");
                loc_copy  = r_GetUniformLocation(prog, "uCopy");
                loc_pos   = r_GetAttribLocation(prog, "aPos");
                loc_uv    = r_GetAttribLocation(prog, "aUV");
                r_GenVertexArrays(1, &vao);
                r_GenBuffers(1, &vbo);
                r_GenFramebuffers(1, &feather_fb);
                if (loc_pos >= 0 && loc_uv >= 0 && vao && vbo && feather_fb)
                    ok_program = 1;
            }
            if (!ok_program)
                fprintf(stderr, "  [glfb] fovea feather: shader unavailable, "
                                "falling back to a hard-edged inset\n");
        }
    }
    if (!ok_program) return 1;

    // Save every piece of state the draw disturbs.
    int32_t s_prog = 0, s_vao = 0, s_vbo = 0, s_at = 0, s_tex2d = 0, s_vp[4];
    int32_t s_draw_fb = 0;
    int32_t s_bsr = 0x1, s_bdr = 0, s_bsa = 0x1, s_bda = 0;
    a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &s_prog);
    a_glGetIntegerv(0x85B5 /* VERTEX_ARRAY_BINDING */, &s_vao);
    a_glGetIntegerv(0x8894 /* ARRAY_BUFFER_BINDING */, &s_vbo);
    a_glGetIntegerv(0x84E0 /* ACTIVE_TEXTURE */, &s_at);
    // TEXTURE_BINDING_2D is per UNIT, and the restore below puts it back on
    // unit 0 — so unit 0 is where it has to be READ. Saving the binding of
    // whichever unit the guest had selected and restoring it onto unit 0
    // overwrites unit 0 with another unit's texture, once per eye per frame.
    r_ActiveTexture(0x84C0 /* TEXTURE0 */);
    a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &s_tex2d);
    a_glGetIntegerv(0x0BA2 /* VIEWPORT */, s_vp);
    a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &s_draw_fb);
    a_glGetIntegerv(0x80C9 /* BLEND_SRC_RGB */,   &s_bsr);
    a_glGetIntegerv(0x80C8 /* BLEND_DST_RGB */,   &s_bdr);
    a_glGetIntegerv(0x80CB /* BLEND_SRC_ALPHA */, &s_bsa);
    a_glGetIntegerv(0x80CA /* BLEND_DST_ALPHA */, &s_bda);
    int blend_was = r_IsEnabled ? (int)r_IsEnabled(0x0BE2 /* BLEND */) : 0;
    int depth_was = r_IsEnabled ? (int)r_IsEnabled(0x0B71 /* DEPTH_TEST */) : 0;
    int cull_was  = r_IsEnabled ? (int)r_IsEnabled(0x0B44 /* CULL_FACE */) : 0;
    int sciss_was = r_IsEnabled ? (int)r_IsEnabled(0x0C11 /* SCISSOR_TEST */) : 0;

    r_BindFramebuffer2(0x8CA9 /* DRAW_FRAMEBUFFER */, feather_fb);
    r_FramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, dst_tex, 0);
    if (r_CheckFbStatus && r_CheckFbStatus(0x8CA9) != 0x8CD5) {
        r_BindFramebuffer2(0x8CA9, (uint32_t)s_draw_fb);
        if (s_at != 0x84C0) r_ActiveTexture((uint32_t)s_at);  // switched above
        return 1;   // let the caller blit instead
    }

    if (depth_was && r_Disable) r_Disable(0x0B71);
    if (cull_was  && r_Disable) r_Disable(0x0B44);
    if (sciss_was && r_Disable) r_Disable(0x0C11);
    if (copy) {
        if (blend_was && r_Disable) r_Disable(0x0BE2 /* BLEND */);
    } else {
        if (r_Enable) r_Enable(0x0BE2 /* BLEND */);
        if (r_BlendFunc) r_BlendFunc(0x0302 /* SRC_ALPHA */, 0x0303 /* ONE_MINUS_SRC_ALPHA */);
    }
    // An sRGB source is decoded when sampled, so an sRGB destination has to
    // encode on write for this to be a copy. Only a guest that has turned the
    // encode off (EXT_sRGB_write_control, g_srgb_write_off) can have it off.
    int srgb_off = copy && g_srgb_write_off && r_IsEnabled &&
                   !r_IsEnabled(KLFB_GL_FRAMEBUFFER_SRGB);
    if (srgb_off && r_Enable) r_Enable(KLFB_GL_FRAMEBUFFER_SRGB);
    r_Viewport(0, 0, dstw, dsth);

    float nx0 = 2.0f * dx0 / dstw - 1.0f, nx1 = 2.0f * dx1 / dstw - 1.0f;
    float ny0 = 2.0f * dy0 / dsth - 1.0f, ny1 = 2.0f * dy1 / dsth - 1.0f;
    float quad[16] = {
        nx0, ny0, 0.0f, 0.0f,
        nx1, ny0, 1.0f, 0.0f,
        nx0, ny1, 0.0f, 1.0f,
        nx1, ny1, 1.0f, 1.0f,
    };
    r_BindVertexArray(vao);
    r_BindBuffer(0x8892 /* ARRAY_BUFFER */, vbo);
    r_BufferData(0x8892, (long)sizeof quad, quad, 0x88E0 /* DYNAMIC_DRAW */);
    r_EnableVertexAttribArray((uint32_t)loc_pos);
    r_VertexAttribPointer((uint32_t)loc_pos, 2, 0x1406 /* FLOAT */, 0,
                          4 * (int)sizeof(float), (const void *)0);
    r_EnableVertexAttribArray((uint32_t)loc_uv);
    r_VertexAttribPointer((uint32_t)loc_uv, 2, 0x1406, 0,
                          4 * (int)sizeof(float),
                          (const void *)(2 * sizeof(float)));
    r_UseProgram(prog);
    r_ActiveTexture(0x84C0 /* TEXTURE0 */);
    r_BindTexture(0x0DE1, src_tex);
    // The shader samples with texture(): the inset must have a mipmap-free filter
    // (a swapchain texture can default to NEAREST_MIPMAP_* and sample as black)
    // and clamp so the fade does not wrap. Saved and restored on the guest's own
    // texture.
    int32_t s_min = 0x2601, s_mag = 0x2601, s_ws = 0x812F, s_wt = 0x812F;
    if (r_GetTexParameteriv) {
        r_GetTexParameteriv(0x0DE1, 0x2801 /* MIN_FILTER */, &s_min);
        r_GetTexParameteriv(0x0DE1, 0x2800 /* MAG_FILTER */, &s_mag);
        r_GetTexParameteriv(0x0DE1, 0x2802 /* WRAP_S */, &s_ws);
        r_GetTexParameteriv(0x0DE1, 0x2803 /* WRAP_T */, &s_wt);
    }
    if (r_TexParameteri) {
        r_TexParameteri(0x0DE1, 0x2801, 0x2601 /* LINEAR */);
        r_TexParameteri(0x0DE1, 0x2800, 0x2601);
        r_TexParameteri(0x0DE1, 0x2802, 0x812F /* CLAMP_TO_EDGE */);
        r_TexParameteri(0x0DE1, 0x2803, 0x812F);
    }
    if (loc_tex >= 0)   r_Uniform1i(loc_tex, 0);
    if (loc_inner >= 0) r_Uniform1f(loc_inner, inner);
    if (loc_copy >= 0)  r_Uniform1i(loc_copy, copy);
    r_DrawArrays(0x0005 /* TRIANGLE_STRIP */, 0, 4);

    // Restore the guest's texture parameters while its texture is still bound.
    if (r_TexParameteri) {
        r_TexParameteri(0x0DE1, 0x2801, s_min);
        r_TexParameteri(0x0DE1, 0x2800, s_mag);
        r_TexParameteri(0x0DE1, 0x2802, s_ws);
        r_TexParameteri(0x0DE1, 0x2803, s_wt);
    }

    // Restore everything else, in reverse.
    r_FramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, 0, 0);
    r_BindFramebuffer2(0x8CA9, (uint32_t)s_draw_fb);
    r_UseProgram((uint32_t)s_prog);
    r_BindVertexArray((uint32_t)s_vao);
    r_BindBuffer(0x8892, (uint32_t)s_vbo);
    r_ActiveTexture(0x84C0);
    r_BindTexture(0x0DE1, (uint32_t)s_tex2d);
    if (s_at != 0x84C0) r_ActiveTexture((uint32_t)s_at);
    r_Viewport(s_vp[0], s_vp[1], s_vp[2], s_vp[3]);
    if (r_BlendFuncSeparate)
        r_BlendFuncSeparate((uint32_t)s_bsr, (uint32_t)s_bdr,
                            (uint32_t)s_bsa, (uint32_t)s_bda);
    if (blend_was && r_Enable) r_Enable(0x0BE2);
    if (!blend_was && r_Disable) r_Disable(0x0BE2);
    if (srgb_off && r_Disable) r_Disable(KLFB_GL_FRAMEBUFFER_SRGB);
    if (depth_was && r_Enable) r_Enable(0x0B71);
    if (cull_was  && r_Enable) r_Enable(0x0B44);
    if (sciss_was && r_Enable) r_Enable(0x0C11);
    return 0;
}

// Give one GL name storage the layer provider owns. Internal: the only caller
// is kl_glfb_mirror_layer below, and the name it passes is one of OURS — see
// that function's header comment for why the guest's own name must not be
// handed to this.
static int klfb_bind_layer_mtl_texture(int layer, int stage, uint32_t gl_tex,
                                       int w, int h, uint32_t internal_fmt) {
    if (!g_mtl_layer_provider || layer < 0 || !gl_tex || w <= 0 || h <= 0) return 0;
    if (stage < 0 || stage >= KL_MTL_MAX_STAGES) {
        fprintf(stderr, "  [glfb] layer stage %d is beyond KL_MTL_MAX_STAGES (%d)\n",
                stage, KL_MTL_MAX_STAGES);
        return 0;
    }
    if (!kl_glfb_init() || !mtl_resolve()) {
        fprintf(stderr, "  [glfb] a Metal layer provider is registered but ANGLE's "
                        "interop entry points are missing — falling back to GL "
                        "storage, so this layer is not composited\n");
        return 0;
    }
    int row = klfb_layer_row(layer, stage, 1);
    if (row < 0) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [glfb] no room for layer %d stage %d — %u (layer, "
                            "stage) pairs fit and this one is NOT composited\n",
                    layer, stage,
                    (unsigned)(sizeof g_layer_mtl / sizeof *g_layer_mtl));
        }
        return 0;
    }
    // Already backed by this same GL name and size: the caller re-asserts it
    // every frame (a quad's swapchain is only known to be a quad at submission),
    // so answering from the row is what keeps that from being an allocation and
    // an eglCreateImageKHR per frame.
    if (g_layer_mtl[row].gl_tex == gl_tex && g_layer_mtl[row].w == w &&
        g_layer_mtl[row].h == h && g_layer_mtl[row].tex)
        return 1;

    kl_mtl_eye_texture t = { NULL, 0, 0, 0 };
    if (!g_mtl_layer_provider(layer, stage, w, h, internal_fmt, &t,
                              g_mtl_layer_provider_ctx) || !t.texture) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [glfb] provider declined layer=%d stage=%d %dx%d "
                            "fmt 0x%x\n", layer, stage, w, h, internal_fmt);
        }
        return 0;
    }
    // The same refusal the eye path makes, for the same reason: the extension
    // takes the size from the MTLTexture and succeeds whatever we asked for, so
    // a mismatch is a guest rendering into storage it does not have.
    if (t.w != w || t.h != h) {
        fprintf(stderr, "  [glfb] provider returned a %dx%d texture for a %dx%d "
                        "layer (layer=%d stage=%d) — refusing\n",
                t.w, t.h, w, h, layer, stage);
        return 0;
    }
    const int32_t attrs[] = {
        EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE_, t.slice,
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE_,   (int32_t)internal_fmt,
        EGL_NONE_,
    };
    void *img = a_eglCreateImageKHR(g_dpy, EGL_NO_CONTEXT_, EGL_METAL_TEXTURE_ANGLE_,
                                    t.texture, attrs);
    if (img == EGL_NO_IMAGE_) {
        fprintf(stderr, "  [glfb] eglCreateImageKHR(EGL_METAL_TEXTURE_ANGLE) failed "
                        "for layer=%d stage=%d — was the texture made on "
                        "kl_glfb_mtl_device()?\n", layer, stage);
        return 0;
    }
    a_glBindTexture_mtl(0x0DE1 /* GL_TEXTURE_2D */, gl_tex);
    while (a_glGetError && a_glGetError() != 0) {}
    a_glEGLImageTargetTexture2DOES(0x0DE1, img);
    uint32_t e = a_glGetError ? a_glGetError() : 0;
    if (e) {
        fprintf(stderr, "  [glfb] glEGLImageTargetTexture2DOES -> GL error 0x%x "
                        "(layer=%d stage=%d)\n", e, layer, stage);
        if (a_eglDestroyImageKHR) a_eglDestroyImageKHR(g_dpy, img);
        return 0;
    }
    if (g_layer_mtl[row].image && a_eglDestroyImageKHR)
        a_eglDestroyImageKHR(g_dpy, g_layer_mtl[row].image);
    g_layer_mtl[row].layer  = layer;
    g_layer_mtl[row].stage  = stage;
    g_layer_mtl[row].tex    = t.texture;
    g_layer_mtl[row].slice  = t.slice;
    g_layer_mtl[row].image  = img;
    g_layer_mtl[row].gl_tex = gl_tex;
    g_layer_mtl[row].w      = w;
    g_layer_mtl[row].h      = h;
    // As on the eye path: an EGLImage-backed name never passes through the
    // glTexStorage2D thunk, so nothing else records what size this texture is,
    // and the capture would read it at the pbuffer's size.
    klfb_note_tex_storage(gl_tex, internal_fmt, w, h);
    // A backed composition layer means the guest is presenting through the XR
    // compositor — it is immersive, not flat — even with no eye textures yet.
    // Without this, an Xash VR menu (a quad layer, plus the SDL window every SDL
    // guest creates) read as MONO and the flat shell covered it in black.
    kl_present_note_xr_layer();
    fprintf(stderr, "  [glfb] layer=%d stage=%d tex=%u is now backed by MTLTexture "
                    "%p slice %d (%dx%d fmt 0x%x)\n",
            layer, stage, gl_tex, t.texture, t.slice, w, h, internal_fmt);
    return 1;
}

// The guest's presented image, copied into storage the compositor can sample.
// See kl_glfb.h for why this is a copy and not the eye path's re-point.
//
// The destination is one GL name per (layer, stage), created here and given
// provider storage by the call above — so the layer table, the compositors and
// the readback all see what they would have seen from a re-point, and the
// guest's own texture is never touched.
static struct { int layer, stage; uint32_t tex; int w, h; uint32_t fmt; }
    g_layer_mirror[KL_MTL_MAX_LAYERS * KL_MTL_MAX_STAGES];

static void klfb_forget_drain(void);
int kl_glfb_mirror_layer(int layer, int stage, uint32_t src_tex, int src_layer,
                         int w, int h, uint32_t internal_fmt) {
    if (!g_mtl_layer_provider || !src_tex || w <= 0 || h <= 0) return 0;
    if (layer < 0 || stage < 0 || stage >= KL_MTL_MAX_STAGES) return 0;
    if (!kl_glfb_init() || !mtl_resolve()) return 0;
    if (!klfb_mirror_resolve(src_layer >= 0)) return 0;
    klfb_forget_drain();

    int row = -1, freerow = -1;
    for (unsigned i = 0; i < sizeof g_layer_mirror / sizeof *g_layer_mirror; i++) {
        if (g_layer_mirror[i].tex && g_layer_mirror[i].layer == layer &&
            g_layer_mirror[i].stage == stage) { row = (int)i; break; }
        if (!g_layer_mirror[i].tex && freerow < 0) freerow = (int)i;
    }
    if (row < 0) row = freerow;
    if (row < 0) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] no room to mirror layer %d stage %d — %u "
                            "(layer, stage) pairs fit and this one is NOT "
                            "composited\n", layer, stage,
                    (unsigned)(sizeof g_layer_mirror / sizeof *g_layer_mirror));
        return 0;
    }
    __typeof__(g_layer_mirror[0]) *m = &g_layer_mirror[row];
    // A size or format change means the guest rebuilt its swapchain, and the old
    // destination describes a picture that no longer exists — the blit would
    // happily scale one into the other.
    if (m->tex && (m->w != w || m->h != h || m->fmt != internal_fmt)) {
        fprintf(stderr, "  [glfb] mirror layer=%d stage=%d: %dx%d fmt 0x%x -> %dx%d "
                        "fmt 0x%x, reallocating\n",
                layer, stage, m->w, m->h, m->fmt, w, h, internal_fmt);
        if (r_DeleteTextures) r_DeleteTextures(1, &m->tex);
        *m = (__typeof__(*m)){0};
    }
    if (!m->tex) {
        uint32_t t = 0;
        r_GenTextures(1, &t);
        if (!t) return 0;
        // klfb_bind_layer_mtl_texture leaves GL_TEXTURE_2D bound to what it was
        // given, and this runs inside the guest's frame, between its calls — so
        // the binding is put back. Once per (layer, stage), not per frame.
        int32_t save_tex = 0;
        a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &save_tex);
        int ok = klfb_bind_layer_mtl_texture(layer, stage, t, w, h, internal_fmt);
        if (a_glBindTexture_mtl) a_glBindTexture_mtl(0x0DE1, (uint32_t)save_tex);
        if (!ok) {
            if (r_DeleteTextures) r_DeleteTextures(1, &t);
            return 0;
        }
        m->layer = layer; m->stage = stage; m->tex = t;
        m->w = w; m->h = h; m->fmt = internal_fmt;
        fprintf(stderr, "  [glfb] mirror layer=%d stage=%d: %dx%d fmt 0x%x <- guest "
                        "tex %u (one blit a frame; the guest's own storage is left "
                        "alone)\n", layer, stage, w, h, internal_fmt, src_tex);
    }

    // A COMPRESSED source cannot be blitted: it is not colour-renderable, so
    // the read framebuffer is incomplete (0x8CD6), the blit raises 0x506 and
    // the panel keeps its fresh, never-written storage — which composites as a
    // pink square. Arizona Sunshine (x2190353671014400) puts its loading
    // screen up that way: a static OVROverlay naming its own 2048x2048
    // SRGB8_ALPHA8_ETC2_EAC texture in every submit (2026-10-30). Sampling it
    // in a draw is the copy GL does allow. KL_GLFB_MIRROR_DRAW=0 goes back to
    // the blit.
    static int draw_compressed = -1;
    if (draw_compressed < 0) draw_compressed = kl_env_on("KL_GLFB_MIRROR_DRAW", 1);
    uint32_t sfmt = 0;
    int bw, bh, bb;
    if (draw_compressed && src_layer < 0 && klfb_tex_info(src_tex, &sfmt, NULL, NULL) &&
        klfb_block_geom(sfmt, &bw, &bh, &bb)) {
        int failed = klfb_feather_blit(m->tex, w, h, src_tex, 0, 0, w, h, 1.0f, 1);
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror layer=%d stage=%d: guest tex %u is "
                            "compressed (fmt 0x%x) — %s\n", layer, stage, src_tex,
                    sfmt, failed ? "the copy shader is unavailable, NOT composited"
                                 : "copied by a draw, not a blit");
        return !failed;
    }

    uint32_t e = klfb_mirror_blit(src_tex, src_layer, m->tex, w, h);
    if (e) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror layer=%d stage=%d: glBlitFramebuffer -> "
                            "GL error 0x%x\n", layer, stage, e);
        return 0;
    }
    return 1;
}

// A destroyed layer gives its rows back. Both tables are keyed by layer id and
// hold KL_MTL_MAX_LAYERS * KL_MTL_MAX_STAGES rows, and a guest that builds and
// tears down overlay panels as it goes never reuses an id: Saints & Sinners was
// at layer 66 a few minutes in, the 64 rows were all held by layers long gone,
// and from then on no new panel was composited — its dialogue choices never
// appeared (fix 15, sent in by a user, 2026-10-11). The provider's own
// MTLTexture stays the provider's.
//
// Not as it was sent in, which freed the rows where ovrp_DestroyLayer is
// called. Unreal destroys a layer on its game thread while its RHI thread goes
// on submitting — and mirroring — it, so that was GL from a thread that may
// hold no context, on rows another thread was using. The destroying thread
// only says which layer went; the rows are freed by the thread that mirrors
// (klfb_forget_drain, from kl_glfb_mirror_layer), which has the context.
//
// Two things this does not settle: a layer that is still submitted after it
// was destroyed gets rows again under its dead id, and the compositor's own
// list of layer textures (provideLayerTexture) is not told and keeps one
// entry per id for as long as the app runs.
static pthread_mutex_t g_forget_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_forget[64], g_nforget;
void kl_glfb_forget_layer(int layer) {
    if (layer < 0) return;
    pthread_mutex_lock(&g_forget_mu);
    if (g_nforget < (int)(sizeof g_forget / sizeof *g_forget)) g_forget[g_nforget++] = layer;
    pthread_mutex_unlock(&g_forget_mu);
}
static void klfb_forget_drain(void) {
    if (!g_nforget) return;
    int gone[sizeof g_forget / sizeof *g_forget], n;
    pthread_mutex_lock(&g_forget_mu);
    n = g_nforget;
    memcpy(gone, g_forget, (size_t)n * sizeof *gone);
    g_nforget = 0;
    pthread_mutex_unlock(&g_forget_mu);
    klfb_layer_rows_init();
    int freed = 0;
    for (int k = 0; k < n; k++) {
        int layer = gone[k];
        for (unsigned i = 0; i < sizeof g_layer_mirror / sizeof *g_layer_mirror; i++) {
            if (!g_layer_mirror[i].tex || g_layer_mirror[i].layer != layer) continue;
            if (r_DeleteTextures) r_DeleteTextures(1, &g_layer_mirror[i].tex);
            g_layer_mirror[i] = (__typeof__(g_layer_mirror[0])){0};
            freed++;
        }
        for (unsigned i = 0; i < sizeof g_layer_mtl / sizeof *g_layer_mtl; i++) {
            if (g_layer_mtl[i].layer != layer) continue;
            if (g_layer_mtl[i].image && a_eglDestroyImageKHR)
                a_eglDestroyImageKHR(g_dpy, g_layer_mtl[i].image);
            g_layer_mtl[i] = (__typeof__(g_layer_mtl[0])){0};
            g_layer_mtl[i].layer = -1;
            freed++;
        }
    }
    static int said;
    if (freed && said < 4) { said++;
        fprintf(stderr, "  [glfb] %d destroyed layer(s) gave %d row(s) back\n", n, freed); }
}

void kl_glfb_note_layer_mtl_texture(int layer, int stage, void *texture,
                                    int w, int h) {
    if (layer < 0 || !texture) return;
    if (stage < 0 || stage >= KL_MTL_MAX_STAGES) return;
    int row = klfb_layer_row(layer, stage, 1);
    if (row < 0) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [glfb] no room for layer %d stage %d — %u (layer, "
                            "stage) pairs fit and this one is NOT composited\n",
                    layer, stage,
                    (unsigned)(sizeof g_layer_mtl / sizeof *g_layer_mtl));
        }
        return;
    }
    if (g_layer_mtl[row].tex == texture && g_layer_mtl[row].w == w) return;
    g_layer_mtl[row].layer = layer;
    g_layer_mtl[row].stage = stage;
    g_layer_mtl[row].tex   = texture;
    g_layer_mtl[row].slice = 0;
    g_layer_mtl[row].w     = w;
    g_layer_mtl[row].h     = h;
    fprintf(stderr, "  [glfb] layer=%d stage=%d is MTLTexture %p (%dx%d), the "
                    "guest's own image\n", layer, stage, texture, w, h);
}

void *kl_glfb_layer_mtl_texture(int layer, int stage, int *out_w, int *out_h) {
    int row = klfb_layer_row(layer, stage, 0);
    if (row < 0 || !g_layer_mtl[row].tex) return NULL;
    if (out_w) *out_w = g_layer_mtl[row].w;
    if (out_h) *out_h = g_layer_mtl[row].h;
    return g_layer_mtl[row].tex;
}

// ---------------------------------------------------------------------------
// The ARRAY swapchain's way onto the compositor — see kl_glfb.h for why a copy
// is the only route and what it costs.
//
// One destination per (eye, stage), allocated the ordinary way: a GL name of
// OURS, given provider storage by the call above. So everything downstream —
// the eye table, the rate map, the sRGB settle, the census, the release path —
// sees exactly what it sees for a Unity/OVRPlugin guest, and this function owns
// nothing but the blit and the name it created.
//
// `src` is the guest texture whose image this slot currently holds, and it is a
// DIAGNOSTIC of a keying problem rather than something the blit needs: `stage`
// is a swapchain's own image index, so two different swapchains both have
// images 0..2 and land in the same slot. A guest submitting more than one
// projection layer therefore has two sources competing for one destination —
// visible as this record reallocating back and forth between their sizes.
static struct { uint32_t tex; int w, h; uint32_t fmt; uint32_t src; }
    g_eye_mirror[2][KL_MTL_MAX_STAGES];

// Which (swapchain, image) each slot currently holds, and the reason a slot is
// allocated rather than being the guest's own image index.
//
// A swapchain's image index runs 0..2 whatever swapchain it belongs to, so two
// of a guest's projection layers name the same index in the same frame and land
// in the same destination. That is why only one layer could ever be composited:
// a second one overwrote the first, and the record reallocated back and forth
// between their sizes. Keyed by (source, image) instead, every image a guest
// keeps in flight has a destination of its own and any number of layers coexist.
//
// The key does NOT include the eye: the destinations are already per-eye
// (g_eye_mirror[2][...]), so one slot number addresses both eyes' copies of the
// same guest image and a layer that names only one eye simply leaves the other
// half unused. That is what makes 16 slots cover Steam Link's three layers of
// three images with room over, instead of half that.
//
// `used` is the allocation clock, for the least-recently-used eviction: a guest
// with more live images than slots has to degrade somehow, and reusing the
// oldest — loudly — is the one that keeps the images it is actually cycling
// through. Evicting silently would present as a layer flickering.
static struct { uint32_t source; int image; uint64_t used; }
    g_eye_slot[KL_MTL_MAX_STAGES];
static uint64_t g_eye_slot_clock;

// The slot holding (source, image), allocating or evicting one if it is new.
// `source` 0 means the caller has no swapchain identity to give, and then the
// image index IS the slot — the OVRPlugin path's direct keying, unchanged.
static int klfb_eye_slot(uint32_t source, int image) {
    if (!source)
        return (image >= 0 && image < KL_MTL_MAX_STAGES) ? image : -1;
    int free_slot = -1, lru = 0;
    for (int i = 0; i < KL_MTL_MAX_STAGES; i++) {
        if (g_eye_slot[i].source == source && g_eye_slot[i].image == image) {
            g_eye_slot[i].used = ++g_eye_slot_clock;
            return i;
        }
        if (!g_eye_slot[i].source && free_slot < 0) free_slot = i;
        if (g_eye_slot[i].used < g_eye_slot[lru].used) lru = i;
    }
    int slot = free_slot >= 0 ? free_slot : lru;
    if (free_slot < 0) {
        // Named, and named every time: an eviction is a destination being
        // reused for a different picture, so a guest cycling through more
        // images than there are slots would otherwise show layers alternating
        // with each other and nothing would say why.
        fprintf(stderr, "  [glfb] eye slot pool full (%d): evicting swapchain %u "
                        "image %d for swapchain %u image %d — this guest keeps "
                        "more images live than there are slots\n",
                KL_MTL_MAX_STAGES, g_eye_slot[slot].source, g_eye_slot[slot].image,
                source, image);
        for (int e = 0; e < 2; e++) {
            if (!g_eye_mirror[e][slot].tex) continue;
            kl_glfb_release_eye_texture(e, slot);
            g_eye_mirror[e][slot] = (__typeof__(g_eye_mirror[0][0])){0};
        }
    }
    g_eye_slot[slot].source = source;
    g_eye_slot[slot].image  = image;
    g_eye_slot[slot].used   = ++g_eye_slot_clock;
    return slot;
}

// Forward-declared for the KL_GLFB_PROBE_VIDEO readbacks (here and in
// kl_glfb_image_bind); both are defined further down with the probe machinery.
static unsigned long klfb_probe_fbo(uint32_t, float *, uint8_t *, char *, size_t,
                                    uint8_t *, int32_t *, int32_t *, int32_t, int32_t);
static uint32_t klfb_read_from_texture_layer(uint32_t, int);

// KL_GLFB_PROBE_VIDEO shared scratch + a lit-count readback of any texture. Used
// to answer "did the guest's draw land lit" (the eye texture, here) vs "is the
// decoded frame in its texture" (the video texture, in kl_glfb_image_bind).
static void klfb_probe_named_tex(const char *tag, uint32_t tex, int layer,
                                 int hint_w, int hint_h) {
    static int pv_on = -1;
    if (pv_on < 0) pv_on = kl_env_on("KL_GLFB_PROBE_VIDEO", 0);
    if (!pv_on) return;
    // A full-frame glReadPixels + glFinish every eye every frame would starve the
    // stream, so SAMPLE periodically rather than every call — but do NOT cap the
    // total, so the readback keeps reporting through the menu->cutscene->gameplay
    // transitions (the 48-total cap stopped after the first ~24 menu frames and
    // never saw whether gameplay content actually reaches the swapchain).
    static unsigned runs;
    if ((runs++ % 120u) != 0) return;
    static float   *pf;
    static uint8_t *pb;
    static long     cap;
    long need = (long)hint_w * hint_h;
    if (need <= 0) return;
    if (need > cap) {
        free(pf); free(pb);
        pf = malloc((size_t)need * 16);
        pb = malloc((size_t)need * 4);
        cap = (pf && pb) ? need : 0;
    }
    if (cap < need) return;
    static void (*r_bindfb)(uint32_t, uint32_t);
    static void (*r_bindtex)(uint32_t, uint32_t);
    if (!r_bindfb)  r_bindfb  = asym("glBindFramebuffer");
    if (!r_bindtex) r_bindtex = asym("glBindTexture");
    int32_t s_t = 0, s_rf = 0, s_df = 0;
    if (a_glGetIntegerv) {
        a_glGetIntegerv(0x8069, &s_t);    // TEXTURE_BINDING_2D
        a_glGetIntegerv(0x8CAA, &s_rf);   // READ_FRAMEBUFFER_BINDING
        a_glGetIntegerv(0x8CA6, &s_df);   // DRAW_FRAMEBUFFER_BINDING
    }
    char nt[160] = "no framebuffer";
    unsigned long lit = 0;
    uint32_t fb = klfb_read_from_texture_layer(tex, layer);
    if (fb) lit = klfb_probe_fbo(fb, pf, pb, nt, sizeof nt, NULL, NULL, NULL,
                                 hint_w, hint_h);
    if (r_bindfb) { r_bindfb(0x8CA8, (uint32_t)s_rf); r_bindfb(0x8CA9, (uint32_t)s_df); }
    if (r_bindtex) r_bindtex(0x0DE1, (uint32_t)s_t);
    // Log every sample (the periodic gate above already throttles); the moving
    // `lit`/mean tells menu-vs-cutscene-vs-gameplay apart.
    fprintf(stderr, "  [glfb] PROBE_%s tex=%u layer=%d: %lu lit of %dx%d (%s)\n",
            tag, tex, layer, lit, hint_w, hint_h, nt);
}

// Both defined further down with the PNG/probe machinery.
static int klfb_write_png(const char *path, const uint8_t *px,
                          int32_t w, int32_t h);

// Read a texture back and write it as a PNG. The alignment instrument: a seam
// whose two sides are DOUBLED rather than merely differently sharp is a
// disagreement between the field of view a layer states and the image behind
// it, and no amount of reasoning about stated frustums can measure that — the
// two images have to be compared to each other. Diagnostic-only and unthrottled,
// because the caller dumps once.
int kl_glfb_dump_tex(const char *path, uint32_t tex, int layer, int w, int h) {
    if (!path || !tex || w <= 0 || h <= 0) return 0;
    static void (*r_bindfb)(uint32_t, uint32_t);
    static void (*r_bindtex)(uint32_t, uint32_t);
    static void (*r_bindbuf)(uint32_t, uint32_t);
    static void (*r_pixelstorei)(uint32_t, int32_t);
    if (!r_bindfb)      r_bindfb      = asym("glBindFramebuffer");
    if (!r_bindtex)     r_bindtex     = asym("glBindTexture");
    if (!r_bindbuf)     r_bindbuf     = asym("glBindBuffer");
    if (!r_pixelstorei) r_pixelstorei = asym("glPixelStorei");
    if (!a_glReadPixels || !r_bindfb) return 0;

    // The size is the CALLER's, taken from the swapchain that owns the image,
    // and it is checked against the allocation table rather than discovered
    // from the framebuffer. The first cut of this read a size back out of the
    // attachment and got the WINDOW (1280x800) for a 1536x1536 layer, then
    // wrote a perfectly plausible PNG of the wrong surface — which is worse
    // than writing nothing, because the picture invites you to measure it.
    uint32_t tfmt = 0; int32_t tw = 0, th = 0;
    klfb_tex_info(tex, &tfmt, &tw, &th);
    if (tw > 0 && th > 0 && (tw != w || th != h)) {
        fprintf(stderr, "  [glfb] dump %s: asked for %dx%d but texture %u is "
                        "%dx%d — refusing rather than writing the wrong "
                        "picture\n", path, w, h, tex, tw, th);
        return 0;
    }

    uint32_t fb = klfb_read_from_texture_layer(tex, layer);
    if (!fb) {
        fprintf(stderr, "  [glfb] dump %s: texture %u layer %d could not be "
                        "attached for reading — nothing written\n",
                path, tex, layer);
        return 0;
    }
    uint8_t *px = malloc((size_t)w * (size_t)h * 4);
    if (!px) return 0;

    int32_t s_rf = 0, s_df = 0, s_t = 0, pack_buf = 0, pack_align = 4,
            pack_rowlen = 0;
    if (a_glGetIntegerv) {
        a_glGetIntegerv(0x8CAA, &s_rf);
        a_glGetIntegerv(0x8CA6, &s_df);
        a_glGetIntegerv(0x8069, &s_t);
        a_glGetIntegerv(0x88ED, &pack_buf);      // PIXEL_PACK_BUFFER_BINDING
        a_glGetIntegerv(0x0D05, &pack_align);
        a_glGetIntegerv(0x0D02, &pack_rowlen);
    }
    // A bound PIXEL_PACK_BUFFER turns the pointer below into an OFFSET: zeroes,
    // no error. Neutralised and restored, like every other read here.
    if (pack_buf && r_bindbuf) r_bindbuf(0x88EB, 0);
    if (r_pixelstorei) {
        if (pack_align != 4) r_pixelstorei(0x0D05, 4);
        if (pack_rowlen)     r_pixelstorei(0x0D02, 0);
    }
    r_bindfb(0x8CA8 /* READ_FRAMEBUFFER */, fb);
    // The read buffer is part of the framebuffer's state and a stale one is a
    // GL_INVALID_ENUM on the read, not a wrong picture — set it explicitly.
    static void (*r_readbuffer)(uint32_t);
    if (!r_readbuffer) r_readbuffer = asym("glReadBuffer");
    if (r_readbuffer) r_readbuffer(0x8CE0 /* COLOR_ATTACHMENT0 */);
    if (a_glFinish) a_glFinish();
    a_glReadPixels(0, 0, w, h, 0x1908 /* RGBA */, 0x1401 /* UNSIGNED_BYTE */, px);
    uint32_t err = a_glGetError ? a_glGetError() : 0;

    if (pack_buf && r_bindbuf) r_bindbuf(0x88EB, (uint32_t)pack_buf);
    if (r_pixelstorei) {
        if (pack_align != 4) r_pixelstorei(0x0D05, pack_align);
        if (pack_rowlen)     r_pixelstorei(0x0D02, pack_rowlen);
    }
    if (r_bindfb) { r_bindfb(0x8CA8, (uint32_t)s_rf); r_bindfb(0x8CA9, (uint32_t)s_df); }
    if (r_bindtex) r_bindtex(0x0DE1, (uint32_t)s_t);

    int ok = 0;
    if (err) {
        fprintf(stderr, "  [glfb] dump %s: glReadPixels error 0x%x — nothing "
                        "written\n", path, err);
    } else {
        errno = 0;
        ok = klfb_write_png(path, px, (int32_t)w, (int32_t)h) != 0;
        int werr = errno;
        // The mean is the instrument checking itself: a uniform 0 here is a
        // read that succeeded and saw nothing, which is a different fault from
        // the two refusals above and must not look like a picture.
        double sum = 0;
        for (long i = 0; i < (long)w * h; i++)
            sum += (px[i*4] + px[i*4+1] + px[i*4+2]) / 3.0;
        // Name the errno: on a device the usual answer is that the directory
        // is outside the sandbox, and "write FAILED" alone sends you looking at
        // the GL instead of at the path.
        fprintf(stderr, "  [glfb] dump %s: tex=%u layer=%d %dx%d mean %.2f -> %s%s%s\n",
                path, tex, layer, w, h, sum / ((double)w * h),
                ok ? "written" : "write FAILED",
                ok || !werr ? "" : " — ", ok || !werr ? "" : strerror(werr));
    }
    free(px);
    return ok;
}

void kl_glfb_probe_tex(const char *tag, uint32_t tex, int layer,
                       int hint_w, int hint_h) {
    klfb_probe_named_tex(tag, tex, layer, hint_w, hint_h);
}

// A foveated guest (Steam Link) hands us a low-resolution base eye and a
// separate high-resolution centre inset. If the compositor's eye texture is the
// base's own size, laying the inset into its centre SUB-rectangle scales the
// inset DOWN to fit — throwing away the very detail it carries, so the centre
// ends up no sharper than the periphery. So the eye texture is allocated LARGER
// than the base (the base is upscaled into it, which costs the base nothing it
// had) and the inset then lands at close to full resolution. num/den is the
// factor; 1/1 (the default) is the old behaviour for every non-foveated guest.
static int g_eye_scale_num = 1, g_eye_scale_den = 1, g_eye_cap = 3456;
void kl_glfb_set_eye_mirror_scale(int num, int den) {
    if (num > 0 && den > 0) { g_eye_scale_num = num; g_eye_scale_den = den; }
}
void kl_glfb_set_eye_mirror_cap(int px) { if (px > 0) g_eye_cap = px; }
// Uniform upscale by num/den, aspect preserved, capped so the larger side never
// exceeds g_eye_cap — a base already near the display's resolution (Steam Link's
// 3072-wide menu layers) must not be doubled to 6144 and churn the allocator,
// while the small streamed base (1152) still grows enough to host the inset.
static void klfb_eye_scaled(int w, int h, int *ow, int *oh) {
    double s = (double)g_eye_scale_num / (double)g_eye_scale_den;
    if (s < 1.0) s = 1.0;
    int mx = w > h ? w : h;
    if (g_eye_cap > 0 && (double)mx * s > (double)g_eye_cap)
        s = (double)g_eye_cap / (double)mx;
    int mw = (int)(w * s + 0.5), mh = (int)(h * s + 0.5);
    if (mw < w) mw = w;
    if (mh < h) mh = h;
    *ow = mw; *oh = mh;
}

// Every slot this swapchain owns, released together — the teardown half of the
// allocator above. Without it a destroyed swapchain's slots stay claimed by a
// source that no longer exists, and a guest that rebuilds its swapchains fills
// the pool and starts evicting the ones it is using: layers alternating with
// each other, every counter healthy.
void kl_glfb_forget_eye_source(uint32_t source) {
    if (!source) return;
    int freed = 0;
    for (int i = 0; i < KL_MTL_MAX_STAGES; i++) {
        if (g_eye_slot[i].source != source) continue;
        for (int e = 0; e < 2; e++) {
            if (!g_eye_mirror[e][i].tex) continue;
            kl_glfb_release_eye_texture(e, i);
            g_eye_mirror[e][i] = (__typeof__(g_eye_mirror[0][0])){0};
        }
        g_eye_slot[i] = (__typeof__(g_eye_slot[0])){0};
        freed++;
    }
    if (freed)
        fprintf(stderr, "  [glfb] swapchain %u destroyed: %d eye slot(s) freed\n",
                source, freed);
}

// The compositor-side destination for one (eye, slot) of a mirrored swapchain:
// found, or (re)allocated.
static __typeof__(g_eye_mirror[0][0]) *klfb_mirror_dest(int eye, int slot, uint32_t source,
                                                         int stage, uint32_t src_tex,
                                                         int src_layer, int w, int h,
                                                         uint32_t internal_fmt) {
    // The destination is allocated at the SCALED size (see klfb_eye_scaled): for
    // a foveated guest it is larger than the source eye so the centre inset laid
    // over it later keeps its detail. The base is upscaled into it here.
    int mw, mh;
    klfb_eye_scaled(w, h, &mw, &mh);

    // (Re)allocate the destination. A size or format change means the guest
    // rebuilt its swapchain, and the old destination describes a picture that no
    // longer exists — release it rather than blit a mismatch, which the blit
    // would happily scale.
    __typeof__(g_eye_mirror[0][0]) *m = &g_eye_mirror[eye][slot];
    // ...and the eye table is the authority on whether our name still exists.
    // kl_glfb_release_eye_texture deletes it, and it has callers that know
    // nothing about this record — a swapchain teardown, a re-bind backstop — so
    // a destination remembered here and gone there would be a blit into a
    // deleted name: GL_INVALID_OPERATION, once, and a black eye for the rest of
    // the run.
    if (m->tex && g_eye_mtl[eye][slot].gl_tex != m->tex) *m = (__typeof__(*m)){0};
    if (m->tex && (m->w != mw || m->h != mh || m->fmt != internal_fmt)) {
        fprintf(stderr, "  [glfb] mirror eye=%d slot=%d: %dx%d fmt 0x%x (guest tex "
                        "%u) -> %dx%d fmt 0x%x (guest tex %u), reallocating — the "
                        "guest rebuilt this swapchain\n",
                eye, slot, m->w, m->h, m->fmt, m->src, mw, mh, internal_fmt, src_tex);
        kl_glfb_release_eye_texture(eye, slot);   // drops the EGLImage AND our name
        *m = (__typeof__(*m)){0};
    }
    if (!m->tex) {
        uint32_t t = 0;
        r_GenTextures(1, &t);
        if (!t) return NULL;
        // kl_glfb_bind_eye_mtl_texture leaves GL_TEXTURE_2D bound to whatever it
        // was given, which is fine where it is normally called from (the guest's
        // own graphics setup, in ovrp_SetupEyeTexture2) and is not fine here:
        // this runs inside the guest's frame, between its calls. Six times a run
        // rather than every frame, and the failure would be a texture binding
        // the guest never asked for surviving into its next draw.
        int32_t save_tex = 0;
        a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &save_tex);
        int ok = kl_glfb_bind_eye_mtl_texture(eye, slot, t, mw, mh, internal_fmt);
        if (a_glBindTexture_mtl) a_glBindTexture_mtl(0x0DE1, (uint32_t)save_tex);
        if (!ok) {
            // The provider declined, so the name is ours and nothing else took a
            // reference to it. kl_glfb_bind_eye_mtl_texture has already said why.
            if (r_DeleteTextures) r_DeleteTextures(1, &t);
            return NULL;
        }
        m->tex = t; m->w = mw; m->h = mh; m->fmt = internal_fmt; m->src = src_tex;
        fprintf(stderr, "  [glfb] mirror eye=%d slot=%d: %dx%d fmt 0x%x <- swapchain "
                        "%u image %d (guest tex %u layer %d) %s(one blit a frame; "
                        "the array swapchain cannot be re-pointed)\n",
                eye, slot, mw, mh, internal_fmt, source, stage, src_tex, src_layer,
                (mw != w || mh != h) ? "UPSCALED from source " : "");
    }

    return m;
}

int kl_glfb_mirror_eye_layer(int eye, int stage, uint32_t source,
                             uint32_t src_tex, int src_layer,
                             int w, int h, uint32_t internal_fmt) {
    if (!g_mtl_provider || !src_tex || w <= 0 || h <= 0) return -1;
    if (eye < 0 || eye > 1 || stage < 0) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror eye=%d image=%d is out of range — "
                            "this eye is not composited\n", eye, stage);
        return -1;
    }
    if (!kl_glfb_init() || !mtl_resolve()) return -1;

    // KL_HL2_THREADED_MIRROR: with mat_queue_mode 2 the guest renders on its own
    // render thread and this eye-mirror runs on a thread (xrEndFrame's) that holds
    // NO current GL context — so r_GenTextures hands back name 0 (this returns -1,
    // the frame's slot never propagates, and klxr_EndFrame keeps replaying the last
    // reachable frame, which is the menu) and the blit no-ops (the destination that
    // WAS allocated keeps its menu-era pixels). Both read on device as the frozen,
    // slightly-flashing menu image in gameplay while the guest's own array textures
    // (the draw census centre-pixel readback) go on changing every frame — the proof
    // that the freeze is this handoff, not the guest. Take a context on THIS thread
    // first: migration mode hands us a shared context in the ROOT share group when
    // the render thread holds the root (so src_tex, a shared texture, is visible to
    // the blit), or the root itself when it is free; idempotent when this thread
    // already holds one, so the single-threaded path re-binds what it already has.
    //
    // Default ON for hl2, whose shipping cfg runs mat_queue_mode 2, and OFF for
    // every other title so the proven GL paths (Steam Link's layer mirror, JKXR's
    // quad, Unity's array eyes — all issued from a thread that already holds the
    // context) are untouched; Vulkan titles (wanderer/twd2) never reach this GL
    // path at all. KL_HL2_THREADED_MIRROR=0 is the A/B that restores the old
    // no-context behaviour for hl2.
    static int threaded_mirror = -1;
    if (threaded_mirror < 0) {
        extern const char *kl_driver_target_name(void);
        const char *tgt = kl_driver_target_name();
        threaded_mirror = kl_env_on("KL_HL2_THREADED_MIRROR",
                                    tgt && !strcmp(tgt, "hl2"));
    }
    if (threaded_mirror) kl_glfb_make_current();

    // The guest's (swapchain, image) pair, mapped to a destination of its own.
    // See klfb_eye_slot: the image index alone is not unique across a guest's
    // swapchains, which is why several projection layers used to collide in one
    // destination and only one of them could be composited.
    int slot = klfb_eye_slot(source, stage);
    if (slot < 0) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror image %d has no slot (%d slots, no "
                            "swapchain identity given) — not composited\n",
                    stage, KL_MTL_MAX_STAGES);
        return -1;
    }

    // Before the first bind below, because that is what asks the provider for
    // storage and both providers build a rate map from the size they are asked
    // for. A map already in force (KleptonCompositor builds one in primeDisplay,
    // before the guest has rendered anything) is retired here.
    if (!g_eye_mirroring) {
        g_eye_mirroring = 1;
        if (g_eye_rate_map) kl_glfb_set_eye_rate_map(0, 0, 0, 0, NULL);
    }

    if (!klfb_mirror_resolve(src_layer >= 0)) return -1;

    __typeof__(g_eye_mirror[0][0]) *m =
        klfb_mirror_dest(eye, slot, source, stage, src_tex, src_layer, w, h, internal_fmt);
    if (!m) return -1;
    int mw = m->w, mh = m->h;

    // KL_GLFB_PROBE_VIDEO: read the guest's rendered EYE texture just before we
    // copy it to the compositor. Bright here => the guest DID render the video,
    // and the black is between this and the display; dark here => the guest's
    // draw itself produced black from the (proven-bright) video texture.
    klfb_probe_named_tex("EYE", src_tex, src_layer, w, h);

    // Identity copy at 1:1 (NEAREST, exact); an upscale into a larger eye
    // texture goes through the LINEAR sub-rect blit, filling it whole.
    // A foveated mirror copies WARPED texels, and the copy itself must not go
    // through the map. The patched blit's texel-for-texel mode would be the
    // tool, and it does not cover this case: it is implemented for a 2D source
    // only, and this source is a slice of an array, so with the map on both
    // sides ANGLE resamples the warped picture through the map a second time
    // (on device, 2026-10-08: the eye's picture squeezed into the corner of the
    // corner — 65% x 49% of the view after the compositor's unwarp, exactly the
    // physical fraction once more). So for the duration of the copy the
    // destination is taken out of the registry, by texture and by size: the pass
    // ANGLE builds for it is then an ordinary one, and an unscaled NEAREST blit
    // between equal rectangles is an identity on texels whatever they hold. The
    // guest's own framebuffers are not touched by this — their descriptors are
    // cached, and nothing else runs on this context in between.
    int unmap = g_eye_rate_map && klfb_mirror_vrr() && a_SetRateMapForSize && a_SetRateMap &&
                g_eye_rate_w == mw && g_eye_rate_h == mh;
    int rule_was = g_rate_rule_on;
    if (unmap) {
        klfb_rate_rule(0);
        if (g_eye_mtl[eye][slot].tex) a_SetRateMap(g_eye_mtl[eye][slot].tex, NULL);
    }
    // ...and of a foveated eye only the part that was written is copied.
    int cw = w, ch = h;
    if (unmap && mw == w && mh == h && g_eye_rate_pw > 0 && g_eye_rate_pw <= w &&
        g_eye_rate_ph > 0 && g_eye_rate_ph <= h) {
        cw = g_eye_rate_pw; ch = g_eye_rate_ph;
    }
    uint32_t e = (mw == w && mh == h)
        ? klfb_mirror_blit(src_tex, src_layer, m->tex, cw, ch)
        : klfb_blit_subrect(src_tex, src_layer, m->tex, w, h, 0, 0, mw, mh);
    if (unmap) {
        klfb_rate_rule(rule_was);
        if (g_eye_mtl[eye][slot].tex) a_SetRateMap(g_eye_mtl[eye][slot].tex, g_eye_rate_map);
    }
    if (e) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror eye=%d slot=%d: glBlitFramebuffer -> "
                            "GL error 0x%x\n", eye, slot, e);
        return -1;
    }
    return slot;
}

// Lay a foveal inset over the base eye already mirrored into (eye, stage). The
// caller (the OpenXR layer loop) has computed, from the two layers' fields of
// view, where the inset's frustum falls inside the base's — as fractions of the
// eye in [0,1] — so this only turns those into pixels and blits. No-op until the
// base is present (m->tex): the inset is meaningless on its own.
void kl_glfb_overlay_eye_inset(int eye, int stage, uint32_t tex, int layer,
                               int iw, int ih,
                               float nx0, float ny0, float nx1, float ny1) {
    if (!g_mtl_provider || !tex || iw <= 0 || ih <= 0) return;
    if (eye < 0 || eye > 1 || stage < 0 || stage >= KL_MTL_MAX_STAGES) return;
    __typeof__(g_eye_mirror[0][0]) *m = &g_eye_mirror[eye][stage];
    if (!m->tex || m->w <= 0 || m->h <= 0) return;    // base not mirrored yet
    if (nx0 < 0) nx0 = 0; if (nx0 > 1) nx0 = 1;
    if (nx1 < 0) nx1 = 0; if (nx1 > 1) nx1 = 1;
    if (ny0 < 0) ny0 = 0; if (ny0 > 1) ny0 = 1;
    if (ny1 < 0) ny1 = 0; if (ny1 > 1) ny1 = 1;
    int dx0 = (int)(nx0 * m->w + 0.5f), dx1 = (int)(nx1 * m->w + 0.5f);
    int dy0 = (int)(ny0 * m->h + 0.5f), dy1 = (int)(ny1 * m->h + 0.5f);
    if (dx1 <= dx0 || dy1 <= dy0) return;
    if (!klfb_mirror_resolve(layer >= 0)) return;

    // Feather the edge into a soft round centre, unless disabled or the inset is
    // an ARRAY slice (the feather shader samples a plain sampler2D). The feather
    // returns nonzero if its program could not be built, so a straight blit is
    // always the fallback — the sharp centre still lands, just with a hard edge.
    static int feather = -1, inner_x100 = -1;
    if (feather < 0)     feather = kl_env_on("KL_XR_FOVEA_FEATHER", 1);
    // Higher = WIDER fully-sharp core before the fade begins (0..100). 78 keeps
    // most of the inset crisp with the fade concentrated at the very rim.
    if (inner_x100 < 0)  inner_x100 = kl_env_int("KL_XR_FOVEA_INNER", 78);
    const char *how = "blit";
    uint32_t e = 0;
    if (feather && layer < 0 &&
        klfb_feather_blit(m->tex, m->w, m->h, tex, dx0, dy0, dx1, dy1,
                          inner_x100 / 100.0f, 0) == 0) {
        how = "feather";
    } else {
        e = klfb_blit_subrect(tex, layer, m->tex, iw, ih, dx0, dy0, dx1, dy1);
    }
    static int said;
    if (said++ < 6)
        fprintf(stderr, "  [glfb] fovea inset eye=%d stage=%d: %dx%d -> "
                        "eye rect [%d,%d..%d,%d] of %dx%d (%s)%s\n",
                eye, stage, iw, ih, dx0, dy0, dx1, dy1, m->w, m->h, how,
                e ? " GL error" : "");
}

// ---------------------------------------------------------------------------
// The decoded-video image. An AHardwareBuffer — which is one of our
// CVPixelBuffers, see kl_mediandk.h — sampled as a GL texture.
//
// The guest's sequence is Android's, and every step of it is missing here:
//
//     buf = eglGetNativeClientBufferANDROID(ahardwarebuffer)
//     img = eglCreateImageKHR(dpy, NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, buf, ...)
//     glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex)
//     glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img)
//
// ANGLE's Metal backend has no external images at all (DisplayMtl.mm:984)
// and no notion of an Android buffer, so there is nothing to forward to. What it
// *does* have is EGL_ANGLE_iosurface_client_buffer, and a VideoToolbox pixel
// buffer is IOSurface-backed by construction (kl_vtdec.c asks for it) — so the
// image becomes a pbuffer over the same IOSurface, and the target call becomes
// eglBindTexImage. DisplayMtl's own comment is the licence: "Metal can bind
// IOSurfaces to regular 2D textures", which is why its configs report
// bindToTextureTarget == EGL_TEXTURE_2D rather than the rectangle target the
// desktop-GL backend needs.
//
// The guest cannot tell the difference. Its sampler is already a plain
// `sampler2D` on GL_TEXTURE_2D by the time this runs (klfb_rewrite_glsl and
// klfb_detarget did that), and BGRA output means the YUV→RGB conversion
// an external image would have promised has already happened in the decoder.
#define EGL_IOSURFACE_ANGLE_        0x3454
#define EGL_IOSURFACE_PLANE_ANGLE_  0x345A
#define EGL_TEXTURE_TYPE_ANGLE_     0x345C
#define EGL_TEXTURE_FORMAT_         0x3080
#define EGL_TEXTURE_RGBA_           0x305E
#define EGL_TEXTURE_TARGET_         0x3081
#define EGL_TEXTURE_2D_EGL_         0x305F
#define EGL_BACK_BUFFER_            0x3084
#define GL_BGRA_EXT_                0x80E1
#define GL_UNSIGNED_BYTE_           0x1401
#define GL_TEXTURE_BINDING_2D_      0x8069

static void *(*a_eglCreatePbufferFromClientBuffer)(void *, uint32_t, void *, void *,
                                                   const int32_t *);
static unsigned (*a_eglDestroySurface_img)(void *, void *);
static unsigned (*a_eglBindTexImage)(void *, void *, int32_t);
static unsigned (*a_eglReleaseTexImage)(void *, void *, int32_t);
static int32_t  (*a_eglGetError_img)(void);
static void     (*a_glTexParameteri_img)(uint32_t, uint32_t, int32_t);
static void     (*a_glGetTexParameteriv_img)(uint32_t, uint32_t, int32_t *);

typedef struct klfb_image {
    uint32_t magic;
    // Exactly one of the two storages is set, and it is the frame's pixel
    // format that chose: BGRA (KL_VTDEC_BGRA=1, or the Simulator) rides an
    // IOSurface pbuffer bound with eglBindTexImage; the decoder's native
    // biplanar YCbCr rides an EGLImage over a private-format MTLTexture,
    // targeted with glEGLImageTargetTexture2DOES.
    void    *pbuf;              // the EGLSurface over the IOSurface
    void    *img;               // the EGLImage over the MTLTexture
    void    *cvtex;             // ...whose storage the CVMetalTexture keeps alive
    void    *pixels;            // ...over this retained CVPixelBuffer
    int      w, h;
    uint32_t bound_tex;         // the GL name it is bound to, 0 if none
    struct klfb_image *next;
} klfb_image;

#define KLFB_IMAGE_MAGIC 0x4b4c4749u   /* 'KLGI' */

// A registry rather than a bare magic check, because kl_glfb_is_image() is asked
// about pointers the guest chose: an EGLImage from somewhere else is a valid
// object we must forward rather than dereference for a magic word.
static klfb_image  *g_images;
static pthread_mutex_t g_images_lk = PTHREAD_MUTEX_INITIALIZER;

static int img_resolve(void) {
    if (a_eglCreatePbufferFromClientBuffer) return 1;
    if (!g_ready) return 0;
    a_eglCreatePbufferFromClientBuffer = asym("eglCreatePbufferFromClientBuffer");
    a_eglDestroySurface_img            = asym("eglDestroySurface");
    a_eglBindTexImage                  = asym("eglBindTexImage");
    a_eglReleaseTexImage               = asym("eglReleaseTexImage");
    a_eglGetError_img                  = asym("eglGetError");
    a_glTexParameteri_img              = asym("glTexParameteri");
    a_glGetTexParameteriv_img          = asym("glGetTexParameteriv");
    return a_eglCreatePbufferFromClientBuffer && a_eglBindTexImage
        && a_eglReleaseTexImage && a_eglDestroySurface_img;
}

int kl_glfb_is_image(const void *h) {
    if (!h) return 0;
    int found = 0;
    pthread_mutex_lock(&g_images_lk);
    for (klfb_image *i = g_images; i; i = i->next)
        if (i == h) { found = 1; break; }
    pthread_mutex_unlock(&g_images_lk);
    return found;
}

// The decoder's native biplanar YCbCr, as ONE texture whose SAMPLER performs
// the YUV->RGB conversion — which is the promise samplerExternalOES makes on
// Android, kept in hardware. The pixel-format numbers are Apple's private
// single-plane YCbCr MTLPixelFormats (WebKit's MetalSPI.h; the patched ANGLE's
// format table knows them). The NON-sRGB variants, deliberately: sampling
// returns the transfer-encoded code values, exactly what the BGRA path
// returned, so the meaning of what the guest writes into its eye texture does
// not move (the sRGB variants would hand the shader linear instead).
//
// The 10-bit row is the PACKED family, not P010: packed (as its
// lossless/lossy compressed forms) is what VideoToolbox actually hands back
// for a 10-bit stream when no format is requested — a Steam Link HEVC Main10
// session decodes to '&xf0'. The unpacked biplanar 10-bit forms are never
// produced unconstrained, so they are deliberately unmapped: anything outside
// these families hits the refusal below, which names the fourcc, and
// KL_VTDEC_BGRA=1 is the escape hatch.
static unsigned long klfb_video_mtl_format(uint32_t pf) {
    switch (pf) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
    case kCVPixelFormatType_Lossless_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_Lossless_420YpCbCr8BiPlanarFullRange:
    case kCVPixelFormatType_Lossy_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_Lossy_420YpCbCr8BiPlanarFullRange:
        return 500;             // MTLPixelFormatYCBCR8_420_2P
    case kCVPixelFormatType_Lossless_420YpCbCr10PackedBiPlanarVideoRange:
    case kCVPixelFormatType_Lossless_420YpCbCr10PackedBiPlanarFullRange:
    case kCVPixelFormatType_Lossy_420YpCbCr10PackedBiPlanarVideoRange:
        return 508;             // MTLPixelFormatYCBCR10_420_2P_PACKED
    default:
        return 0;
    }
}

static void *klfb_image_from_native(void *pixels, uint32_t pf,
                                    int *out_w, int *out_h) {
    unsigned long fmt = klfb_video_mtl_format(pf);
    if (!fmt) {
        static unsigned said;
        if (said++ < 4)
            fprintf(stderr, "  [glfb] decoded frame is pixel format '%c%c%c%c', which "
                            "has no single-plane YCbCr MTLPixelFormat here — "
                            "KL_VTDEC_BGRA=1 restores the BGRA path\n",
                    (char)(pf >> 24), (char)(pf >> 16), (char)(pf >> 8), (char)pf);
        return NULL;
    }
    if (!mtl_resolve()) {
        fprintf(stderr, "  [glfb] no ANGLE Metal interop entry points — a native-"
                        "format frame cannot be turned into a texture\n");
        return NULL;
    }
    void *cvtex = NULL;
    void *tex = kl_cvmtl_texture(pixels, fmt, &cvtex);
    if (!tex) return NULL;
    const int32_t attrs[] = { EGL_NONE_ };
    void *img = a_eglCreateImageKHR(g_dpy, EGL_NO_CONTEXT_, EGL_METAL_TEXTURE_ANGLE_,
                                    tex, attrs);
    if (img == EGL_NO_IMAGE_) {
        fprintf(stderr, "  [glfb] eglCreateImageKHR(EGL_METAL_TEXTURE_ANGLE) failed "
                        "for a '%c%c%c%c' video frame — is the vendored ANGLE the "
                        "patched one (it knows the private YCbCr formats by "
                        "number)?\n",
                (char)(pf >> 24), (char)(pf >> 16), (char)(pf >> 8), (char)pf);
        kl_cvmtl_release(cvtex);
        return NULL;
    }
    klfb_image *im = calloc(1, sizeof *im);
    if (!im) {
        if (a_eglDestroyImageKHR) a_eglDestroyImageKHR(g_dpy, img);
        kl_cvmtl_release(cvtex);
        return NULL;
    }
    im->magic  = KLFB_IMAGE_MAGIC;
    im->img    = img;
    im->cvtex  = cvtex;
    im->pixels = pixels;
    CVPixelBufferRetain((CVPixelBufferRef)pixels);
    im->w = (int)CVPixelBufferGetWidth((CVPixelBufferRef)pixels);
    im->h = (int)CVPixelBufferGetHeight((CVPixelBufferRef)pixels);
    pthread_mutex_lock(&g_images_lk);
    im->next = g_images;
    g_images = im;
    pthread_mutex_unlock(&g_images_lk);
    if (out_w) *out_w = im->w;
    if (out_h) *out_h = im->h;
    static unsigned made;
    if (made++ < 4)
        fprintf(stderr, "  [glfb] video image %p: '%c%c%c%c' frame as a %dx%d "
                        "YCbCr MTLTexture (format %lu, sampler converts)\n",
                (void *)im, (char)(pf >> 24), (char)(pf >> 16), (char)(pf >> 8),
                (char)pf, im->w, im->h, fmt);
    return im;
}

void *kl_glfb_image_from_pixels(void *pixels, int *out_w, int *out_h) {
    if (!pixels) return NULL;
    if (!kl_glfb_init() || !img_resolve()) {
        fprintf(stderr, "  [glfb] no ANGLE IOSurface entry points — a decoded frame "
                        "cannot be turned into a texture (is KL_GLFB=1?)\n");
        return NULL;
    }
    // The frame's own pixel format picks the storage: anything that is not BGRA
    // goes to the Metal-texture route, whose format switch is the authority on
    // what is servable. BGRA continues below on the IOSurface pbuffer.
    OSType pf = CVPixelBufferGetPixelFormatType((CVPixelBufferRef)pixels);
    if (pf != kCVPixelFormatType_32BGRA)
        return klfb_image_from_native(pixels, (uint32_t)pf, out_w, out_h);
    IOSurfaceRef surf = CVPixelBufferGetIOSurface((CVPixelBufferRef)pixels);
    if (!surf) {
        fprintf(stderr, "  [glfb] the decoded frame is not IOSurface-backed — "
                        "kCVPixelBufferIOSurfacePropertiesKey is what makes it so\n");
        return NULL;
    }
    // The attribute list below states BGRA/UNSIGNED_BYTE to ANGLE, and ANGLE only
    // WARNs when the IOSurface disagrees (IOSurfaceSurfaceMtl::ValidateAttributes
    // compares bytes-per-element, which every 4-byte format passes). The pixel
    // format was checked above, so a decoder handing back something else routes
    // to the Metal path rather than being described here as what it is not.
    int w = (int)CVPixelBufferGetWidth((CVPixelBufferRef)pixels);
    int h = (int)CVPixelBufferGetHeight((CVPixelBufferRef)pixels);
    const int32_t attrs[] = {
        EGL_WIDTH,                          w,
        EGL_HEIGHT,                         h,
        EGL_IOSURFACE_PLANE_ANGLE_,         0,
        EGL_TEXTURE_TARGET_,                EGL_TEXTURE_2D_EGL_,
        EGL_TEXTURE_FORMAT_,                EGL_TEXTURE_RGBA_,
        EGL_TEXTURE_TYPE_ANGLE_,            GL_UNSIGNED_BYTE_,
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE_, GL_BGRA_EXT_,
        EGL_NONE_,
    };
    // ANGLE validates all six of those as REQUIRED for EGL_IOSURFACE_ANGLE and
    // fails the call if any is missing, so this list is a contract, not a
    // preference.
    void *pbuf = a_eglCreatePbufferFromClientBuffer(g_dpy, EGL_IOSURFACE_ANGLE_,
                                                    (void *)surf, g_cfg, attrs);
    if (!pbuf) {
        fprintf(stderr, "  [glfb] eglCreatePbufferFromClientBuffer(EGL_IOSURFACE_ANGLE) "
                        "failed for %dx%d — EGL error 0x%x\n",
                w, h, a_eglGetError_img ? a_eglGetError_img() : 0);
        return NULL;
    }
    klfb_image *im = calloc(1, sizeof *im);
    if (!im) { a_eglDestroySurface_img(g_dpy, pbuf); return NULL; }
    im->magic = KLFB_IMAGE_MAGIC;
    im->pbuf  = pbuf;
    im->w = w; im->h = h;
    pthread_mutex_lock(&g_images_lk);
    im->next = g_images;
    g_images = im;
    pthread_mutex_unlock(&g_images_lk);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    static unsigned made;
    if (made++ < 4)
        fprintf(stderr, "  [glfb] video image %p: IOSurface %p as a %dx%d BGRA pbuffer\n",
                (void *)im, (void *)surf, w, h);
    return im;
}

// The other half of the detarget, and it is the difference between a picture and
// a black rectangle.
//
// GL_OES_EGL_image_external specifies its OWN sampler defaults, and they are not
// GL_TEXTURE_2D's: MIN_FILTER defaults to LINEAR (external textures have no
// mipmaps at all) and WRAP_S/T to CLAMP_TO_EDGE. A guest targeting Android is
// entitled to rely on them and never set any of it. Once we retarget the same
// texture to GL_TEXTURE_2D, the defaults it inherits are NEAREST_MIPMAP_LINEAR
// and REPEAT — and a 2D texture with a mipmap min filter and no mip levels is
// *incomplete*, which samples as BLACK with no GL error anywhere. The frame
// arrives, the bind succeeds, the draw succeeds, and nothing is on screen.
//
// So the external defaults are applied here, and only where the state is still
// the 2D default: a guest that did set a filter deliberately keeps it. That
// test is what stops this being a blanket override of the guest's choices.
#define GL_TEXTURE_2D_IMG_     0x0DE1
#define GL_TEXTURE_MIN_FILTER_ 0x2801
#define GL_TEXTURE_MAG_FILTER_ 0x2800
#define GL_TEXTURE_WRAP_S_     0x2802
#define GL_TEXTURE_WRAP_T_     0x2803
#define GL_NEAREST_MIPMAP_LINEAR_ 0x2702
#define GL_LINEAR_             0x2601
#define GL_REPEAT_             0x2901
#define GL_CLAMP_TO_EDGE_      0x812F
static void klfb_external_defaults(int32_t tex) {
    if (!a_glTexParameteri_img || !a_glGetTexParameteriv_img) return;
    int32_t v = 0;
    a_glGetTexParameteriv_img(GL_TEXTURE_2D_IMG_, GL_TEXTURE_MIN_FILTER_, &v);
    if (v == GL_NEAREST_MIPMAP_LINEAR_) {          // untouched 2D default
        a_glTexParameteri_img(GL_TEXTURE_2D_IMG_, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_);
        static unsigned said;
        if (said++ < 2)
            fprintf(stderr, "  [glfb] texture %d had the GL_TEXTURE_2D mipmap min "
                            "filter and no mip levels — incomplete, and would have "
                            "sampled black; set to LINEAR, which is what "
                            "GL_TEXTURE_EXTERNAL_OES defaults to\n", tex);
    }
    a_glGetTexParameteriv_img(GL_TEXTURE_2D_IMG_, GL_TEXTURE_WRAP_S_, &v);
    if (v == GL_REPEAT_)
        a_glTexParameteri_img(GL_TEXTURE_2D_IMG_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
    a_glGetTexParameteriv_img(GL_TEXTURE_2D_IMG_, GL_TEXTURE_WRAP_T_, &v);
    if (v == GL_REPEAT_)
        a_glTexParameteri_img(GL_TEXTURE_2D_IMG_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
    (void)GL_TEXTURE_MAG_FILTER_;   // its 2D default IS external's (LINEAR)
}

// eglBindTexImage binds to whatever is bound to GL_TEXTURE_2D on the active unit
// of the calling thread's context — which is exactly where the guest's
// glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex) landed after klfb_detarget. So the
// only work here is the bookkeeping ANGLE will not do for us: a surface may be
// bound to at most one texture, and binding a second surface into a texture that
// already has one is undefined (Texture::bindTexImageFromSurface asserts on it).
int kl_glfb_image_bind(void *image) {
    klfb_image *im = image;
    if (!kl_glfb_is_image(im) || !img_resolve()) return 0;
    int32_t tex = 0;
    if (a_glGetIntegerv) a_glGetIntegerv(GL_TEXTURE_BINDING_2D_, &tex);
    if (!tex) {
        fprintf(stderr, "  [glfb] video image %p: no texture bound to GL_TEXTURE_2D — "
                        "the guest's bind did not reach us\n", image);
        return 0;
    }
    if (im->bound_tex == (uint32_t)tex) return 1;      // already there
    pthread_mutex_lock(&g_images_lk);
    if (im->bound_tex && im->pbuf) {
        a_eglReleaseTexImage(g_dpy, im->pbuf, EGL_BACK_BUFFER_);
        im->bound_tex = 0;
    }
    // Whatever else believed it was this texture's storage is not any more: a
    // pbuffer is released, an EGLImage merely forgets (retargeting replaces the
    // storage wholesale, but a stale bound_tex would satisfy the "already
    // there" shortcut above and skip a real rebind).
    for (klfb_image *o = g_images; o; o = o->next)
        if (o != im && o->bound_tex == (uint32_t)tex) {
            if (o->pbuf) a_eglReleaseTexImage(g_dpy, o->pbuf, EGL_BACK_BUFFER_);
            o->bound_tex = 0;
        }
    pthread_mutex_unlock(&g_images_lk);
    while (a_glGetError && a_glGetError()) {}
    if (im->img) {
        // The Metal route: the frame is an EGLImage, and the guest's texture
        // takes it as its storage. Same call the eye path makes.
        a_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D_IMG_, im->img);
        uint32_t e = a_glGetError ? a_glGetError() : 0;
        if (e) {
            static int esaid;
            if (esaid++ < 8)
                fprintf(stderr, "  [glfb] glEGLImageTargetTexture2DOES(video image "
                                "%p -> texture %d) -> GL error 0x%x\n",
                        image, tex, e);
            return 0;
        }
        im->bound_tex = (uint32_t)tex;
        klfb_external_defaults(tex);
        static unsigned mbound;
        if (mbound++ < 4)
            fprintf(stderr, "  [glfb] video image %p is now texture %d (%dx%d, "
                            "YCbCr sampled in hardware)\n", image, tex, im->w, im->h);
        return 1;
    }
    if (!a_eglBindTexImage(g_dpy, im->pbuf, EGL_BACK_BUFFER_)) {
        static int said;
        if (said++ < 8)
            fprintf(stderr, "  [glfb] eglBindTexImage(video image %p -> texture %d) "
                            "failed, EGL error 0x%x\n", image, tex,
                    a_eglGetError_img ? a_eglGetError_img() : 0);
        return 0;
    }
    im->bound_tex = (uint32_t)tex;
    klfb_external_defaults(tex);
    static unsigned bound;
    if (bound++ < 4)
        fprintf(stderr, "  [glfb] video image %p is now texture %d (%dx%d)\n",
                image, tex, im->w, im->h);

    // KL_GLFB_PROBE_VIDEO=1: is the decoded frame actually IN this texture?
    // (proven yes — bright, changing content — which is why the eye probe in the
    // mirror path is now the interesting one.)
    klfb_probe_named_tex("VIDEO", (uint32_t)tex, 0, im->w, im->h);
    return 1;
}

// SurfaceTexture names its texture explicitly; preserve the caller's binding.
int kl_glfb_image_bind_texture(void *image, uint32_t texture) {
    void (*bind)(uint32_t, uint32_t) = asym("glBindTexture");
    if (!texture || !bind || !a_glGetIntegerv) return 0;
    int32_t previous = 0;
    a_glGetIntegerv(GL_TEXTURE_BINDING_2D_, &previous);
    bind(GL_TEXTURE_2D_IMG_, texture);
    int ok = kl_glfb_image_bind(image);
    bind(GL_TEXTURE_2D_IMG_, (uint32_t)previous);
    return ok;
}

void kl_glfb_image_destroy(void *image) {
    klfb_image *im = image;
    if (!kl_glfb_is_image(im)) return;
    pthread_mutex_lock(&g_images_lk);
    for (klfb_image **p = &g_images; *p; p = &(*p)->next)
        if (*p == im) { *p = im->next; break; }
    pthread_mutex_unlock(&g_images_lk);
    // Release before destroy: a surface still bound to a texture keeps ANGLE's
    // reference on the IOSurface, and the texture would go on sampling storage
    // the decoder has recycled.
    if (im->pbuf) {
        if (im->bound_tex && a_eglReleaseTexImage)
            a_eglReleaseTexImage(g_dpy, im->pbuf, EGL_BACK_BUFFER_);
        if (a_eglDestroySurface_img) a_eglDestroySurface_img(g_dpy, im->pbuf);
    }
    if (im->img) {
        // A texture still holding this image as storage keeps it (EGL images
        // are refcounted under their siblings), so destruction order is free
        // here; the CVMetalTexture and the pixel buffer go with the image.
        if (a_eglDestroyImageKHR) a_eglDestroyImageKHR(g_dpy, im->img);
        kl_cvmtl_release(im->cvtex);
        CVPixelBufferRelease((CVPixelBufferRef)im->pixels);
    }
    im->magic = 0;
    free(im);
}

static void (*g_real_BlitFramebuffer)(int32_t, int32_t, int32_t, int32_t, int32_t,
                                      int32_t, int32_t, int32_t, uint32_t, uint32_t);

static void klfb_errprobe(const char *what, const char *detail);
// Defined with the capture below; the blit probe uses it. dump/dw/dh are an
// optional pixel out: when dump is non-NULL the probe tone-maps the readback
// into it (g_w*g_h*4 bytes, bottom-up rows) and reports the clipped size.
// hint_w/hint_h are the size to read when the attachment's own size is not
// known — see the definition; 0,0 means "no hint".
static unsigned long klfb_probe_fbo(uint32_t fb, float *fbuf, uint8_t *bbuf,
                                    char *note, size_t note_n,
                                    uint8_t *dump, int32_t *dw, int32_t *dh,
                                    int32_t hint_w, int32_t hint_h);
static uint32_t klfb_read_from_texture_layer(uint32_t tex, int layer);

// glInvalidateFramebuffer matters here because ANGLE's Metal backend actually
// discards (memoryless attachments): an invalidate of the scene color
// renderbuffer placed BEFORE the eye-resolve blit reads as "the blit copied
// black" downstream. The thunk exists to put the call on the probe timeline.
static void (*g_real_InvalidateFramebuffer)(uint32_t, int32_t, const uint32_t *);

static void klfb_InvalidateFramebuffer(int64_t target, int32_t n,
                                       const uint32_t *attachments) {
    if (kl_env_on("KL_GLFB_BLIT_PROBE", 0) && a_glGetIntegerv) {
        int32_t dfb = -1;
        a_glGetIntegerv(0x8CA6, &dfb);
        // Name the color attachment being discarded — an invalidate of "fb N's
        // COLOR_ATTACHMENT0" discards the *storage* behind it, which another
        // FBO can share. The eye blit reading black right after a lit scene
        // draw is only consistent with that.
        static void (*getfap_)(uint32_t, uint32_t, uint32_t, int32_t *);
        if (!getfap_) getfap_ = asym("glGetFramebufferAttachmentParameteriv");
        int32_t otype = 0, oname = 0;
        if (getfap_) {
            getfap_((uint32_t)target, 0x8CE0, 0x8CD0, &otype);
            getfap_((uint32_t)target, 0x8CE0, 0x8CD1, &oname);
        }
        fprintf(stderr, "  [glfb] glInvalidateFramebuffer(target=0x%llx, n=%d,"
                        " att0=0x%x) on draw_fb=%d color0=%s%d atts=",
                (long long)target, n, attachments ? attachments[0] : 0, dfb,
                otype == 0x8D41 ? "rb" : otype == 0x1702 ? "tex" : "?",
                oname);
        for (int32_t i = 0; i < n; i++)
            fprintf(stderr, "%s0x%x", i ? "," : "", attachments ? attachments[i] : 0);
        fprintf(stderr, "\n");
    }
    // KL_GLFB_NO_INVALIDATE=1: don't forward. ANGLE's Metal backend gives the
    // MSAA scene renderbuffers memoryless behavior — the depth+stencil-only
    // invalidate Unity issues after each eye's draws is the only call between
    // "draw probe reads the scene lit" and "the eye-resolve blit reads black",
    // so the discard's blast radius is the open question. Dropping the call
    // answers it: lit blits after this mean the invalidate was the eraser.
    static int no_inv = -1;
    if (no_inv < 0) no_inv = kl_env_on("KL_GLFB_NO_INVALIDATE", 0);
    if (g_real_InvalidateFramebuffer && !no_inv)
        g_real_InvalidateFramebuffer((uint32_t)target, n, attachments);
}

// The eye-blit-reads-black timeline work found the scene renderbuffer lit at
// draw time and black at blit time with no invalidate in between, so the
// eraser is a clear or a storage reallocation. These thunks put glClear and
// renderbuffer (re)storage on the same timeline. Logging is behind
// KL_GLFB_BLIT_PROBE; the forwarding is unconditional.
static void (*g_real_Clear)(uint32_t);
static void (*g_real_ClearColor)(float, float, float, float);
static void (*g_real_RenderbufferStorageMultisample)(uint32_t, int32_t, uint32_t,
                                                     int32_t, int32_t);
static void (*g_real_RenderbufferStorage)(uint32_t, uint32_t, int32_t, int32_t);

static int klfb_timeline(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_BLIT_PROBE", 0);
    return on;
}

static void klfb_Clear(uint32_t mask) {
    // KL_HL2_FORCE_CLEAR: Source never clears the scene COLOUR (the skybox is meant to
    // cover it). When the skybox pass draws to only one multiview eye/layer, the other
    // eye's sky region shows the previous frame and smears. On the scene's depth clear of
    // the MULTIVIEW FBO, add a black colour clear so any undrawn sky reads black instead
    // of trailing — a mitigation AND a diagnostic (one eye clean vs both = which case).
    // Gated on GL_DEPTH_BUFFER_BIT set + colour NOT already requested, and on the FBO
    // actually being multiview (num_views>1), so ordinary single-view clears are untouched.
    if ((mask & 0x00000100 /* GL_DEPTH_BUFFER_BIT */) &&
        !(mask & 0x00004000 /* GL_COLOR_BUFFER_BIT */) && a_glGetIntegerv) {
        static int fc = -1;
        if (fc < 0) fc = kl_env_on("KL_HL2_FORCE_CLEAR", 0);
        if (fc) {
            static void (*getfap)(uint32_t, uint32_t, uint32_t, int32_t *);
            static void (*getfv)(uint32_t, float *);
            if (!getfap) {
                getfap = asym("glGetFramebufferAttachmentParameteriv");
                getfv  = asym("glGetFloatv");
            }
            int32_t nv = 0;
            if (getfap)
                getfap(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, 0x8CE0 /* COLOR_ATTACHMENT0 */,
                       0x9630 /* GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR */, &nv);
            if (nv > 1 && g_real_Clear && g_real_ClearColor) {
                float saved[4] = {0, 0, 0, 1};
                if (getfv) getfv(0x0C22 /* GL_COLOR_CLEAR_VALUE */, saved);
                g_real_ClearColor(0.f, 0.f, 0.f, 1.f);
                g_real_Clear(mask | 0x00004000 /* + GL_COLOR_BUFFER_BIT */);
                g_real_ClearColor(saved[0], saved[1], saved[2], saved[3]);
                static int said;
                if (said++ < 4)
                    fprintf(stderr, "  [glfb] FORCE_CLEAR: black colour-cleared the "
                                    "multiview scene FBO (num_views=%d)\n", nv);
                return;
            }
        }
    }
    if (klfb_timeline() && a_glGetIntegerv) {
        int32_t dfb = -1;
        a_glGetIntegerv(0x8CA6, &dfb);
        // The completeness of the framebuffer being cleared, and the error the
        // clear itself raises. A clear to a non-zero colour followed by a
        // readback of zero has exactly two readings — the clear did not happen,
        // or the readback is looking somewhere else — and only the first of
        // them leaves a trace, here.
        static uint32_t (*ck)(uint32_t);
        if (!ck) ck = asym("glCheckFramebufferStatus");
        uint32_t st = ck ? ck(0x8CA9) : 0;
        if (a_glGetError) while (a_glGetError()) {}
        {
            static void (*getfv)(uint32_t, float *);
            if (!getfv) getfv = asym("glGetFloatv");
            float cc[4] = {-1,-1,-1,-1};
            if (getfv) getfv(0x0C22 /* COLOR_CLEAR_VALUE */, cc);
            static int said_cc;
            if (said_cc++ < 3)
                fprintf(stderr, "  [glfb]   clear value as GL holds it: "
                                "%.3f %.3f %.3f %.3f (real glClear=%s, "
                                "real glClearColor=%s)\n",
                        cc[0], cc[1], cc[2], cc[3],
                        g_real_Clear ? "bound" : "NULL",
                        g_real_ClearColor ? "bound" : "NULL");
        }
        if (g_real_Clear) g_real_Clear(mask);
        uint32_t e = a_glGetError ? a_glGetError() : 0;
        // The three pieces of state that make a clear write nothing without
        // raising anything: the scissor box (a clear IS scissored), the colour
        // write mask, and rasteriser discard.
        int32_t sc[4] = {0,0,0,0}; uint8_t cm[4] = {1,1,1,1};
        int32_t sc_on = 0, rd = 0, vp[4] = {0,0,0,0};
        static uint8_t (*isen)(uint32_t);
        static void (*getbv)(uint32_t, uint8_t *);
        if (!isen) { isen = asym("glIsEnabled"); getbv = asym("glGetBooleanv"); }
        a_glGetIntegerv(0x0C10 /* SCISSOR_BOX */, sc);
        a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
        if (isen) { sc_on = isen(0x0C11 /* SCISSOR_TEST */);
                    rd = isen(0x8C89 /* RASTERIZER_DISCARD */); }
        if (getbv) getbv(0x0C23 /* COLOR_WRITEMASK */, cm);
        // ...and the fourth: a clear only touches the colour buffers ENABLED
        // FOR WRITING, so glDrawBuffers({GL_NONE}) makes both the clear and
        // every draw a silent no-op. It is per-framebuffer state, so it
        // survives every bind.
        int32_t db0 = 0, rb0 = 0;
        a_glGetIntegerv(0x8825 /* DRAW_BUFFER0 */, &db0);
        a_glGetIntegerv(0x0C02 /* READ_BUFFER */,  &rb0);
        fprintf(stderr, "  [glfb] glClear(mask=0x%x) on draw_fb=%d status=0x%x%s "
                        "scissor=%d(%d,%d %dx%d) mask=%d%d%d%d discard=%d vp=%dx%d "
                        "drawbuf0=0x%x readbuf=0x%x\n",
                mask, dfb, st, e ? " -> ERROR" : "", sc_on, sc[0], sc[1], sc[2], sc[3],
                cm[0], cm[1], cm[2], cm[3], rd, vp[2], vp[3], db0, rb0);
        if (e) fprintf(stderr, "  [glfb]   the clear raised 0x%x\n", e);
        // ...and read the cleared framebuffer straight back. This is the
        // readback path's own control: whatever the guest draws afterwards, the
        // colour it just cleared to is known, so a zero here indicts the
        // instrument and a non-zero one indicts the draws.
        // KL_GLFB_CLEAR_PROBE_N picks WHICH clear to read back. Not the first:
        // a run's first glClear happens during start-up, before the guest has
        // ever set a clear colour, so probing it reports GL's default
        // (0,0,0,1) and reads back an opaque black that is entirely correct —
        // an instrument answering honestly about the wrong frame.
        static int clear_n = -1, clears;
        if (clear_n < 0) clear_n = kl_env_int("KL_GLFB_CLEAR_PROBE_N", 200);
        if (++clears == clear_n) {
            static float *cf; static uint8_t *cb;
            static void (*cbind)(uint32_t, uint32_t);
            if (!cf) {
                cf = malloc((size_t)g_w * g_h * 16);
                cb = malloc((size_t)g_w * g_h * 4);
                cbind = asym("glBindFramebuffer");
            }
            if (cf && cb && cbind && dfb >= 0) {
                int32_t keep = -1;
                a_glGetIntegerv(0x8CAA, &keep);
                char n[160] = "";
                unsigned long l = klfb_probe_fbo((uint32_t)dfb, cf, cb, n, sizeof n,
                                                 NULL, NULL, NULL, vp[2], vp[3]);
                fprintf(stderr, "  [glfb]   read straight back: %lu lit (%s)\n", l, n);
                // ...and four raw bytes, because "0 lit" is a summary and the
                // question at this point is whether ANY value came back.
                if (a_glReadPixels) {
                    uint8_t px[16] = {0};
                    cbind(0x8CA8, (uint32_t)dfb);
                    while (a_glGetError()) {}
                    a_glReadPixels(64, 64, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    uint32_t re = a_glGetError();
                    fprintf(stderr, "  [glfb]   raw px @64,64: %u %u %u %u  err=0x%x\n",
                            px[0], px[1], px[2], px[3], re);
                    // The control, and it is DESTRUCTIVE — it overwrites one of
                    // the guest's frames — so it has a knob of its own rather
                    // than riding on the probe: clear the SAME framebuffer
                    // ourselves, to a value nothing else would produce, and
                    // read it straight back. If ours reads back and the guest's
                    // does not, the readback is sound and the guest's clear is
                    // being lost; if neither does, the instrument is the liar.
                    // That distinction is worth a corrupted frame — it is what
                    // separated "the eye is black" from "the probe reads black".
                    static void (*ccol)(float, float, float, float);
                    static void (*cclear)(uint32_t);
                    if (!ccol) { ccol = asym("glClearColor"); cclear = asym("glClear"); }
                    if (ccol && cclear && kl_env_on("KL_GLFB_CLEAR_CONTROL", 0)) {
                        cbind(0x8CA9, (uint32_t)dfb);
                        ccol(1.0f, 0.0f, 1.0f, 1.0f);
                        cclear(0x4000 /* COLOR_BUFFER_BIT */);
                        memset(px, 0, sizeof px);
                        cbind(0x8CA8, (uint32_t)dfb);
                        a_glReadPixels(64, 64, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, px);
                        fprintf(stderr, "  [glfb]   our own magenta clear reads "
                                        "back: %u %u %u %u\n",
                                px[0], px[1], px[2], px[3]);
                        ccol(0, 0, 0, 0);
                    }
                }
                cbind(0x8CA8, (uint32_t)(keep >= 0 ? keep : 0));
                cbind(0x8CA9, (uint32_t)dfb);
            }
        }
        return;
    }
    if (g_real_Clear) g_real_Clear(mask);
}
static void klfb_ClearColor(float r, float g, float b, float a) {
    // KL_GLFB_X_CLEAR_MAGENTA=1 — an experiment: every clear colour becomes
    // opaque magenta. A scene that comes out black then says which black it
    // is — magenta left standing is geometry that never reached the target,
    // black over magenta is geometry that was drawn black.
    {
        static int magenta = -1;
        if (magenta < 0) magenta = kl_env_on("KL_GLFB_X_CLEAR_MAGENTA", 0);
        if (magenta) { r = 1.0f; g = 0.0f; b = 1.0f; a = 1.0f; }
    }
    if (klfb_timeline())
        fprintf(stderr, "  [glfb] glClearColor(%.2f, %.2f, %.2f, %.2f)\n",
                r, g, b, a);
    if (g_real_ClearColor) g_real_ClearColor(r, g, b, a);
}
static void klfb_RenderbufferStorageMultisample(uint32_t target, int32_t samples,
                                                uint32_t fmt, int32_t w, int32_t h) {
    if (klfb_timeline() && a_glGetIntegerv) {
        int32_t rb = -1;
        a_glGetIntegerv(0x8CA7 /* RENDERBUFFER_BINDING */, &rb);
        fprintf(stderr, "  [glfb] glRenderbufferStorageMultisample rb=%d "
                        "fmt=0x%x %dx%d samples=%d (storage wiped)\n", rb, fmt,
                w, h, samples);
    }
    if (a_glGetIntegerv && klfb_census_every()) {
        int32_t rb = -1;
        a_glGetIntegerv(0x8CA7 /* RENDERBUFFER_BINDING */, &rb);
        if (rb > 0)
            klfb_vram_note(KLC_RBO, (uint32_t)rb,
                           klfb_storage_bytes(fmt, w, h, 1, 1) *
                               (uint64_t)(samples > 0 ? samples : 1));
    }
    if (g_real_RenderbufferStorageMultisample)
        g_real_RenderbufferStorageMultisample(target, samples, fmt, w, h);
}
static void klfb_RenderbufferStorage(uint32_t target, uint32_t fmt,
                                     int32_t w, int32_t h) {
    if (klfb_timeline() && a_glGetIntegerv) {
        int32_t rb = -1;
        a_glGetIntegerv(0x8CA7, &rb);
        fprintf(stderr, "  [glfb] glRenderbufferStorage rb=%d fmt=0x%x %dx%d "
                        "(storage wiped)\n", rb, fmt, w, h);
    }
    if (a_glGetIntegerv && klfb_census_every()) {
        int32_t rb = -1;
        a_glGetIntegerv(0x8CA7, &rb);
        if (rb > 0)
            klfb_vram_note(KLC_RBO, (uint32_t)rb,
                           klfb_storage_bytes(fmt, w, h, 1, 1));
    }
    if (g_real_RenderbufferStorage) g_real_RenderbufferStorage(target, fmt, w, h);
}

// An MSAA resolve into a destination of a DIFFERENT format. ES 3.0 forbids it
// (GL_INVALID_OPERATION, and the blit does nothing); the Quest's driver does it.
// Keep Talking and Nobody Explodes renders its scene into an RGBA8 multisample
// renderbuffer and resolves it straight into the SRGB8_ALPHA8 eye texture, so on
// ANGLE every eye resolve failed, the eye texture was never written, and both
// eyes showed whatever the storage held — solid pink. Called only after the
// guest's own blit has failed with 0x502 and only for a multisampled
// renderbuffer source: resolve into a same-format staging texture (legal), then
// blit that single-sampled copy to the guest's destination, where a format
// conversion is legal. KL_GLFB_RESOLVE_STAGE=0 turns it off.
static int klfb_resolve_via_stage(int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1,
                                  int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1,
                                  uint32_t mask, uint32_t filter) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_RESOLVE_STAGE", 1);
    if (!on || !(mask & 0x4000 /* COLOR_BUFFER_BIT */) || !a_glGetIntegerv ||
        !g_real_BlitFramebuffer)
        return 0;
    static void (*bindfb)(uint32_t, uint32_t);
    static void (*getfap)(uint32_t, uint32_t, uint32_t, int32_t *);
    static void (*bindrb)(uint32_t, uint32_t);
    static void (*getrbp)(uint32_t, uint32_t, int32_t *);
    static void (*gentex)(int32_t, uint32_t *);
    static void (*deltex)(int32_t, const uint32_t *);
    static void (*bindtex)(uint32_t, uint32_t);
    static void (*texstorage)(uint32_t, int32_t, uint32_t, int32_t, int32_t);
    static void (*genfb)(int32_t, uint32_t *);
    static void (*fbtex)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
    if (!bindfb) {
        bindfb     = asym("glBindFramebuffer");
        getfap     = asym("glGetFramebufferAttachmentParameteriv");
        bindrb     = asym("glBindRenderbuffer");
        getrbp     = asym("glGetRenderbufferParameteriv");
        gentex     = asym("glGenTextures");
        deltex     = asym("glDeleteTextures");
        bindtex    = asym("glBindTexture");
        texstorage = asym("glTexStorage2D");
        genfb      = asym("glGenFramebuffers");
        fbtex      = asym("glFramebufferTexture2D");
    }
    if (!bindfb || !getfap || !bindrb || !getrbp || !gentex || !deltex ||
        !bindtex || !texstorage || !genfb || !fbtex)
        return 0;
    int32_t rfb = 0, dfb = 0, rbuf = 0;
    a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &rfb);
    a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &dfb);
    a_glGetIntegerv(0x0C02 /* READ_BUFFER */, &rbuf);
    if (rfb <= 0 || rbuf == 0 /* NONE */) return 0;
    int32_t otype = 0, oname = 0;
    getfap(0x8CA8 /* READ_FRAMEBUFFER */, (uint32_t)rbuf, 0x8CD0 /* OBJECT_TYPE */, &otype);
    if (otype != 0x8D41 /* RENDERBUFFER */) return 0;
    getfap(0x8CA8, (uint32_t)rbuf, 0x8CD1 /* OBJECT_NAME */, &oname);
    int32_t save_rb = 0, fmt = 0, samples = 0, w = 0, h = 0;
    a_glGetIntegerv(0x8CA7 /* RENDERBUFFER_BINDING */, &save_rb);
    bindrb(0x8D41, (uint32_t)oname);
    getrbp(0x8D41, 0x8D44 /* INTERNAL_FORMAT */, &fmt);
    getrbp(0x8D41, 0x8CAB /* SAMPLES */, &samples);
    getrbp(0x8D41, 0x8D42 /* WIDTH */, &w);
    getrbp(0x8D41, 0x8D43 /* HEIGHT */, &h);
    bindrb(0x8D41, (uint32_t)save_rb);
    if (samples <= 0 || w <= 0 || h <= 0) return 0;

    static uint32_t stex, sfb;
    static int32_t sfmt, sw, sh;
    if (!sfb) genfb(1, &sfb);
    if (!sfb) return 0;
    if (!stex || sfmt != fmt || sw != w || sh != h) {
        if (stex) deltex(1, &stex);
        stex = 0;
        gentex(1, &stex);
        if (!stex) return 0;
        int32_t save_tex = 0;
        a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &save_tex);
        bindtex(0x0DE1 /* TEXTURE_2D */, stex);
        texstorage(0x0DE1, 1, (uint32_t)fmt, w, h);
        bindtex(0x0DE1, (uint32_t)save_tex);
        bindfb(0x8CA9 /* DRAW_FRAMEBUFFER */, sfb);
        fbtex(0x8CA9, 0x8CE0 /* COLOR_ATTACHMENT0 */, 0x0DE1, stex, 0);
        sfmt = fmt; sw = w; sh = h;
        fprintf(stderr, "  [glfb] MSAA resolve across formats refused by GL — "
                        "resolving through a staging 0x%x %dx%d texture instead\n",
                fmt, w, h);
    }
    // The source is foveated by the size rule (multisampled, eye-sized) and so
    // is the eye texture; the staging texture is single-sampled and would not
    // be. Patched ANGLE copies texel for texel only between targets carrying
    // the SAME map and otherwise resamples through screen space — which
    // warped the picture twice: squeezed into the lower part of the view and
    // stretching as the head moved. So for these two blits the rule's sample
    // floor drops to 1, which puts the staging texture under the same map.
    int vrr = g_eye_rate_map && a_SetRateMapForSize && g_rate_rule_on &&
              w == g_eye_rate_w && h == g_eye_rate_h;
    if (vrr) a_SetRateMapForSize((uint32_t)w, (uint32_t)h, 1, g_eye_rate_map);
    // 1: the resolve, same format and same rectangle on both sides.
    bindfb(0x8CA9, sfb);
    // Re-attached so ANGLE re-syncs the framebuffer and builds its pass with
    // the map now in force (see klfb_resync_fbo for the caching this defeats).
    if (vrr) fbtex(0x8CA9, 0x8CE0, 0x0DE1, stex, 0);
    g_real_BlitFramebuffer(sx0, sy0, sx1, sy1, sx0, sy0, sx1, sy1,
                           0x4000, 0x2600 /* NEAREST */);
    // 2: the guest's blit, from the single-sampled copy.
    bindfb(0x8CA8, sfb);
    bindfb(0x8CA9, (uint32_t)dfb);
    g_real_BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, 0x4000, filter);
    if (vrr)
        a_SetRateMapForSize((uint32_t)w, (uint32_t)h, (uint32_t)g_eye_rate_min,
                            g_eye_rate_map);
    bindfb(0x8CA8, (uint32_t)rfb);
    // Depth/stencil were refused along with the colour; their formats were not
    // the problem, so they go as the guest asked.
    if (mask & ~0x4000u)
        g_real_BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                               mask & ~0x4000u, filter);
    return 1;
}

static void klfb_BlitFramebuffer(int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1,
                                 int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1,
                                 int64_t mask, int64_t filter) {
    klfb_errprobe("glBlitFramebuffer(before)", NULL);
    // The per-blit src/dst/status dump is a FIREHOSE (every blit, tens of thousands
    // a frame, plus a glGet + CheckFramebufferStatus per blit) — its own opt-in flag
    // so KL_FULL can arm the cheap error-only errprobe above without this overhead.
    static int blit_log = -1;
    if (blit_log < 0) blit_log = kl_env_on("KL_GLFB_BLIT_LOG", 0);
    // WHO is blitting. A GL call that arrives with the wrong state bound is a
    // question about the caller, not about the call, and the guest is thirteen
    // libraries — "Unity" and "the OpenXR plugin" issue GL through completely
    // different code and only the frame walk tells them apart.
    if (blit_log) {
        static int said_who;
        if (!said_who) {
            said_who = 1;
            fprintf(stderr, "  [glfb] the first glBlitFramebuffer comes from:\n");
            kl_fault_print_frames(stderr, NULL);
        }
    }
    // KL_GLFB_BLIT_BIG=<px>: only log blits whose destination is at least this wide.
    // The HL2Q3VR cinema eye-copy blits the 2D scene RT into the ~2064-wide per-eye
    // swapchain image just before xrReleaseSwapchainImage; every other blit (scene
    // effects, tiny UI) is smaller, so BLIT_BIG=1500 isolates the eye-copy from the
    // firehose and lets the "N lit" readback below show whether its SOURCE is black.
    static int blit_big = -2;
    if (blit_big == -2) blit_big = kl_env_int("KL_GLFB_BLIT_BIG", 0);
    int klfb_destW = dx1 > dx0 ? dx1 - dx0 : dx0 - dx1;
    if (blit_log && a_glGetIntegerv && (blit_big <= 0 || klfb_destW >= blit_big)) {
        int32_t dfb = -1, rfb = -1, rb = -1;
        a_glGetIntegerv(0x8CA6, &dfb);
        a_glGetIntegerv(0x8CAA, &rfb);
        a_glGetIntegerv(0x0C02, &rb);
        fprintf(stderr, "  [glfb] blit (%d,%d)-(%d,%d) -> (%d,%d)-(%d,%d) "
                "mask=0x%llx filter=0x%llx draw_fb=%d read_fb=%d readbuf=0x%x\n",
                sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                (long long)mask, (long long)filter, dfb, rfb, rb);
        static uint32_t (*gl_CheckFbStatus)(uint32_t);
        if (!gl_CheckFbStatus) gl_CheckFbStatus = asym("glCheckFramebufferStatus");
        uint32_t (*bindfb_)(uint32_t, uint32_t) = asym("glBindFramebuffer");
        void (*getfap)(uint32_t, uint32_t, uint32_t, int32_t *) =
            asym("glGetFramebufferAttachmentParameteriv");
        if (bindfb_ && gl_CheckFbStatus && getfap) {
            for (int i = 0; i < 2; i++) {
                int32_t fb = i ? dfb : rfb;
                bindfb_(0x8CA9, fb);
                int32_t otype = -1, oname = -1;
                getfap(0x8CA9, 0x8CE0, 0x8CD0 /* OBJECT_TYPE */, &otype);
                getfap(0x8CA9, 0x8CE0, 0x8CD1 /* OBJECT_NAME */, &oname);
                fprintf(stderr, "  [glfb]   fb %d status=0x%x color0 type=0x%x name=%d",
                        fb, gl_CheckFbStatus(0x8CA9), otype, oname);
                if (otype == 0x8D41 /* RENDERBUFFER */) {
                    void (*bindrb)(uint32_t, uint32_t) = asym("glBindRenderbuffer");
                    void (*getrbp)(uint32_t, uint32_t, int32_t *) =
                        asym("glGetRenderbufferParameteriv");
                    if (bindrb && getrbp) {
                        int32_t fmt = -1, samples = -1, w = -1, h = -1;
                        bindrb(0x8D41, oname);
                        getrbp(0x8D41, 0x8D44 /* INTERNAL_FORMAT */, &fmt);
                        getrbp(0x8D41, 0x8CAB /* SAMPLES */, &samples);
                        getrbp(0x8D41, 0x8D42 /* WIDTH */, &w);
                        getrbp(0x8D41, 0x8D43 /* HEIGHT */, &h);
                        fprintf(stderr, " rb fmt=0x%x samples=%d %dx%d",
                                fmt, samples, w, h);
                    }
                }
                fprintf(stderr, "\n");
            }
            bindfb_(0x8CA8, rfb);   // restore: READ<-rfb, DRAW<-dfb
            bindfb_(0x8CA9, dfb);
        }
    }
    // A blit whose read framebuffer is 0 is counted, not repaired: the repair
    // lives at the attach (KL_GLFB_READ_ATTACH_FIX), which carries the
    // attachment the guest named rather than a framebuffer chosen here.
    if (a_glGetIntegerv) {
        int32_t rfb2 = -1;
        a_glGetIntegerv(0x8CAA, &rfb2);
        g_blits++;
        if (rfb2 == 0) g_blits_read0++;
    }
    // KL_GLFB_BLIT_PROBE=1: probe the blit's source BEFORE the blit and its
    // destination AFTER — the swap-time capture found every FBO black, which
    // has two very different readings: the pixels were gone by then
    // (glInvalidateFramebuffer discards), or the draws themselves produce
    // black. Probing both sides of the blit puts the loss on the timeline.
    static int blit_probe = -1;
    if (blit_probe < 0) blit_probe = kl_env_on("KL_GLFB_BLIT_PROBE", 0);
    static float *pfb;
    static uint8_t *pbb;
    static void (*bp_bind)(uint32_t, uint32_t);
    int32_t dfb = -1, rfb = -1;
    // Same KL_GLFB_BLIT_BIG gate as the log above, so the source/dest "N lit"
    // readback (a full glReadPixels per side) fires only for the large eye-copy
    // blit and not on every scene blit — otherwise it is unusable in the cinema.
    if (blit_probe && a_glGetIntegerv && (blit_big <= 0 || klfb_destW >= blit_big)) {
        if (!pfb) {
            pfb = malloc((size_t)g_w * g_h * 16);
            pbb = malloc((size_t)g_w * g_h * 4);
            bp_bind = asym("glBindFramebuffer");
        }
        a_glGetIntegerv(0x8CA6, &dfb);
        a_glGetIntegerv(0x8CAA, &rfb);
        if (pfb && pbb && bp_bind) {
            char ns[160] = "";
            // The blit NAMES its source rectangle, so the source's size never
            // has to be looked up: this is the one reader that always knows.
            int32_t srw = sx1 > sx0 ? sx1 - sx0 : sx0 - sx1;
            int32_t srh = sy1 > sy0 ? sy1 - sy0 : sy0 - sy1;
            unsigned long ls = klfb_probe_fbo((uint32_t)rfb, pfb, pbb, ns, sizeof ns,
                                              NULL, NULL, NULL, srw, srh);
            fprintf(stderr, "  [glfb] BLIT_PROBE before: read_fb=%d %lu lit (%s)\n",
                    rfb, ls, ns);
            // KL_GLFB_PROBE_TEX=<name>: read a NAMED texture back, both array
            // layers, at the same moment. A guest that renders into its own
            // target and copies from it puts two failures on the same
            // timeline — "nothing was drawn" and "the copy read the wrong
            // source" — and probing only the blit's own source cannot tell
            // them apart, because the source is whatever the guest bound. The
            // texture is named rather than guessed: an instrument that
            // assumes reports a working pipeline as a broken one. Layer 1 on a
            // non-array texture
            // reports "incomplete", which is the honest answer.
            static int probe_tex = -1;
            if (probe_tex < 0) probe_tex = kl_env_int("KL_GLFB_PROBE_TEX", 0);
            if (probe_tex > 0) {
                for (int L = 0; L < 2; L++) {
                    char nt[160] = "no framebuffer";
                    unsigned long lt = 0;
                    uint32_t tfb = klfb_read_from_texture_layer((uint32_t)probe_tex, L);
                    if (tfb) lt = klfb_probe_fbo(tfb, pfb, pbb, nt, sizeof nt,
                                                 NULL, NULL, NULL, srw, srh);
                    fprintf(stderr, "  [glfb] PROBE_TEX tex=%d layer=%d: %lu lit (%s)\n",
                            probe_tex, L, lt, nt);
                }
            }
            // ...and the framebuffer the guest was drawing into immediately
            // before, measured in the SAME instant. "The source is empty" and
            // "the scene went somewhere else" are different bugs, and telling
            // them apart by correlating two runs' framebuffer numbers does not
            // work — the guest's FBO-to-texture mapping is not stable between
            // runs. One line, both answers, no correlation.
            if (g_last_draw_fb && (int32_t)g_last_draw_fb != rfb) {
                char np[160] = "";
                unsigned long lp = klfb_probe_fbo(g_last_draw_fb, pfb, pbb,
                                                  np, sizeof np, NULL, NULL, NULL,
                                                  srw, srh);
                fprintf(stderr, "  [glfb] BLIT_PROBE last draw target: fb=%u %lu lit"
                                " (%s)\n", g_last_draw_fb, lp, np);
            }
            bp_bind(0x8CA8, (uint32_t)rfb);
            bp_bind(0x8CA9, (uint32_t)dfb);
        }
        // The guest's own blit, error-bracketed honestly: drain first, so an
        // error after is provably this blit's. The question it answers is
        // whether the eye resolve itself fails on this driver.
        if (a_glGetError) while (a_glGetError()) {}
    }
    // KL_HL2_CINEMA_FIX: HL2Q3VR's 2D-cutscene eye-copy blits _rt_menu into the per-eye
    // swapchain, but on Klepton the scene is never composited into _rt_menu (probed 0 lit
    // / black), so the cutscene is black. The lit 2D intro IS rendered — it is the SOURCE
    // of the engine's flat "spectator" present (probed ~921600 lit), which blits to the
    // DEFAULT framebuffer (fb 0) and is thrown away on ANGLE-Metal. So: learn which
    // texture _rt_menu is (the eye-copy's read-fbo colour0), and redirect the flat
    // present's DESTINATION from fb 0 into that texture. Then the very next eye-copy reads
    // a lit _rt_menu. Redirecting the flat present's write (when tex 1 is valid and bound)
    // is robust where redirecting the eye-copy's read was not — the menu read-fbo it would
    // have borrowed is stale by eye-copy time. Opt-in, full-eye-blit only, so it never
    // touches the direct-array gameplay path.
    int32_t klfb_cf_saved_read = -1;
    {
        static int cinema_fix = -1;
        if (cinema_fix < 0) cinema_fix = kl_env_on("KL_HL2_CINEMA_FIX", 0);
        if (cinema_fix && a_glGetIntegerv && klfb_destW >= 1500) {
            int32_t cdfb = -1, crfb = -1;
            a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &cdfb);
            a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &crfb);
            void (*bindfb)(uint32_t, uint32_t)                = asym("glBindFramebuffer");
            void (*getfap)(uint32_t, uint32_t, uint32_t, int32_t *) =
                asym("glGetFramebufferAttachmentParameteriv");
            static int32_t cinema_src_tex = -1;   // tex 1 = the lit 2D intro scene
            static uint32_t my_read_fbo   = 0;
            if (cdfb == 0 && crfb > 0 && getfap) {
                // The flat "spectator" present, on the rare frames it runs with a live GL
                // context (usually it is context-less and a no-op): its READ colour0 is the
                // lit 2D scene (tex 1). Learn that texture id.
                int32_t otype = 0, oname = 0;
                getfap(0x8CA8 /* GL_READ_FRAMEBUFFER (target, NOT ..._BINDING 0x8CAA) */,
                       0x8CE0 /* COLOR_ATTACHMENT0 */,
                       0x8CD0 /* ATTACHMENT_OBJECT_TYPE */, &otype);
                getfap(0x8CA8, 0x8CE0, 0x8CD1 /* ATTACHMENT_OBJECT_NAME */, &oname);
                if (otype == 0x1702 /* GL_TEXTURE */ && oname > 0)
                    cinema_src_tex = oname;
            } else if (cdfb > 0 && crfb > 0 && crfb != cdfb && bindfb && getfap) {
                // The eye-copy (dst is a per-eye swapchain fbo, always has a live context):
                // it reads black _rt_menu. The Quest trace shows the g-man intro is a normal
                // STEREO projection; on Klepton that stereo scene renders to the multiview
                // arrays (tex 95/96/97) while the cinema-2D path submits black _rt_menu.
                // KL_HL2_CINEMA_SRC_TEX=<n> overrides the source texture (e.g. an array), and
                // KL_HL2_CINEMA_SRC_LAYER picks the array slice; default falls back to the
                // learned tex 1. We attach it to our own read fbo and read THAT instead.
                static int32_t override_tex = -2, override_layer = -2;
                if (override_tex == -2)   override_tex   = kl_env_int("KL_HL2_CINEMA_SRC_TEX", 0);
                if (override_layer == -2) override_layer = kl_env_int("KL_HL2_CINEMA_SRC_LAYER", 0);
                int32_t src_tex = override_tex > 0 ? override_tex : cinema_src_tex;
                void (*genfb)(int32_t, uint32_t *)                            = asym("glGenFramebuffers");
                void (*fbtex)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t) =
                    asym("glFramebufferTexture2D");
                void (*fbtl)(uint32_t, uint32_t, uint32_t, int32_t, int32_t)  =
                    asym("glFramebufferTextureLayer");
                uint32_t (*ckfb)(uint32_t)                                    = asym("glCheckFramebufferStatus");
                if (!my_read_fbo && genfb) genfb(1, &my_read_fbo);
                if (my_read_fbo && fbtex && src_tex > 0) {
                    klfb_cf_saved_read = crfb;
                    bindfb(0x8CA8 /* READ_FRAMEBUFFER */, my_read_fbo);
                    // Try 2D attach; if the fbo comes back incomplete the source is an array
                    // texture (multiview) — attach the requested layer instead.
                    fbtex(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */, 0x0DE1 /* GL_TEXTURE_2D */,
                          (uint32_t)src_tex, 0);
                    uint32_t st = ckfb ? ckfb(0x8CA8) : 0x8CD5;
                    if (st != 0x8CD5 /* COMPLETE */ && fbtl) {
                        fbtex(0x8CA8, 0x8CE0, 0x0DE1, 0, 0);   // detach the 2D try
                        fbtl(0x8CA8, 0x8CE0, (uint32_t)src_tex, 0, override_layer);
                        st = ckfb ? ckfb(0x8CA8) : 0;
                    }
                    static int said;
                    if (said++ < 4)
                        fprintf(stderr, "  [glfb] CINEMA_FIX: eye-copy read _rt_menu -> tex "
                                "%d layer %d (fbo %u status=0x%x)\n", src_tex,
                                override_tex > 0 ? override_layer : 0, my_read_fbo, st);
                    // Is that source actually lit HERE (during the cinema)? Read its centre.
                    if (a_glReadPixels && a_glGetError && st == 0x8CD5) {
                        while (a_glGetError()) {}
                        uint8_t px[16] = {0};
                        a_glReadPixels(1032, 1104, 2, 2, 0x1908 /* RGBA */,
                                       0x1401 /* UNSIGNED_BYTE */, px);
                        uint32_t re = a_glGetError();
                        static int said_px;
                        if (said_px++ < 8)
                            fprintf(stderr, "  [glfb] CINEMA_FIX: tex %d centre px="
                                    "%u,%u,%u,%u err=0x%x\n", src_tex,
                                    px[0], px[1], px[2], px[3], re);
                    }
                }
            }
        }
    }
    if (g_real_BlitFramebuffer) {
        // Drained first so a 0x502 after the blit is provably the blit's own.
        int check = a_glGetError && (mask & 0x4000);
        if (check) while (a_glGetError()) {}
        g_real_BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                               (uint32_t)mask, (uint32_t)filter);
        if (check && a_glGetError() == 0x502 /* INVALID_OPERATION */)
            klfb_resolve_via_stage(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                                   (uint32_t)mask, (uint32_t)filter);
    }
    // Did the redirected eye-copy actually land the lit source into the swapchain dest?
    // Read the DRAW (swapchain) fbo's centre. Lit here but black on-screen = the per-eye
    // layer we fill is not the one submitted (a deeper submission-path problem, not a blit
    // one). Black here = the blit itself is not writing (fbo-name collision / format).
    if (klfb_cf_saved_read >= 0 && a_glGetIntegerv && a_glReadPixels && a_glGetError) {
        void (*bfb)(uint32_t, uint32_t) = asym("glBindFramebuffer");
        int32_t d = -1, keepr = -1;
        a_glGetIntegerv(0x8CA6 /* DRAW binding = swapchain fbo */, &d);
        a_glGetIntegerv(0x8CAA /* READ binding to restore */, &keepr);
        if (bfb && d > 0) {
            bfb(0x8CA8 /* READ_FRAMEBUFFER */, (uint32_t)d);
            while (a_glGetError()) {}
            uint8_t px[16] = {0};
            a_glReadPixels(1032, 1104, 2, 2, 0x1908 /* RGBA */, 0x1401 /* UBYTE */, px);
            uint32_t re = a_glGetError();
            static int said_d;
            if (said_d++ < 6)
                fprintf(stderr, "  [glfb] CINEMA_FIX: swapchain dest fb %d centre px="
                        "%u,%u,%u,%u err=0x%x\n", d, px[0], px[1], px[2], px[3], re);
            bfb(0x8CA8, (uint32_t)keepr);
        }
    }
    // Restore the guest's read binding so nothing downstream sees the swap.
    if (klfb_cf_saved_read >= 0) {
        void (*rb_)(uint32_t, uint32_t) = asym("glBindFramebuffer");
        if (rb_) rb_(0x8CA8 /* READ_FRAMEBUFFER */, (uint32_t)klfb_cf_saved_read);
    }
    if (blit_probe && a_glGetError) {
        uint32_t be = a_glGetError();
        if (be)
            fprintf(stderr, "  [glfb] BLIT_PROBE: guest blit fb%d -> fb%d "
                            "raised 0x%x\n", rfb, dfb, be);
    }
    if (blit_probe && pfb && pbb && bp_bind) {
        char nd[160] = "";
        int32_t drw = dx1 > dx0 ? dx1 - dx0 : dx0 - dx1;
        int32_t drh = dy1 > dy0 ? dy1 - dy0 : dy0 - dy1;
        unsigned long ld = klfb_probe_fbo((uint32_t)dfb, pfb, pbb, nd, sizeof nd,
                                          NULL, NULL, NULL, drw, drh);
        fprintf(stderr, "  [glfb] BLIT_PROBE after: draw_fb=%d %lu lit (%s)\n",
                dfb, ld, nd);
        bp_bind(0x8CA8, (uint32_t)rfb);          // restore the guest's bindings
        bp_bind(0x8CA9, (uint32_t)dfb);
    }
    klfb_errprobe("glBlitFramebuffer(after)", NULL);
}

static void (*g_real_TexSubImage3D)(uint32_t, int32_t, int32_t, int32_t, int32_t,
                                    int32_t, int32_t, int32_t, uint32_t, uint32_t,
                                    const void *);

static void klfb_TexSubImage3D(uint32_t target, int32_t level, int32_t xoff,
                               int32_t yoff, int32_t zoff, int32_t w, int32_t h,
                               int32_t d, int64_t format, int64_t type,
                               const void *pixels) {
    if (g_real_TexSubImage3D)
        g_real_TexSubImage3D(target, level, xoff, yoff, zoff, w, h, d,
                             (uint32_t)format, (uint32_t)type, pixels);
}

// ----------------------------------------- the compressed-upload probe (ETC2)
//
// The Simulator dies inside ANGLE on a compressed upload — signal 10 in
// _platform_memmove under TextureMtl::setPerSliceSubImage, from
// glCompressedTexSubImage2D of 0x9279 at 2048x1024. A memmove overruns, so
// something disagrees about how many bytes the call describes. Everything about
// the *format* has already been ruled out: the emulated-format
// table entry is gated on macOS/Catalyst rather than the simulator, the
// simulator block maps ETC2 to native EAC, and a standalone
// EAC_RGBA8_sRGB + replaceRegion with exactly the guest's 2 MB succeeds.
//
// What was never measured is the guest's own arithmetic. This computes the size
// the dimensions imply and compares it with the imageSize the guest passed —
// and separately checks the sub-region against the mip level's real extent,
// because a region that overruns the level is the other way to make a correct
// imageSize describe the wrong destination.
//
// Block geometry, not bytes-per-pixel: every format here is 4x4-blocked except
// ASTC, whose footprint is in its enum. Returns 0 for "not a compressed format
// I know", which is reported rather than guessed at.
static int klfb_block_geom(uint32_t fmt, int *bw, int *bh, int *bytes) {
    *bw = *bh = 4;
    switch (fmt) {
        case 0x9270: case 0x9271:                       // R11_EAC (+signed)
        case 0x9274: case 0x9275:                       // RGB8_ETC2, SRGB8_ETC2
        case 0x9276: case 0x9277:                       // ...A1 punchthrough
            *bytes = 8;  return 1;
        case 0x9272: case 0x9273:                       // RG11_EAC (+signed)
        case 0x9278: case 0x9279:                       // RGBA8_ETC2_EAC, SRGB8_A8
            *bytes = 16; return 1;
        default: break;
    }
    // ASTC: 0x93B0-0x93BD LDR, 0x93D0-0x93DD sRGB, same footprint order. Always
    // 16 bytes per block; only the block dimensions vary.
    static const struct { int w, h; } astc[] = {
        {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},
        {10,5},{10,6},{10,8},{10,10},{12,10},{12,12},
    };
    unsigned i = 0xffffffffu;
    if (fmt >= 0x93B0 && fmt <= 0x93BD) i = fmt - 0x93B0;
    if (fmt >= 0x93D0 && fmt <= 0x93DD) i = fmt - 0x93D0;
    if (i < sizeof astc / sizeof *astc) {
        *bw = astc[i].w; *bh = astc[i].h; *bytes = 16; return 1;
    }
    return 0;
}

// KL_GLFB_PROBE_CTEX=1 — also check each compressed upload against the
// allocation glTexStorage2D recorded, which costs a GL query per upload.
static int klfb_probe_ctex(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_PROBE_CTEX", 0);
    return on;
}

static void (*g_real_CompressedTexSubImage2D)(uint32_t, int32_t, int32_t, int32_t,
                                              int32_t, int32_t, uint32_t, int32_t,
                                              const void *);
static void klfb_cimg_note_upload(uint32_t target, int32_t level, int32_t xoff,
                                  int32_t yoff, int32_t w, int32_t h, uint32_t fmt,
                                  int32_t sz, const void *data);

// The Simulator's crash, found (2026-10-11, Tales from the Galaxy's Edge, an
// 892x512 ASTC 8x8 upload of 114688 bytes): ANGLE's Metal backend uploads
// through a staging buffer there, and its CopyTextureData walks the source by
// PIXEL rows — regionSize.height rows of one block-row's bytes each — where a
// compressed image has only height/blockHeight of them. It reads blockHeight
// times the bytes the call describes (4x for ETC2/EAC, 8x for ASTC 8x8), which
// is harmless while the guest's buffer has readable memory behind it and a
// fault as soon as it does not. The destination is sized by the same sum, and
// only its true extent is blitted on, so the picture is right either way.
//
// So in the Simulator the upload is handed a copy with that much behind it.
// Not on a device: there the upload does not take this path, and the copy
// would cost every texture a second allocation. A bound unpack buffer makes
// `data` an offset rather than memory; that case is passed on as it is.
// Returns the copy to free, or NULL when the caller's pointer is used.
#include <TargetConditionals.h>
static void *klfb_sim_pad_compressed(uint32_t fmt, int32_t w, int32_t h, int32_t sz,
                                     const void **data) {
#if TARGET_OS_SIMULATOR
    int bw, bh, bpb;
    if (!*data || sz <= 0 || w <= 0 || h <= 0 || !klfb_block_geom(fmt, &bw, &bh, &bpb)) return NULL;
    size_t need = (size_t)((w + bw - 1) / bw) * (size_t)bpb * (size_t)h;
    if (need <= (size_t)sz) return NULL;
    int32_t pbo = 0;
    if (a_glGetIntegerv) a_glGetIntegerv(0x88EF /* PIXEL_UNPACK_BUFFER_BINDING */, &pbo);
    if (pbo) return NULL;
    void *copy = calloc(1, need);
    if (!copy) return NULL;
    memcpy(copy, *data, (size_t)sz);
    *data = copy;
    return copy;
#else
    (void)fmt; (void)w; (void)h; (void)sz; (void)data;
    return NULL;
#endif
}

static void klfb_CompressedTexSubImage2D(uint32_t target, int32_t level, int32_t xoff,
                                         int32_t yoff, int32_t w, int32_t h,
                                         int64_t format, int64_t imageSize,
                                         const void *data) {
    uint32_t fmt = (uint32_t)format;
    int32_t  sz  = (int32_t)imageSize;
    int bw, bh, bpb, known = klfb_block_geom(fmt, &bw, &bh, &bpb);
    long expect = known ? (long)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bpb : -1;

    // What the level is actually supposed to be, from the glTexStorage2D record.
    //
    // Behind its own knob, because unlike `expect` above this is not free: it is
    // a glGetIntegerv per compressed upload, a synchronous round trip into
    // ANGLE, and instrumentation that perturbs the thing it measures has bitten
    // this file before. It cannot be folded into "only look when the size is
    // already wrong" either — a region that overruns its mip level has a
    // perfectly correct imageSize, and that is exactly the case still open.
    int32_t bound = -1, tw = 0, th = 0;
    uint32_t tfmt = 0;
    int have = 0;
    if ((klfb_probe_ctex() || klfb_trace_tex()) && a_glGetIntegerv) {
        a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &bound);
        if (bound > 0) have = klfb_tex_info((uint32_t)bound, &tfmt, &tw, &th);
    }
    int32_t lw = have ? (tw >> level ? tw >> level : 1) : 0;
    int32_t lh = have ? (th >> level ? th >> level : 1) : 0;

    // Three ways this call can be wrong, each named separately so the log says
    // which one rather than just "something is off".
    const char *verdict = "";
    if (!known)                        verdict = "  <-- UNKNOWN COMPRESSED FORMAT";
    else if (expect != sz)             verdict = "  <-- SIZE MISMATCH";
    else if (have && (xoff + w > lw || yoff + h > lh))
                                       verdict = "  <-- REGION OVERRUNS LEVEL";
    else if (have && fmt != tfmt)      verdict = "  <-- FORMAT != ALLOCATED";

    // A verdict always prints, even with no knob set — a wrong upload is worth
    // a line in any run. The knobs add the clean ones, so the host and the
    // Simulator are diffable.
    if (klfb_trace_tex() || klfb_probe_ctex() || *verdict)
        fprintf(stderr, "  [glfb] #%u glCompressedTexSubImage2D target=0x%04x "
                        "level=%d %dx%d at %d,%d fmt=0x%04x imageSize=%d "
                        "expect=%ld (tex=%d alloc 0x%04x %dx%d, level %dx%d)%s\n",
                g_texcalls++, target, level, w, h, xoff, yoff, fmt, sz, expect,
                bound, tfmt, tw, th, lw, lh, verdict);

    klfb_cimg_note_upload(target, level, xoff, yoff, w, h, fmt, sz, data);
    if (g_real_CompressedTexSubImage2D) {
        void *padded = klfb_sim_pad_compressed(fmt, w, h, sz, &data);
        g_real_CompressedTexSubImage2D(target, level, xoff, yoff, w, h, fmt, sz, data);
        free(padded);
    }
}

static void (*g_real_CompressedTexSubImage3D)(uint32_t, int32_t, int32_t, int32_t,
                                              int32_t, int32_t, int32_t, int32_t,
                                              uint32_t, int32_t, const void *);

static void klfb_CompressedTexSubImage3D(uint32_t target, int32_t level, int32_t xoff,
                                         int32_t yoff, int32_t zoff, int32_t w,
                                         int32_t h, int32_t d, int64_t format,
                                         int64_t imageSize, const void *data) {
    if (g_real_CompressedTexSubImage3D)
        g_real_CompressedTexSubImage3D(target, level, xoff, yoff, zoff, w, h, d,
                                       (uint32_t)format, (int32_t)imageSize, data);
}

// ------------------------------------------- glCopyImageSubData, emulated
//
// ES 3.2 core, and ANGLE's Metal backend does not offer it — not under the core
// name and not as EXT/OES_copy_image ("ES 3.2 by extension: copy_image=NO" at
// init). Unreal's texture streaming depends on it: a texture that gains mips is
// re-created and its resident mips are COPIED across, so every failed copy left
// a streamed texture empty — the pink knife, the untextured menu street in
// Saints & Sinners (x2897337400373711). Unreal's textures are ASTC, which GL
// cannot read back or render into, so the copy is served from a CPU shadow of
// what was uploaded: every compressed 2D upload is kept per (texture, level),
// and a copy re-uploads the source region into the destination with
// glCompressedTexSubImage2D and shadows that too (a streamed texture is often
// copied again). Uncompressed 2D copies go through a scratch read framebuffer
// and glCopyTexSubImage2D. Anything else is reported once and passed through.
//
// The shadow costs memory equal to the compressed textures resident, so it is
// on only where it is needed: when ANGLE lacks copy_image, and by default only
// for the Unreal titles that were seen to need it (fix 15; by the same need
// Tales from the Galaxy's Edge, x3484270071659289; and Maestro,
// x6963344583787383, which was brought up with an emulation of its own before
// this one existed; 2026-10-11). The call is
// core in the ES 3.2 this runtime says it is, so every Unreal guest on GLES
// that streams textures wants this — and would pay that memory, which nobody
// has measured on a headset yet. KL_GLFB_COPYIMAGE_EMU=0/1 overrides.
static int klfb_cimg_on(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        on = !g_angle_copy_image &&
             // Tetris Effect: Connected (x3386618894743567) is the fourth: its
             // scenery turned plain purple when looked at — the streamer gives
             // what is in view more mips, the copy of the resident ones failed —
             // and green and textured again when it left the view and the
             // texture shrank back (owner, headset, 2026-10-13).
             // Robo Recall: Unplugged (x1906871599408213, UE 4.21) is the
             // fifth: once it booted, its street, cars, trees and robots were
             // flat magenta with the lighting still on them while the sidewalk,
             // pillars and billboards drew textured — the streamed textures
             // empty, the unstreamed ones fine (owner, headset, 2026-10-14).
             // The Walking Dead: Saints & Sinners - Chapter 2: Retribution
             // (x5190288877703616, UE 4.27.2) is the sixth: once it drew,
             // all its textures were purple (on device, 2026-10-17).
             // Vader Immortal I–III (x2108775495884888, x2223615721048141,
             // x2426206484098337, UE 4.19) next: the title logo's stone texture
             // read as solid magenta — all-zero ASTC blocks, the decoder's
             // error colour — so the logo came out pink instead of grey stone
             // with red edges (his headset, 2026-10-19; Mac, texture dump).
             kl_env_on("KL_GLFB_COPYIMAGE_EMU", t && (strcmp(t, "x2108775495884888") == 0 ||
                                                      strcmp(t, "x2223615721048141") == 0 ||
                                                      strcmp(t, "x2426206484098337") == 0 ||
                                                      strcmp(t, "x2897337400373711") == 0 ||
                                                      strcmp(t, "x3484270071659289") == 0 ||
                                                      strcmp(t, "x6963344583787383") == 0 ||
                                                      strcmp(t, "x3386618894743567") == 0 ||
                                                      strcmp(t, "x1906871599408213") == 0 ||
                                                      strcmp(t, "x5190288877703616") == 0 ||
                                                      // Warhammer 40,000: Battle Sister
                                                      // (Unity) copies its level's textures
                                                      // into 2D arrays while loading, and
                                                      // its floors stayed black (2026-10-28).
                                                      strcmp(t, "x3231001163584154") == 0));
        if (on) fprintf(stderr, "  [glfb] glCopyImageSubData emulated (compressed "
                                "uploads shadowed on the CPU)\n");
    }
    return on;
}

#define KLFB_CIMG_LEVELS 16
typedef struct {
    uint32_t name, fmt;
    int32_t  w, h, levels;
    uint8_t *lv[KLFB_CIMG_LEVELS];          // whole level, block rows packed
} klfb_cimg;
#define KLFB_CIMG_MAX 16384
static klfb_cimg *g_cimg[KLFB_CIMG_MAX];   // open addressing on the name
static pthread_mutex_t g_cimg_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t g_cimg_bytes;

static klfb_cimg **klfb_cimg_slot(uint32_t name, int make) {
    unsigned h = (name * 2654435761u) % KLFB_CIMG_MAX;
    klfb_cimg **tomb = NULL;
    for (unsigned i = 0; i < KLFB_CIMG_MAX; i++, h = (h + 1) % KLFB_CIMG_MAX) {
        klfb_cimg *e = g_cimg[h];
        if (!e) return make ? (tomb ? tomb : &g_cimg[h]) : NULL;
        if (e == (klfb_cimg *)1) { if (!tomb) tomb = &g_cimg[h]; continue; }
        if (e->name == name) return &g_cimg[h];
    }
    return make ? tomb : NULL;
}
static void klfb_cimg_free(klfb_cimg *e) {
    for (int l = 0; l < KLFB_CIMG_LEVELS; l++) free(e->lv[l]);
    free(e);
}
static int32_t klfb_cimg_dim(int32_t v, int l) { v >>= l; return v ? v : 1; }
static size_t klfb_cimg_level_bytes(const klfb_cimg *e, int l, int *bx) {
    int bw, bh, bpb;
    if (!klfb_block_geom(e->fmt, &bw, &bh, &bpb)) return 0;
    int x = (klfb_cimg_dim(e->w, l) + bw - 1) / bw;
    int y = (klfb_cimg_dim(e->h, l) + bh - 1) / bh;
    if (bx) *bx = x;
    return (size_t)x * y * bpb;
}

static uint32_t klfb_bound_tex2d(void) {
    int32_t b = 0;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &b);
    return b > 0 ? (uint32_t)b : 0;
}

static void klfb_cimg_note_storage(uint32_t target, int32_t levels, uint32_t fmt,
                                   int32_t w, int32_t h) {
    if (target != 0x0DE1 /* TEXTURE_2D */ || !klfb_cimg_on()) return;
    int bw, bh, bpb;
    if (!klfb_block_geom(fmt, &bw, &bh, &bpb)) return;
    uint32_t name = klfb_bound_tex2d();
    if (!name) return;
    klfb_cimg *e = calloc(1, sizeof *e);
    if (!e) return;
    e->name = name; e->fmt = fmt; e->w = w; e->h = h;
    e->levels = levels < KLFB_CIMG_LEVELS ? levels : KLFB_CIMG_LEVELS;
    pthread_mutex_lock(&g_cimg_lock);
    klfb_cimg **s = klfb_cimg_slot(name, 1);
    if (s) {
        if (*s && *s != (klfb_cimg *)1) {
            for (int l = 0; l < KLFB_CIMG_LEVELS; l++)
                if ((*s)->lv[l]) g_cimg_bytes -= klfb_cimg_level_bytes(*s, l, NULL);
            klfb_cimg_free(*s);
        }
        *s = e;
    } else {
        free(e);
    }
    pthread_mutex_unlock(&g_cimg_lock);
}

static void klfb_cimg_forget(int32_t n, const uint32_t *ids) {
    if (!klfb_cimg_on()) return;
    pthread_mutex_lock(&g_cimg_lock);
    for (int32_t i = 0; i < n; i++) {
        klfb_cimg **s = klfb_cimg_slot(ids[i], 0);
        if (!s) continue;
        for (int l = 0; l < KLFB_CIMG_LEVELS; l++)
            if ((*s)->lv[l]) g_cimg_bytes -= klfb_cimg_level_bytes(*s, l, NULL);
        klfb_cimg_free(*s);
        *s = (klfb_cimg *)1;
    }
    pthread_mutex_unlock(&g_cimg_lock);
}

// Copy a block-aligned region of rows into a level shadow.
static void klfb_cimg_put(klfb_cimg *e, int level, int32_t x, int32_t y,
                          int32_t w, int32_t h, const uint8_t *src) {
    int bw, bh, bpb, bx = 0;
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return;   // the guest's numbers, not ours
    if (!klfb_block_geom(e->fmt, &bw, &bh, &bpb)) return;
    size_t lb = klfb_cimg_level_bytes(e, level, &bx);
    if (!lb) return;
    if (!e->lv[level]) {
        e->lv[level] = calloc(1, lb);
        if (!e->lv[level]) return;
        g_cimg_bytes += lb;
    }
    int x0 = x / bw, y0 = y / bh;
    int rw = (w + bw - 1) / bw, rh = (h + bh - 1) / bh;
    int by = (int)(lb / ((size_t)bx * bpb));
    for (int r = 0; r < rh && y0 + r < by; r++) {
        int cols = (x0 + rw <= bx) ? rw : bx - x0;
        if (cols <= 0) break;
        memcpy(e->lv[level] + ((size_t)(y0 + r) * bx + x0) * bpb,
               src + (size_t)r * rw * bpb, (size_t)cols * bpb);
    }
}

static void klfb_cimg_note_upload(uint32_t target, int32_t level, int32_t xoff,
                                  int32_t yoff, int32_t w, int32_t h, uint32_t fmt,
                                  int32_t sz, const void *data) {
    if (target != 0x0DE1 || !klfb_cimg_on() || level < 0 || level >= KLFB_CIMG_LEVELS)
        return;
    // An upload that says it carries fewer bytes than its rows of blocks need
    // is not copied from: the copy below reads rows x columns, not `sz`.
    {
        int bw, bh, bpb;
        if (xoff < 0 || yoff < 0 || w <= 0 || h <= 0 || sz <= 0) return;
        if (!klfb_block_geom(fmt, &bw, &bh, &bpb)) return;
        if ((int64_t)sz < (int64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bpb) return;
    }
    uint32_t name = klfb_bound_tex2d();
    if (!name) return;
    // A pixel-unpack buffer makes `data` an offset into it: read it back.
    int32_t pbo = 0;
    if (a_glGetIntegerv) a_glGetIntegerv(0x88EF /* PIXEL_UNPACK_BUFFER_BINDING */, &pbo);
    const uint8_t *bytes = data;
    void *mapped = NULL;
    static void *(*map)(uint32_t, intptr_t, intptr_t, uint32_t);
    static uint8_t (*unmap)(uint32_t);
    if (pbo > 0) {
        if (!map) { map = asym("glMapBufferRange"); unmap = asym("glUnmapBuffer"); }
        if (!map || !unmap) return;
        mapped = map(0x88EC /* PIXEL_UNPACK_BUFFER */, (intptr_t)data, sz,
                     0x0001 /* MAP_READ_BIT */);
        if (!mapped) {
            static int said;
            if (!said++) fprintf(stderr, "  [glfb] copy-image shadow: could not map the "
                                         "unpack buffer — that upload is not shadowed\n");
            return;
        }
        bytes = mapped;
    }
    if (bytes) {
        pthread_mutex_lock(&g_cimg_lock);
        klfb_cimg **s = klfb_cimg_slot(name, 0);
        if (s && (*s)->fmt == fmt && level < (*s)->levels)
            klfb_cimg_put(*s, level, xoff, yoff, w, h, bytes);
        pthread_mutex_unlock(&g_cimg_lock);
    }
    if (mapped) unmap(0x88EC);
}

static void (*g_real_CopyImageSubData)(uint32_t, uint32_t, int32_t, int32_t, int32_t,
                                       int32_t, uint32_t, uint32_t, int32_t, int32_t,
                                       int32_t, int32_t, int32_t, int32_t, int32_t);
// Fifteen arguments, so the last seven arrive on the stack, and the guest
// pushed them the AAPCS64 way: one 8-byte slot each. Apple's arm64 ABI packs
// 32-bit stack arguments into 4 bytes, so they are taken as int64_t here (the
// same fixup as the compressed-upload thunks) and narrowed.
static void klfb_CopyImageSubData_impl(uint32_t src, uint32_t stgt, int32_t slevel,
                                       int32_t sx, int32_t sy, int32_t sz,
                                       uint32_t dst, uint32_t dtgt, int32_t dlevel,
                                       int32_t dx, int32_t dy, int32_t dz,
                                       int32_t w, int32_t h, int32_t d);
static void klfb_CopyImageSubData(uint32_t src, uint32_t stgt, int32_t slevel,
                                  int32_t sx, int32_t sy, int32_t sz,
                                  uint32_t dst, uint32_t dtgt, int64_t dlevel,
                                  int64_t dx, int64_t dy, int64_t dz,
                                  int64_t w, int64_t h, int64_t d) {
    klfb_CopyImageSubData_impl(src, stgt, slevel, sx, sy, sz, dst, dtgt,
                               (int32_t)dlevel, (int32_t)dx, (int32_t)dy,
                               (int32_t)dz, (int32_t)w, (int32_t)h, (int32_t)d);
}
static void klfb_CopyImageSubData_impl(uint32_t src, uint32_t stgt, int32_t slevel,
                                       int32_t sx, int32_t sy, int32_t sz,
                                       uint32_t dst, uint32_t dtgt, int32_t dlevel,
                                       int32_t dx, int32_t dy, int32_t dz,
                                       int32_t w, int32_t h, int32_t d) {
    static unsigned long copies, compressed, viafbo, passed;
    // Not emulating: the call goes on to ANGLE, with its stack arguments now
    // read the way the guest pushed them — which is what this thunk is handed
    // out for even then.
    if (!klfb_cimg_on()) {
        if (g_real_CopyImageSubData)
            g_real_CopyImageSubData(src, stgt, slevel, sx, sy, sz, dst, dtgt, dlevel,
                                    dx, dy, dz, w, h, d);
        return;
    }
    if (sx < 0 || sy < 0 || dx < 0 || dy < 0 || w <= 0 || h <= 0) return;   // GL_INVALID_VALUE on a real driver
    // 2D and 2D-array slices both ways (a slice is a layer index in z); a
    // compressed slice of an array source has no shadow and fails as before.
    int sarr = stgt == 0x8C1A /* TEXTURE_2D_ARRAY */, darr = dtgt == 0x8C1A;
    if ((stgt != 0x0DE1 && !sarr) || (dtgt != 0x0DE1 && !darr) ||
        (sz && !sarr) || (dz && !darr) || sz < 0 || dz < 0 || d != 1 ||
        slevel < 0 || dlevel < 0 || slevel >= KLFB_CIMG_LEVELS ||
        dlevel >= KLFB_CIMG_LEVELS) {
        if (!passed++)
            fprintf(stderr, "  [glfb] glCopyImageSubData: target 0x%x->0x%x depth %d "
                            "is not emulated — passed through\n", stgt, dtgt, d);
        if (g_real_CopyImageSubData)
            g_real_CopyImageSubData(src, stgt, slevel, sx, sy, sz, dst, dtgt, dlevel,
                                    dx, dy, dz, w, h, d);
        return;
    }
    copies++;
    uint32_t prev = klfb_bound_tex2d();
    int32_t prev_arr = 0;
    if (darr && a_glGetIntegerv) a_glGetIntegerv(0x8C1D /* TEXTURE_BINDING_2D_ARRAY */, &prev_arr);
    static void (*bind)(uint32_t, uint32_t);
    if (!bind) bind = asym("glBindTexture");

    // Compressed: from the shadow.
    uint8_t *buf = NULL;
    uint32_t fmt = 0;
    pthread_mutex_lock(&g_cimg_lock);
    klfb_cimg **s = sarr ? NULL : klfb_cimg_slot(src, 0);
    if (s && (*s)->lv[slevel]) {
        int bw, bh, bpb, bx = 0;
        klfb_cimg *e = *s;
        fmt = e->fmt;
        size_t lb = klfb_cimg_level_bytes(e, slevel, &bx);
        if (lb && klfb_block_geom(fmt, &bw, &bh, &bpb)) {
            int x0 = sx / bw, y0 = sy / bh;
            int rw = (w + bw - 1) / bw, rh = (h + bh - 1) / bh;
            int by = (int)(lb / ((size_t)bx * bpb));
            if (x0 + rw <= bx && y0 + rh <= by && (buf = malloc((size_t)rw * rh * bpb)))
                for (int r = 0; r < rh; r++)
                    memcpy(buf + (size_t)r * rw * bpb,
                           e->lv[slevel] + ((size_t)(y0 + r) * bx + x0) * bpb,
                           (size_t)rw * bpb);
            if (buf && !darr) {
                klfb_cimg **ds = klfb_cimg_slot(dst, 0);
                if (ds && (*ds)->fmt == fmt && dlevel < (*ds)->levels)
                    klfb_cimg_put(*ds, dlevel, dx, dy, w, h, buf);
            }
        }
    }
    pthread_mutex_unlock(&g_cimg_lock);
    if (buf) {
        int bw, bh, bpb;
        klfb_block_geom(fmt, &bw, &bh, &bpb);
        int32_t bytes = ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bpb;
        int32_t pbo = 0;
        static void (*bindbuf)(uint32_t, uint32_t);
        if (!bindbuf) bindbuf = asym("glBindBuffer");
        if (a_glGetIntegerv) a_glGetIntegerv(0x88EF, &pbo);
        if (pbo && bindbuf) bindbuf(0x88EC, 0);
        if (bind) bind(dtgt, dst);
        if (darr ? g_real_CompressedTexSubImage3D != NULL : g_real_CompressedTexSubImage2D != NULL) {
            const void *src = buf;
            void *padded = klfb_sim_pad_compressed(fmt, w, h, bytes, &src);
            if (darr)
                g_real_CompressedTexSubImage3D(dtgt, dlevel, dx, dy, dz, w, h, 1, fmt, bytes, src);
            else
                g_real_CompressedTexSubImage2D(0x0DE1, dlevel, dx, dy, w, h, fmt, bytes, src);
            free(padded);
        }
        if (bind) bind(dtgt, darr ? (uint32_t)prev_arr : prev);
        if (pbo && bindbuf) bindbuf(0x88EC, (uint32_t)pbo);
        free(buf);
        compressed++;
    } else {
        // Uncompressed (or a compressed source never seen uploading): read
        // framebuffer + glCopyTexSubImage2D. A compressed source with no shadow
        // fails here exactly as before, and is counted.
        static void (*genfb)(int32_t, uint32_t *);
        static void (*delfb)(int32_t, const uint32_t *);
        static void (*bindfb)(uint32_t, uint32_t);
        static void (*fbtex)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
        static void (*copysub)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                               int32_t, int32_t);
        static void (*fblayer)(uint32_t, uint32_t, uint32_t, int32_t, int32_t);
        static void (*copysub3)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                                int32_t, int32_t, int32_t);
        if (!genfb) {
            genfb = asym("glGenFramebuffers"); delfb = asym("glDeleteFramebuffers");
            bindfb = asym("glBindFramebuffer"); fbtex = asym("glFramebufferTexture2D");
            copysub = asym("glCopyTexSubImage2D");
            fblayer = asym("glFramebufferTextureLayer"); copysub3 = asym("glCopyTexSubImage3D");
        }
        if (genfb && delfb && bindfb && fbtex && copysub && bind &&
            (!sarr || fblayer) && (!darr || copysub3)) {
            int32_t rfb = 0;
            if (a_glGetIntegerv) a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &rfb);
            uint32_t fb = 0;
            genfb(1, &fb);
            bindfb(0x8CA8 /* READ_FRAMEBUFFER */, fb);
            if (sarr) fblayer(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */, src, slevel, sz);
            else      fbtex(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */, 0x0DE1, src, slevel);
            bind(dtgt, dst);
            if (darr) copysub3(dtgt, dlevel, dx, dy, dz, sx, sy, w, h);
            else      copysub(0x0DE1, dlevel, dx, dy, sx, sy, w, h);
            bind(dtgt, darr ? (uint32_t)prev_arr : prev);
            bindfb(0x8CA8, (uint32_t)rfb);
            delfb(1, &fb);
            viafbo++;
        }
    }
    if (copies == 1 || copies % 500 == 0)
        fprintf(stderr, "  [glfb] glCopyImageSubData emulated: %lu copies (%lu "
                        "compressed from the shadow, %lu via framebuffer), shadow "
                        "%zu MiB\n", copies, compressed, viafbo, g_cimg_bytes >> 20);
}

// ------------------------------------------------------- draw-target census
//
// The frame reads back black even though the capture is now correct, so the
// question is where the draws are going. Unity believes it is in VR
// (ovrp_Initialize5 answers success), and a VR frame does not present to the
// backbuffer — it renders into eye textures and hands them to a compositor. We
// serve no eye textures, so the engine may be drawing somewhere it never shows us.
//
// This counts draws per bound framebuffer, which distinguishes the two cases
// outright: draws on framebuffer 0 mean a flat frame we are failing to capture,
// draws only on other framebuffers mean the eye-buffer path and a different job.
#define KLFB_MAX_FBS 16
static struct { int32_t fb; unsigned draws; uint64_t tid; } g_draw_fbs[KLFB_MAX_FBS];
static unsigned g_ndraw_fbs;

// KL_GLFB_X_NO_DEPTH=1 / KL_GLFB_X_NO_CULL=1 — experiments, not fixes: switch
// the depth test or face culling off before every draw. A scene that draws and
// leaves nothing behind is, more often than anything else, one of these two
// rejecting every fragment; this separates them in one run each.
static void klfb_draw_experiments(void) {
    static int nodepth = -1, nocull = -1;
    static void (*disable)(uint32_t);
    if (nodepth < 0) {
        nodepth = kl_env_on("KL_GLFB_X_NO_DEPTH", 0);
        nocull  = kl_env_on("KL_GLFB_X_NO_CULL", 0);
        disable = asym("glDisable");
    }
    if (!disable) return;
    if (nodepth) disable(0x0B71);
    if (nocull)  disable(0x0B44);
}

static void klfb_note_draw(void) {
    if (klfb_slow_ms()) g_draw_t0 = klfb_slow_now();
    klfb_draw_experiments();
    int32_t fb = -1;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
    // Fall back to the binding Klepton tracks in the glBindFramebuffer wrapper when
    // the live GL query yields nothing (it comes back -1 for the hl2 gameplay draws —
    // that "fb -1" bucket is really the guest's own render target, just unidentified
    // by the query). g_draw_fb names it so the census can attribute the two-pass
    // gameplay scene to its actual FBO (the source for the eye->array composite).
    if (fb < 0) fb = (int32_t)g_draw_fb;
    // The framebuffer the last DRAW CALL went to, which is not the last binding:
    // this guest draws into one framebuffer and blits out of another.
    if (fb > 0) g_last_draw_fb = (uint32_t)fb;
    // The same timeline the blit probe reads: order the draws against the
    // clears/invalidates/blits, because "black at blit time" is meaningless
    // without knowing whether a draw came after the last clear. Program,
    // viewport and tex0 name the pass — the frame's last draw paints the
    // whole scene target black, and identifying it is the whole game now.
    if (klfb_timeline()) {
        int32_t prog = -1, vp[4] = {0,0,0,0}, tex0 = -1;
        if (a_glGetIntegerv) {
            a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &prog);
            a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
            a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &tex0);
        }
        uint32_t tfmt = 0; int32_t tw = 0, th = 0;
        const char *tinfo = "";
        char tbuf[48];
        if (tex0 > 0 && klfb_tex_info((uint32_t)tex0, &tfmt, &tw, &th)) {
            snprintf(tbuf, sizeof tbuf, " fmt=0x%x %dx%d", tfmt, tw, th);
            tinfo = tbuf;
        }
        fprintf(stderr, "  [glfb] draw on fb=%d program=%d vp=%dx%d tex0=%d%s\n",
                fb, prog, vp[2], vp[3], tex0, tinfo);
    }
    uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
    for (unsigned i = 0; i < g_ndraw_fbs; i++)
        if (g_draw_fbs[i].fb == fb && g_draw_fbs[i].tid == tid) {
            g_draw_fbs[i].draws++;
            return;
        }
    if (g_ndraw_fbs < KLFB_MAX_FBS) {
        g_draw_fbs[g_ndraw_fbs].fb = fb;
        g_draw_fbs[g_ndraw_fbs].draws = 1;
        g_draw_fbs[g_ndraw_fbs].tid = tid;
        g_ndraw_fbs++;
    }
}

// ...and the table SAID so, to nobody: it was filled from the first day of the
// GL path and never printed, so "where is the guest actually drawing?" — the
// question it was built to answer — still needed a bespoke trace every time it
// came up. Each row's colour attachment is resolved at report time rather than
// at draw time, because an FBO's attachment is re-pointed per frame and the
// last one is the one that matters for reading the row.
void kl_glfb_draw_census(FILE *f) {
    if (g_blits)
        fprintf(f, "  [glfb] glBlitFramebuffer: %lu, of which %lu read the "
                   "DEFAULT framebuffer\n", g_blits, g_blits_read0);
    if (!g_ndraw_fbs) return;
    fprintf(f, "  [glfb] draws per framebuffer:\n");
    for (unsigned i = 0; i < g_ndraw_fbs; i++) {
        int32_t fb = g_draw_fbs[i].fb;
        char what[96] = "";
        if (fb == 0) snprintf(what, sizeof what, " (the default framebuffer)");
        else {
            uint32_t tex = 0; int layer = -1;
            for (unsigned k = 0; k < g_nfbo_color; k++)
                if (g_fbo_color[k].fbo == (uint32_t)fb) {
                    tex = g_fbo_color[k].tex; layer = g_fbo_color[k].layer; break;
                }
            if (tex) {
                uint32_t fmt = 0; int32_t w = 0, h = 0;
                char lb[24] = "";
                klfb_tex_info(tex, &fmt, &w, &h);
                if (layer >= 0) snprintf(lb, sizeof lb, " layer %d", layer);
                // Centre-pixel readback: a LIVE eye slice's centre changes every
                // census; a FROZEN one is static (the array is a stale copy). Reads
                // the FBO's currently-attached layer, so array slices read per-eye.
                char pxs[40] = "";
                if (a_glReadPixels && r_BindFramebuffer && a_glGetIntegerv &&
                    w > 1 && h > 1) {
                    int32_t save = 0;
                    a_glGetIntegerv(0x8CA8 /* READ_FRAMEBUFFER_BINDING */, &save);
                    r_BindFramebuffer(0x8CA8, (uint32_t)fb);
                    if (!a_glCheckFramebufferStatus ||
                        a_glCheckFramebufferStatus(0x8CA8) == 0x8CD5 /* COMPLETE */) {
                        unsigned char px[4] = {0,0,0,0};
                        a_glReadPixels(w/2, h/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                        snprintf(pxs, sizeof pxs, " centre=(%u,%u,%u,%u)",
                                 px[0], px[1], px[2], px[3]);
                    }
                    r_BindFramebuffer(0x8CA8, (uint32_t)save);
                }
                snprintf(what, sizeof what, " (colour0 tex %u%s %dx%d fmt 0x%x)%s",
                         tex, lb, w, h, fmt, pxs);
            }
        }
        fprintf(f, "    fb %-3d thread %-8llu %u draws%s\n", fb,
                (unsigned long long)g_draw_fbs[i].tid, g_draw_fbs[i].draws, what);
    }
}

static void (*g_real_DrawElements)(uint32_t, int32_t, uint32_t, const void *);
static void (*g_real_DrawArrays)(uint32_t, int32_t, int32_t);
static void (*g_real_DrawElementsInstanced)(uint32_t, int32_t, uint32_t, const void *,
                                            int32_t);
static void (*g_real_DrawElementsBaseVertex)(uint32_t, int32_t, uint32_t, const void *,
                                             int32_t);
static void (*g_real_DrawArraysInstanced)(uint32_t, int32_t, int32_t, int32_t);
static void klfb_service_capture(void);

// KL_GLFB_ERRPROBE=1: check the GL error after each draw and print the first
// few with the state around them. The guest accumulates errors it never reads
// (the capture keeps meeting them as pre-existing), so one of its own calls is
// failing silently — this names it. Consumes the error; the guest demonstrably
// was not reading them anyway.
static void klfb_errprobe(const char *what, const char *detail) {
    static int on = -1, said;
    if (on < 0) on = kl_env_on("KL_GLFB_ERRPROBE", 0);
    if (!on || !a_glGetError) return;
    // Consumes the pending error. Called before and after the real call:
    // "pre=0xN" on the BEFORE line is an earlier unwrapped call's leftover, on
    // the AFTER line it is provably this call's own error.
    uint32_t err = a_glGetError();
    if (!err) return;
    int32_t fb = -1, prog = -1, vp[4] = {0,0,0,0};
    if (a_glGetIntegerv) {
        a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
        a_glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &prog);
        a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
    }
    // Klepton: the engine's flat "spectator" present blits the scene to the DEFAULT
    // framebuffer (draw fb 0), which is invalid on ANGLE-Metal (0x502) but irrelevant
    // to the VR swapchain path — the menu renders fine despite it. Don't let those
    // thousands of failures exhaust the cap; the VR-path errors are the ones worth
    // seeing. KL_GLFB_ERRPROBE_FB0=1 keeps them if ever needed.
    if (fb == 0 && !kl_env_on("KL_GLFB_ERRPROBE_FB0", 0)) return;
    // Dedupe by (fb, program, err) so a flood of identical errors in one phase
    // (e.g. the menu's stereo draws) doesn't exhaust the cap before the cinema /
    // gameplay errors ever appear — each distinct site logs once. This is what lets
    // us tell whether the mvmm fix cleared the gameplay effect-RT draws vs. the menu
    // errors merely masking them. KL_GLFB_ERRPROBE_ALL=1 logs every occurrence.
    static uint32_t seen_keys[128]; static int seen_n;
    static int all = -1;
    if (all < 0) all = kl_env_on("KL_GLFB_ERRPROBE_ALL", 0);
    if (!all) {
        uint32_t key = ((uint32_t)fb << 20) ^ ((uint32_t)prog << 4) ^ err;
        for (int i = 0; i < seen_n; i++)
            if (seen_keys[i] == key) return;
        if (seen_n >= (int)(sizeof seen_keys / sizeof seen_keys[0])) return;
        seen_keys[seen_n++] = key;
    }
    said++;
    fprintf(stderr, "  [glfb] ERRPROBE %s: pre=0x%x fb=%d program=%d viewport %dx%d%s\n",
            what, err, fb, prog, vp[2], vp[3], detail ? detail : "");
    if (err == 0x506 /* INVALID_FRAMEBUFFER_OPERATION */) {
        static uint32_t (*ck)(uint32_t);
        if (!ck) ck = asym("glCheckFramebufferStatus");
        if (ck)
            fprintf(stderr, "  [glfb] ERRPROBE %s: CheckFramebufferStatus(DRAW)=0x%x\n",
                    what, ck(0x8CA9));
    }
}

static void klfb_errprobe(const char *what, const char *detail);
#define klfb_errprobe0(w) klfb_errprobe((w), NULL)

// KL_GLFB_DRAW_PROBE=1: after each of the first few draws, read back the
// current DRAW framebuffer and say whether the draw emitted anything.
// Distinguishes "draws run but output black" from "content drawn but lost
// before the capture" — the readback happens inside the same command stream,
// so there is no timing or thread question left. The read goes through
// klfb_probe_fbo: the scene target is an RGBA16F 4xMSAA renderbuffer, which a
// direct RGBA/UNSIGNED_BYTE readback cannot see — it answers err 0x500.
static void klfb_draw_probe(int verts) {
    if (g_draw_t0) {
        double ms = (double)(klfb_slow_now() - g_draw_t0) / 1.0e6;
        g_draw_t0 = 0;
        if (ms >= (double)klfb_slow_ms()) klfb_slow_note(2, 0, ms, verts);
    }
    // KL_GLFB_DUMP_PC: read back the ambient-cube pixel constants (pc[5..10] in the
    // VertexLitGeneric PS — six cube-face colours weighted by the normal) for the bound
    // program. Forks the black-in-shadow props: zero here = the ambient VALUES never reach
    // the shader (Source light-cache / togl upload); non-zero = a shader/translation issue
    // on ANGLE-Metal. Independent of DRAW_PROBE, its own env gate.
    {
        static int dpc = -1;
        static int32_t pc_seen[64]; static int pc_nseen = 0;
        if (dpc < 0) dpc = kl_env_on("KL_GLFB_DUMP_PC", 0);
        if (dpc && pc_nseen < 64 && a_glGetIntegerv) {
            int32_t prog = 0;
            a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &prog);
            // Dedupe by program so one heavily-drawn early shader can't fill the cap —
            // we want a sample from EACH program, including the model ones.
            int pc_dup = 0;
            for (int i = 0; i < pc_nseen; i++) if (pc_seen[i] == prog) { pc_dup = 1; break; }
            static int32_t (*getloc)(uint32_t, const char *);
            static void (*getunif)(uint32_t, int32_t, float *);
            if (!getloc) {
                getloc  = asym("glGetUniformLocation");
                getunif = asym("glGetUniformfv");
            }
            if (!pc_dup && prog > 0 && getloc && getunif) {
                int32_t l5 = getloc((uint32_t)prog, "pc[5]");
                pc_seen[pc_nseen++] = prog;
                if (l5 >= 0) {   // a togl shader with the pc constant array
                    float a5[4] = {0}, a6[4] = {0}, a10[4] = {0};
                    int32_t l6  = getloc((uint32_t)prog, "pc[6]");
                    int32_t l10 = getloc((uint32_t)prog, "pc[10]");
                    getunif((uint32_t)prog, l5, a5);
                    if (l6 >= 0)  getunif((uint32_t)prog, l6, a6);
                    if (l10 >= 0) getunif((uint32_t)prog, l10, a10);
                    fprintf(stderr, "  [glfb] DUMP_PC prog %d ambientcube "
                            "pc5=(%.3f %.3f %.3f) pc6=(%.3f %.3f %.3f) "
                            "pc10=(%.3f %.3f %.3f)\n", prog, a5[0], a5[1], a5[2],
                            a6[0], a6[1], a6[2], a10[0], a10[1], a10[2]);
                }
            }
        }
    }
    static int on = -1, said, quota, skip, seen;
    if (on < 0) {
        on = kl_env_on("KL_GLFB_DRAW_PROBE", 0);
        // KL_GLFB_DRAW_PROBE_N overrides the 12-line default; scene frames
        // burn the quota on early frames otherwise. 0 means unlimited.
        quota = kl_env_int("KL_GLFB_DRAW_PROBE_N", 12);
        // ...and KL_GLFB_DRAW_PROBE_SKIP is the other end of that problem: a
        // guest whose STARTUP renders and whose steady state does not can only
        // be caught after the transition, and "unlimited from draw 0" is a
        // readback per draw for the whole run. Skip the first N eligible draws
        // and spend the quota where the question is.
        skip = kl_env_int("KL_GLFB_DRAW_PROBE_SKIP", 0);
    }
    if (!on || (quota && said >= quota) || !a_glReadPixels) return;
    // KL_GLFB_DRAW_PROBE_MIN lowers the 32-vert floor — the frame's last few
    // draws are small, and one of them is a suspect in the scene's erasure.
    static int minv = -1;
    if (minv < 0) {
        minv = kl_env_int("KL_GLFB_DRAW_PROBE_MIN", 12);
    }
    if (verts < minv) return;
    if (seen++ < skip) return;
    int32_t fb = -1, rfb = -1;
    if (a_glGetIntegerv) {
        a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
        a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &rfb);
    }
    if (a_glFinish) a_glFinish();
    said++;
    static float *pfb;
    static uint8_t *pbb;
    static void (*dp_bind)(uint32_t, uint32_t);
    if (!pfb) {
        pfb = malloc((size_t)g_w * g_h * 16);
        pbb = malloc((size_t)g_w * g_h * 4);
        dp_bind = asym("glBindFramebuffer");
    }
    char note[160] = "";
    unsigned long lit = 0;
    // The viewport is the size the draw itself was aimed at, which is the best
    // available answer for a target the allocation table never saw.
    int32_t dvp[4] = {0, 0, 0, 0};
    if (a_glGetIntegerv) a_glGetIntegerv(0x0BA2 /* VIEWPORT */, dvp);
    if (pfb && pbb)
        lit = klfb_probe_fbo((uint32_t)fb, pfb, pbb, note, sizeof note,
                             NULL, NULL, NULL, dvp[2], dvp[3]);
    if (dp_bind) dp_bind(0x8CA8, (uint32_t)(rfb >= 0 ? rfb : 0));
    fprintf(stderr, "  [glfb] DRAW_PROBE fb=%d: %lu lit (%s)\n", fb, lit, note);
    // ...and what the draw was made of. "Nothing lit" has a dozen causes that
    // all look alike from the framebuffer — a scissor, a mask, a blend that
    // multiplies by zero, a texture that is not the one believed — and they
    // are told apart only by the state the draw ran with.
    if (a_glGetIntegerv) {
        int32_t prog = 0, sc[4] = {0}, mask[4] = {0}, bsrc = 0, bdst = 0, basrc = 0, badst = 0, unit = 0;
        static uint8_t (*enabled)(uint32_t);
        static void (*active)(uint32_t);
        if (!enabled) { enabled = asym("glIsEnabled"); active = asym("glActiveTexture"); }
        a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &prog);
        a_glGetIntegerv(0x0C10 /* SCISSOR_BOX */, sc);
        a_glGetIntegerv(0x0C23 /* COLOR_WRITEMASK */, mask);
        a_glGetIntegerv(0x80C9 /* BLEND_SRC_RGB */, &bsrc);
        a_glGetIntegerv(0x80C8 /* BLEND_DST_RGB */, &bdst);
        a_glGetIntegerv(0x80CB /* BLEND_SRC_ALPHA */, &basrc);
        a_glGetIntegerv(0x80CA /* BLEND_DST_ALPHA */, &badst);
        a_glGetIntegerv(0x84E0 /* ACTIVE_TEXTURE */, &unit);
        char tex[160] = ""; size_t tn = 0;
        for (int u = 0; active && u < 6; u++) {
            int32_t t2 = 0, tx = 0;
            active(0x84C0 + (uint32_t)u);
            a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &t2);
            a_glGetIntegerv(0x8D67 /* TEXTURE_BINDING_EXTERNAL_OES */, &tx);
            if (t2 || tx)
                tn += (size_t)snprintf(tex + tn, sizeof tex - tn, " u%d=%d%s", u, t2 ? t2 : tx, t2 ? "" : "(ext)");
            if (tn >= sizeof tex) break;
        }
        if (active) active((uint32_t)unit);
        {
            int32_t dfunc = 0, dmask = 0, cmode = 0, ffront = 0, dobj = 0, dname = 0;
            float dclear = -1.0f, drange[2] = {-1.0f, -1.0f};
            static void (*getf)(uint32_t, float *);
            static void (*fbattach)(uint32_t, uint32_t, uint32_t, int32_t *);
            if (!getf) { getf = asym("glGetFloatv"); fbattach = asym("glGetFramebufferAttachmentParameteriv"); }
            a_glGetIntegerv(0x0B74 /* DEPTH_FUNC */, &dfunc);
            a_glGetIntegerv(0x0B72 /* DEPTH_WRITEMASK */, &dmask);
            a_glGetIntegerv(0x0B45 /* CULL_FACE_MODE */, &cmode);
            a_glGetIntegerv(0x0B46 /* FRONT_FACE */, &ffront);
            if (getf) { getf(0x0B73 /* DEPTH_CLEAR_VALUE */, &dclear); getf(0x0B70 /* DEPTH_RANGE */, drange); }
            if (fbattach) {
                fbattach(0x8CA9, 0x8D00 /* DEPTH_ATTACHMENT */, 0x8CD0 /* OBJECT_TYPE */, &dobj);
                if (dobj) fbattach(0x8CA9, 0x8D00, 0x8CD1 /* OBJECT_NAME */, &dname);
            }
            fprintf(stderr, "  [glfb]   depth func=0x%x write=%d clear=%.3f range=%.3f..%.3f attachment=%s %d | "
                            "cull mode=0x%x front=0x%x\n", dfunc, dmask, dclear, drange[0], drange[1],
                    dobj == 0x1702 ? "texture" : dobj == 0x8D41 ? "renderbuffer" : "NONE", dname, cmode, ffront);
        }
        fprintf(stderr, "  [glfb]   verts=%d program=%d viewport=%dx%d+%d+%d scissor=%s %dx%d+%d+%d "
                        "mask=%d%d%d%d blend=%s rgb 0x%x/0x%x a 0x%x/0x%x depth=%s cull=%s textures:%s\n",
                verts, prog, dvp[2], dvp[3], dvp[0], dvp[1],
                enabled && enabled(0x0C11) ? "on" : "off", sc[2], sc[3], sc[0], sc[1],
                mask[0], mask[1], mask[2], mask[3],
                enabled && enabled(0x0BE2) ? "on" : "off", bsrc, bdst, basrc, badst,
                enabled && enabled(0x0B71) ? "on" : "off", enabled && enabled(0x0B44) ? "on" : "off",
                tex[0] ? tex : " none");
    }
}

// ---- ES 3.1 vertex-attribute-binding, translated to ES 3.0 -----------------
//
// UE4/UE5's GLES renderer uses the SEPARATE attribute-format API — glVertexAttrib
// Format / glVertexAttribBinding / glBindVertexBuffer / glVertexBindingDivisor —
// which is ES 3.1. ANGLE's Metal backend is an ES 3.0 context, so it resolves
// those entry points but rejects every call at validation; the format is never
// set, and the first draw streams a NULL client array (ANGLE's
// VertexArrayMtl::updateClientAttribs, memmove from 0). TWD2 dies there before
// its first picture. We intercept the four setters, remember the state per VAO,
// and just before each draw re-express it through glVertexAttribPointer /
// glVertexAttribIPointer + glBindBuffer + glVertexAttribDivisor — all ES 3.0.
#define KLFB_VTX_ATTRS 16
#define KLFB_VTX_BINDS 16
typedef struct klfb_vao_state {
    uint32_t name;
    int      used;                                   // any format-API call seen
    struct { uint8_t has, integer, normalized; int32_t size; uint32_t type;
             uint32_t reloff, binding; } a[KLFB_VTX_ATTRS];
    struct { uint32_t buffer; int64_t offset; int32_t stride; uint32_t divisor; }
             b[KLFB_VTX_BINDS];
} klfb_vao_state;

#define KLFB_MAX_VAOS 512
// Per context (klfb_ctx_slot): VAOs are container objects, never shared, and
// their NAMES repeat across contexts — VAO 2 of one context is not VAO 2 of
// another, and which one is bound is each context's own business.
// One attribute of the DEFAULT vertex array object, as the guest last set it.
typedef struct { uint8_t on, integer, norm; int32_t size; uint32_t type;
                 int32_t stride; uint32_t buffer; uintptr_t off; uint32_t divisor; } klfb_attr0;
typedef struct {
    klfb_vao_state  vaos[KLFB_MAX_VAOS];
    unsigned        nvaos;
    klfb_vao_state *cur;                             // the bound VAO's state
    klfb_attr0      vao0[KLFB_VTX_ATTRS];            // what THIS context's VAO 0 holds
    uint32_t        vao0_real_div[KLFB_VTX_ATTRS];   // ...and the divisor ANGLE was given
} klfb_vao_ctx;
#define KLFB_VAO_CTX ((klfb_vao_ctx *)klfb_ctx_slot(KLFB_CTX_SLOT_VAO, sizeof(klfb_vao_ctx)))
#define g_vaos    (KLFB_VAO_CTX->vaos)
#define g_nvaos   (KLFB_VAO_CTX->nvaos)
#define g_cur_vao (KLFB_VAO_CTX->cur)

static klfb_vao_state *klfb_vao_get(uint32_t name) {
    for (unsigned i = 0; i < g_nvaos; i++)
        if (g_vaos[i].name == name) return &g_vaos[i];
    if (g_nvaos < KLFB_MAX_VAOS) {
        klfb_vao_state *v = &g_vaos[g_nvaos++];
        memset(v, 0, sizeof *v); v->name = name;
        return v;
    }
    return NULL;
}

static void (*g_real_BindVertexArray_vtx)(uint32_t);
static void klfb_BindVertexArray(uint32_t vao) {
    if (!g_real_BindVertexArray_vtx) g_real_BindVertexArray_vtx = asym("glBindVertexArray");
    if (g_real_BindVertexArray_vtx) g_real_BindVertexArray_vtx(vao);
    g_cur_vao = klfb_vao_get(vao);
}
// Backing slots for the loader (it stores *real = fn); we never call these — the
// translation goes through glVertexAttribPointer instead.
static void (*g_real_VtxAttribFormat)(uint32_t, int32_t, uint32_t, uint8_t, uint32_t);
static void (*g_real_VtxAttribIFormat)(uint32_t, int32_t, uint32_t, uint32_t);
static void (*g_real_VtxAttribBinding)(uint32_t, uint32_t);
static void (*g_real_BindVertexBuffer)(uint32_t, uint32_t, int64_t, int32_t);
static void (*g_real_VtxBindingDivisor)(uint32_t, uint32_t);
static void klfb_VertexAttribFormat(uint32_t idx, int32_t size, uint32_t type,
                                    uint8_t norm, uint32_t reloff) {
    if (!g_cur_vao) g_cur_vao = klfb_vao_get(0);
    if (g_cur_vao && idx < KLFB_VTX_ATTRS) {
        g_cur_vao->a[idx] = (typeof(g_cur_vao->a[idx])){
            .has=1, .integer=0, .normalized=norm, .size=size, .type=type,
            .reloff=reloff, .binding=g_cur_vao->a[idx].binding };
        g_cur_vao->used = 1;
    }
}
static void klfb_VertexAttribIFormat(uint32_t idx, int32_t size, uint32_t type,
                                     uint32_t reloff) {
    if (!g_cur_vao) g_cur_vao = klfb_vao_get(0);
    if (g_cur_vao && idx < KLFB_VTX_ATTRS) {
        g_cur_vao->a[idx] = (typeof(g_cur_vao->a[idx])){
            .has=1, .integer=1, .normalized=0, .size=size, .type=type,
            .reloff=reloff, .binding=g_cur_vao->a[idx].binding };
        g_cur_vao->used = 1;
    }
}
static void klfb_VertexAttribBinding(uint32_t idx, uint32_t binding) {
    if (!g_cur_vao) g_cur_vao = klfb_vao_get(0);
    if (g_cur_vao && idx < KLFB_VTX_ATTRS && binding < KLFB_VTX_BINDS) {
        g_cur_vao->a[idx].binding = binding; g_cur_vao->used = 1;
    }
}
static void klfb_BindVertexBuffer(uint32_t binding, uint32_t buffer,
                                  int64_t offset, int32_t stride) {
    if (!g_cur_vao) g_cur_vao = klfb_vao_get(0);
    if (g_cur_vao && binding < KLFB_VTX_BINDS) {
        g_cur_vao->b[binding].buffer = buffer;
        g_cur_vao->b[binding].offset = offset;
        g_cur_vao->b[binding].stride = stride;
        g_cur_vao->used = 1;
    }
}
static void klfb_VertexBindingDivisor(uint32_t binding, uint32_t divisor) {
    if (!g_cur_vao) g_cur_vao = klfb_vao_get(0);
    if (g_cur_vao && binding < KLFB_VTX_BINDS) {
        g_cur_vao->b[binding].divisor = divisor; g_cur_vao->used = 1;
    }
}

// ---- the default VAO across a render-thread handoff ------------------------
//
// The guest has ONE rendering context; a thread that makes it current while
// another still holds it gets a second ANGLE context in the same share group
// (klfb_thread_gl, the UE4 render-thread handoff). Shared objects come along,
// container state does not — and the default VAO is container state. Unreal
// caches every attribute it has enabled and every pointer it has set
// (FOpenGLContextState) and calls GL only when its cache says something
// changed. Vader Immortal I starts its VR render thread after the loading
// screen; on that thread's fresh context attributes 0 and 1 were never enabled,
// Unreal's cache said they were, and every world draw read a constant position:
// all triangles on one point, eyes black, sound playing. So the guest's view of
// VAO 0 is kept once, and a context that holds something else is brought in
// line before its next draw. One context throughout (the migration default)
// never differs, and the check is a compare.
static klfb_attr0 g_guest_vao0[KLFB_VTX_ATTRS];
static void (*g_real_EnableVAA)(uint32_t);
static void (*g_real_DisableVAA)(uint32_t);
static void (*g_real_VAPointer)(uint32_t, int32_t, uint32_t, uint8_t, int32_t, const void *);
static void (*g_real_VAIPointer)(uint32_t, int32_t, uint32_t, int32_t, const void *);
static void (*g_real_VADivisor)(uint32_t, uint32_t);
static int klfb_vao0_bound(void) { return !g_cur_vao || g_cur_vao->name == 0; }
static uint32_t klfb_array_buffer(void) {
    int32_t b = 0;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8894 /* ARRAY_BUFFER_BINDING */, &b);
    return (uint32_t)b;
}
static void klfb_EnableVertexAttribArray(uint32_t i) {
    if (g_real_EnableVAA) g_real_EnableVAA(i);
    if (i < KLFB_VTX_ATTRS && klfb_vao0_bound())
        g_guest_vao0[i].on = KLFB_VAO_CTX->vao0[i].on = 1;
}
static void klfb_DisableVertexAttribArray(uint32_t i) {
    if (g_real_DisableVAA) g_real_DisableVAA(i);
    if (i < KLFB_VTX_ATTRS && klfb_vao0_bound())
        g_guest_vao0[i].on = KLFB_VAO_CTX->vao0[i].on = 0;
}
static void klfb_vao0_note(uint32_t i, int integer, int32_t size, uint32_t type,
                           uint8_t norm, int32_t stride, const void *ptr) {
    if (i >= KLFB_VTX_ATTRS || !klfb_vao0_bound()) return;
    klfb_attr0 *g = &g_guest_vao0[i];
    g->integer = (uint8_t)integer; g->norm = norm; g->size = size; g->type = type;
    g->stride = stride; g->buffer = klfb_array_buffer(); g->off = (uintptr_t)ptr;
    klfb_attr0 *c = &KLFB_VAO_CTX->vao0[i];
    uint8_t on = c->on; uint32_t div = c->divisor;
    *c = *g; c->on = on; c->divisor = div;
}
static void klfb_VertexAttribPointer(uint32_t i, int32_t size, uint32_t type, uint8_t norm,
                                     int32_t stride, const void *ptr) {
    if (g_real_VAPointer) g_real_VAPointer(i, size, type, norm, stride, ptr);
    klfb_vao0_note(i, 0, size, type, norm, stride, ptr);
}
static void klfb_VertexAttribIPointer(uint32_t i, int32_t size, uint32_t type,
                                      int32_t stride, const void *ptr) {
    if (g_real_VAIPointer) g_real_VAIPointer(i, size, type, stride, ptr);
    klfb_vao0_note(i, 1, size, type, 0, stride, ptr);
}
static void klfb_VertexAttribDivisor(uint32_t i, uint32_t div) {
    if (g_real_VADivisor) g_real_VADivisor(i, div);
    if (i < KLFB_VTX_ATTRS && klfb_vao0_bound()) {
        g_guest_vao0[i].divisor = KLFB_VAO_CTX->vao0[i].divisor = div;
        KLFB_VAO_CTX->vao0_real_div[i] = div;
    }
}
// The multiview step rate of klfb_apply_vertex_bindings, for the ES 3.0 way of
// setting it: glVertexAttribDivisor on the default VAO. ANGLE draws a two-view
// framebuffer as twice the instances and does not scale a per-instance
// attribute's divisor to match, so instance k of the doubled draw reads element
// k — the left eye gets the even instances, the right eye the odd ones, and the
// second half reads past the end of the data. Unreal 4.19's GLES renderer sets
// its divisors this way and never touches the ES 3.1 binding API. Vader
// Immortal I (x2108775495884888): meteors in the right eye only, and effects
// and holograms flickering. Scaled just before a draw into a multiview
// framebuffer and put back for the next draw into anything else; the guest's
// own value stays what it set. KL_GLFB_MV_DIVISOR=0 leaves it as it was.
static void klfb_vao0_mv_divisors(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_MV_DIVISOR", 1);
    if (!on || !g_real_VADivisor || !klfb_vao0_bound()) return;
    // A default VAO driven through the format API is scaled by the translation
    // in klfb_apply_vertex_bindings instead.
    if (g_cur_vao && g_cur_vao->used) return;
    uint32_t dfb = g_draw_fb;
    unsigned views = dfb < KLFB_RESYNC_FBOS ? g_fbo_multiview[dfb] : 0;
    if (views < 1) views = 1;
    klfb_attr0 *c = KLFB_VAO_CTX->vao0;
    uint32_t *real = KLFB_VAO_CTX->vao0_real_div;
    for (unsigned i = 0; i < KLFB_VTX_ATTRS; i++) {
        uint32_t want = c[i].divisor <= UINT32_MAX / views ? c[i].divisor * views
                                                           : c[i].divisor;
        if (real[i] == want) continue;
        g_real_VADivisor(i, want);
        real[i] = want;
        static int said;
        if (views > 1 && !said++)
            fprintf(stderr, "  [glfb] instanced draw into a %u-view framebuffer (default "
                            "VAO): per-instance attributes stepped every %u instances "
                            "(attribute %u, divisor %u; KL_GLFB_MV_DIVISOR=0 reverts)\n",
                    views, want, i, c[i].divisor);
    }
}
static void klfb_vao0_sync(void) {
    if (!klfb_vao0_bound()) return;
    klfb_attr0 *c = KLFB_VAO_CTX->vao0;
    if (!memcmp(c, g_guest_vao0, sizeof g_guest_vao0)) return;
    if (!g_real_EnableVAA || !g_real_DisableVAA || !g_real_VAPointer) return;
    static void (*r_BindBuffer)(uint32_t, uint32_t);
    if (!r_BindBuffer) r_BindBuffer = asym("glBindBuffer");
    if (!r_BindBuffer) return;
    uint32_t keep = klfb_array_buffer();
    int n = 0;
    for (unsigned i = 0; i < KLFB_VTX_ATTRS; i++) {
        klfb_attr0 *g = &g_guest_vao0[i];
        if (!memcmp(&c[i], g, sizeof *g)) continue;
        n++;
        if (g->size) {
            r_BindBuffer(0x8892 /* ARRAY_BUFFER */, g->buffer);
            if (g->integer && g_real_VAIPointer)
                g_real_VAIPointer(i, g->size, g->type, g->stride, (const void *)g->off);
            else
                g_real_VAPointer(i, g->size, g->type, g->norm, g->stride, (const void *)g->off);
        }
        if (g_real_VADivisor) g_real_VADivisor(i, g->divisor);
        KLFB_VAO_CTX->vao0_real_div[i] = g->divisor;
        if (g->on) g_real_EnableVAA(i); else g_real_DisableVAA(i);
        c[i] = *g;
    }
    r_BindBuffer(0x8892, keep);
    static int said;
    if (said < 4) {
        said++;
        fprintf(stderr, "  [glfb] default VAO replayed on this context: %d attribute(s) "
                        "the guest set on another (render-thread handoff)\n", n);
    }
}

// Re-express the bound VAO's ES 3.1 attribute-binding state as ES 3.0 attribute
// pointers, right before a draw. No-op for a VAO that never used the format API.
static void klfb_ssbo_apply(void);                 // read-only storage buffers, below
static void klfb_apply_vertex_bindings(void) {
    klfb_ssbo_apply();
    klfb_vao0_sync();
    klfb_vao0_mv_divisors();
    klfb_vao_state *v = g_cur_vao;
    if (!v || !v->used) return;
    static void (*r_BindBuffer)(uint32_t, uint32_t);
    static void (*r_AttribPointer)(uint32_t, int32_t, uint32_t, uint8_t, int32_t, const void *);
    static void (*r_AttribIPointer)(uint32_t, int32_t, uint32_t, int32_t, const void *);
    static void (*r_AttribDivisor)(uint32_t, uint32_t);
    static void (*r_GetIntegerv_vtx)(uint32_t, int32_t *);
    if (!r_BindBuffer)     r_BindBuffer     = asym("glBindBuffer");
    if (!r_AttribPointer)  r_AttribPointer  = asym("glVertexAttribPointer");
    if (!r_AttribIPointer) r_AttribIPointer = asym("glVertexAttribIPointer");
    if (!r_AttribDivisor)  r_AttribDivisor  = asym("glVertexAttribDivisor");
    if (!r_GetIntegerv_vtx) r_GetIntegerv_vtx = asym("glGetIntegerv");
    if (!r_BindBuffer || !r_AttribPointer) return;
    int32_t save_ab = 0;
    if (r_GetIntegerv_vtx) r_GetIntegerv_vtx(0x8894 /* ARRAY_BUFFER_BINDING */, &save_ab);
    uint32_t dfb = g_draw_fb;
    unsigned views = dfb < KLFB_RESYNC_FBOS ? g_fbo_multiview[dfb] : 0;
    for (unsigned i = 0; i < KLFB_VTX_ATTRS; i++) {
        if (!v->a[i].has) continue;
        unsigned bi = v->a[i].binding;
        if (bi >= KLFB_VTX_BINDS) continue;
        r_BindBuffer(0x8892 /* ARRAY_BUFFER */, v->b[bi].buffer);
        const void *off = (const void *)(uintptr_t)(v->b[bi].offset + v->a[i].reloff);
        if (v->a[i].integer && r_AttribIPointer)
            r_AttribIPointer(i, v->a[i].size, v->a[i].type, v->b[bi].stride, off);
        else
            r_AttribPointer(i, v->a[i].size, v->a[i].type, v->a[i].normalized,
                            v->b[bi].stride, off);
        // Stride 0 does not mean the same thing on the two sides of this
        // translation. To glBindVertexBuffer (ES 3.1) it is a ZERO stride: every
        // vertex reads the one element — Unreal's stand-in stream for data a mesh
        // does not have. To glVertexAttribPointer (ES 3.0) it means "tightly
        // packed", so each vertex read further on, past the end of a one-element
        // buffer. ES 3.0 spells "the same element for every vertex" as a divisor
        // no draw reaches. KL_GLFB_ZERO_STRIDE=0 restores the plain pass-through.
        uint32_t div = v->b[bi].divisor;
        // A multiview framebuffer is drawn by instancing: ANGLE issues every
        // draw views-times-instances and the vertex stage splits the instance
        // id back into (view, instance). Its Metal backend does not do the same
        // for the STEP RATE of a per-instance attribute, so with a divisor of 1
        // instance k of the doubled draw read element k — the left eye got the
        // even instances, the right eye the odd ones, and the second half read
        // past the end of the data (Maestro's instanced chairs: a different
        // place in each eye, and shapes that came and went). One element per
        // views instances puts it back.
        if (div && views > 1) {
            div *= views;
            static int saidmv;
            if (!saidmv++)
                fprintf(stderr, "  [glfb] instanced draw into a %u-view framebuffer: "
                                "per-instance attributes stepped every %u instances "
                                "(attribute %u, divisor %u)\n",
                        views, div, i, v->b[bi].divisor);
        }
        static int zs = -1;
        if (zs < 0) zs = kl_env_on("KL_GLFB_ZERO_STRIDE", 1);
        if (zs && v->b[bi].stride == 0 && v->b[bi].divisor == 0) {
            div = 0x7fffffffu;
            static int said_zs;
            if (said_zs < 8) { said_zs++;
                fprintf(stderr, "  [glfb] attribute %u reads a zero-stride stream "
                                "(binding %u, buffer %u) — held constant with a "
                                "divisor\n", i, bi, v->b[bi].buffer); }
        }
        if (r_AttribDivisor) r_AttribDivisor(i, div);
    }
    if (r_GetIntegerv_vtx) r_BindBuffer(0x8892, (uint32_t)save_ab);
    static int said;
    if (said < 4) { said++;
        fprintf(stderr, "  [glfb] translated ES 3.1 vertex-attrib bindings to ES 3.0 "
                        "pointers for VAO %u\n", v->name); }
    // KL_GLFB_VTX_LOG=<n>: the layout handed to ANGLE for the first n draws that
    // go through this translation — what each attribute reads, and from where.
    static int vlog = -1, vlogged;
    if (vlog < 0) vlog = kl_env_int("KL_GLFB_VTX_LOG", 0);
    if (vlogged < vlog) {
        vlogged++;
        fprintf(stderr, "  [glfb] vtx layout #%d (VAO %u):", vlogged, v->name);
        for (unsigned i = 0; i < KLFB_VTX_ATTRS; i++) {
            if (!v->a[i].has) continue;
            unsigned bi = v->a[i].binding;
            if (bi >= KLFB_VTX_BINDS) continue;
            fprintf(stderr, " a%u[%d x 0x%x%s%s +%u b%u buf %u off %lld stride %d div %u]",
                    i, v->a[i].size, v->a[i].type, v->a[i].integer ? " int" : "",
                    v->a[i].normalized ? " norm" : "", v->a[i].reloff, bi,
                    v->b[bi].buffer, (long long)v->b[bi].offset, v->b[bi].stride,
                    v->b[bi].divisor);
        }
        fprintf(stderr, "\n");
    }
}

static void klfb_DrawElements(uint32_t mode, int32_t count, uint32_t type,
                              const void *indices) {
    KLFB_VFX_SKIP();
    // A pending capture first: it runs on THIS thread, which is the one that
    // draws, so it reads the pbuffer the frame actually landed in — not the
    // swap thread's, which is empty by construction.
    klfb_apply_vertex_bindings();
    klfb_service_capture();
    klfb_note_draw();
    klfb_errprobe0("glDrawElements(before)");
    if (g_real_DrawElements) g_real_DrawElements(mode, count, type, indices);
    klfb_draw_probe(count);
    char d[64];
    snprintf(d, sizeof d, " mode=0x%x count=%d type=0x%x", mode, count, type);
    klfb_errprobe("glDrawElements(after)", d);
}
static void klfb_DrawArrays(uint32_t mode, int32_t first, int32_t count) {
    KLFB_VFX_SKIP();
    klfb_apply_vertex_bindings();
    klfb_service_capture();
    klfb_note_draw();
    klfb_errprobe0("glDrawArrays(before)");
    if (g_real_DrawArrays) g_real_DrawArrays(mode, first, count);
    klfb_draw_probe(count);
    klfb_errprobe0("glDrawArrays(after)");
}
// Source's togl draws its scene through glDrawRangeElements — the plain two never
// fire for portal, so it was neither tracked (stage attribution) nor error-probed,
// and the INVALID_OPERATION its draws raise only surfaced later as a stale pre=
// on the eye blit. Wrap it like the others so a draw that fails names itself.
static void (*g_real_DrawRangeElements)(uint32_t, uint32_t, uint32_t, int32_t,
                                        uint32_t, const void *);
static void klfb_DrawRangeElements(uint32_t mode, uint32_t start, uint32_t end,
                                   int32_t count, uint32_t type, const void *indices) {
    KLFB_VFX_SKIP();
    klfb_apply_vertex_bindings();
    klfb_service_capture();
    klfb_note_draw();
    klfb_errprobe0("glDrawRangeElements(before)");
    if (g_real_DrawRangeElements)
        g_real_DrawRangeElements(mode, start, end, count, type, indices);
    klfb_draw_probe(count);
    char d[64];
    snprintf(d, sizeof d, " mode=0x%x count=%d type=0x%x", mode, count, type);
    klfb_errprobe("glDrawRangeElements(after)", d);
}
// The instanced/basevertex variants: Unity's actual scene geometry goes
// through these, not the plain two — the first census wrapped only those and
// was blind to the scene entirely.
static void klfb_DrawElementsInstanced(uint32_t mode, int32_t count, uint32_t type,
                                       const void *indices, int32_t instances) {
    KLFB_VFX_SKIP();
    klfb_apply_vertex_bindings();
    klfb_note_draw();
    klfb_errprobe0("glDrawElementsInstanced(before)");
    if (g_real_DrawElementsInstanced)
        g_real_DrawElementsInstanced(mode, count, type, indices, instances);
    klfb_draw_probe(count * instances);
    char di[64];
    snprintf(di, sizeof di, " mode=0x%x count=%d inst=%d", mode, count, instances);
    klfb_errprobe("glDrawElementsInstanced(after)", di);
}
// glDrawElementsBaseVertex is core in GLES 3.2 but NOT in ES 3.0 — and ES 3.0
// is what ANGLE's Metal backend gives us, while kl_egl describes 3.2, so Unity
// calls it freely. ANGLE *resolves* the entry point and then rejects every call
// at validation with INVALID_OPERATION, so the draw silently does not happen.
// That is what ate the UI text: Unity's canvas/TMP batching draws sub-ranges of
// one shared mesh buffer, and a sub-range is exactly what basevertex expresses,
// so text draws failed while ordinary geometry (plain glDrawElements) rendered.
// Same version-gap class as the Bloom fix, one API family over.
//
// basevertex == 0 is definitionally plain glDrawElements, which ES 3.0 does
// have; otherwise prefer an EXT/OES entry point if ANGLE exposes one. Anything
// left over falls through to the core call and is named once, rather than
// silently dropping the draw the way the unconditional forward did.
static void (*g_real_DrawElementsBaseVertexEXT)(uint32_t, int32_t, uint32_t,
                                                const void *, int32_t);
static void klfb_DrawElementsBaseVertex(uint32_t mode, int32_t count, uint32_t type,
                                        const void *indices, int32_t basevertex) {
    KLFB_VFX_SKIP();
    klfb_apply_vertex_bindings();
    klfb_note_draw();
    static int resolved;
    if (!resolved) {
        resolved = 1;
        g_real_DrawElementsBaseVertexEXT = asym("glDrawElementsBaseVertexEXT");
        if (!g_real_DrawElementsBaseVertexEXT)
            g_real_DrawElementsBaseVertexEXT = asym("glDrawElementsBaseVertexOES");
        fprintf(stderr, "  [glfb] glDrawElementsBaseVertex: core call is invalid on "
                "ANGLE's ES 3.0; EXT/OES entry point %s\n",
                g_real_DrawElementsBaseVertexEXT ? "available" : "ABSENT "
                "(basevertex!=0 draws will still fail)");
    }
    if (basevertex == 0 && g_real_DrawElements) {
        g_real_DrawElements(mode, count, type, indices);
    } else if (g_real_DrawElementsBaseVertexEXT) {
        g_real_DrawElementsBaseVertexEXT(mode, count, type, indices, basevertex);
    } else {
        static int said;
        if (said++ < 5)
            fprintf(stderr, "  [glfb] glDrawElementsBaseVertex(basevertex=%d) has no "
                    "ES 3.0 route — draw dropped by ANGLE\n", basevertex);
        if (g_real_DrawElementsBaseVertex)
            g_real_DrawElementsBaseVertex(mode, count, type, indices, basevertex);
    }
    klfb_draw_probe(count);
    char db[72];
    snprintf(db, sizeof db, " mode=0x%x count=%d basevertex=%d", mode, count, basevertex);
    klfb_errprobe("glDrawElementsBaseVertex(after)", db);
}
// glDrawRangeElementsBaseVertex — the SAME ES 3.2-vs-ES 3.0 gap as the call above,
// one API family over, and the one that ate PORTAL. Source's togl draws its whole
// scene through it; ANGLE's Metal backend (ES 3.0) resolves the core entry point
// then rejects every call at validation with INVALID_OPERATION, silently dropping
// the draw — so ~8500 scene draws/frame vanished, the eye target stayed at its
// black clear, and the only symptom that surfaced was a sticky 0x502 on the eye
// blit (the dropped draws were never wrapped, so they named nothing). basevertex==0
// is plain glDrawRangeElements, which ES 3.0 has; otherwise route through the
// EXT/OES base-vertex entry ANGLE exposes rather than the core call it rejects.
static void (*g_real_DrawRangeElementsBaseVertex)(uint32_t, uint32_t, uint32_t,
                                                  int32_t, uint32_t, const void *, int32_t);
static void (*g_real_DrawRangeElementsBaseVertexEXT)(uint32_t, uint32_t, uint32_t,
                                                     int32_t, uint32_t, const void *, int32_t);
static void klfb_DrawRangeElementsBaseVertex(uint32_t mode, uint32_t start, uint32_t end,
                                             int32_t count, uint32_t type,
                                             const void *indices, int32_t basevertex) {
    klfb_apply_vertex_bindings();
    klfb_service_capture();
    klfb_note_draw();
    static int resolved;
    if (!resolved) {
        resolved = 1;
        g_real_DrawRangeElementsBaseVertexEXT = asym("glDrawRangeElementsBaseVertexEXT");
        if (!g_real_DrawRangeElementsBaseVertexEXT)
            g_real_DrawRangeElementsBaseVertexEXT = asym("glDrawRangeElementsBaseVertexOES");
        if (!g_real_DrawElementsBaseVertexEXT) {
            g_real_DrawElementsBaseVertexEXT = asym("glDrawElementsBaseVertexEXT");
            if (!g_real_DrawElementsBaseVertexEXT)
                g_real_DrawElementsBaseVertexEXT = asym("glDrawElementsBaseVertexOES");
        }
        fprintf(stderr, "  [glfb] glDrawRangeElementsBaseVertex: core call is invalid on "
                "ANGLE's ES 3.0; range-EXT/OES %s, base-EXT/OES %s\n",
                g_real_DrawRangeElementsBaseVertexEXT ? "available" : "absent",
                g_real_DrawElementsBaseVertexEXT ? "available" : "absent");
    }
    if (basevertex == 0 && g_real_DrawRangeElements) {
        g_real_DrawRangeElements(mode, start, end, count, type, indices);
    } else if (g_real_DrawRangeElementsBaseVertexEXT) {
        g_real_DrawRangeElementsBaseVertexEXT(mode, start, end, count, type, indices, basevertex);
    } else if (g_real_DrawElementsBaseVertexEXT) {
        // The range (start/end) is only an advisory hint; dropping it to reach a
        // working base-vertex path is far better than ANGLE rejecting the draw.
        g_real_DrawElementsBaseVertexEXT(mode, count, type, indices, basevertex);
    } else {
        static int said;
        if (said++ < 5)
            fprintf(stderr, "  [glfb] glDrawRangeElementsBaseVertex(basevertex=%d) has no "
                    "ES 3.0 route — draw dropped by ANGLE\n", basevertex);
        if (g_real_DrawRangeElementsBaseVertex)
            g_real_DrawRangeElementsBaseVertex(mode, start, end, count, type, indices, basevertex);
    }
    klfb_draw_probe(count);
    char db[80];
    snprintf(db, sizeof db, " mode=0x%x count=%d basevertex=%d", mode, count, basevertex);
    klfb_errprobe("glDrawRangeElementsBaseVertex(after)", db);
}
static void klfb_DrawArraysInstanced(uint32_t mode, int32_t first, int32_t count,
                                     int32_t instances) {
    KLFB_VFX_SKIP();
    klfb_apply_vertex_bindings();
    klfb_note_draw();
    klfb_errprobe0("glDrawArraysInstanced(before)");
    if (g_real_DrawArraysInstanced)
        g_real_DrawArraysInstanced(mode, first, count, instances);
    klfb_draw_probe(count * instances);
    char di[64];
    snprintf(di, sizeof di, " mode=0x%x count=%d inst=%d", mode, count, instances);
    klfb_errprobe("glDrawArraysInstanced(after)", di);
}

// The pin table, out loud. A table that never fires and one that re-points half
// the guest's uniforms are indistinguishable from the picture, and the second
// one cost Beat Saber its menu UI for nine commits.
static void klfb_report_pins(void) {
    if (!g_prog_pins_n) return;
    fprintf(stderr, "  [glfb] uniform pins: %u program(s) pinned, %u of them resolve "
                    "by name (pins not honoured there); %u remap%s fired, %u changed "
                    "a location\n",
            g_prog_pins_n, g_pin_byname_progs, g_pin_remap_hits,
            g_pin_remap_hits == 1 ? "" : "s", g_pin_remap_changed);
}

static void klfb_report_draws(void) {
    klfb_report_pins();
    if (!g_ndraw_fbs) { fprintf(stderr, "  [glfb] no draws seen at all\n"); return; }
    fprintf(stderr, "  [glfb] draws by target framebuffer:");
    for (unsigned i = 0; i < g_ndraw_fbs; i++)
        fprintf(stderr, " fb%d=%u(t%llu)", g_draw_fbs[i].fb, g_draw_fbs[i].draws,
                (unsigned long long)g_draw_fbs[i].tid);
    fprintf(stderr, "\n");
}

// ---------------------- ES 3.1 program-interface queries, on an ES 3.0 driver
//
// We describe GLES 3.2 (kl_egl.c), so Unity 2019.4's program setup enumerates
// uniforms and uniform blocks through the ES 3.1 program-interface family —
// glGetProgramInterfaceiv / glGetProgramResourceiv / glGetProgramResourceName /
// glGetProgramResourceIndex / glGetProgramResourceLocation. ANGLE's context is
// ES 3.0 and *rejects the whole family*: RecordVersionErrorES31 sets a
// validation error, the out-params stay unwritten, and in the debug build each
// call also pays a gl::Trace log line. Sampled in the act: 1925 of 3698 samples
// of a guest worker inside glGetProgramResourceiv error logging — the guest
// spinning on a garbage resource count while the main thread futex-waits on
// it. That was the "IL2CPP abort" end state the Bloom fallback walked into.
//
// Everything this family can ask about uniforms, uniform blocks and vertex
// inputs is ES 3.0-queryable (glGetActiveUniform{s,}iv, glGetActiveUniformBlock*,
// glGetActiveAttrib), so translate. The ES 3.1 GL_UNIFORM resource index IS the
// ES 3.0 active-uniform index; same for GL_UNIFORM_BLOCK. Interfaces with no
// ES 3.0 source (program outputs, SSBOs, atomics) answer empty/invalid and log
// once per shape — if Unity ever needs one, the log says which.

#define KLFB_IF_UNIFORM        0x92E1
#define KLFB_IF_UNIFORM_BLOCK  0x92E2
#define KLFB_IF_PROGRAM_INPUT  0x92E3
#define KLFB_IF_PROGRAM_OUTPUT 0x92E4
#define KLFB_IF_BUFFER_VARIABLE 0x92E5
#define KLFB_IF_SHADER_STORAGE_BLOCK 0x92E6
#define KLFB_IF_TRANSFORM_FEEDBACK_VARYING 0x92F4
#define KLFB_ACTIVE_RESOURCES  0x92F5
#define KLFB_MAX_NAME_LENGTH   0x92F6
#define KLFB_MAX_NUM_ACTIVE_VARIABLES 0x92F7
// resource props (gl31.h)
#define KLFB_P_NAME_LENGTH     0x92F9
#define KLFB_P_TYPE            0x92FA
#define KLFB_P_ARRAY_SIZE      0x92FB
#define KLFB_P_OFFSET          0x92FC
#define KLFB_P_BLOCK_INDEX     0x92FD
#define KLFB_P_ARRAY_STRIDE    0x92FE
#define KLFB_P_MATRIX_STRIDE   0x92FF
#define KLFB_P_IS_ROW_MAJOR    0x9300
#define KLFB_P_ATOMIC_COUNTER_BUFFER_INDEX 0x9301
#define KLFB_P_BUFFER_BINDING  0x9302
#define KLFB_P_BUFFER_DATA_SIZE 0x9303
#define KLFB_P_NUM_ACTIVE_VARIABLES 0x9304
#define KLFB_P_ACTIVE_VARIABLES 0x9305
#define KLFB_P_REFERENCED_BY_VERTEX   0x9306
#define KLFB_P_REFERENCED_BY_FRAGMENT 0x930A
#define KLFB_P_LOCATION        0x930E
// ES 3.0 introspection (gl3.h)
#define KLFB_ACTIVE_UNIFORMS        0x8B86
#define KLFB_ACTIVE_UNIFORM_MAX_LENGTH 0x8B87
#define KLFB_ACTIVE_ATTRIBUTES      0x8B89
#define KLFB_ACTIVE_ATTRIBUTE_MAX_LENGTH 0x8B8A
#define KLFB_ACTIVE_UNIFORM_BLOCKS  0x8A36
#define KLFB_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH 0x8A35
#define KLFB_U_TYPE            0x8A37
#define KLFB_U_SIZE            0x8A38
#define KLFB_U_NAME_LENGTH     0x8A39
#define KLFB_U_BLOCK_INDEX     0x8A3A
#define KLFB_U_OFFSET          0x8A3B
#define KLFB_U_ARRAY_STRIDE    0x8A3C
#define KLFB_U_MATRIX_STRIDE   0x8A3D
#define KLFB_U_IS_ROW_MAJOR    0x8A3E
#define KLFB_UB_BINDING        0x8A3F
#define KLFB_UB_DATA_SIZE      0x8A40
#define KLFB_UB_NAME_LENGTH    0x8A41
#define KLFB_UB_ACTIVE_UNIFORMS 0x8A42
#define KLFB_UB_ACTIVE_UNIFORM_INDICES 0x8A43
#define KLFB_UB_REF_BY_VERTEX   0x8A44
#define KLFB_UB_REF_BY_FRAGMENT 0x8A46
#define KLFB_INVALID_INDEX     0xFFFFFFFFu

static void (*r_GetProgramiv)(uint32_t, uint32_t, int32_t *);
static void (*r_GetActiveUniform)(uint32_t, uint32_t, int32_t, int32_t *,
                                  int32_t *, uint32_t *, char *);
static int32_t (*r_GetUniformLocation)(uint32_t, const char *);
static void (*r_GetUniformIndices)(uint32_t, int32_t, const char *const *,
                                   uint32_t *);
static void (*r_GetActiveUniformsiv)(uint32_t, int32_t, const uint32_t *,
                                     uint32_t, int32_t *);
static void (*r_GetActiveUniformBlockiv)(uint32_t, uint32_t, uint32_t, int32_t *);
static void (*r_GetActiveUniformBlockName)(uint32_t, uint32_t, int32_t,
                                           int32_t *, char *);
static uint32_t (*r_GetUniformBlockIndex)(uint32_t, const char *);
static void (*r_GetActiveAttrib)(uint32_t, uint32_t, int32_t, int32_t *,
                                 int32_t *, uint32_t *, char *);

static void klfb_res_resolve(void) {
    if (r_GetProgramiv) return;
    r_GetProgramiv   = (void *)asym("glGetProgramiv");
    r_GetActiveUniform = (void *)asym("glGetActiveUniform");
    r_GetUniformLocation = (void *)asym("glGetUniformLocation");
    r_GetUniformIndices  = (void *)asym("glGetUniformIndices");
    r_GetActiveUniformsiv = (void *)asym("glGetActiveUniformsiv");
    r_GetActiveUniformBlockiv = (void *)asym("glGetActiveUniformBlockiv");
    r_GetActiveUniformBlockName = (void *)asym("glGetActiveUniformBlockName");
    r_GetUniformBlockIndex = (void *)asym("glGetUniformBlockIndex");
    r_GetActiveAttrib = (void *)asym("glGetActiveAttrib");
}

// The g_thunks real-slot for the program-interface family: the table wires
// *real = fn for every entry, but these thunks self-resolve through asym
// (klfb_res_resolve), so their slot points here and is never read.
static void *g_res_sink;

// Scout data, not spam: each unhandled (interface, prop) shape logs once.
static struct { uint32_t iface, prop; } g_res_unhandled[32];
static unsigned g_res_nunhandled;

static void klfb_res_log_unhandled(const char *what, uint32_t iface, uint32_t prop) {
    for (unsigned i = 0; i < g_res_nunhandled; i++)
        if (g_res_unhandled[i].iface == iface && g_res_unhandled[i].prop == prop)
            return;
    if (g_res_nunhandled < 32) {
        g_res_unhandled[g_res_nunhandled].iface = iface;
        g_res_unhandled[g_res_nunhandled].prop  = prop;
        g_res_nunhandled++;
    }
    fprintf(stderr, "  [glfb] %s: unhandled interface 0x%x prop 0x%x "
                    "(answered empty/invalid)\n", what, iface, prop);
}

static void klfb_err_say(const char *what, uint32_t a0);   // with the thunk table

// The storage blocks of a kernel rewritten by klfb_cs_rewrite_sb. The program
// ANGLE linked is a vertex shader with none, so it answers nothing — and Unity
// then binds by its own register numbers: the kernel's sources at 0, 1, 2 and
// its destination at 0 again, over the first source, where the kernel reads
// them at 1, 2, 3 and writes at 0. Arizona Sunshine's skinning read no bone
// weights that way and every skinned vertex came out at zero (2026-10-31).
// Told the blocks as declared, Unity binds each at its own binding.
static klfb_cs_obj *klfb_cs_reflect(uint32_t program, uint32_t iface) {
    if (iface != KLFB_IF_SHADER_STORAGE_BLOCK) return NULL;
    klfb_cs_obj *pr = klfb_cs_find(g_cs_progs, program);
    if (pr && pr->sb && pr->sbn > 0) return pr;
    // ...and a graphics program whose blocks were read as textures, in the
    // same shape (a scratch object: these queries are made on one thread).
    klfb_sb_refl *pg = kl_glfb_ssbo_tex() ? klfb_sb_refl_get(g_pg_sb, &g_pg_sb_next, program, 0) : NULL;
    if (!pg || !pg->n) return NULL;
    static __thread klfb_cs_obj tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.name = program; tmp.sb = 1; tmp.sbn = pg->n;
    for (int i = 0; i < pg->n && i < KLFB_CS_BINDINGS; i++) {
        snprintf(tmp.sbb[i].name, sizeof tmp.sbb[0].name, "%s", pg->b[i].name);
        tmp.sbb[i].binding = pg->b[i].binding; tmp.sbb[i].words = pg->b[i].words;
    }
    return &tmp;
}

static void klfb_GetProgramInterfaceiv(uint32_t program, uint32_t iface,
                                       uint32_t pname, int32_t *params) {
    klfb_res_resolve();
    klfb_cs_obj *cr = klfb_cs_reflect(program, iface);
    if (cr && params) {
        int32_t v = 0;
        if (pname == KLFB_ACTIVE_RESOURCES) v = cr->sbn;
        else if (pname == KLFB_MAX_NAME_LENGTH)
            for (int i = 0; i < cr->sbn; i++) { int32_t l = (int32_t)strlen(cr->sbb[i].name) + 1; if (l > v) v = l; }
        else if (pname == KLFB_MAX_NUM_ACTIVE_VARIABLES) v = 1;
        *params = v;
        return;
    }
    // Unity's "Invalid texture unit!" lands immediately after this call in the
    // trace: its own GL-error check misattributes whatever we raise here. A
    // drain/report bracket names the query that actually sets the error.
    if (a_glGetError) while (a_glGetError()) {}
    if (!params) return;
    *params = 0;
    uint32_t src = 0;
    if (pname == KLFB_ACTIVE_RESOURCES) {
        switch (iface) {
        case KLFB_IF_UNIFORM:        src = KLFB_ACTIVE_UNIFORMS; break;
        case KLFB_IF_UNIFORM_BLOCK:  src = KLFB_ACTIVE_UNIFORM_BLOCKS; break;
        case KLFB_IF_PROGRAM_INPUT:  src = KLFB_ACTIVE_ATTRIBUTES; break;
        default:
            klfb_res_log_unhandled("glGetProgramInterfaceiv", iface, pname);
            return;
        }
    } else if (pname == KLFB_MAX_NAME_LENGTH) {
        switch (iface) {
        case KLFB_IF_UNIFORM:        src = KLFB_ACTIVE_UNIFORM_MAX_LENGTH; break;
        case KLFB_IF_UNIFORM_BLOCK:  src = KLFB_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH; break;
        case KLFB_IF_PROGRAM_INPUT:  src = KLFB_ACTIVE_ATTRIBUTE_MAX_LENGTH; break;
        default:
            klfb_res_log_unhandled("glGetProgramInterfaceiv", iface, pname);
            return;
        }
    } else if (pname == KLFB_MAX_NUM_ACTIVE_VARIABLES && iface == KLFB_IF_UNIFORM_BLOCK) {
        // No single ES 3.0 query: the max over blocks of their active-uniform
        // counts. Cold path, so just walk them.
        int32_t nb = 0, best = 0;
        if (r_GetProgramiv) r_GetProgramiv(program, KLFB_ACTIVE_UNIFORM_BLOCKS, &nb);
        for (int32_t i = 0; i < nb; i++) {
            int32_t n = 0;
            if (r_GetActiveUniformBlockiv)
                r_GetActiveUniformBlockiv(program, (uint32_t)i, KLFB_UB_ACTIVE_UNIFORMS, &n);
            if (n > best) best = n;
        }
        *params = best;
        return;
    } else {
        klfb_res_log_unhandled("glGetProgramInterfaceiv", iface, pname);
        return;
    }
    if (r_GetProgramiv) r_GetProgramiv(program, src, params);
    klfb_err_say("GetProgramInterfaceiv/GetProgramiv", src);
}

// One scalar prop of one GL_UNIFORM resource, via the ES 3.0 uniform queries.
// Returns 1 when *out was written.
static int klfb_uniform_prop(uint32_t program, uint32_t index, uint32_t prop,
                             int32_t *out) {
    uint32_t es30 = 0;
    switch (prop) {
    case KLFB_P_NAME_LENGTH:   es30 = KLFB_U_NAME_LENGTH; break;
    case KLFB_P_TYPE:          es30 = KLFB_U_TYPE; break;
    case KLFB_P_ARRAY_SIZE:    es30 = KLFB_U_SIZE; break;
    case KLFB_P_OFFSET:        es30 = KLFB_U_OFFSET; break;
    case KLFB_P_BLOCK_INDEX:   es30 = KLFB_U_BLOCK_INDEX; break;
    case KLFB_P_ARRAY_STRIDE:  es30 = KLFB_U_ARRAY_STRIDE; break;
    case KLFB_P_MATRIX_STRIDE: es30 = KLFB_U_MATRIX_STRIDE; break;
    case KLFB_P_IS_ROW_MAJOR:  es30 = KLFB_U_IS_ROW_MAJOR; break;
    case KLFB_P_REFERENCED_BY_VERTEX:
    case KLFB_P_REFERENCED_BY_FRAGMENT:
        // glGetActiveUniformsiv has no per-stage referenced flag for uniforms
        // (only for blocks). "Referenced" is the safe answer: it keeps Unity
        // from stripping a live uniform.
        *out = 1;
        return 1;
    case KLFB_P_LOCATION: {
        // By name: ES 3.0 has no location query by index.
        char name[256];
        int32_t len = 0, size = 0;
        uint32_t type = 0;
        if (!r_GetActiveUniform || !r_GetUniformLocation) return 0;
        r_GetActiveUniform(program, index, sizeof name, &len, &size, &type, name);
        name[sizeof name - 1] = 0;
        *out = r_GetUniformLocation(program, name);
        return 1;
    }
    default:
        klfb_res_log_unhandled("glGetProgramResourceiv", KLFB_IF_UNIFORM, prop);
        *out = 0;
        return 1;
    }
    if (!r_GetActiveUniformsiv) return 0;
    r_GetActiveUniformsiv(program, 1, &index, es30, out);
    return 1;
}

static void klfb_GetProgramResourceiv(uint32_t program, uint32_t iface,
                                      uint32_t index, int32_t propCount,
                                      const uint32_t *props, int32_t bufSize,
                                      int32_t *length, int32_t *params) {
    klfb_res_resolve();
    int32_t want = propCount < bufSize ? propCount : bufSize;
    if (length) *length = 0;
    if (!props || !params || want <= 0) return;
    klfb_cs_obj *cr = klfb_cs_reflect(program, iface);
    if (cr) {
        if ((int)index >= cr->sbn) return;
        for (int32_t i = 0; i < want; i++) {
            int32_t v = 0;
            switch (props[i]) {
            case KLFB_P_BUFFER_BINDING:      v = cr->sbb[index].binding; break;
            case KLFB_P_NAME_LENGTH:         v = (int32_t)strlen(cr->sbb[index].name) + 1; break;
            case KLFB_P_BUFFER_DATA_SIZE:    v = cr->sbb[index].words * 4; break;
            case KLFB_P_NUM_ACTIVE_VARIABLES: v = 1; break;
            case 0x930B /* REFERENCED_BY_COMPUTE_SHADER */:
            case KLFB_P_REFERENCED_BY_VERTEX:
            case KLFB_P_REFERENCED_BY_FRAGMENT: v = 1; break;
            default:                         v = 0; break;
            }
            params[i] = v;
        }
        if (length) *length = want;
        return;
    }
    int32_t written = 0;
    for (int32_t i = 0; i < want; i++) {
        int32_t v = 0;
        int ok = 0;
        if (iface == KLFB_IF_UNIFORM) {
            ok = klfb_uniform_prop(program, index, props[i], &v);
        } else if (iface == KLFB_IF_UNIFORM_BLOCK) {
            uint32_t es30 = 0;
            switch (props[i]) {
            case KLFB_P_NAME_LENGTH:   es30 = KLFB_UB_NAME_LENGTH; break;
            case KLFB_P_BUFFER_DATA_SIZE: es30 = KLFB_UB_DATA_SIZE; break;
            case KLFB_P_NUM_ACTIVE_VARIABLES: es30 = KLFB_UB_ACTIVE_UNIFORMS; break;
            case KLFB_P_BUFFER_BINDING: es30 = KLFB_UB_BINDING; break;
            case KLFB_P_REFERENCED_BY_VERTEX:   es30 = KLFB_UB_REF_BY_VERTEX; break;
            case KLFB_P_REFERENCED_BY_FRAGMENT: es30 = KLFB_UB_REF_BY_FRAGMENT; break;
            case KLFB_P_ACTIVE_VARIABLES: {
                // Multi-value prop: fills the rest of params with the member
                // uniform indices, spec-style.
                int32_t n = 0;
                if (r_GetActiveUniformBlockiv)
                    r_GetActiveUniformBlockiv(program, index,
                                              KLFB_UB_ACTIVE_UNIFORMS, &n);
                int32_t room = want - i;
                if (n > room) n = room;
                if (n > 0 && r_GetActiveUniformBlockiv)
                    r_GetActiveUniformBlockiv(program, index,
                                              KLFB_UB_ACTIVE_UNIFORM_INDICES,
                                              params + i);
                if (length) *length = i + n;
                return;
            }
            default:
                klfb_res_log_unhandled("glGetProgramResourceiv", iface, props[i]);
                v = 0; ok = 1;
                break;
            }
            if (es30) {
                if (r_GetActiveUniformBlockiv)
                    r_GetActiveUniformBlockiv(program, index, es30, &v);
                ok = 1;
            }
        } else if (iface == KLFB_IF_PROGRAM_INPUT) {
            // The ES 3.1 program-input resource index is the attribute index.
            switch (props[i]) {
            case KLFB_P_NAME_LENGTH:
            case KLFB_P_TYPE:
            case KLFB_P_ARRAY_SIZE:
            case KLFB_P_LOCATION: {
                char name[256];
                int32_t len = 0, size = 0;
                uint32_t type = 0;
                if (!r_GetActiveAttrib) break;
                r_GetActiveAttrib(program, index, sizeof name, &len, &size, &type, name);
                if (props[i] == KLFB_P_NAME_LENGTH)   v = len + 1;
                if (props[i] == KLFB_P_TYPE)          v = (int32_t)type;
                if (props[i] == KLFB_P_ARRAY_SIZE)    v = size;
                if (props[i] == KLFB_P_LOCATION)      v = (int32_t)index; // see ResourceLocation
                ok = 1;
                break;
            }
            case KLFB_P_REFERENCED_BY_VERTEX:   v = 1; ok = 1; break;
            case KLFB_P_REFERENCED_BY_FRAGMENT: v = 0; ok = 1; break;
            default:
                klfb_res_log_unhandled("glGetProgramResourceiv", iface, props[i]);
                v = 0; ok = 1;
                break;
            }
        } else {
            klfb_res_log_unhandled("glGetProgramResourceiv", iface, props[i]);
            ok = 1;
        }
        if (ok) { params[i] = v; written = i + 1; }
    }
    if (length) *length = written;
    klfb_err_say("GetProgramResourceiv", iface);
}

static void klfb_GetProgramResourceName(uint32_t program, uint32_t iface,
                                        uint32_t index, int32_t bufSize,
                                        int32_t *length, char *name) {
    klfb_res_resolve();
    if (length) *length = 0;
    if (!name || bufSize <= 0) return;
    name[0] = 0;
    klfb_cs_obj *cr = klfb_cs_reflect(program, iface);
    if (cr) {
        if ((int)index < cr->sbn) {
            int n = snprintf(name, (size_t)bufSize, "%s", cr->sbb[index].name);
            if (length) *length = n < bufSize ? n : bufSize - 1;
        }
        return;
    }
    if (iface == KLFB_IF_UNIFORM) {
        int32_t len = 0, size = 0;
        uint32_t type = 0;
        if (r_GetActiveUniform)
            r_GetActiveUniform(program, index, bufSize, &len, &size, &type, name);
        if (length) *length = len;
    } else if (iface == KLFB_IF_UNIFORM_BLOCK) {
        int32_t len = 0;
        if (r_GetActiveUniformBlockName)
            r_GetActiveUniformBlockName(program, index, bufSize, &len, name);
        if (length) *length = len;
    } else if (iface == KLFB_IF_PROGRAM_INPUT) {
        int32_t len = 0, size = 0;
        uint32_t type = 0;
        if (r_GetActiveAttrib)
            r_GetActiveAttrib(program, index, bufSize, &len, &size, &type, name);
        if (length) *length = len;
    } else {
        klfb_res_log_unhandled("glGetProgramResourceName", iface, 0);
    }
}

static uint32_t klfb_GetProgramResourceIndex(uint32_t program, uint32_t iface,
                                             const char *name) {
    klfb_res_resolve();
    if (!name) return KLFB_INVALID_INDEX;
    klfb_cs_obj *cr = klfb_cs_reflect(program, iface);
    if (cr) {
        for (int i = 0; i < cr->sbn; i++) if (strcmp(cr->sbb[i].name, name) == 0) return (uint32_t)i;
        return KLFB_INVALID_INDEX;
    }
    if (iface == KLFB_IF_UNIFORM) {
        uint32_t idx = KLFB_INVALID_INDEX;
        if (r_GetUniformIndices) r_GetUniformIndices(program, 1, &name, &idx);
        return idx;
    }
    if (iface == KLFB_IF_UNIFORM_BLOCK)
        return r_GetUniformBlockIndex ? r_GetUniformBlockIndex(program, name)
                                      : KLFB_INVALID_INDEX;
    klfb_res_log_unhandled("glGetProgramResourceIndex", iface, 0);
    return KLFB_INVALID_INDEX;
}

static int32_t klfb_GetUniformLocation(uint32_t prog, const char *name);
static int32_t klfb_GetProgramResourceLocation(uint32_t program, uint32_t iface,
                                               const char *name) {
    klfb_res_resolve();
    // THE door Unity 2019.4 uses — it never calls glGetUniformLocation. Routed
    // through the same thunk rather than to the driver, so the two entry points
    // cannot answer differently about the same program; a raw driver location
    // here and a pin remap on the glUniform* side is what broke Beat Saber's
    // menu UI (see klfb_pins_byname).
    if (name && iface == KLFB_IF_UNIFORM && r_GetUniformLocation)
        return klfb_GetUniformLocation(program, name);
    if (name && iface == KLFB_IF_PROGRAM_INPUT) {
        // Attrib "location" is queried by name too.
        static int32_t (*r_GetAttribLocation)(uint32_t, const char *);
        if (!r_GetAttribLocation) r_GetAttribLocation = (void *)asym("glGetAttribLocation");
        if (r_GetAttribLocation) return r_GetAttribLocation(program, name);
    }
    klfb_res_log_unhandled("glGetProgramResourceLocation", iface, 0);
    return -1;
}

static int32_t klfb_GetProgramResourceLocationIndex(uint32_t program,
                                                    uint32_t iface,
                                                    const char *name) {
    (void)program; (void)name;
    // Fragment-output location indices exist for dual-source blending, which
    // this title does not use.
    klfb_res_log_unhandled("glGetProgramResourceLocationIndex", iface, 0);
    return -1;
}

// The capture has to happen on the thread that owns the context, and the guest's
// eglSwapBuffers does not arrive there — measured: the GL thread owns the context,
// the swap comes in on a different one, and EGL refuses to migrate (EGL_BAD_ACCESS).
//
// So a swap only *requests* a capture, and the request is serviced from a call the
// GL thread makes itself. glFlush/glFinish are the natural place: the guest issues
// them at frame boundaries, and by definition they run where the drawing did.
static int  g_capture_pending;
static char g_capture_dir[512];
static unsigned glfb_capture_now(const char *dir);

// The frame-out seam (kl_glfb.h). Set once by the frontend before the guest
// starts, read on the GL thread at capture time; plain stores are fine for the
// same reason as the pose seam — registration happens long before the first
// swap, so there is no race to arbitrate.
static kl_glfb_frame_sink g_frame_sink;
static void              *g_frame_sink_ctx;
static unsigned long      g_last_frame_lit;

void kl_glfb_set_frame_sink(kl_glfb_frame_sink fn, void *ctx) {
    g_frame_sink = fn;
    g_frame_sink_ctx = ctx;
}
int kl_glfb_has_frame_sink(void) { return g_frame_sink != NULL; }
unsigned long kl_glfb_last_frame_lit(void) { return g_last_frame_lit; }

static void (*g_real_Flush)(void);
static void (*g_real_Finish)(void);

// ---- the GPU frame seam (kl_glfb.h) -----------------------------------------
//
// EGL_SYNC_METAL_SHARED_EVENT_ANGLE, not eglCreateSyncKHR's EGLint attribute
// list: the shared event arrives as a 64-bit pointer and an EGLint cannot hold
// one. eglCreateSync (EGL 1.5) takes EGLAttrib, which can. Getting this wrong
// truncates the pointer and ANGLE creates its OWN event instead — a sync that
// works perfectly and orders nothing we can wait on.
#define EGL_SYNC_METAL_SHARED_EVENT_ANGLE_              0x34D8
#define EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE_       0x34D9
#define EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_    0x34DA
#define EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_    0x34DB

static void *(*a_eglCreateSync)(void *, uint32_t, const intptr_t *);
static unsigned (*a_eglDestroySync)(void *, void *);
// Resolved from ANGLE directly rather than reusing g_real_Flush: that one is
// only filled in once the GUEST has resolved glFlush through the trampoline
// table, and a fence created but never committed leaves the compositor waiting
// on a value the GPU never reaches.
static void (*a_glFlush_fence)(void);
static void     *g_gpu_fence;        // an id<MTLSharedEvent>, owned by the caller
static void     *g_gpu_fence_sync;   // last frame's EGLSync, destroyed one frame late
static uint64_t  g_gpu_fence_value;

void kl_glfb_set_gpu_fence(void *mtl_shared_event) {
    g_gpu_fence = mtl_shared_event;
}
int kl_glfb_has_gpu_fence(void) { return g_gpu_fence != NULL; }
uint64_t kl_glfb_gpu_fence_value(void) {
    return __atomic_load_n(&g_gpu_fence_value, __ATOMIC_ACQUIRE);
}
static unsigned klfb_gpu_frame_now(void);
// A guest frame that reaches the compositor by COPY (kl_glfb_mirror_eye_layer)
// and never swaps: the copy is the last thing in ANGLE's command stream for the
// frame, the frame's record is published straight after, and without a fence
// the compositor samples the destination before the copy has run — the stage's
// previous picture, three frames old, under this frame's pose. With a turning
// head that is the picture pulled back against the turn on every frame (Maestro:
// "as if two trackers ran against each other"). The caller invokes this after
// its copies, on the thread that issued them.
static int g_frame_fenced;
void kl_glfb_frame_fence(void) {
    if (!g_ready || !g_gpu_fence) return;
    klfb_gpu_frame_now();
    __atomic_store_n(&g_frame_fenced, 1, __ATOMIC_RELEASE);
}
int kl_glfb_frame_fenced(void) { return __atomic_load_n(&g_frame_fenced, __ATOMIC_ACQUIRE); }

// Runs on the context-owning thread, in place of glfb_capture_now, once per
// swap. Encodes "the guest's frame is done" into ANGLE's command stream and
// publishes the value that will carry it.
static unsigned klfb_gpu_frame_now(void) {
    if (!g_ready || !g_gpu_fence) return g_presented;
    if (!a_eglCreateSync) {
        a_eglCreateSync  = asym("eglCreateSync");
        a_eglDestroySync = asym("eglDestroySync");
        a_glFlush_fence  = asym("glFlush");
        // eglCreateSync, not eglCreateSyncKHR — the KHR form's attribute list is
        // EGLint, and a 64-bit MTLSharedEvent pointer does not fit in one.
        if (!a_eglCreateSync || !a_glFlush_fence) {
            fprintf(stderr, "  [glfb] this ANGLE has no %s — the GPU compositor "
                            "cannot order against the guest's frame\n",
                    a_eglCreateSync ? "glFlush" : "eglCreateSync");
            g_gpu_fence = NULL;
            return g_presented;
        }
    }
    uint64_t v = g_gpu_fence_value + 1;
    const intptr_t attrs[] = {
        EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE_,    (intptr_t)g_gpu_fence,
        EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_, (intptr_t)(uint32_t)v,
        EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_, (intptr_t)(uint32_t)(v >> 32),
        EGL_NONE_,
    };
    void *sync = a_eglCreateSync(g_dpy, EGL_SYNC_METAL_SHARED_EVENT_ANGLE_, attrs);
    if (!sync) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] eglCreateSync(METAL_SHARED_EVENT) failed — "
                            "the compositor will show whatever is in the eye "
                            "texture, unordered\n");
        return g_presented;
    }
    // Commit, so the signal actually reaches the GPU: eglCreateSync only
    // *queues* the event onto ANGLE's current command buffer. No wait here —
    // a glFinish would be the stall this whole path exists to remove.
    a_glFlush_fence();
    __atomic_store_n(&g_gpu_fence_value, v, __ATOMIC_RELEASE);
    // One frame late, so the object outlives the encode it belongs to. Nothing
    // documents that destroying it immediately is safe, and the failure mode
    // would be a signal that never arrives — i.e. a compositor that hangs.
    if (g_gpu_fence_sync && a_eglDestroySync)
        a_eglDestroySync(g_dpy, g_gpu_fence_sync);
    g_gpu_fence_sync = sync;
    return ++g_presented;
}

static void klfb_service_capture(void) {
    if (!g_capture_pending) return;
    g_capture_pending = 0;
    if (g_gpu_fence) { klfb_gpu_frame_now(); return; }
    glfb_capture_now(g_capture_dir);
}
static void klfb_Flush(void)  { if (g_real_Flush)  g_real_Flush();  klfb_service_capture(); }
static void klfb_Finish(void) { if (g_real_Finish) g_real_Finish(); klfb_service_capture(); }

static void klfb_GetIntegerv(uint32_t, int32_t *);
static void klfb_GetFloatv(uint32_t, float *);
static void klfb_GetBooleanv(uint32_t, uint8_t *);
static void klfb_GetInteger64v(uint32_t, int64_t *);
static void klfb_GetIntegeri_v(uint32_t, uint32_t, int32_t *);
static void klfb_GetInternalformativ(uint32_t, uint32_t, uint32_t, int32_t, int32_t *);
static void (*g_real_GetIntegerv)(uint32_t, int32_t *);
static void (*g_real_GetFloatv)(uint32_t, float *);
static void (*g_real_GetBooleanv)(uint32_t, uint8_t *);
static void (*g_real_GetInteger64v)(uint32_t, int64_t *);
static void (*g_real_GetIntegeri_v)(uint32_t, uint32_t, int32_t *);
static void (*g_real_GetInternalformativ)(uint32_t, uint32_t, uint32_t, int32_t, int32_t *);

// Unity spams "OpenGL Error: Invalid texture unit!" ~6x/frame from the very
// first frames. glActiveTexture is the only unit-selecting call; bracket it
// and print the first failures with the unit requested — we describe 32
// combined units, so a failing unit <= 31 is the driver disagreeing with the
// description, and a unit > 31 is the guest over-reading it.
static void (*g_real_ActiveTexture)(uint32_t);
static void klfb_ActiveTexture(uint32_t unit) {
    // Drain first: the guest leaves errors unread, so an undrained bracket
    // reports a leftover as glActiveTexture's own. Errors after this point are
    // provably this call's.
    if (a_glGetError) while (a_glGetError()) {}
    if (g_real_ActiveTexture) g_real_ActiveTexture(unit);
    g_tb_active_unit = (int)(unit - 0x84C0);   // track for texel-buffer sync
    // Which units does the guest actually select? Unity's "Invalid texture
    // unit" check rejects above its own cap — if that preempts the bind, no
    // high unit ever reaches GL, and measuring the request stream proves it.
    {
        static int logu = -1;
        static uint8_t seen[64];
        if (logu < 0) logu = kl_env_on("KL_GLFB_LOG_UNITS", 0);
        int u = (int)(unit - 0x84C0);
        if (logu && u >= 0 && u < 64 && !seen[u]) {
            seen[u] = 1;
            fprintf(stderr, "  [glfb] glActiveTexture: first use of unit %d\n", u);
        }
    }
    static int said;
    if (said < 20 && a_glGetError) {
        uint32_t e = a_glGetError();
        if (e) {
            said++;
            fprintf(stderr, "  [glfb] glActiveTexture(0x%x) (unit %d) -> GL error 0x%x\n",
                    unit, (int)(unit - 0x84C0), e);
        }
    }
}

// The 0x502 the errprobe meets before program-7 draws is generated by a call
// the probe family doesn't wrap. Bracket the per-draw state setters the same
// drain-and-report way; the one that prints is the generator.
static void klfb_err_say(const char *what, uint32_t a0) {
    static int said;
    if (said < 30 && a_glGetError) {
        uint32_t e = a_glGetError();
        if (e) {
            said++;
            int32_t prog = -1, fb = -1;
            if (a_glGetIntegerv) {
                a_glGetIntegerv(0x8B8D, &prog);
                a_glGetIntegerv(0x8CA6, &fb);
            }
            fprintf(stderr, "  [glfb] ERRSRC %s(0x%x) -> GL error 0x%x "
                            "(program=%d fb=%d)\n", what, a0, e, prog, fb);
        }
    }
}
// Occlusion query results, counted — and, with KL_GL_QUERY_VISIBLE=1, all
// answered "something passed". An engine that culls by query hides whatever a
// query says drew nothing, so a query that wrongly answers 0 is an object that
// is simply never drawn again; this says how many answer 0, and the knob says
// what the picture is without them.
static void (*g_real_GetQueryObjectuiv)(uint32_t, uint32_t, uint32_t *);
static void klfb_GetQueryObjectuiv(uint32_t id, uint32_t pname, uint32_t *params) {
    if (g_real_GetQueryObjectuiv) g_real_GetQueryObjectuiv(id, pname, params);
    if (pname != 0x8866 /* GL_QUERY_RESULT */ || !params) return;
    static unsigned n, zero;
    static int force = -1;
    if (force < 0) force = kl_env_on("KL_GL_QUERY_VISIBLE", 0);
    n++; if (!*params) zero++;
    if (n == 200 || n == 5000 || n == 50000)
        fprintf(stderr, "  [glfb] occlusion queries: %u results read, %u of them 0%s\n",
                n, zero, force ? " (all answered 1: KL_GL_QUERY_VISIBLE)" : "");
    if (force) *params = 1;
}
static void (*g_real_UseProgram)(uint32_t);
static void klfb_UseProgram(uint32_t p) {
    if (a_glGetError) while (a_glGetError()) {}
    if (g_real_UseProgram) g_real_UseProgram(p);
    g_cur_prog = p;                       // what the uniform remap is scoped to
    klfb_err_say("glUseProgram", p);
}
// The other half of the external-image retarget in klfb_rewrite_glsl. The
// shader now declares sampler2D, so every target that names an external texture
// has to become GL_TEXTURE_2D or the sampler reads nothing — and ANGLE would
// reject the target anyway, since it does not advertise the extension that
// defines it.
#define GL_TEXTURE_EXTERNAL_OES_ 0x8D65
#define GL_TEXTURE_2D_           0x0DE1
#define GL_TEXTURE_BUFFER_       0x8C2A
static uint32_t klfb_detarget(uint32_t t) {
    if (t == GL_TEXTURE_EXTERNAL_OES_ || t == GL_TEXTURE_BUFFER_) return GL_TEXTURE_2D_;
    return t;
}

// ...and the third piece of it. An image of OURS is not an ANGLE EGLImage at
// all — it is a pbuffer over the decoded frame's IOSurface — so the call becomes
// eglBindTexImage. Anything else is forwarded, detargeted, so a guest that ever
// does have a real external image is unaffected by our presence.
static void (*g_real_EGLImageTargetTexture2DOES)(uint32_t, void *);
static void klfb_EGLImageTargetTexture2DOES(uint32_t target, void *image) {
    if (kl_glfb_is_image(image)) { kl_glfb_image_bind(image); return; }
    if (g_real_EGLImageTargetTexture2DOES)
        g_real_EGLImageTargetTexture2DOES(klfb_detarget(target), image);
}

static void (*g_real_BindTexture)(uint32_t, uint32_t);
static void (*g_real_BindSampler)(uint32_t, uint32_t);
static void klfb_BindTexture(uint32_t t, uint32_t n) {
    if (a_glGetError) while (a_glGetError()) {}
    if (g_real_BindTexture) g_real_BindTexture(klfb_detarget(t), n);
    // A buffer texture has no sampler state: whatever sampler object sits on
    // its unit is ignored. The 2D texture standing in for it is NOT exempt — a
    // sampler left on that unit by the unit's last user (trilinear, say) applies
    // to it, and a float or integer texture under a linear filter is INCOMPLETE:
    // it samples as zero, texelFetch included. Zero bone matrices are a skinned
    // mesh with every vertex at one point (Maestro's musicians and its book,
    // with their bone buffers correctly filled). So while a stand-in is on a
    // unit, no sampler object is: the guest's is taken off, remembered, and put
    // back when the unit goes to an ordinary texture again.
    {
        int u = g_tb_active_unit;
        if (u >= 0 && u < KLFB_TB_UNITS && n) {
            if (!g_real_BindSampler) g_real_BindSampler = asym("glBindSampler");
            if (t == GL_TEXTURE_BUFFER_) {
                if (!g_tb_unit[u]) {
                    g_tb_unit[u] = 1;
                    if (g_tb_sampler[u] && g_real_BindSampler) {
                        g_real_BindSampler((uint32_t)u, 0);
                        static int said;
                        if (!said++)
                            fprintf(stderr, "  [glfb] texel-buffer stand-in on unit %d: sampler "
                                            "object %u taken off the unit while it is there\n",
                                    u, g_tb_sampler[u]);
                    }
                }
            } else if (g_tb_unit[u]) {
                g_tb_unit[u] = 0;
                if (g_tb_sampler[u] && g_real_BindSampler)
                    g_real_BindSampler((uint32_t)u, g_tb_sampler[u]);
            }
        }
    }
    // Track the 2D binding per unit — a texel buffer arrives as glBindTexture(
    // GL_TEXTURE_BUFFER, tex) which detargets to 2D, so glTexBuffer can name the
    // texture it must keep in sync (klfb_texbuffer_upload / klfb_tb_refresh).
    if (klfb_detarget(t) == GL_TEXTURE_2D_) {
        int u = g_tb_active_unit;
        if (u >= 0 && u < KLFB_TB_UNITS) g_tb_bound2d[u] = n;
    }
    klfb_err_say("glBindTexture", n);
}

// glTexBuffer(target, internalformat, buffer): on real ES 3.1 this points the
// texture bound to GL_TEXTURE_BUFFER at a buffer object's data store. There is
// no such target on ES 3.0/Metal, and the binding was redirected to a 2D
// texture (klfb_detarget), so here we copy the buffer's bytes INTO that 2D
// texture, laid out KLFB_TB_W texels wide to match the shader rewrite
// (klfb_rewrite_texel_buffers). This is a snapshot at glTexBuffer time; a guest
// that refills the buffer per frame and re-binds via glTexBuffer (UE4's SRV
// path does) gets a fresh upload each time. Formats seen from UE4 skinning are
// float4 / uint4; the switch covers the common set and defaults to RGBA32F.
static void klfb_texbuffer_upload(uint32_t internalformat, uint32_t buffer,
                                  intptr_t off, intptr_t len) {
    // ANGLE's own entry points, resolved HERE: the slots are otherwise only
    // filled when the GUEST resolves those names, and Unreal never asks for
    // glTexImage2D (it allocates with glTexStorage2D) — so for it this whole
    // function returned at its first line, on every call, and no texel-buffer
    // texture was ever given a single byte.
    if (!g_real_TexImage2D)    g_real_TexImage2D    = asym("glTexImage2D");
    if (!g_real_TexParameteri) g_real_TexParameteri = asym("glTexParameteri");
    if (!buffer || !g_real_TexImage2D) return;
    static void   (*r_BindBuffer)(uint32_t, uint32_t);
    static void   (*r_GetBufferParameteriv)(uint32_t, uint32_t, int32_t *);
    static void  *(*r_MapBufferRange)(uint32_t, intptr_t, intptr_t, uint32_t);
    static uint8_t(*r_UnmapBuffer)(uint32_t);
    if (!r_BindBuffer)           r_BindBuffer           = asym("glBindBuffer");
    if (!r_GetBufferParameteriv) r_GetBufferParameteriv = asym("glGetBufferParameteriv");
    if (!r_MapBufferRange)       r_MapBufferRange       = asym("glMapBufferRange");
    if (!r_UnmapBuffer)          r_UnmapBuffer          = asym("glUnmapBuffer");
    if (!r_BindBuffer || !r_GetBufferParameteriv || !r_MapBufferRange || !r_UnmapBuffer)
        return;

    uint32_t fmt, type; int texel;
    switch (internalformat) {
        case 0x8814: fmt = 0x1908; type = 0x1406; texel = 16; break;  // RGBA32F
        case 0x881A: fmt = 0x1908; type = 0x140B; texel = 8;  break;  // RGBA16F
        case 0x8D70: fmt = 0x8D99; type = 0x1405; texel = 16; break;  // RGBA32UI
        case 0x8D82: fmt = 0x8D99; type = 0x1404; texel = 16; break;  // RGBA32I
        case 0x8230: fmt = 0x8227; type = 0x1406; texel = 8;  break;  // RG32F
        case 0x822E: fmt = 0x1903; type = 0x1406; texel = 4;  break;  // R32F
        case 0x8236: fmt = 0x8D94; type = 0x1405; texel = 4;  break;  // R32UI
        case 0x8235: fmt = 0x8D94; type = 0x1404; texel = 4;  break;  // R32I
        case 0x8058: fmt = 0x1908; type = 0x1401; texel = 4;  break;  // RGBA8
        case 0x8D7C: fmt = 0x8D99; type = 0x1401; texel = 4;  break;  // RGBA8UI
        // The narrow ones Unreal points at index and weight data. Without a row
        // here a format fell to the RGBA32F default: eight of its texels read
        // as one, through a sampler of the wrong kind.
        case 0x8234: fmt = 0x8D94; type = 0x1403; texel = 2;  break;  // R16UI
        case 0x8233: fmt = 0x8D94; type = 0x1402; texel = 2;  break;  // R16I
        case 0x822D: fmt = 0x1903; type = 0x140B; texel = 2;  break;  // R16F
        case 0x8232: fmt = 0x8D94; type = 0x1401; texel = 1;  break;  // R8UI
        case 0x8229: fmt = 0x1903; type = 0x1401; texel = 1;  break;  // R8
        case 0x823A: fmt = 0x8228; type = 0x1403; texel = 4;  break;  // RG16UI
        case 0x823C: fmt = 0x8228; type = 0x1405; texel = 8;  break;  // RG32UI
        case 0x822F: fmt = 0x8227; type = 0x140B; texel = 4;  break;  // RG16F
        case 0x8D76: fmt = 0x8D99; type = 0x1403; texel = 8;  break;  // RGBA16UI
        case 0x8D88: fmt = 0x8D99; type = 0x1402; texel = 8;  break;  // RGBA16I
        default:     fmt = 0x1908; type = 0x1406; texel = 16; internalformat = 0x8814; break;
    }
    const uint32_t COPY_READ = 0x8F36, BUFFER_SIZE = 0x8764, MAP_READ = 0x0001;
    if (a_glGetError) while (a_glGetError()) {}
    // The kept bytes first (klfb_tbm); the read-back below is for a buffer that
    // has not been written since a texture was pointed at it.
    {
        size_t klen = 0;
        void *kept = klfb_tbm_copy(buffer, len > 0 && off > 0 ? (size_t)off : 0,
                                   len > 0 ? (size_t)len : 0, &klen);
        static unsigned n_kept;
        if (kept && klen >= (size_t)texel) {
            int W = KLFB_TB_W;
            int texels = (int)(klen / (size_t)texel);
            int H = (texels + W - 1) / W; if (H < 1) H = 1;
            size_t need = (size_t)W * (size_t)H * (size_t)texel;
            void *tmp = calloc(1, need);
            if (tmp) {
                memcpy(tmp, kept, klen < need ? klen : need);
                int32_t pbo = 0;
                if (a_glGetIntegerv) a_glGetIntegerv(0x88EF, &pbo);
                if (pbo) r_BindBuffer(0x88EC, 0);     // the pointer below is memory, not an offset
                g_real_TexImage2D(GL_TEXTURE_2D_, 0, (int32_t)internalformat, W, H, 0, fmt, type, tmp);
                if (pbo) r_BindBuffer(0x88EC, (uint32_t)pbo);
                if (g_real_TexParameteri) {
                    g_real_TexParameteri(GL_TEXTURE_2D_, 0x2801, 0x2600);
                    g_real_TexParameteri(GL_TEXTURE_2D_, 0x2800, 0x2600);
                    g_real_TexParameteri(GL_TEXTURE_2D_, 0x2802, 0x812F);
                    g_real_TexParameteri(GL_TEXTURE_2D_, 0x2803, 0x812F);
                }
                uint32_t e = a_glGetError ? a_glGetError() : 0;
                if (n_kept++ < 6 || e) {
                    static int said_e;
                    if (!e || said_e++ < 6) {
                        const uint32_t *w = (const uint32_t *)tmp;
                        fprintf(stderr, "  [glfb] texel buffer %u: %zu kept bytes -> %dx%d fmt 0x%x "
                                        "(first %08x %08x %08x %08x)%s\n",
                                buffer, klen, W, H, internalformat, w[0], w[1], w[2], w[3],
                                e ? "  <-- UPLOAD FAILED" : "");
                    }
                }
                free(tmp);
            }
            free(kept);
            if (a_glGetError) while (a_glGetError()) {}
            return;
        }
        free(kept);
    }
    r_BindBuffer(COPY_READ, buffer);
    int32_t size = 0; r_GetBufferParameteriv(COPY_READ, BUFFER_SIZE, &size);
    // A glTexBufferRange window: the texel buffer STARTS at `off`. Unreal pools
    // many meshes' data in one buffer and gives each its own window, so reading
    // from byte 0 handed every user the first one's data.
    if (off < 0 || off >= size) off = 0;
    if (len > 0 && off + len <= size) size = (int32_t)len;
    else size -= (int32_t)off;
    if (size <= 0) { r_BindBuffer(COPY_READ, 0); return; }
    void *src = r_MapBufferRange(COPY_READ, off, size, MAP_READ);
    int W = KLFB_TB_W;
    int texels = (int)((size_t)size / (size_t)texel);
    int H = (texels + W - 1) / W; if (H < 1) H = 1;
    void *tmp = NULL;
    if (src) {
        size_t need = (size_t)W * (size_t)H * (size_t)texel;
        tmp = malloc(need);
        if (tmp) { memset(tmp, 0, need); memcpy(tmp, src, (size_t)size); }
        // KL_GLFB_TB_TRACE / KL_FULL: prove the texel buffer refreshes and whether
        // its bytes are zero. A climbing count + non-zero first texels = the fix is
        // feeding fresh data; a frozen count or all-zero first texels = still stale.
        { static int tt = -1; if (tt < 0) tt = kl_env_on("KL_GLFB_TB_TRACE", 0);
          static unsigned n;
          unsigned c = ++n;   // GL context thread only; no atomic needed
          if (tt && tmp && (c <= 40 || c % 200 == 0)) {
              const uint32_t *w = (const uint32_t *)tmp;
              fprintf(stderr, "  [tb] upload #%u buf=%u ifmt=0x%x size=%d texel0=%08x %08x %08x %08x\n",
                      c, buffer, internalformat, size, w[0], w[1], w[2], w[3]);
          } }
        r_UnmapBuffer(COPY_READ);
    }
    r_BindBuffer(COPY_READ, 0);
    {
        static unsigned n_ok, n_fail;
        if (tmp) n_ok++; else n_fail++;
        if ((!tmp && n_fail <= 6) || (tmp && n_ok <= 3))
            fprintf(stderr, "  [glfb] texel buffer %u: read-back of %d bytes %s (%u read, %u "
                            "failed so far)\n", buffer, size,
                    tmp ? "ok" : "FAILED — nothing uploaded, the texture keeps what it had",
                    n_ok, n_fail);
    }
    if (!tmp) return;
    if (len <= 0) klfb_tbm_respecify(buffer, (size_t)size, tmp);   // seed the kept copy
    g_real_TexImage2D(GL_TEXTURE_2D_, 0, (int32_t)internalformat, W, H, 0, fmt, type, tmp);
    if (g_real_TexParameteri) {
        g_real_TexParameteri(GL_TEXTURE_2D_, 0x2801, 0x2600);   // MIN_FILTER NEAREST
        g_real_TexParameteri(GL_TEXTURE_2D_, 0x2800, 0x2600);   // MAG_FILTER NEAREST
        g_real_TexParameteri(GL_TEXTURE_2D_, 0x2802, 0x812F);   // WRAP_S CLAMP_TO_EDGE
        g_real_TexParameteri(GL_TEXTURE_2D_, 0x2803, 0x812F);   // WRAP_T CLAMP_TO_EDGE
    }
    free(tmp);
    if (a_glGetError) while (a_glGetError()) {}   // swallow any residual, never fatal the guest
}
// Backing slots for the loader to fill with ANGLE's real entry points. We do
// not call them (there is no ES 3.0 texture buffer); they exist only so the
// resolver, which stores *real = fn, has somewhere to store.
static void (*g_real_TexBuffer)(uint32_t, uint32_t, uint32_t);
static void (*g_real_TexBufferRange)(uint32_t, uint32_t, uint32_t, intptr_t, intptr_t);

// Re-copy a written backing buffer's CURRENT bytes into its emulated 2D texture,
// so texelFetch sees fresh data (the whole point of GL_TEXTURE_BUFFER). Binds the
// texture on the active unit, uploads, and restores whatever the guest had bound.
// KL_GLFB_TB_REPORT=1: every texture standing in for a texel buffer, with the
// buffer behind it, how many bytes are kept for that buffer and how many of
// them are not zero — printed after 300 and after 3000 refreshes. "Is the data
// there" for all of them at once, which the per-upload lines cannot say.
static void klfb_tb_report(void) {
    fprintf(stderr, "  [glfb] texel buffers: %u texture(s) over buffers, %u buffer(s) kept\n",
            g_tb_assoc_n, g_tbm_n);
    pthread_mutex_lock(&g_tbm_mu);
    for (unsigned i = 0; i < g_tb_assoc_n && i < 96; i++) {
        int k = klfb_tbm_ix(g_tb_assoc[i].buffer);
        size_t size = k >= 0 ? g_tbm[k].size : 0, nz = 0;
        if (k >= 0 && g_tbm[k].bytes)
            for (size_t b = 0; b < size; b++) nz += g_tbm[k].bytes[b] != 0;
        fprintf(stderr, "  [glfb]   tex %u <- buffer %u fmt 0x%x range %ld+%ld: %zu bytes kept, "
                        "%zu non-zero%s\n",
                g_tb_assoc[i].tex, g_tb_assoc[i].buffer, g_tb_assoc[i].ifmt,
                (long)g_tb_assoc[i].off, (long)g_tb_assoc[i].len, size, nz,
                k < 0 ? "  (not kept)" : "");
    }
    pthread_mutex_unlock(&g_tbm_mu);
}
static void klfb_tb_refresh_for_buffer(uint32_t buffer) {
    {
        static int rep = -1; static unsigned n;
        if (rep < 0) rep = kl_env_on("KL_GLFB_TB_REPORT", 0);
        if (rep) { unsigned c = ++n; if (c == 300 || c == 3000 || c == 30000) klfb_tb_report(); }
    }
    if (!g_real_BindTexture) g_real_BindTexture = asym("glBindTexture");
    if (!buffer || !g_real_BindTexture) return;
    int u = g_tb_active_unit;
    if (u < 0 || u >= KLFB_TB_UNITS) u = 0;
    for (unsigned i = 0; i < g_tb_assoc_n; i++) {
        if (g_tb_assoc[i].buffer != buffer) continue;
        uint32_t saved = g_tb_bound2d[u];
        g_real_BindTexture(GL_TEXTURE_2D_, g_tb_assoc[i].tex);
        klfb_texbuffer_upload(g_tb_assoc[i].ifmt, buffer, g_tb_assoc[i].off,
                              g_tb_assoc[i].len);
        g_real_BindTexture(GL_TEXTURE_2D_, saved);          // put the guest's texture back
    }
}

// ...and for a buffer the GPU itself has just written (a compute kernel served
// as transform feedback, klfb_cs_dispatch_sb): what the CPU saw of it is stale,
// and a read-back would wait for the GPU. Its R32UI stand-ins are copied from it
// on the GPU instead, through GL_PIXEL_UNPACK_BUFFER, row by row of the layout
// the shaders read (KLFB_TB_W words wide). Arizona Sunshine draws its zombies'
// heads with a shader that reads the skinned vertices back as a storage buffer;
// left with the bytes the CPU last saw, the head flickered and showed through
// itself (2026-10-31).
static void klfb_tb_gpu_refresh(uint32_t buffer, intptr_t written) {
    static void (*sub)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *);
    static void (*store)(uint32_t, int32_t);
    if (!sub) { sub = asym("glTexSubImage2D"); store = asym("glPixelStorei"); }
    if (!g_real_BindTexture) g_real_BindTexture = asym("glBindTexture");
    if (!g_real_BindBuffer) g_real_BindBuffer = asym("glBindBuffer");
    if (!buffer || !sub || !store || !g_real_BindTexture || !g_real_BindBuffer || !a_glGetIntegerv) return;
    int have = 0;
    for (unsigned i = 0; i < g_tb_assoc_n && !have; i++) have = g_tb_assoc[i].buffer == buffer;
    if (!have) return;
    int32_t pbo = 0, align = 4, rowlen = 0, skiprows = 0, skippix = 0, tex2d = 0;
    a_glGetIntegerv(0x88EF /* PIXEL_UNPACK_BUFFER_BINDING */, &pbo);
    a_glGetIntegerv(0x0CF5 /* UNPACK_ALIGNMENT */, &align);
    a_glGetIntegerv(0x0CF2 /* UNPACK_ROW_LENGTH */, &rowlen);
    a_glGetIntegerv(0x0CF3 /* UNPACK_SKIP_ROWS */, &skiprows);
    a_glGetIntegerv(0x0CF4 /* UNPACK_SKIP_PIXELS */, &skippix);
    a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &tex2d);
    store(0x0CF5, 4); store(0x0CF2, 0); store(0x0CF3, 0); store(0x0CF4, 0);
    g_real_BindBuffer(0x88EC /* PIXEL_UNPACK_BUFFER */, buffer);
    for (unsigned i = 0; i < g_tb_assoc_n; i++) {
        if (g_tb_assoc[i].buffer != buffer || g_tb_assoc[i].ifmt != 0x8236 /* R32UI */) continue;
        intptr_t off = g_tb_assoc[i].off > 0 ? g_tb_assoc[i].off : 0;
        intptr_t len = written - off;
        if (g_tb_assoc[i].len > 0 && g_tb_assoc[i].len < len) len = g_tb_assoc[i].len;
        intptr_t words = len / 4;
        if (words <= 0) continue;
        int rows = (int)(words / KLFB_TB_W), rest = (int)(words % KLFB_TB_W);
        g_real_BindTexture(GL_TEXTURE_2D_, g_tb_assoc[i].tex);
        if (rows) sub(GL_TEXTURE_2D_, 0, 0, 0, KLFB_TB_W, rows, 0x8D94 /* RED_INTEGER */, 0x1405, (const void *)off);
        if (rest) sub(GL_TEXTURE_2D_, 0, 0, rows, rest, 1, 0x8D94, 0x1405,
                      (const void *)(off + (intptr_t)rows * KLFB_TB_W * 4));
    }
    g_real_BindBuffer(0x88EC, (uint32_t)pbo);
    g_real_BindTexture(GL_TEXTURE_2D_, (uint32_t)tex2d);
    store(0x0CF5, align); store(0x0CF2, rowlen); store(0x0CF3, skiprows); store(0x0CF4, skippix);
}

// glTexBuffer points the bound texture at `buffer`: record tex<-buffer so later
// writes to the buffer can refresh the texture, then do the initial upload.
static void klfb_tb_bind(uint32_t internalformat, uint32_t buffer,
                         intptr_t off, intptr_t len) {
    int u = g_tb_active_unit;
    if (u < 0 || u >= KLFB_TB_UNITS) u = 0;
    uint32_t tex = g_tb_bound2d[u];
    if (tex) {
        unsigned i = 0;
        unsigned cap = klfb_tb_cap();
        for (; i < g_tb_assoc_n; i++) if (g_tb_assoc[i].tex == tex) break;   // one entry per texture
        if (i == g_tb_assoc_n && g_tb_assoc_n < cap) g_tb_assoc_n++;
        if (i < cap) {
            g_tb_assoc[i].tex = tex; g_tb_assoc[i].buffer = buffer; g_tb_assoc[i].ifmt = internalformat;
            g_tb_assoc[i].off = off; g_tb_assoc[i].len = len;
        } else {
            static int said;
            if (!said++)
                fprintf(stderr, "  [glfb] texel buffers: more than %u textures over buffers — "
                                "the rest are not kept fresh\n", cap);
        }
    }
    klfb_tbm_want(buffer);
    klfb_texbuffer_upload(internalformat, buffer, off, len);
}
static void klfb_TexBuffer(uint32_t target, uint32_t internalformat, uint32_t buffer) {
    (void)target; klfb_tb_bind(internalformat, buffer, 0, 0);
}
static void klfb_TexBufferRange(uint32_t target, uint32_t internalformat, uint32_t buffer,
                                intptr_t offset, intptr_t size) {
    (void)target;
    static int said;
    if (said < 4) { said++;
        fprintf(stderr, "  [glfb] glTexBufferRange(fmt 0x%x, buffer %u, offset %ld, size %ld)\n",
                internalformat, buffer, (long)offset, (long)size); }
    klfb_tb_bind(internalformat, buffer, offset, size);
}

// ---- read-only storage buffers: the host half (see klfb_rewrite_ro_ssbo) -----
//
// Before each draw, every kl_sbN the current program declares is pointed at the
// buffer the guest has bound at storage binding N, as an R32UI texture laid out
// the way the shader reads it. The texture is a texel-buffer stand-in like any
// other (g_tb_assoc + the kept bytes, klfb_tbm), so a write to the buffer
// re-uploads it. The units are the last ones ANGLE has, one per binding point,
// where Unity binds nothing.
#define KLFB_SB_PROGS 4096
static struct { uint32_t prog; int8_t n; int8_t bind[KLFB_SB_MAXB]; int32_t loc[KLFB_SB_MAXB]; }
    g_sb_prog[KLFB_SB_PROGS];
// 2048, not 64: a Unity title that skins on the GPU reads three storage
// buffers per skinned mesh, plus a ring of bone-matrix buffers, and Arizona
// Sunshine went past 64 in every run ("more than 64 — the rest read zeros").
// Which meshes then read zeros depended on the order they first drew in, so
// its zombie's head, and then its gun, were there in one run and gone in the
// next (2026-10-31).
#define KLFB_SB_TEX 2048
static struct { uint32_t buf, tex; intptr_t off, len; } g_sb_tex[KLFB_SB_TEX];
static unsigned g_sb_tex_n;
// A program that is (re)linked or deleted is asked about its kl_sbN again.
static void klfb_ssbo_forget(uint32_t prog) {
    if (!prog) return;
    for (unsigned k = 0; k < KLFB_SB_PROGS; k++) {
        unsigned i = (prog + k) & (KLFB_SB_PROGS - 1);
        if (!g_sb_prog[i].prog) return;
        if (g_sb_prog[i].prog == prog) { g_sb_prog[i].n = -1; return; }
    }
}
// The texture standing in for [off, off+len) of `buf`, made and filled on first
// use; bound on the active unit. A texel-buffer association that was dropped
// (the buffer was deleted and its name came back) is made again.
static uint32_t klfb_sb_tex_for(uint32_t buf, intptr_t off, intptr_t len) {
    static void (*gen)(int32_t, uint32_t *);
    if (!gen) gen = asym("glGenTextures");
    unsigned i = 0;
    for (; i < g_sb_tex_n; i++)
        if (g_sb_tex[i].buf == buf && g_sb_tex[i].off == off && g_sb_tex[i].len == len) break;
    if (i == g_sb_tex_n) {
        if (g_sb_tex_n == KLFB_SB_TEX) {
            static int said;
            if (!said++) fprintf(stderr, "  [glfb] storage buffers: more than %d — the rest read zeros\n", KLFB_SB_TEX);
            return 0;
        }
        uint32_t t = 0;
        if (gen) gen(1, &t);
        if (!t) return 0;
        g_sb_tex[i].buf = buf; g_sb_tex[i].tex = t; g_sb_tex[i].off = off; g_sb_tex[i].len = len;
        g_sb_tex_n++;
    }
    uint32_t tex = g_sb_tex[i].tex;
    g_real_BindTexture(GL_TEXTURE_2D_, tex);
    unsigned a = 0;
    for (; a < g_tb_assoc_n; a++) if (g_tb_assoc[a].tex == tex) break;
    if (a < g_tb_assoc_n && g_tb_assoc[a].buffer == buf) return tex;
    if (a == g_tb_assoc_n) {
        if (g_tb_assoc_n == KLFB_TB_ASSOC) return tex;
        g_tb_assoc_n++;
    }
    g_tb_assoc[a].tex = tex; g_tb_assoc[a].buffer = buf; g_tb_assoc[a].ifmt = 0x8236 /* R32UI */;
    g_tb_assoc[a].off = off; g_tb_assoc[a].len = len;
    klfb_tbm_want(buf);
    klfb_texbuffer_upload(0x8236, buf, off, len);
    static int said;
    if (said < 8) { said++;
        fprintf(stderr, "  [glfb] storage buffer %u (offset %ld, size %ld) read through texture %u\n",
                buf, (long)off, (long)len, tex); }
    return tex;
}
static void klfb_ssbo_apply(void) {
    if (!kl_glfb_ssbo_tex()) return;
    uint32_t prog = g_cur_prog;
    if (!prog) return;
    unsigned i = 0, k = 0;
    for (; k < KLFB_SB_PROGS; k++) {
        i = (prog + k) & (KLFB_SB_PROGS - 1);
        if (!g_sb_prog[i].prog || g_sb_prog[i].prog == prog) break;
    }
    if (k == KLFB_SB_PROGS) return;
    if (!g_sb_prog[i].prog) { g_sb_prog[i].prog = prog; g_sb_prog[i].n = -1; }
    if (g_sb_prog[i].n < 0) {
        static int32_t (*loc)(uint32_t, const char *);
        if (!loc) loc = asym("glGetUniformLocation");
        int n = 0;
        for (int b = 0; loc && b < KLFB_SB_MAXB; b++) {
            char nm[16]; snprintf(nm, sizeof nm, "kl_sb%d", b);
            int32_t l = loc(prog, nm);
            if (l >= 0) { g_sb_prog[i].bind[n] = (int8_t)b; g_sb_prog[i].loc[n] = l; n++; }
        }
        g_sb_prog[i].n = (int8_t)n;
    }
    if (!g_sb_prog[i].n) return;
    static void (*active)(uint32_t), (*sampler)(uint32_t, uint32_t), (*uni)(int32_t, int32_t);
    static int units;
    if (!active)  active  = asym("glActiveTexture");
    if (!sampler) sampler = asym("glBindSampler");
    if (!uni)     uni     = asym("glUniform1i");
    if (!g_real_BindTexture) g_real_BindTexture = asym("glBindTexture");
    if (!units && a_glGetIntegerv) {
        int32_t v = 0; a_glGetIntegerv(0x8B4D /* MAX_COMBINED_TEXTURE_IMAGE_UNITS */, &v);
        units = v >= 16 + KLFB_SB_MAXB ? v : -1;
        fprintf(stderr, "  [glfb] storage buffers read as textures on units %d..%d (KL_GLFB_SSBO_TEX)\n",
                v - KLFB_SB_MAXB, v - 1);
    }
    if (units <= 0 || !active || !uni || !g_real_BindTexture) return;
    for (int j = 0; j < g_sb_prog[i].n; j++) {
        int b = g_sb_prog[i].bind[j], unit = units - 1 - b;
        uint32_t buf = g_cs_ssbo[b].buf;
        intptr_t off = g_cs_ssbo[b].off, len = g_cs_ssbo[b].size > 0 ? g_cs_ssbo[b].size : 0;
        active(0x84C0 + (uint32_t)unit);
        if (!buf || !klfb_sb_tex_for(buf, off, len)) {
            g_real_BindTexture(GL_TEXTURE_2D_, 0);     // reads zeros
            static int said;
            if (said < 4) { said++;
                fprintf(stderr, "  [glfb] program %u reads storage binding %d, and nothing is bound there\n",
                        prog, b); }
        }
        if (sampler) sampler((uint32_t)unit, 0);       // texelFetch wants no filtering state
        uni(g_sb_prog[i].loc[j], unit);
    }
    int u = g_tb_active_unit;
    active(0x84C0 + (uint32_t)(u >= 0 ? u : 0));
}

// glBindBuffer: remember the buffer bound per target, so a later glBufferSubData
// or unmap can name the buffer it wrote and refresh a texel-buffer texture over it.
static void (*g_real_BindBuffer)(uint32_t, uint32_t);
static void klfb_BindBuffer(uint32_t target, uint32_t buffer) {
    if (KLFB_CS_GENERIC(target)) {                                    // ours: ANGLE has no such target
        g_cs_generic = buffer;
        if (kl_glfb_ssbo_tex()) klfb_tbm_want(buffer);  // keep its bytes from the first upload on
        return;
    }
    if (!g_real_BindBuffer) g_real_BindBuffer = asym("glBindBuffer");
    if (g_real_BindBuffer) g_real_BindBuffer(target, buffer);
    int s = klfb_tb_tgtslot(target);
    if (s >= 0) g_tb_bound_buf[s] = buffer;
}
// glBufferSubData writes into the bound buffer — if that buffer backs a texel-
// buffer texture, its emulated 2D copy is now stale; re-upload the current bytes.
static void klfb_BufferSubData(uint32_t target, intptr_t offset, intptr_t size, const void *data) {
    if (KLFB_CS_GENERIC(target)) {
        KLFB_CS_VIA_COPY_WRITE(klfb_BufferSubData(KLFB_GL_COPY_WRITE, offset, size, data));
        return;
    }
    if (!g_real_BufferSubData_t) g_real_BufferSubData_t = asym("glBufferSubData");
    if (g_real_BufferSubData_t) g_real_BufferSubData_t(target, offset, size, data);
    int s = klfb_tb_tgtslot(target);
    if (s >= 0 && offset >= 0 && size > 0)
        klfb_tbm_write(g_tb_bound_buf[s], (size_t)offset, (size_t)size, data);
    if (s >= 0) klfb_tb_refresh_for_buffer(g_tb_bound_buf[s]);
}
// glGetUniformLocation, so the built-in-name rename above stays invisible to the
// guest: it still asks for `length` and still gets the location of the uniform it
// declared. Only a miss is retried, so a shader we did not rewrite is untouched
// and a real -1 stays -1.
// ...and every answer it gives is recorded, so a uniform call that fails can say
// where its location CAME from. "Location 2 is a mat4" is only half a finding:
// the other half is which program the guest was looking at when it asked, and
// nothing else on the path knows that.
#define KLFB_LOCS 512
static struct { uint32_t prog; int32_t loc; char name[48]; } g_uloc[KLFB_LOCS];
static unsigned g_uloc_n;

static int32_t klfb_GetUniformLocation(uint32_t prog, const char *name) {
    // Resolved here as well as in the thunk table: the ES 3.1 door below calls
    // this for a guest that never resolves glGetUniformLocation itself, so the
    // thunk's real slot can still be empty.
    if (!g_real_GetUniformLocation)
        g_real_GetUniformLocation = (void *)asym("glGetUniformLocation");
    if (!g_real_GetUniformLocation || !name) return -1;
    // Asking is the whole decision: from here on this program is described to
    // the guest in the LINKER's numbering and glUniform* leaves it alone. The
    // alternative — answer in the guest's pinned numbering and remap on the way
    // back — was what shipped, and it cannot be made right: the two numberings
    // share one integer space, so an unpinned uniform whose linker location
    // happens to equal some other uniform's pinned one is redirected into it,
    // silently. There is no such ambiguity once only one numbering is in use.
    klfb_pins_byname(prog);
    int32_t loc = klfb_actual_loc(prog, name);
    if (loc >= 0) {
        pthread_mutex_lock(&g_compile_lock);
        unsigned i = 0;
        for (; i < g_uloc_n; i++)
            if (g_uloc[i].prog == prog && g_uloc[i].loc == loc) break;
        if (i == g_uloc_n && g_uloc_n < KLFB_LOCS) g_uloc_n++;
        if (i < KLFB_LOCS) {
            g_uloc[i].prog = prog; g_uloc[i].loc = loc;
            snprintf(g_uloc[i].name, sizeof g_uloc[i].name, "%s", name);
        }
        pthread_mutex_unlock(&g_compile_lock);
    }
    return loc;
}

// What the guest was told about this program, in the order it asked. Appends to
// `out`. The WHOLE list, not just the matching location: "why is the guest
// writing a float at location 1" is answered by the guest never having asked
// for location 1, and only the whole list says so.
static void klfb_locs_say(uint32_t prog, char *out, size_t n) {
    size_t at = strlen(out);
    pthread_mutex_lock(&g_compile_lock);
    at += (size_t)snprintf(out + at, n - at, " — it was told:");
    int any = 0;
    for (unsigned i = 0; i < g_uloc_n && at + 40 < n; i++)
        if (g_uloc[i].prog == prog) {
            at += (size_t)snprintf(out + at, n - at, " %s=%d",
                                   g_uloc[i].name, g_uloc[i].loc);
            any = 1;
        }
    if (!any) snprintf(out + at, n - at, " nothing (it never asked)");
    pthread_mutex_unlock(&g_compile_lock);
}
static void klfb_BindSampler(uint32_t u, uint32_t s) {
    if (a_glGetError) while (a_glGetError()) {}
    // Remembered per unit, and withheld while the unit shows a texel-buffer
    // stand-in (see klfb_BindTexture).
    int held = 0;
    if (u < KLFB_TB_UNITS) { g_tb_sampler[u] = s; held = g_tb_unit[u]; }
    if (g_real_BindSampler && !held) g_real_BindSampler(u, s);
    klfb_err_say("glBindSampler", s);
}
// The uniform family, every entry point of it, for one reason: each one carries
// a location the guest may have pinned in the shader, and a location the guest
// pinned means something different to the linker. klfb_remap_loc is the whole
// body — it is the identity for any guest that resolves uniforms by name, which
// is why this can be unconditional. The two hand-written members below
// (glUniform1i, glUniform1f) also carry error probes and predate this.
#define KLFB_UNIFORM_N(fn, T, N, PARAMS, ARGS)                                \
    static void (*g_real_##fn) PARAMS;                                        \
    static void klfb_##fn PARAMS {                                            \
        if (g_real_##fn) g_real_##fn ARGS;                                    \
    }
#define KLFB_U1(fn, T) KLFB_UNIFORM_N(fn, T, 1, (int32_t l, T a), \
                                      (klfb_remap_loc(l), a))
#define KLFB_U2(fn, T) KLFB_UNIFORM_N(fn, T, 2, (int32_t l, T a, T b), \
                                      (klfb_remap_loc(l), a, b))
#define KLFB_U3(fn, T) KLFB_UNIFORM_N(fn, T, 3, (int32_t l, T a, T b, T c), \
                                      (klfb_remap_loc(l), a, b, c))
#define KLFB_U4(fn, T) KLFB_UNIFORM_N(fn, T, 4, (int32_t l, T a, T b, T c, T d), \
                                      (klfb_remap_loc(l), a, b, c, d))
// The vector forms also answer for a refusal when ERRSCAN is on: the error alone
// says a write was rejected, and which uniform it was aimed at is the question.
static int glfb_errscan(void);
static void klfb_uniform_refused(const char *fn, int32_t loc, int32_t count, uint32_t err);
#define KLFB_UV(fn, T)                                                        \
    static void (*g_real_##fn)(int32_t, int32_t, const T *);                  \
    static void klfb_##fn(int32_t l, int32_t n, const T *v) {                 \
        if (!g_real_##fn) return;                                             \
        if (!glfb_errscan() || !a_glGetError) {                               \
            g_real_##fn(klfb_remap_loc(l), n, v); return;                     \
        }                                                                     \
        while (a_glGetError()) {}                                             \
        g_real_##fn(klfb_remap_loc(l), n, v);                                 \
        uint32_t e = a_glGetError();                                          \
        if (e) klfb_uniform_refused("gl" #fn, l, n, e);                       \
    }
#define KLFB_UM(fn)    KLFB_UNIFORM_N(fn, float, 0, \
                                      (int32_t l, int32_t n, uint8_t t, const float *v), \
                                      (klfb_remap_loc(l), n, t, v))

KLFB_U2(Uniform2f, float) KLFB_U3(Uniform3f, float) KLFB_U4(Uniform4f, float)
KLFB_U2(Uniform2i, int32_t) KLFB_U3(Uniform3i, int32_t) KLFB_U4(Uniform4i, int32_t)
KLFB_U1(Uniform1ui, uint32_t) KLFB_U2(Uniform2ui, uint32_t)
KLFB_U3(Uniform3ui, uint32_t) KLFB_U4(Uniform4ui, uint32_t)
KLFB_UV(Uniform1fv, float) KLFB_UV(Uniform2fv, float)
KLFB_UV(Uniform3fv, float) KLFB_UV(Uniform4fv, float)
KLFB_UV(Uniform1iv, int32_t) KLFB_UV(Uniform2iv, int32_t)
KLFB_UV(Uniform3iv, int32_t) KLFB_UV(Uniform4iv, int32_t)
KLFB_UV(Uniform1uiv, uint32_t) KLFB_UV(Uniform2uiv, uint32_t)
KLFB_UV(Uniform3uiv, uint32_t) KLFB_UV(Uniform4uiv, uint32_t)
KLFB_UM(UniformMatrix2fv) KLFB_UM(UniformMatrix3fv) KLFB_UM(UniformMatrix4fv)
KLFB_UM(UniformMatrix2x3fv) KLFB_UM(UniformMatrix3x2fv)
KLFB_UM(UniformMatrix2x4fv) KLFB_UM(UniformMatrix4x2fv)
KLFB_UM(UniformMatrix3x4fv) KLFB_UM(UniformMatrix4x3fv)

// A refused glUniform*v, described: the program, the location and count the
// guest used, and what the linker actually has at that location and nearby.
static void klfb_uniform_refused(const char *fn, int32_t loc, int32_t count, uint32_t err) {
    static int said;
    if (said >= 16) return;
    said++;
    static void (*r_gpi)(uint32_t, uint32_t, int32_t *);
    static void (*r_gau)(uint32_t, uint32_t, int32_t, int32_t *, int32_t *, uint32_t *, char *);
    if (!r_gpi) r_gpi = asym("glGetProgramiv");
    if (!r_gau) r_gau = asym("glGetActiveUniform");
    int32_t cur = 0;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &cur);
    fprintf(stderr, "  [glfb] %s(loc %d, count %d) REFUSED 0x%x — current program %d "
                    "(tracked %u), thread %p\n",
            fn, loc, count, err, cur, g_cur_prog, (void *)pthread_self());
    if (!cur || !r_gpi || !r_gau) return;
    int32_t nu = 0;
    r_gpi((uint32_t)cur, 0x8B86 /* ACTIVE_UNIFORMS */, &nu);
    for (int32_t i = 0; i < nu && i < 48; i++) {
        char name[96]; int32_t len = 0, size = 0; uint32_t type = 0;
        name[0] = 0;
        r_gau((uint32_t)cur, (uint32_t)i, sizeof name, &len, &size, &type, name);
        int32_t at = klfb_actual_loc((uint32_t)cur, name);
        fprintf(stderr, "  [glfb]   uniform %-28s type 0x%04x size %d at %d%s\n",
                name, type, size, at,
                (loc >= at && loc < at + (size > 0 ? size : 1)) ? "   <== the target" : "");
    }
    char told[1500]; told[0] = 0;
    klfb_locs_say((uint32_t)cur, told, sizeof told);
    fprintf(stderr, "  [glfb]   program %d%s\n", cur, told);
    while (a_glGetError()) {}
}

// Is the uniform at `loc` in the current program a sampler? Asked of the
// linker's own reflection, so an int uniform that happens to hold 40 is never
// mistaken for a texture unit. Only reached for values of 32 and up, which is
// sampler setup once per program, so the walk is not on a per-draw path.
static int klfb_loc_is_sampler(int32_t loc) {
    static void (*r_gpi)(uint32_t, uint32_t, int32_t *);
    static void (*r_gau)(uint32_t, uint32_t, int32_t, int32_t *, int32_t *, uint32_t *, char *);
    if (!r_gpi) r_gpi = asym("glGetProgramiv");
    if (!r_gau) r_gau = asym("glGetActiveUniform");
    int32_t prog = 0, nu = 0;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &prog);
    if (!prog || !r_gpi || !r_gau) return 0;
    r_gpi((uint32_t)prog, 0x8B86 /* ACTIVE_UNIFORMS */, &nu);
    for (int32_t i = 0; i < nu; i++) {
        char n[96]; int32_t len = 0, size = 0; uint32_t type = 0;
        n[0] = 0;
        r_gau((uint32_t)prog, (uint32_t)i, sizeof n, &len, &size, &type, n);
        int32_t at = klfb_actual_loc((uint32_t)prog, n);
        if (at < 0 || loc < at || loc >= at + (size > 0 ? size : 1)) continue;
        return (type >= 0x8B5D && type <= 0x8B64)    // SAMPLER_1D .. 2D_RECT_SHADOW
            || (type >= 0x8DC0 && type <= 0x8DD8)    // array/shadow/int/uint samplers
            || type == 0x8D66                         // SAMPLER_EXTERNAL_OES
            || (type >= 0x9108 && type <= 0x910D);   // multisample samplers
    }
    return 0;
}

static void (*g_real_Uniform1i)(int32_t, int32_t);
static void klfb_Uniform1i(int32_t loc, int32_t v) {
    if (a_glGetError) while (a_glGetError()) {}
    loc = klfb_remap_loc(loc);
    // The GL half of unity-texunit-mask (kl_guestpatch.c): libunity binds a
    // texture for unit u at u & 31, so the sampler that reads it has to name
    // u & 31 too. Only when the loaded image carries the patch, only for
    // sampler-typed uniforms, and only above 31, where nothing else could have
    // put the value.
    if (v >= 32 && kl_texunit_mask_active() && klfb_loc_is_sampler(loc)) {
        static int said;
        if (said < 8) {
            said++;
            fprintf(stderr, "  [glfb] sampler unit %d -> %d (unity-texunit-mask)\n", v, v & 31);
        }
        v &= 31;
    }
    if (g_real_Uniform1i) g_real_Uniform1i(loc, v);
    static int said;
    if (said < 30 && a_glGetError) {
        uint32_t e = a_glGetError();
        if (e) {
            said++;
            int32_t prog = -1;
            if (a_glGetIntegerv) a_glGetIntegerv(0x8B8D, &prog);
            fprintf(stderr, "  [glfb] ERRSRC glUniform1i(loc=%d, %d) -> 0x%x "
                            "(program=%d)\n", loc, v, e, prog);
        }
    }
}
// glUniform1f is INVALID_OPERATION about twelve times a frame on the Steam Link
// VR path (glutils.cpp:205, ~17k a run), and the error code alone cannot say
// which of its three causes it is: no current program, a location belonging to
// a different program, or a location whose uniform is not a float. So name the
// uniform. glGetActiveUniform indexes the CURRENT program's own list, and
// glGetUniformLocation maps each name back to a location — if none of them
// matches, the location came from somewhere else, and that is the answer.
static void (*g_real_Uniform1f)(int32_t, float);
static void klfb_Uniform1f(int32_t loc, float v) {
    if (a_glGetError) while (a_glGetError()) {}
    loc = klfb_remap_loc(loc);
    if (g_real_Uniform1f) g_real_Uniform1f(loc, v);
    static int said;
    if (said >= 20 || !a_glGetError) return;
    uint32_t e = a_glGetError();
    if (!e) return;
    said++;
    int32_t prog = -1;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &prog);

    static void (*r_GetProgramiv)(uint32_t, uint32_t, int32_t *);
    static void (*r_GetActiveUniform)(uint32_t, uint32_t, int32_t, int32_t *,
                                      int32_t *, uint32_t *, char *);
    if (!r_GetProgramiv) {
        r_GetProgramiv = asym("glGetProgramiv");
        r_GetActiveUniform = asym("glGetActiveUniform");
    }
    char who[512];
    snprintf(who, sizeof who, "no current program");
    if (prog > 0 && r_GetProgramiv && r_GetActiveUniform && g_real_GetUniformLocation) {
        int32_t n = 0;
        r_GetProgramiv((uint32_t)prog, 0x8B86 /* ACTIVE_UNIFORMS */, &n);
        snprintf(who, sizeof who, "no uniform of program %d has location %d "
                                  "(it has %d)", prog, loc, n);
        for (int32_t i = 0; i < n; i++) {
            char name[96] = ""; int32_t len = 0, size = 0; uint32_t type = 0;
            r_GetActiveUniform((uint32_t)prog, (uint32_t)i, (int32_t)sizeof name,
                               &len, &size, &type, name);
            if (g_real_GetUniformLocation((uint32_t)prog, name) != loc) continue;
            snprintf(who, sizeof who, "`%s` of program %d is type 0x%x[%d], "
                                      "not a float", name, prog, type, size);
            break;
        }
    }
    if (prog > 0) klfb_locs_say((uint32_t)prog, who, sizeof who);
    fprintf(stderr, "  [glfb] ERRSRC glUniform1f(loc=%d, %g) -> 0x%x — %s\n",
            loc, (double)v, e, who);
}

static void (*g_real_Enable)(uint32_t);
static void (*g_real_Disable)(uint32_t);

static void klfb_srgb_note(int enabled) {
    static int said;
    if (g_srgb_write_off == !enabled) return;
    if (!enabled) g_srgb_write_off = 1;      // sticky — see above
    if (!said++)
        fprintf(stderr, "  [glfb] the guest %s GL_FRAMEBUFFER_SRGB — ANGLE has no "
                        "EXT_sRGB_write_control, so the encode is applied "
                        "regardless and the composite undoes it\n",
                enabled ? "enabled" : "disabled");
    klfb_srgb_settle();
}

static void klfb_Enable(uint32_t cap) {
    if (cap == KLFB_GL_FRAMEBUFFER_SRGB) { klfb_srgb_note(1); return; }
    // KL_HL2_NO_CULL=1: swallow glEnable(GL_CULL_FACE) so nothing is back-face culled.
    // Diagnostic for "walls invisible from the inside" — if geometry (train interior)
    // reappears with culling off, the faces are being back-face culled (winding), not
    // PVS-culled. Everything renders double-sided (noisy, slower) — it is a test, not a fix.
    if (cap == 0x0B44 /* GL_CULL_FACE */) {
        static int nocull = -1;
        if (nocull < 0) nocull = kl_env_on("KL_HL2_NO_CULL", 0);
        if (nocull) {
            static int said;
            if (said++ < 2)
                fprintf(stderr, "  [glfb] NO_CULL: swallowing glEnable(GL_CULL_FACE)\n");
            return;
        }
    }
    if (g_real_Enable) g_real_Enable(cap);
}

static void klfb_Disable(uint32_t cap) {
    if (cap == KLFB_GL_FRAMEBUFFER_SRGB) { klfb_srgb_note(0); return; }
    if (g_real_Disable) g_real_Disable(cap);
}

static void (*g_real_GenerateMipmap)(uint32_t);
static void klfb_GenerateMipmap(uint32_t t) {
    if (a_glGetError) while (a_glGetError()) {}
    if (g_real_GenerateMipmap) g_real_GenerateMipmap(t);
    klfb_err_say("glGenerateMipmap", t);
}

static const struct { const char *name; void *thunk; void **real; } g_thunks[] = {
    {"glActiveTexture", (void *)klfb_ActiveTexture, (void **)&g_real_ActiveTexture},
    {"glUseProgram",  (void *)klfb_UseProgram,  (void **)&g_real_UseProgram},
    {"glGetQueryObjectuiv", (void *)klfb_GetQueryObjectuiv, (void **)&g_real_GetQueryObjectuiv},
    {"glBindTexture", (void *)klfb_BindTexture, (void **)&g_real_BindTexture},
    {"glTexBuffer",      (void *)klfb_TexBuffer,      (void **)&g_real_TexBuffer},
    {"glTexBufferRange", (void *)klfb_TexBufferRange, (void **)&g_real_TexBufferRange},
    {"glTexBufferEXT",   (void *)klfb_TexBuffer,      (void **)&g_real_TexBuffer},
    {"glTexBufferOES",   (void *)klfb_TexBuffer,      (void **)&g_real_TexBuffer},
    {"glEGLImageTargetTexture2DOES", (void *)klfb_EGLImageTargetTexture2DOES,
                                     (void **)&g_real_EGLImageTargetTexture2DOES},
    {"glBindSampler", (void *)klfb_BindSampler, (void **)&g_real_BindSampler},
    {"glGetUniformLocation", (void *)klfb_GetUniformLocation, (void **)&g_real_GetUniformLocation},
    // the uniform family — every one of them, so an explicitly pinned location
    // is translated to the linker's own (see klfb_strip_uniform_layout)
    {"glUniform1i",   (void *)klfb_Uniform1i,   (void **)&g_real_Uniform1i},
    {"glUniform1f",   (void *)klfb_Uniform1f,   (void **)&g_real_Uniform1f},
#define KLFB_UT(fn) {"gl" #fn, (void *)klfb_##fn, (void **)&g_real_##fn}
    KLFB_UT(Uniform2f), KLFB_UT(Uniform3f), KLFB_UT(Uniform4f),
    KLFB_UT(Uniform2i), KLFB_UT(Uniform3i), KLFB_UT(Uniform4i),
    KLFB_UT(Uniform1ui), KLFB_UT(Uniform2ui), KLFB_UT(Uniform3ui), KLFB_UT(Uniform4ui),
    KLFB_UT(Uniform1fv), KLFB_UT(Uniform2fv), KLFB_UT(Uniform3fv), KLFB_UT(Uniform4fv),
    KLFB_UT(Uniform1iv), KLFB_UT(Uniform2iv), KLFB_UT(Uniform3iv), KLFB_UT(Uniform4iv),
    KLFB_UT(Uniform1uiv), KLFB_UT(Uniform2uiv), KLFB_UT(Uniform3uiv), KLFB_UT(Uniform4uiv),
    KLFB_UT(UniformMatrix2fv), KLFB_UT(UniformMatrix3fv), KLFB_UT(UniformMatrix4fv),
    KLFB_UT(UniformMatrix2x3fv), KLFB_UT(UniformMatrix3x2fv),
    KLFB_UT(UniformMatrix2x4fv), KLFB_UT(UniformMatrix4x2fv),
    KLFB_UT(UniformMatrix3x4fv), KLFB_UT(UniformMatrix4x3fv),
#undef KLFB_UT
    {"glGenerateMipmap", (void *)klfb_GenerateMipmap, (void **)&g_real_GenerateMipmap},
    // GL_FRAMEBUFFER_SRGB only — every other cap falls straight through
    {"glEnable",  (void *)klfb_Enable,  (void **)&g_real_Enable},
    {"glDisable", (void *)klfb_Disable, (void **)&g_real_Disable},
    {"glFlush",  (void *)klfb_Flush,  (void **)&g_real_Flush},
    // Buffer lifecycle trace (KL_GLFB_TRACE_BUF) — pass-through wrappers that log
    // the map result, to diagnose togl's "failed to lock vertex buffer" (hl2).
    {"glMapBufferRange", (void *)klfb_MapBufferRange, (void **)&g_real_MapBufferRange_t},
    {"glUnmapBuffer",    (void *)klfb_UnmapBuffer,    (void **)&g_real_UnmapBuffer_t},
    {"glFlushMappedBufferRange", (void *)klfb_FlushMappedBufferRange, (void **)&g_real_FlushMappedBufferRange_t},
    {"glBufferData",     (void *)klfb_BufferData,     (void **)&g_real_BufferData_t},
    {"glBindBuffer",     (void *)klfb_BindBuffer,     (void **)&g_real_BindBuffer},
    {"glBufferSubData",  (void *)klfb_BufferSubData,  (void **)&g_real_BufferSubData_t},
    {"glBindVertexArray", (void *)klfb_BindVertexArray, (void **)&g_real_BindVertexArray_vtx},
    {"glEnableVertexAttribArray",  (void *)klfb_EnableVertexAttribArray,  (void **)&g_real_EnableVAA},
    {"glDisableVertexAttribArray", (void *)klfb_DisableVertexAttribArray, (void **)&g_real_DisableVAA},
    {"glVertexAttribPointer",      (void *)klfb_VertexAttribPointer,      (void **)&g_real_VAPointer},
    {"glVertexAttribIPointer",     (void *)klfb_VertexAttribIPointer,     (void **)&g_real_VAIPointer},
    {"glVertexAttribDivisor",      (void *)klfb_VertexAttribDivisor,      (void **)&g_real_VADivisor},
    {"glVertexAttribFormat",  (void *)klfb_VertexAttribFormat,  (void **)&g_real_VtxAttribFormat},
    {"glVertexAttribIFormat", (void *)klfb_VertexAttribIFormat, (void **)&g_real_VtxAttribIFormat},
    {"glVertexAttribBinding", (void *)klfb_VertexAttribBinding, (void **)&g_real_VtxAttribBinding},
    {"glBindVertexBuffer",    (void *)klfb_BindVertexBuffer,    (void **)&g_real_BindVertexBuffer},
    {"glVertexBindingDivisor",(void *)klfb_VertexBindingDivisor,(void **)&g_real_VtxBindingDivisor},
    {"glDrawElements", (void *)klfb_DrawElements, (void **)&g_real_DrawElements},
    {"glDrawArrays",   (void *)klfb_DrawArrays,   (void **)&g_real_DrawArrays},
    {"glDrawRangeElements", (void *)klfb_DrawRangeElements, (void **)&g_real_DrawRangeElements},
    {"glDrawRangeElementsBaseVertex", (void *)klfb_DrawRangeElementsBaseVertex, (void **)&g_real_DrawRangeElementsBaseVertex},
    {"glDrawElementsInstanced", (void *)klfb_DrawElementsInstanced, (void **)&g_real_DrawElementsInstanced},
    {"glDrawElementsBaseVertex", (void *)klfb_DrawElementsBaseVertex, (void **)&g_real_DrawElementsBaseVertex},
    {"glDrawArraysInstanced", (void *)klfb_DrawArraysInstanced, (void **)&g_real_DrawArraysInstanced},
    {"glFinish", (void *)klfb_Finish, (void **)&g_real_Finish},
    {"glBlitFramebuffer",         (void *)klfb_BlitFramebuffer,         (void **)&g_real_BlitFramebuffer},
    {"glInvalidateFramebuffer",   (void *)klfb_InvalidateFramebuffer,   (void **)&g_real_InvalidateFramebuffer},
    {"glClear",                   (void *)klfb_Clear,                   (void **)&g_real_Clear},
    {"glClearColor",              (void *)klfb_ClearColor,              (void **)&g_real_ClearColor},
    {"glRenderbufferStorageMultisample", (void *)klfb_RenderbufferStorageMultisample, (void **)&g_real_RenderbufferStorageMultisample},
    {"glTexStorage2DMultisample", (void *)klfb_TexStorage2DMultisample, (void **)&g_real_TexStorage2DMultisample},
    {"glRenderbufferStorage",     (void *)klfb_RenderbufferStorage,     (void **)&g_real_RenderbufferStorage},
    // GL_EXT_multisampled_render_to_texture — implicit resolve, downgraded to
    // single-sample (see the block near klfb_force_ss). ANGLE-Metal never
    // resolved these into the texture, leaving Source's eye black.
    {"glFramebufferTexture2DMultisampleEXT", (void *)klfb_FramebufferTexture2DMultisampleEXT, (void **)&g_real_FramebufferTexture2DMultisampleEXT_ss},
    {"glRenderbufferStorageMultisampleEXT",  (void *)klfb_RenderbufferStorageMultisampleEXT,  (void **)&g_real_RenderbufferStorageMultisampleEXT_ss},
    {"glTexSubImage3D",           (void *)klfb_TexSubImage3D,           (void **)&g_real_TexSubImage3D},
    {"glCompressedTexSubImage2D", (void *)klfb_CompressedTexSubImage2D, (void **)&g_real_CompressedTexSubImage2D},
    {"glCompressedTexSubImage3D", (void *)klfb_CompressedTexSubImage3D, (void **)&g_real_CompressedTexSubImage3D},
    // the shared-context compile lock — see the block above s10's numbers
    {"glShaderSource", (void *)klfb_ShaderSource, (void **)&g_real_ShaderSource},
    {"glCompileShader", (void *)klfb_CompileShader, (void **)&g_real_CompileShader},
    {"glGetShaderiv",   (void *)klfb_GetShaderiv,   (void **)&g_real_GetShaderiv_g},
    {"glLinkProgram",   (void *)klfb_LinkProgram,   (void **)&g_real_LinkProgram},
    // compute kernels as transform feedback (see klfb_cs_rewrite)
    {"glAttachShader",    (void *)klfb_AttachShader,    (void **)&g_real_AttachShader},
    {"glGetProgramiv",    (void *)klfb_GetProgramiv,    (void **)&g_real_GetProgramiv},
    {"glBindBufferBase",  (void *)klfb_BindBufferBase,  (void **)&g_real_BindBufferBase},
    {"glBindBufferRange", (void *)klfb_BindBufferRange, (void **)&g_real_BindBufferRange},
    {"glDispatchCompute", (void *)klfb_DispatchCompute, (void **)&g_res_sink},
    {"glMemoryBarrier",   (void *)klfb_MemoryBarrier,   (void **)&g_res_sink},
    // the other way a program gets linked, and the only one that was silent
    {"glProgramBinary", (void *)klfb_ProgramBinary, (void **)&g_real_ProgramBinary},
    // the min-filter writes KL_GLFB_NOMIP can demote (glTexParameteri has its own row)
    {"glTexParameterf", (void *)klfb_TexParameterf, (void **)&g_real_TexParameterf},
    {"glSamplerParameteri", (void *)klfb_SamplerParameteri, (void **)&g_real_SamplerParameteri},
    {"glSamplerParameterf", (void *)klfb_SamplerParameterf, (void **)&g_real_SamplerParameterf},
    // the texture storage census — which format the AGX abort follows
    {"glTexStorage2D", (void *)klfb_TexStorage2D, (void **)&g_real_TexStorage2D},
    {"glTexStorage3D", (void *)klfb_TexStorage3D, (void **)&g_real_TexStorage3D},
    {"glTexSubImage2D", (void *)klfb_TexSubImage2D, (void **)&g_real_TexSubImage2D},
    {"glTexParameteri", (void *)klfb_TexParameteri, (void **)&g_real_TexParameteri},
    {"glGenFramebuffers", (void *)klfb_GenFramebuffers, (void **)&g_real_GenFramebuffers},
    {"glBindFramebuffer", (void *)klfb_BindFramebuffer, (void **)&g_real_BindFramebuffer},
    {"glFramebufferTextureMultiviewOVR", (void *)klfb_FramebufferTextureMultiviewOVR,
     (void **)&g_real_FramebufferTextureMultiviewOVR},
    {"glFramebufferRenderbuffer", (void *)klfb_FramebufferRenderbuffer,
     (void **)&g_real_FramebufferRenderbuffer},
    {"glCopyTexImage2D", (void *)klfb_CopyTexImage2D, (void **)&g_real_CopyTexImage2D},
    {"glCopyTexSubImage2D", (void *)klfb_CopyTexSubImage2D, (void **)&g_real_CopyTexSubImage2D},
    // the object census — every gen/delete pair, so a class that never shrinks
    // names itself (KL_GL_CENSUS)
    {"glDeleteFramebuffers", (void *)klfb_DeleteFramebuffers, (void **)&g_real_DeleteFramebuffers},
    {"glGenTextures",     (void *)klfb_GenTextures,     (void **)&g_real_GenTextures},
    {"glDeleteTextures",  (void *)klfb_DeleteTextures,  (void **)&g_real_DeleteTextures},
    {"glGenRenderbuffers",    (void *)klfb_GenRenderbuffers,    (void **)&g_real_GenRenderbuffers},
    {"glDeleteRenderbuffers", (void *)klfb_DeleteRenderbuffers, (void **)&g_real_DeleteRenderbuffers},
    {"glGenBuffers",      (void *)klfb_GenBuffers,      (void **)&g_real_GenBuffers},
    {"glDeleteBuffers",   (void *)klfb_DeleteBuffers,   (void **)&g_real_DeleteBuffers},
    {"glGenVertexArrays", (void *)klfb_GenVertexArrays, (void **)&g_real_GenVertexArrays},
    {"glDeleteVertexArrays", (void *)klfb_DeleteVertexArrays, (void **)&g_real_DeleteVertexArrays},
    {"glCreateShader",  (void *)klfb_CreateShader,  (void **)&g_real_CreateShader},
    {"glDeleteShader",  (void *)klfb_DeleteShader,  (void **)&g_real_DeleteShader},
    {"glCreateProgram", (void *)klfb_CreateProgram, (void **)&g_real_CreateProgram},
    {"glDeleteProgram", (void *)klfb_DeleteProgram, (void **)&g_real_DeleteProgram},
    {"glTexImage2D",    (void *)klfb_TexImage2D,    (void **)&g_real_TexImage2D},
    {"glFramebufferTexture2D", (void *)klfb_FramebufferTexture2D, (void **)&g_real_FramebufferTexture2D},
    {"glFramebufferTextureLayer", (void *)klfb_FramebufferTextureLayer, (void **)&g_real_FramebufferTextureLayer},
    // the hybrid query family — ours if the capability tables describe the
    // pname, ANGLE's otherwise (see the gateway comment below)
    {"glGetIntegerv", (void *)klfb_GetIntegerv, (void **)&g_real_GetIntegerv},
    {"glGetFloatv",   (void *)klfb_GetFloatv,   (void **)&g_real_GetFloatv},
    {"glGetBooleanv", (void *)klfb_GetBooleanv, (void **)&g_real_GetBooleanv},
    {"glGetInteger64v", (void *)klfb_GetInteger64v, (void **)&g_real_GetInteger64v},
    {"glGetIntegeri_v", (void *)klfb_GetIntegeri_v, (void **)&g_real_GetIntegeri_v},
    {"glGetInternalformativ", (void *)klfb_GetInternalformativ, (void **)&g_real_GetInternalformativ},
    // the ES 3.1 program-interface family — translated to ES 3.0 introspection
    // in the block above; the real pointers self-resolve via asym, so the
    // table's real slot is a sink
    {"glGetProgramInterfaceiv", (void *)klfb_GetProgramInterfaceiv, (void **)&g_res_sink},
    {"glGetProgramResourceiv",  (void *)klfb_GetProgramResourceiv,  (void **)&g_res_sink},
    {"glGetProgramResourceName",(void *)klfb_GetProgramResourceName,(void **)&g_res_sink},
    {"glGetProgramResourceIndex",(void *)klfb_GetProgramResourceIndex, (void **)&g_res_sink},
    {"glGetProgramResourceLocation",(void *)klfb_GetProgramResourceLocation, (void **)&g_res_sink},
    {"glGetProgramResourceLocationIndex",(void *)klfb_GetProgramResourceLocationIndex, (void **)&g_res_sink},
};

// ---------------------------------------------------------- the gateway
//
// Capability answers stay ours. kl_egl.c tells the guest it is driving a GLES 3.2
// device with no extensions, and Unity built its renderer against that answer;
// letting ANGLE answer instead would change the description underneath a decision
// already made. Everything operational goes to ANGLE.
//
// The query family is the hybrid: the tables answer the pnames they describe,
// and anything else is dynamic *state* (READ_BUFFER, bindings, viewport) that
// only ANGLE tracks. Answering those with the tables' 0 was not neutral: a 0
// for GL_READ_BUFFER became GL_INVALID_ENUM one glReadBuffer later, and the
// eye blit died GL_INVALID_FRAMEBUFFER_OPERATION every frame.
// The query family is the hybrid, but the split is not "table vs rest":
// ANGLE's context here is ES 3.0 while we describe 3.2 to the guest, so a
// capability pname the table does not list (the SSBO family, 0x90d6..) must
// NOT go to ANGLE — there it is a real GL_INVALID_ENUM, and the null driver's
// named zero is exactly what keeps Unity off those paths. Only genuine
// dynamic *state* — Unity's save/restore set around its blits — belongs to
// ANGLE, which alone tracks it. The proof was 0x0C02 (READ_BUFFER): our 0
// became GL_INVALID_ENUM one glReadBuffer later, and the eye blit died
// GL_INVALID_FRAMEBUFFER_OPERATION every frame.
static int glfb_state_pname(uint32_t p) {
    switch (p) {
    case 0x0C01: case 0x0C02:   // DRAW_BUFFER, READ_BUFFER
    case 0x8CA6: case 0x8CAA:   // DRAW/READ_FRAMEBUFFER_BINDING
    case 0x8B8D:                // CURRENT_PROGRAM
    case 0x84E0:                // ACTIVE_TEXTURE
    case 0x0BA2: case 0x0C10:   // VIEWPORT, SCISSOR_BOX
    // Klepton: the object-binding queries a guest's own save/restore set reads
    // around a blit/resolve. These are genuine dynamic STATE that only ANGLE
    // tracks — returning the null driver's 0 makes the guest RESTORE a binding
    // to "nothing", unbinding the very texture/VAO/sampler it was about to use.
    // hl2's gameplay path (HL2Quest_MultiviewResolveEx: resolve the opaque
    // multiview scene target into the OpenXR array swapchain) queries
    // TEXTURE_BINDING_2D_ARRAY / SAMPLER_BINDING / VERTEX_ARRAY_BINDING tens of
    // thousands of times a frame; answering 0 corrupted the resolve so the
    // swapchain kept the last MENU frame and gameplay was never seen (the world
    // renders into the opaque target, then the resolve lands nowhere). The menu
    // is unaffected — it renders BSP DIRECTLY into the swapchain and never asks.
    // All are core ES 3.0 bindings ANGLE's context accepts (no GL_INVALID_ENUM,
    // unlike the ES 3.1+/SSBO capability pnames the tables must keep answering).
    case 0x85B5:                // VERTEX_ARRAY_BINDING
    case 0x8919:                // SAMPLER_BINDING
    case 0x8C1D:                // TEXTURE_BINDING_2D_ARRAY
    case 0x8069:                // TEXTURE_BINDING_2D
    case 0x806A:                // TEXTURE_BINDING_3D
    case 0x8514:                // TEXTURE_BINDING_CUBE_MAP
    case 0x8894: case 0x8895:   // ARRAY_BUFFER / ELEMENT_ARRAY_BUFFER_BINDING
    case 0x8CA7:                // RENDERBUFFER_BINDING
    case 0x8A28:                // UNIFORM_BUFFER_BINDING
    case 0x88ED: case 0x88EF:   // PIXEL_PACK / PIXEL_UNPACK_BUFFER_BINDING
    case 0x8F36: case 0x8F37:   // COPY_READ / COPY_WRITE_BUFFER_BINDING
    // The rest of the save/restore set a guest brackets a pass with. A zero
    // here is not a missing answer but a WRONG one, and it does its damage on
    // the restore: GL_ZERO is a valid blend factor, so a guest that saves
    // BLEND_SRC/DST_RGB around a quad and puts them back gets
    // glBlendFunc(GL_ZERO, GL_ZERO), and everything it draws afterwards blends
    // to black (the Simpsons Hit & Run VR port's HUD minimap). The equations
    // and the depth/cull enums are the same family. All are ES 3.0 state that
    // ANGLE's context accepts.
    case 0x80C8: case 0x80C9:   // BLEND_DST_RGB, BLEND_SRC_RGB
    case 0x80CA: case 0x80CB:   // BLEND_DST_ALPHA, BLEND_SRC_ALPHA
    case 0x0BE0: case 0x0BE1:   // BLEND_DST, BLEND_SRC (ES 1.x names, same state)
    case 0x8009: case 0x883D:   // BLEND_EQUATION_RGB, BLEND_EQUATION_ALPHA
    case 0x0B74: case 0x0B45:   // DEPTH_FUNC, CULL_FACE_MODE
    case 0x0B71: case 0x0BE2:   // DEPTH_TEST, BLEND (as integers)
        return 1;
    }
    return 0;
}

// The pnames that stop being ours the moment an extension is adopted from the
// backend. Each is a question ABOUT that extension, so the driver that answered
// "I have it" is the only thing that can answer "and here is what it costs".
// Our tables say 0 to all three, and a guest reading a maximum anisotropy of 0
// turns anisotropy straight back off.
static int glfb_backend_ext_pname(uint32_t pname) {
    if (kl_gl_cap_adopted_extension_count() == 0) return 0;
    return pname == 0x84FF /* MAX_TEXTURE_MAX_ANISOTROPY_EXT */
        || pname == 0x86A2 /* NUM_COMPRESSED_TEXTURE_FORMATS */
        || pname == 0x86A3 /* COMPRESSED_TEXTURE_FORMATS     */;
}

static void klfb_GetIntegerv(uint32_t pname, int32_t *params) {
    if (glfb_state_pname(pname) || glfb_backend_ext_pname(pname)) {
        if (g_real_GetIntegerv) g_real_GetIntegerv(pname, params);
        return;
    }
    if (kl_gl_cap_integerv(pname, params)) return;
    // Same contract as the null driver: named, not guessed — and crucially
    // no GL error set, which forwarding to the ES 3.0 context would do.
    //
    // Said ONCE per pname: a guest that asks every draw asks a hundred thousand
    // times a session, and an unthrottled line here buries the rest of the log.
    static uint32_t said[64]; static int said_n;
    int seen = 0;
    for (int i = 0; i < said_n; i++) if (said[i] == pname) { seen = 1; break; }
    if (!seen) {
        { int n = said_n; if (n < (int)(sizeof said / sizeof said[0])) { said[n] = pname; said_n = n + 1; } }
        fprintf(stderr, "  [glfb] glGetIntegerv: unhandled pname 0x%x (kept ours; said once)\n", pname);
    }
    if (params) params[0] = 0;
}
static void klfb_GetFloatv(uint32_t pname, float *data) {
    if (kl_gl_cap_floatv(pname, data)) return;
    if (g_real_GetFloatv) g_real_GetFloatv(pname, data);
}
static void klfb_GetBooleanv(uint32_t pname, uint8_t *data) {
    if (g_real_GetBooleanv) g_real_GetBooleanv(pname, data);   // no table: all state
}
static void klfb_GetInteger64v(uint32_t pname, int64_t *data) {
    if (kl_gl_cap_integer64v(pname, data)) return;
    if (g_real_GetInteger64v) g_real_GetInteger64v(pname, data);
}
static void klfb_GetIntegeri_v(uint32_t target, uint32_t index, int32_t *data) {
    // Indexed *binding* state is dynamic, like the pnames above.
    switch (target) {
    case 0x8A28: case 0x8A29: case 0x8A2A:          // UNIFORM_BUFFER_*
    case 0x8C8C: case 0x8C8D: case 0x8C8F:          // TRANSFORM_FEEDBACK_BUFFER_*
    case 0x8E51:                                    // SAMPLE_MASK_VALUE
        if (g_real_GetIntegeri_v) { g_real_GetIntegeri_v(target, index, data); return; }
    }
    if (kl_gl_cap_integeri_v(target, index, data)) return;
    fprintf(stderr, "  [glfb] glGetIntegeri_v: unhandled target 0x%x (kept ours)\n", target);
    if (data) data[0] = 0;
}
static void klfb_GetInternalformativ(uint32_t target, uint32_t internalformat,
                                     uint32_t pname, int32_t bufSize, int32_t *params) {
    if (kl_gl_cap_internalformativ(target, internalformat, pname, bufSize, params)) return;
    if (g_real_GetInternalformativ)
        g_real_GetInternalformativ(target, internalformat, pname, bufSize, params);
}

static const char *const g_keep_ours[] = {
    "glGetString", "glGetStringi",
};

static int keep_ours(const char *name) {
    for (size_t i = 0; i < sizeof g_keep_ours / sizeof g_keep_ours[0]; i++)
        if (strcmp(g_keep_ours[i], name) == 0) return 1;
    return 0;
}

// KL_GLFB_SKIP=<comma-separated names> hands the listed entry points back to the
// caller, which means they fall through to the null driver and do nothing. That
// makes the ANGLE path *bisectable* by family without a rebuild: no-out the
// compressed uploads, or the blits, or the draws, and see which one the AGX abort
// follows. It is a diagnostic, not a mode — the resulting frame is wrong by
// construction, since whole classes of call have been dropped on the floor.
//
// This exists because under KL_GLFB the guest gets ANGLE's real function pointers
// and kl_egl's call counters never see them: the GL report says "called: 0" on a
// run that plainly called plenty. A per-name tracing trampoline is the other way
// to get that visibility and needs hand-written asm to preserve the arguments
// across the log call; this needed none.
static int glfb_skipped(const char *name) {
    const char *list = kl_env_str("KL_GLFB_SKIP", NULL);
    if (!list || !*list) return 0;
    size_t n = strlen(name);
    for (const char *p = list; *p; ) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == n && strncmp(p, name, n) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

// ------------------------------------------------------------- the self-test
//
// KL_GLFB_SELFTEST=1 runs a trivially correct piece of GL — create a sharing
// context, clear it, read it back — on a *host* thread inside the loaded guest
// process, right after init.
//
// The question it settles is which side is broken. Both ANGLE backends fail the
// same way in this process and neither fails in spikes/s10_shared.c: under Metal
// AGX cannot build a BlitComputeProgramVariant, and under ANGLE's OpenGL backend
// Apple's GLD asserts `clearFunction != 0` in getClearShaderFragmentFunction. Two
// different drivers, both failing to obtain one of their own *built-in* shaders.
// That is a strange thing for guest GL usage to cause and a very ordinary thing
// for a poisoned process to cause.
//
//   selftest aborts  -> the process is the problem: something the shim does
//                       globally (signal handlers, TSD, environment, fork state)
//                       breaks Apple's shader compilation for everyone in it, and
//                       the guest's GL is incidental.
//   selftest passes  -> the process is fine and the guest's own GL is doing
//                       something the drivers dislike; back to bisecting calls.
//
// A host thread, not a guest one, and plain pthread_create: no kl_thread_init, no
// TLS slot, no x18 veneers in the frame. This is deliberately the *most* ordinary
// GL any thread could ask for.
static void *klfb_selftest_thread(void *arg) {
    (void)arg;
    void *(*mkpb)(void *, void *, const int32_t *) = a_eglCreatePbufferSurface;
    void *(*mkctx)(void *, void *, void *, const int32_t *) = a_eglCreateContext;
    if (!mkpb || !mkctx || !a_eglMakeCurrent) return NULL;

    const int32_t surf_attrs[] = { EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE };
    const int32_t ctx_attrs[]  = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    void *surf = mkpb(g_dpy, g_cfg, surf_attrs);
    void *ctx  = mkctx(g_dpy, g_cfg, g_ctx, ctx_attrs);
    fprintf(stderr, "  [glfb] selftest: surf=%p ctx=%p\n", surf, ctx);
    if (!surf || !ctx) return NULL;
    if (!a_eglMakeCurrent(g_dpy, surf, surf, ctx)) {
        fprintf(stderr, "  [glfb] selftest: eglMakeCurrent failed\n");
        return NULL;
    }

    void (*clearcolor)(float, float, float, float) = asym("glClearColor");
    void (*clear)(uint32_t) = asym("glClear");
    if (clearcolor) clearcolor(0.25f, 0.5f, 0.75f, 1.0f);
    fprintf(stderr, "  [glfb] selftest: about to glClear (the driver builds its "
                    "internal clear shader here)\n");
    if (clear) clear(0x4000 /* GL_COLOR_BUFFER_BIT */);
    if (a_glFinish) a_glFinish();
    fprintf(stderr, "  [glfb] selftest: glClear survived\n");

    unsigned char px[16] = {0};
    if (a_glReadPixels) a_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    fprintf(stderr, "  [glfb] selftest: readback %u,%u,%u,%u — PASSED (the process "
                    "is fine; the guest's own GL is the difference)\n",
            px[0], px[1], px[2], px[3]);
    a_eglMakeCurrent(g_dpy, NULL, NULL, NULL);
    return NULL;
}

static void klfb_selftest(void) {
    if (!kl_env_on("KL_GLFB_SELFTEST", 0)) return;
    pthread_t th;
    if (pthread_create(&th, NULL, klfb_selftest_thread, NULL) == 0)
        pthread_join(th, NULL);
}

// ---------------------------------------------------------- the call trace
//
// KL_GLFB_TRACE=1 routes every entry point the guest resolves through a per-name
// forwarding stub, so the log names the exact GL call rather than the family a
// census can reach. The texture trace can only say a crash arrived somewhere
// after an R8 upload, and "somewhere after" spans every call the engine makes
// next.
//
// It is expensive by construction — a log line per GL call — so it is off unless
// asked for. KL_GLFB_TRACE_FROM=<n> starts printing only after the nth call, which
// is how a crash tens of thousands of calls in stays readable.
typedef struct { const char *name; void *real; } klfb_trace_desc;

extern void kl_gl_trace_tramp(void);       // runtime/gfx/kl_gl_trace.S
extern void kl_gl_lock_tramp(void);        // runtime/gfx/kl_gl_lock.S

// KL_GLFB_LOCK=1 wraps every resolved GL entry point in a process-wide lock,
// held ACROSS the call (kl_gl_lock.S). It tests whether the transient
// INVALID_FRAMEBUFFER_OPERATION under KL_GLFB_SHARED is Bug 1 — ANGLE's
// shared-context thread safety — at draw granularity: if the frame renders
// with this on, it is. It serialises all guest GL, so it is a probe, not a
// mode.
//
// The trampoline cannot keep the guest's return address in a register across
// the call (x30 dies at every bl, and any callee-saved register is the
// guest's own — clobbering x19 with it once presented as a SIGBUS in guest
// code, the guest writing through its own ra). It is parked here on a
// per-thread stack instead.
static pthread_mutex_t g_gl_lock = PTHREAD_MUTEX_INITIALIZER;
#define KLGL_LOCK_MAXDEPTH 8
static _Thread_local uint64_t g_ra_stack[KLGL_LOCK_MAXDEPTH];
static _Thread_local unsigned g_ra_depth;
void kl_gl_lock_acquire(uint64_t ra) {
    if (g_ra_depth < KLGL_LOCK_MAXDEPTH) g_ra_stack[g_ra_depth++] = ra;
    pthread_mutex_lock(&g_gl_lock);
}
uint64_t kl_gl_lock_release(void) {
    pthread_mutex_unlock(&g_gl_lock);
    return g_ra_depth ? g_ra_stack[--g_ra_depth] : 0;
}

static unsigned g_trace_calls;
static unsigned g_trace_from;

// Called from the trampoline with the argument registers already saved. Anything
// this touches must be safe on any guest thread, so it stays to stderr and a
// relaxed counter — no allocation, no locks worth deadlocking on.
// KL_GLFB_ERRSCAN=1: name the call that GENERATES a GL error, anywhere in the
// stream — not just the hand-wrapped few ERRSRC covers. The trampoline calls
// this BEFORE the real call, so a non-zero glGetError on entry to call N was
// produced by call N-1: record the previous name and blame it. This is what
// attributes Unity's "OPENGL NATIVE PLUG-IN ERROR" spam, which is only ever
// observed later, at a plugin-event boundary, long after the guilty call.
//
// Two things it changes by existing: it CLEARS the error flag, so the guest's
// own glGetError sees nothing and the Unity spam stops (that disappearance is
// itself confirmation the scan is seeing the same errors); and g_prev is a
// plain cross-thread global, so with several guest threads in GL the blame can
// land one call off. Diagnostic only.
// KL_GLFB_ERRSCAN=<code> reports only that GL error, so the known sticky
// INVALID_ENUM from TEXTURE_SRGB_DECODE_EXT (0x8a48, absent on ANGLE's ES 3.0)
// does not spend the whole report budget thousands of calls before the
// interesting one. =1 reports every code.
static const char *g_err_prev;
static uint32_t g_err_only;
static int getenv_trace_print(void);
static int glfb_errscan(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = kl_env_str("KL_GLFB_ERRSCAN", NULL);
        on = e != NULL;
        if (e) {
            uint32_t v = (uint32_t)strtoul(e, NULL, 0);
            if (v > 1) g_err_only = v;
        }
    }
    return on;
}

// GL_INVALID_FRAMEBUFFER_OPERATION says a command ran against a framebuffer GL
// considers incomplete, and names neither the framebuffer nor the reason. Both
// are askable, and without them the error is only ever "something, somewhere":
// VRChat's world load dies right after one of these, and which attachment is
// wrong is the whole question.
//
// Everything here is a QUERY — no state is set — and the error queue is drained
// afterwards so this cannot become the thing the next scan reports — an
// instrument must not perturb what it measures.
static const char *glfb_fb_status_name(uint32_t s) {
    switch (s) {
    case 0x8CD5: return "COMPLETE";
    case 0x8CD6: return "INCOMPLETE_ATTACHMENT";
    case 0x8CD7: return "INCOMPLETE_MISSING_ATTACHMENT";
    case 0x8CD9: return "INCOMPLETE_DIMENSIONS";
    case 0x8CDD: return "UNSUPPORTED";
    case 0x8D56: return "INCOMPLETE_MULTISAMPLE";
    case 0x8219: return "UNDEFINED";
    case 0x9317: return "INCOMPLETE_LAYER_TARGETS";
    default:     return "?";
    }
}

static void glfb_report_incomplete_fb(const char *what) {
    if (!a_glCheckFramebufferStatus || !a_glGetIntegerv) return;
    int32_t draw = 0, read = 0;
    a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &draw);
    a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &read);
    uint32_t st = a_glCheckFramebufferStatus(0x8CA9 /* DRAW_FRAMEBUFFER */);
    fprintf(stderr, "  [glfb] ...%s drew into draw_fb=%d (read_fb=%d): "
                    "status 0x%x %s\n",
            what, draw, read, st, glfb_fb_status_name(st));

    if (a_glGetFramebufferAttachmentParameteriv) {
        static const struct { uint32_t p; const char *n; } att[] = {
            { 0x8CE0, "COLOR0" }, { 0x8CE1, "COLOR1" },
            { 0x8D00, "DEPTH"  }, { 0x8D20, "STENCIL" },
        };
        for (size_t i = 0; i < sizeof att / sizeof att[0]; i++) {
            int32_t type = 0, obj = 0, layered = 0;
            a_glGetFramebufferAttachmentParameteriv(
                0x8CA9, att[i].p, 0x8CD0 /* OBJECT_TYPE */, &type);
            if (type == 0) continue;                    // GL_NONE: not attached
            a_glGetFramebufferAttachmentParameteriv(
                0x8CA9, att[i].p, 0x8CD1 /* OBJECT_NAME */, &obj);
            a_glGetFramebufferAttachmentParameteriv(
                0x8CA9, att[i].p, 0x8210 /* COMPONENT_TYPE */, &layered);
            fprintf(stderr, "  [glfb]      %-8s type=0x%x name=%d componentType=0x%x\n",
                    att[i].n, type, obj, layered);
        }
    }

    // ...and what the GUEST asked for, which is the other half of the question.
    // GL reporting a binding the guest never named means the bind was refused
    // (candidate b) or ANGLE is reporting a sentinel (candidate a); the two need
    // different fixes and are indistinguishable from the query alone.
    fprintf(stderr, "  [glfb]      guest shadow: draw_fb=%u read_fb=%u\n",
            g_draw_fb, g_read_fb);
    unsigned n = __atomic_load_n(&g_bindlog_n, __ATOMIC_RELAXED);
    unsigned first = n > KLFB_BINDLOG ? n - KLFB_BINDLOG : 0;
    for (unsigned k = first; k < n; k++) {
        unsigned i = k % KLFB_BINDLOG;
        size_t off = 0;
        const char *img = kl_addr_image((void *)g_bindlog[i].site, &off);
        char err[32];
        if (g_bindlog_err[i] == 0xffffffffu) snprintf(err, sizeof err, "err=?");
        else if (g_bindlog_err[i] == 0)      snprintf(err, sizeof err, "ok");
        else snprintf(err, sizeof err, "REFUSED 0x%x", g_bindlog_err[i]);
        fprintf(stderr, "  [glfb]      bind#%u t%llu (0x%x, %u) %s ctx=%p <- %s+0x%zx\n",
                k, (unsigned long long)g_bindlog[i].tid,
                g_bindlog[i].target, g_bindlog[i].fb, err, g_bindlog[i].ctx,
                img ? img : "?", off);
    }
    while (a_glGetError && a_glGetError()) {}
}

void kl_gl_trace_log(const char *name) {
    unsigned n = __atomic_fetch_add(&g_trace_calls, 1, __ATOMIC_RELAXED);
    if (glfb_errscan() && a_glGetError) {
        // A budget per CALL NAME, not one for the run: forty reports in all were
        // spent on the first thing that failed every frame, and whatever failed
        // after it was never named. Racy across threads, which costs a line.
        static struct { const char *name; unsigned n; } seen[48];
        uint32_t e = a_glGetError();
        int say = 0;
        if (e && (!g_err_only || e == g_err_only)) {
            const char *who = g_err_prev ? g_err_prev : "(nothing yet)";
            for (unsigned i = 0; i < 48; i++) {
                if (!seen[i].name) seen[i].name = who;
                if (seen[i].name == who || !strcmp(seen[i].name, who)) {
                    say = seen[i].n++ < 6;
                    break;
                }
            }
        }
        if (say) {
            fprintf(stderr, "  [glfb] ERRSCAN GL error 0x%x generated by %s "
                    "(before #%u %s)\n",
                    e, g_err_prev ? g_err_prev : "(nothing yet)", n, name);
            while (a_glGetError()) {}          // drain the rest of the queue
            // ...and for the one error that is ABOUT a framebuffer, say which.
            if (e == 0x506)
                glfb_report_incomplete_fb(g_err_prev ? g_err_prev : "a call");
        }
        g_err_prev = name;
    }
    if (getenv_trace_print() && n >= g_trace_from)
        fprintf(stderr, "  [gl] #%u %s\n", n, name);
}

// The per-call log line is KL_GLFB_TRACE's; ERRSCAN needs the same trampoline
// but not the firehose, so the two are separate questions.
static int getenv_trace_print(void) {
    static int on = -1;
    if (on < 0) {
        on = kl_env_on("KL_GLFB_TRACE", 0);
        g_trace_from = kl_env_uint("KL_GLFB_TRACE_FROM", g_trace_from);
    }
    return on;
}

static int glfb_tracing(void) {
    return getenv_trace_print() || glfb_errscan();
}

static int glfb_locking(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_LOCK", 0);
    return on;
}

static void *glfb_wrap_traced(const char *name, void *real) {
    if (!real) return real;
    void *tramp = glfb_locking() ? (void *)kl_gl_lock_tramp
                : glfb_tracing() ? (void *)kl_gl_trace_tramp
                : NULL;
    if (!tramp) return real;
    klfb_trace_desc *d = malloc(sizeof *d);
    if (!d) return real;
    d->name = strdup(name);
    d->real = real;
    if (!d->name) { free(d); return real; }
    void *stub = kl_trace_stub(name, d, tramp);
    return stub ? stub : real;
}

// ES 3.2 core entry points the guest is entitled to — GL_VERSION says 3.2 —
// but that ANGLE's ES 3.0 context rejects under their core names ("Command
// requires OpenGL ES 3.2", GL_INVALID_OPERATION, nothing done). ANGLE carries
// the same functions as EXT_copy_image and OES/EXT_draw_buffers_indexed, so the
// guest gets those. Unreal (Saints & Sinners) is the measured case: every
// streamed texture's resident mips are moved with glCopyImageSubData, so each
// failed copy left a texture empty (pink, or simply untextured), and its
// per-render-target blend state (glEnablei / glBlendFuncSeparatei /
// glBlendEquationSeparatei / glColorMaski) never reached GL, so translucent
// meshes drew opaque. glCopyImageSubData is not in this table: ANGLE on Metal
// has it under no name, and it has a thunk of its own (klfb_CopyImageSubData).
// On for Unreal guests, where it was seen and sent in twice (fix 15, fix 16;
// 2026-10-11); the answer is right for any guest, but the others were brought
// up with those calls doing nothing. KL_GLFB_ES32_ALIAS=0|1 overrides.
static const struct { const char *core, *ext[2]; } g_es32_alias[] = {
    {"glEnablei",                {"glEnableiOES",                "glEnableiEXT"}},
    {"glDisablei",               {"glDisableiOES",               "glDisableiEXT"}},
    {"glIsEnabledi",             {"glIsEnablediOES",             "glIsEnablediEXT"}},
    {"glBlendEquationi",         {"glBlendEquationiOES",         "glBlendEquationiEXT"}},
    {"glBlendEquationSeparatei", {"glBlendEquationSeparateiOES", "glBlendEquationSeparateiEXT"}},
    {"glBlendFunci",             {"glBlendFunciOES",             "glBlendFunciEXT"}},
    {"glBlendFuncSeparatei",     {"glBlendFuncSeparateiOES",     "glBlendFuncSeparateiEXT"}},
    {"glColorMaski",             {"glColorMaskiOES",             "glColorMaskiEXT"}},
};

static void *klfb_es32_alias(const char *name) {
    static int on = -1;
    if (on < 0) {
        extern kl_guest_kind kl_driver_kind(void);
        on = kl_env_on("KL_GLFB_ES32_ALIAS", kl_driver_kind() == KL_GUEST_UE4);
    }
    if (!on) return NULL;
    for (size_t i = 0; i < sizeof g_es32_alias / sizeof g_es32_alias[0]; i++) {
        if (strcmp(g_es32_alias[i].core, name) != 0) continue;
        for (int k = 0; k < 2; k++) {
            const char *e = g_es32_alias[i].ext[k];
            void *fn = asym(e);
            if (!fn && a_eglGetProcAddress) fn = a_eglGetProcAddress(e);
            if (fn) {
                fprintf(stderr, "  [glfb] %s -> %s (ES 3.2 core name, served by "
                                "ANGLE's extension)\n", name, e);
                return fn;
            }
        }
        return NULL;
    }
    return NULL;
}

void *kl_glfb_sym(const char *name) {
    if (!kl_glfb_enabled() || !name) return NULL;
    if (!g_ready && !kl_glfb_init()) return NULL;
    if (keep_ours(name)) return NULL;              // caller keeps its own answer
    if (glfb_skipped(name)) {
        fprintf(stderr, "  [glfb] KL_GLFB_SKIP: %s stays on the null driver\n", name);
        return NULL;
    }

    // ANGLE-Metal has no texture_view entry point; serve our emulation for it. (The glTexStorage
    // thunks that record texture dims for it are already wired via g_thunks below.)
    if (strcmp(name, "glTextureViewOES") == 0 || strcmp(name, "glTextureViewEXT") == 0)
        return (void *)klfb_TextureViewOES;
    // glCopyImageSubData under any of its names goes through our thunk
    // whenever there is something to answer it with — the emulation, or an
    // entry point of ANGLE's. Fifteen arguments, seven on the stack: handed
    // ANGLE's function directly, the guest's 8-byte stack slots would be read
    // as 4-byte ones. With neither, the name stays as absent as it was.
    if (strcmp(name, "glCopyImageSubData") == 0 ||
        strcmp(name, "glCopyImageSubDataEXT") == 0 ||
        strcmp(name, "glCopyImageSubDataOES") == 0) {
        static const char *const spell[] = { "glCopyImageSubData", "glCopyImageSubDataEXT",
                                             "glCopyImageSubDataOES" };
        for (int k = 0; k < 3 && !g_real_CopyImageSubData; k++) {
            g_real_CopyImageSubData = asym(spell[k]);
            if (!g_real_CopyImageSubData && a_eglGetProcAddress)
                g_real_CopyImageSubData = a_eglGetProcAddress(spell[k]);
        }
        if (klfb_cimg_on() || g_real_CopyImageSubData)
            return glfb_wrap_traced(name, (void *)klfb_CopyImageSubData);
    }
    void *fn = klfb_es32_alias(name);
    if (!fn) fn = asym(name);
    // Extension entry points (glTextureViewOES, …) aren't dlsym-exported by ANGLE; they resolve
    // only through its eglGetProcAddress. Fall back to it before giving up to the null driver.
    if (!fn && a_eglGetProcAddress && strncmp(name, "gl", 2) == 0) fn = a_eglGetProcAddress(name);
    if (!fn) return NULL;                          // ANGLE has no such entry point
    for (size_t i = 0; i < sizeof g_thunks / sizeof g_thunks[0]; i++)
        if (strcmp(g_thunks[i].name, name) == 0) {
            *g_thunks[i].real = fn;
            // Trace in front of the thunk, so the log reflects what the guest
            // called and the thunk's own ABI fixups still happen.
            return glfb_wrap_traced(name, g_thunks[i].thunk);
        }
    return glfb_wrap_traced(name, fn);
}

// ---------------------------------------------------------- present
static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t h[4]; be32(h, n); fwrite(h, 1, 4, f); fwrite(t, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    uLong c = crc32(0, (const Bytef *)t, 4);
    if (n) c = crc32(c, (const Bytef *)d, n);
    be32(h, (uint32_t)c); fwrite(h, 1, 4, f);
}

// Called from the guest's eglSwapBuffers. The swap does not arrive on the GL
// thread — measured — and under KL_GLFB_SHARED each thread's pbuffer is its
// own, so capturing on the swap thread reads a framebuffer nothing was ever
// drawn into (0 lit, by construction). The swap only *requests* a capture;
// klfb_service_capture does the work from a call the GL thread makes itself:
// glFlush/glFinish, and the draw wrappers, which by definition run where the
// drawing did. Reading pixels with no current context is not an error, it is
// silence, which is exactly how that presented: a perfectly black frame with
// the framebuffer queries writing nothing at all.
//
// KL_GLFB_OUT_EVERY=N throttles to every Nth swap (default 1) — a 300-frame
// pump otherwise writes 300 full-size PNGs. The throttle applies to the PNG
// path only: a registered frame sink is a live display, not a batch of files,
// so it wants every swap and dir may be NULL.
unsigned kl_glfb_present(const char *dir) {
    if (!g_ready || (!dir && !g_frame_sink && !g_gpu_fence)) return g_presented;
    // A registered GPU fence replaces the readback entirely — the compositor
    // samples the eye texture itself and only needs to know when the guest's
    // frame is done. Same thread rule as the capture below: the sync has to be
    // created where the context is current, so a swap off the GL thread defers.
    if (g_gpu_fence) {
        pthread_once(&g_tls_once, klfb_tls_init);
        klfb_thread_gl *tg = pthread_getspecific(g_tls_key);
        if (!(tg && tg->ctx)) { g_capture_pending = 1; return g_presented; }
        unsigned fenced = klfb_gpu_frame_now();
        // With KL_GLFB_OUT the PNG capture runs as well. It used to be skipped
        // whenever a fence was registered, which is always in the app — so the
        // one instrument that shows what the guest drew into its eye textures
        // was inert exactly where a black picture needs it (the simulator,
        // Tales from the Galaxy's Edge, 2026-10-11).
        if (!dir) return fenced;
    }
    static int every = -1;
    if (every < 0) {
        every = kl_env_int("KL_GLFB_OUT_EVERY", 1);
        if (every < 1) every = 1;
    }
    static unsigned swap_n;
    if (!g_frame_sink && swap_n++ % (unsigned)every) return g_presented;
    if (dir) snprintf(g_capture_dir, sizeof g_capture_dir, "%s", dir);
    // If the swap arrived on a thread that owns a context — in migration mode
    // the render thread, which is exactly where the frame was drawn — capture
    // NOW, before the next frame's clears overwrite it. (Capturing later, from
    // the next draw call, measured black even where the frame rendered.)
    pthread_once(&g_tls_once, klfb_tls_init);
    klfb_thread_gl *t = pthread_getspecific(g_tls_key);
    if (t && t->ctx) return glfb_capture_now(dir);
    g_capture_pending = 1;
    return g_presented;
}

// ---- which FBO is the picture in?
//
// fb0 is black by construction (the VR frame goes to eye textures, not the
// backbuffer), and the first census was blind twice over: it scanned only
// fbo1..8, and it read everything as GL_RGBA/UNSIGNED_BYTE — which an RGBA16F
// attachment rejects (INVALID_ENUM, the 0x500 in the draw-probe log) and an
// MSAA attachment rejects outright (INVALID_OPERATION). klfb_probe_fbo reads
// one FBO the way its own color attachment allows: the read format/type come
// from GL_IMPLEMENTATION_COLOR_READ_FORMAT/TYPE, the attachment is identified
// against the allocation table the glTexStorage2D thunk keeps (ES 3.0 has no
// glGetTexLevelParameteriv — the ANGLE extension entry point rejects on this
// context), and MSAA attachments are reported rather than misread.
// Returns the lit count and writes a one-line description into note.
//
// It disturbs GL state (binds the FBO, maybe a renderbuffer), so it belongs
// to the debug paths (PNG census, probe runs), not the sink handoff. The
// caller restores the READ_FRAMEBUFFER binding; the renderbuffer binding is
// saved and restored here.
#define KLFB_IMPLEMENTATION_COLOR_READ_FORMAT 0x8B9B
#define KLFB_IMPLEMENTATION_COLOR_READ_TYPE   0x8B9A
#define KLFB_HALF_FLOAT 0x140B

// IEEE half -> float, for RGBA16F readbacks. Bit-exact exponent rebase; no
// denormal/inf finesse needed for a lit-pixel census and a debug picture.
static float klfb_half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t out;
    if (exp == 0)        out = sign | (mant << 13);              // ~denormal
    else if (exp == 31)  out = sign | 0x7F800000 | (mant << 13); // inf/nan
    else                 out = sign | ((exp + 112) << 23) | (mant << 13);
    float f;
    memcpy(&f, &out, 4);
    return f;
}

static uint8_t klfb_dbg_tone(float c);   // defined with the capture below

static unsigned long klfb_probe_fbo(uint32_t fb, float *fbuf, uint8_t *bbuf,
                                    char *note, size_t note_n,
                                    uint8_t *dump, int32_t *dw, int32_t *dh,
                                    int32_t hint_w, int32_t hint_h) {
    static void (*r_BindFramebuffer)(uint32_t, uint32_t);
    static uint32_t (*r_CheckFramebufferStatus)(uint32_t);
    static void (*r_GetFramebufferAttachmentParameteriv)(uint32_t, uint32_t,
                                                         uint32_t, int32_t *);
    static void (*r_BindRenderbuffer)(uint32_t, uint32_t);
    static void (*r_GetRenderbufferParameteriv)(uint32_t, uint32_t, int32_t *);
    static void (*r_BindTexture2)(uint32_t, uint32_t);
    if (!r_BindFramebuffer) {
        r_BindFramebuffer = asym("glBindFramebuffer");
        r_CheckFramebufferStatus = asym("glCheckFramebufferStatus");
        r_GetFramebufferAttachmentParameteriv =
            asym("glGetFramebufferAttachmentParameteriv");
        r_BindRenderbuffer = asym("glBindRenderbuffer");
        r_GetRenderbufferParameteriv = asym("glGetRenderbufferParameteriv");
        r_BindTexture2 = asym("glBindTexture");
    }
    if (!r_BindFramebuffer || !r_CheckFramebufferStatus ||
        !r_GetFramebufferAttachmentParameteriv || !a_glGetIntegerv ||
        !a_glReadPixels) {
        snprintf(note, note_n, "no GL entry points");
        return 0;
    }
    // Finish first, for EVERY caller. One probe that finishes and one that does
    // not disagree about the same texture in the same run, and a difference
    // between the instruments then reads as a difference in the pipeline. A
    // debug readback can afford the stall.
    if (a_glFinish) a_glFinish();
    r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, fb);
    if (r_CheckFramebufferStatus(0x8CA8) != 0x8CD5) {
        snprintf(note, note_n, "incomplete");
        return 0;
    }
    int32_t otype = 0, oname = 0;
    r_GetFramebufferAttachmentParameteriv(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */,
                                          0x8CD0 /* OBJECT_TYPE */, &otype);
    r_GetFramebufferAttachmentParameteriv(0x8CA8, 0x8CE0, 0x8CD1 /* OBJECT_NAME */,
                                          &oname);
    int32_t fmt = 0, aw = 0, ah = 0, samples = 0;
    uint32_t resolved_via = 0, resolve_err = 0;
    // Which staging format makes this attachment readable, if it isn't
    // already: MSAA renderbuffers resolve to a SAME-FORMAT texture (a
    // float->unorm blit is the forbidden one), and R11F_G11F_B10F textures —
    // the bloom pyramid — blit to RGBA16F, because this driver's own read
    // format for R11F (0x1907/0x8c3b) is one glReadPixels rejects outright.
    // Both are float->float blits, the legal path.
    uint32_t stage_want = 0;
    int hinted = 0;
    if (otype == 0x1702 /* TEXTURE */) {
        uint32_t ufmt = 0;
        klfb_tex_info((uint32_t)oname, &ufmt, &aw, &ah);
        fmt = (int32_t)ufmt;
        if (ufmt == 0x8C3A /* R11F_G11F_B10F */) stage_want = 0x881A;
        // The allocation table is a WATCH, not an oracle: a texture created
        // through a path it does not see (glTexImage2D, or any allocation once
        // the table is full) leaves aw/ah at 0, and the read below then bails
        // out and returns 0 lit — an instrument reporting "black" for a
        // framebuffer it never looked at. That is exactly the reading that made
        // VRChat's eye copy look like a guest drawing nothing. Where the caller
        // knows a size from the call it is bracketing — a blit names its own
        // source and destination rectangles — take it, and SAY the size is a
        // hint so the two are never confused in the log.
        if ((aw <= 0 || ah <= 0) && hint_w > 0 && hint_h > 0) {
            aw = hint_w; ah = hint_h; hinted = 1;
        }
    } else if (otype == 0x8D41 /* RENDERBUFFER */ && r_BindRenderbuffer &&
               r_GetRenderbufferParameteriv) {
        int32_t save_rb = 0;
        a_glGetIntegerv(0x8CA7 /* RENDERBUFFER_BINDING */, &save_rb);
        r_BindRenderbuffer(0x8D41, (uint32_t)oname);
        r_GetRenderbufferParameteriv(0x8D41, 0x8D44 /* INTERNAL_FORMAT */, &fmt);
        r_GetRenderbufferParameteriv(0x8D41, 0x8D42 /* WIDTH */, &aw);
        r_GetRenderbufferParameteriv(0x8D41, 0x8D43 /* HEIGHT */, &ah);
        r_GetRenderbufferParameteriv(0x8D41, 0x8CAB /* SAMPLES */, &samples);
        r_BindRenderbuffer(0x8D41, (uint32_t)save_rb);
        if (samples > 0) stage_want = (uint32_t)fmt;
    }
    if (stage_want) {
        {
            // MSAA can't be read directly, so resolve through a staging FBO of
            // our own with a SAME-FORMAT texture — a float->unorm blit is the
            // forbidden one, an MSAA resolve to the same format is the legal
            // path. This is how the scene renderbuffer (where the geometry
            // draws actually land) becomes readable. Debug path only: it
            // creates two real GL objects on the guest's shared context.
            static void (*r_GenTextures)(int32_t, uint32_t *);
            static void (*r_GenFramebuffers2)(int32_t, uint32_t *);
            static void (*r_TexStorage2D)(uint32_t, int32_t, uint32_t, int32_t,
                                          int32_t);
            static void (*r_FramebufferTexture2D)(uint32_t, uint32_t, uint32_t,
                                                  uint32_t, int32_t);
            static void (*r_BlitFramebuffer)(int32_t, int32_t, int32_t, int32_t,
                                             int32_t, int32_t, int32_t, int32_t,
                                             uint32_t, uint32_t);
            if (!r_GenTextures) {
                r_GenTextures = asym("glGenTextures");
                r_GenFramebuffers2 = asym("glGenFramebuffers");
                r_TexStorage2D = asym("glTexStorage2D");
                r_FramebufferTexture2D = asym("glFramebufferTexture2D");
                r_BlitFramebuffer = asym("glBlitFramebuffer");
            }
            static uint32_t stage_tex, stage_fb;
            static int32_t stage_w, stage_h;
            static uint32_t stage_fmt;
            if (!r_GenTextures || !r_BlitFramebuffer) {
                snprintf(note, note_n, "rb=%d MSAA x%d (no staging entry points)",
                         oname, samples);
                return 0;
            }
            if (!stage_tex) {
                r_GenTextures(1, &stage_tex);
                r_GenFramebuffers2(1, &stage_fb);
            }
            if (stage_want != stage_fmt || aw != stage_w || ah != stage_h) {
                int32_t save_tex = 0, save_draw = 0;
                a_glGetIntegerv(0x8069, &save_tex);
                a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &save_draw);
                r_BindTexture2(0x0DE1, stage_tex);
                r_TexStorage2D(0x0DE1, 1, stage_want, aw, ah);
                r_BindTexture2(0x0DE1, (uint32_t)save_tex);
                r_BindFramebuffer(0x8CA9 /* DRAW_FRAMEBUFFER */, stage_fb);
                r_FramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, stage_tex, 0);
                r_BindFramebuffer(0x8CA9, (uint32_t)save_draw);
                stage_fmt = stage_want; stage_w = aw; stage_h = ah;
            }
            int32_t save_draw = 0;
            a_glGetIntegerv(0x8CA6, &save_draw);
            r_BindFramebuffer(0x8CA9, stage_fb);
            // Sentinel first: a pixel the resolve blit did not write keeps the
            // sentinel colour, so a silently-failing resolve reads as sentinel
            // rather than as whatever the staging texture held before.
            // Without it the same renderbuffer reads back differently on
            // consecutive probes.
            static void (*r_ClearColor)(float, float, float, float);
            static void (*r_Clear)(uint32_t);
            if (!r_ClearColor) {
                r_ClearColor = asym("glClearColor");
                r_Clear = asym("glClear");
            }
            if (r_ClearColor && r_Clear) {
                r_ClearColor(0.6f, 0.1f, 0.9f, 1.0f);   // loud magenta
                r_Clear(0x4000);
            }
            // fb is still bound as READ from the top of the probe. Flush the
            // command stream first: the draw probe reads the same renderbuffer
            // lit where the blit probe reads it black a few calls later, and
            // the draw probe glFinish()es first — the outstanding difference.
            if (a_glFinish) a_glFinish();
            r_BlitFramebuffer(0, 0, aw, ah, 0, 0, aw, ah,
                              0x4000 /* COLOR_BUFFER_BIT */, 0x2600 /* NEAREST */);
            if (a_glGetError) resolve_err = a_glGetError();
            r_BindFramebuffer(0x8CA9, (uint32_t)save_draw);
            r_BindFramebuffer(0x8CA8, stage_fb);
            resolved_via = stage_fb;
            // fall through to the ordinary read, now against the staging FBO
        }
    }
    // A multiview attachment (OVR_multiview, KL_OVRP_GL_ARRAY) cannot be read
    // from the guest's framebuffer at all — glReadPixels answers
    // INVALID_FRAMEBUFFER_OPERATION — so read its first view through a
    // framebuffer of our own holding that one layer. Debug path only.
    if (otype == 0x1702 && !resolved_via) {
        int32_t views = 0;
        r_GetFramebufferAttachmentParameteriv(0x8CA8, 0x8CE0,
                                              0x9630 /* TEXTURE_NUM_VIEWS_OVR */, &views);
        if (a_glGetError) a_glGetError();
        static void (*r_FramebufferTextureLayer)(uint32_t, uint32_t, uint32_t, int32_t, int32_t);
        static void (*r_GenFramebuffers3)(int32_t, uint32_t *);
        static uint32_t layer_fb;
        if (!r_FramebufferTextureLayer) {
            r_FramebufferTextureLayer = asym("glFramebufferTextureLayer");
            r_GenFramebuffers3 = asym("glGenFramebuffers");
        }
        if (views > 1 && r_FramebufferTextureLayer && r_GenFramebuffers3) {
            if (!layer_fb) r_GenFramebuffers3(1, &layer_fb);
            r_BindFramebuffer(0x8CA8, layer_fb);
            r_FramebufferTextureLayer(0x8CA8, 0x8CE0, (uint32_t)oname, 0, 0);
            if (aw <= 0 || ah <= 0) { aw = g_w; ah = g_h; hinted = 1; }
            resolved_via = layer_fb;
        }
    }
    int32_t rfmt = 0, rtype = 0;
    a_glGetIntegerv(KLFB_IMPLEMENTATION_COLOR_READ_FORMAT, &rfmt);
    a_glGetIntegerv(KLFB_IMPLEMENTATION_COLOR_READ_TYPE, &rtype);
    // The LAYER, for an array attachment. Naming the texture without it is not
    // an identification: both eyes of an OpenXR guest are slices of one name,
    // so "tex=29" reads identically for a lit eye and an empty one, and the two
    // readings differ only here.
    int32_t alayer = 0, alevel = 0;
    if (otype == 0x1702) {
        r_GetFramebufferAttachmentParameteriv(0x8CA8, 0x8CE0,
                                              0x8CD4 /* TEXTURE_LAYER */, &alayer);
        // The LEVEL, for the same reason as the layer: a name plus a layer is
        // still not an identification while a mip level can differ, and two
        // framebuffers on one texture reading differently is exactly what that
        // looks like.
        r_GetFramebufferAttachmentParameteriv(0x8CA8, 0x8CE0,
                                              0x8CD2 /* TEXTURE_LEVEL */, &alevel);
    }
    snprintf(note, note_n, "%s=%d layer=%d level=%d fmt=0x%x %dx%d%s read=0x%x/0x%x",
             otype == 0x1702 ? "tex" : otype == 0x8D41 ? "rb" : "type?",
             oname, alayer, alevel, fmt, aw, ah,
             hinted ? " (size from the caller)" : "", rfmt, rtype);
    if (resolved_via) {
        size_t l = strlen(note);
        snprintf(note + l, note_n - l, " %s resolved-via-fb%u%s%x",
                 samples > 0 ? "MSAA" : "blit", resolved_via,
                 resolve_err ? " resolvebliterr=0x" : "", resolve_err);
    }
    // The buffers are g_w*g_h; a larger attachment (the resized eye textures
    // are 2304x2198 against a 1832x1920 default) reads clipped rather than
    // overflowing.
    if (aw > g_w) aw = g_w;
    if (ah > g_h) ah = g_h;
    if (aw <= 0 || ah <= 0) {
        // Nothing was read. Returning a bare 0 here reads identically to a
        // framebuffer measured and found black, which is the more serious of
        // the two answers — so say which one this is.
        size_t l = strlen(note);
        snprintf(note + l, note_n - l, " — SIZE UNKNOWN, not read");
        return 0;
    }
    if (dw) *dw = aw;
    if (dh) *dh = ah;

    // A bound PIXEL_PACK_BUFFER turns the pointer below into an OFFSET, so
    // glReadPixels writes into the guest's buffer object and leaves ours
    // untouched — no error, and a readback of whatever malloc handed us, which
    // is zeroes. Unity binds one for its async readbacks, so this probe read
    // "black" off a framebuffer that had just been cleared to a visible colour.
    // Pack alignment and row length are the same class of ambient state: they
    // are the guest's, and every one of them silently changes what this reads.
    static void (*r_BindBuffer)(uint32_t, uint32_t);
    static void (*r_PixelStorei)(uint32_t, int32_t);
    if (!r_BindBuffer) { r_BindBuffer = asym("glBindBuffer");
                         r_PixelStorei = asym("glPixelStorei"); }
    int32_t pack_buf = 0, pack_align = 4, pack_rowlen = 0, pack_skip_p = 0, pack_skip_r = 0;
    a_glGetIntegerv(0x88ED /* PIXEL_PACK_BUFFER_BINDING */, &pack_buf);
    a_glGetIntegerv(0x0D05 /* PACK_ALIGNMENT */,  &pack_align);
    a_glGetIntegerv(0x0D02 /* PACK_ROW_LENGTH */, &pack_rowlen);
    a_glGetIntegerv(0x0D04 /* PACK_SKIP_PIXELS */, &pack_skip_p);
    a_glGetIntegerv(0x0D03 /* PACK_SKIP_ROWS */,   &pack_skip_r);
    if (pack_buf && r_BindBuffer) r_BindBuffer(0x88EB /* PIXEL_PACK_BUFFER */, 0);
    if (r_PixelStorei) {
        if (pack_align != 4)  r_PixelStorei(0x0D05, 4);
        if (pack_rowlen)      r_PixelStorei(0x0D02, 0);
        if (pack_skip_p)      r_PixelStorei(0x0D04, 0);
        if (pack_skip_r)      r_PixelStorei(0x0D03, 0);
    }
    if (pack_buf || pack_rowlen || pack_skip_p || pack_skip_r || pack_align != 4) {
        size_t l = strlen(note);
        snprintf(note + l, note_n - l, " [guest pack: buf=%d align=%d row=%d "
                 "skip=%d,%d — neutralised]", pack_buf, pack_align, pack_rowlen,
                 pack_skip_p, pack_skip_r);
    }
    // ...and READ_BUFFER, which is the same class again and the one this probe
    // was still blind to: it is PER-FRAMEBUFFER state, and it selects WHICH
    // colour attachment glReadPixels takes. The probe identifies COLOR
    // ATTACHMENT0 and then reads whatever this framebuffer's read buffer names,
    // so an MRT framebuffer pointed at attachment 1 reports attachment 0's
    // texture and attachment 1's pixels — with no error, which is how two
    // framebuffers on ONE texture can read differently and look impossible.
    static void (*r_ReadBuffer)(uint32_t);
    if (!r_ReadBuffer) r_ReadBuffer = asym("glReadBuffer");
    int32_t read_buf = 0x8CE0;
    a_glGetIntegerv(0x0C02 /* READ_BUFFER */, &read_buf);
    int read_buf_changed = 0;
    if (fb != 0 && read_buf != 0x8CE0 && r_ReadBuffer) {
        r_ReadBuffer(0x8CE0 /* COLOR_ATTACHMENT0 */);
        read_buf_changed = 1;
    }
    if (read_buf != 0x8CE0) {
        size_t l = strlen(note);
        snprintf(note + l, note_n - l, " [read buffer was 0x%x%s]", read_buf,
                 read_buf_changed ? " — neutralised to COLOR_ATTACHMENT0" : "");
    }
    if (a_glGetError) while (a_glGetError()) {}        // drain guest leftovers
    unsigned long lit = 0, sentinel = 0;
    float fsum = 0;
    if (rtype == 0x1406 /* FLOAT */) {
        a_glReadPixels(0, 0, aw, ah, GL_RGBA, GL_FLOAT, fbuf);
        for (int32_t i = 0; i < aw * ah; i++) {
            float r = fbuf[i * 4], g = fbuf[i * 4 + 1], b = fbuf[i * 4 + 2];
            if (resolved_via && r > 0.55f && r < 0.65f && g > 0.05f &&
                g < 0.15f && b > 0.85f && b < 0.95f) { sentinel++; continue; }
            if (dump) {
                dump[i * 4 + 0] = klfb_dbg_tone(r);
                dump[i * 4 + 1] = klfb_dbg_tone(g);
                dump[i * 4 + 2] = klfb_dbg_tone(b);
                dump[i * 4 + 3] = 255;
            }
            float lum = r + g + b;
            fsum += lum;
            if (lum > 0.05f) lit++;
        }
    } else if (rtype == KLFB_HALF_FLOAT) {
        // 8 bytes a pixel — fits in fbuf, which is sized for 16.
        uint16_t *hbuf = (uint16_t *)fbuf;
        a_glReadPixels(0, 0, aw, ah, GL_RGBA, KLFB_HALF_FLOAT, hbuf);
        for (int32_t i = 0; i < aw * ah; i++) {
            float r = klfb_half_to_float(hbuf[i * 4]);
            float g = klfb_half_to_float(hbuf[i * 4 + 1]);
            float b = klfb_half_to_float(hbuf[i * 4 + 2]);
            if (resolved_via && r > 0.55f && r < 0.65f && g > 0.05f &&
                g < 0.15f && b > 0.85f && b < 0.95f) { sentinel++; continue; }
            if (dump) {
                dump[i * 4 + 0] = klfb_dbg_tone(r);
                dump[i * 4 + 1] = klfb_dbg_tone(g);
                dump[i * 4 + 2] = klfb_dbg_tone(b);
                dump[i * 4 + 3] = 255;
            }
            float lum = r + g + b;
            fsum += lum;
            if (lum > 0.05f) lit++;
        }
    } else {
        a_glReadPixels(0, 0, aw, ah, GL_RGBA, GL_UNSIGNED_BYTE, bbuf);
        if (dump) memcpy(dump, bbuf, (size_t)aw * ah * 4);
        for (int32_t i = 0; i < aw * ah; i++) {
            unsigned lum = bbuf[i * 4] + bbuf[i * 4 + 1] + bbuf[i * 4 + 2];
            fsum += (float)lum / 255.0f;
            if (lum > 12) lit++;
        }
    }
    uint32_t err = a_glGetError ? a_glGetError() : 0;
    // Put the guest's pixel-store state back before anything else runs: this is
    // a debug read on the guest's own context, and leaving it changed would
    // turn an instrument into a cause.
    if (pack_buf && r_BindBuffer) r_BindBuffer(0x88EB, (uint32_t)pack_buf);
    if (r_PixelStorei) {
        if (pack_align != 4)  r_PixelStorei(0x0D05, pack_align);
        if (pack_rowlen)      r_PixelStorei(0x0D02, pack_rowlen);
        if (pack_skip_p)      r_PixelStorei(0x0D04, pack_skip_p);
        if (pack_skip_r)      r_PixelStorei(0x0D03, pack_skip_r);
    }
    if (read_buf_changed && r_ReadBuffer) r_ReadBuffer((uint32_t)read_buf);
    if (err) {
        size_t l = strlen(note);
        snprintf(note + l, note_n - l, " readerr=0x%x", err);
        return 0;
    }
    {
        size_t l = strlen(note);
        snprintf(note + l, note_n - l, " mean %.4f",
                 aw * ah ? fsum / ((float)aw * ah) : 0.0f);
        if (resolved_via) {
            l = strlen(note);
            snprintf(note + l, note_n - l, " sentinel=%lu", sentinel);
        }
    }
    return lit;
}

// Debug transform, not color management: clamp HDR into [0,1] and apply a
// plain 1/2.2 gamma so linear-space float content is visible on a monitor.
// The reference renderer's job is "is there a picture", not colorimetry.
static uint8_t klfb_dbg_tone(float c) {
    // Debug-only display curve, env-tunable: KL_GLFB_EXPOSURE scales the
    // linear value first, KL_GLFB_GAMMA overrides the 1/2.2 encode exponent.
    // Statics: the 64K LUT caller memoizes this, so the env is read once.
    static float expo = -1, gam = -1;
    if (expo < 0) {
        expo = kl_env_float("KL_GLFB_EXPOSURE", 1.0f);
        gam = kl_env_float("KL_GLFB_GAMMA", 1.0f / 2.2f);
        if (expo <= 0) expo = 1.0f;
        if (gam <= 0) gam = 1.0f / 2.2f;
    }
    if (!(c > 0)) return 0;                    // also NaN
    c *= expo;
    if (c > 1) c = 1;
    return (uint8_t)(powf(c, gam) * 255.0f + 0.5f);
}

// 8-bit RGBA PNG, bottom-up GL rows flipped to top-down. Shared by the swap
// capture and the per-FBO dump; returns 0 on any failure (path, alloc, zlib).
static int klfb_write_png(const char *path, const uint8_t *px,
                          int32_t w, int32_t h) {
    size_t   stride = (size_t)w * 4;
    size_t   raw_n = (stride + 1) * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    uLongf   cn  = compressBound((uLong)raw_n);
    uint8_t *cb  = malloc(cn);
    if (!raw || !cb) { free(raw); free(cb); return 0; }
    for (int32_t y = 0; y < h; y++) {            // GL bottom-up -> PNG top-down
        raw[(stride + 1) * (size_t)y] = 0;
        memcpy(raw + (stride + 1) * (size_t)y + 1,
               px + stride * (size_t)(h - 1 - y), stride);
    }
    if (compress(cb, &cn, raw, (uLong)raw_n) != Z_OK) {
        free(raw); free(cb); return 0;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(cb); return 0; }
    static const uint8_t sig[8] = {0x89,'P','N','G',13,10,26,10};
    fwrite(sig, 1, 8, f);
    uint8_t ih[13]; be32(ih, (uint32_t)w); be32(ih + 4, (uint32_t)h);
    ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    chunk(f, "IDAT", cb, (uint32_t)cn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(cb);
    return 1;
}

// Make a texture readable without going through anything the guest bound.
//
// The FBO scan below asks "which of the guest's framebuffers has an eye texture
// attached", and on the OpenXR path that question can have no answer while the
// frame is perfectly fine: a swapchain rotates three images, and the FBO's
// attachment is whatever the guest last pointed it at, not necessarily the
// image it just presented. Falling back to fb0 there is the worst possible
// answer, because fb0 is black by construction — a run with pictures and a run
// with none read identically. Attaching the named image to a framebuffer of our
// own removes the guest from the question entirely.
//
// Returns the framebuffer, bound as READ_FRAMEBUFFER, or 0. The caller restores
// the previous binding.
// Attach one LAYER of a 2D array texture. Same shape as klfb_read_from_texture
// and deliberately separate: glFramebufferTexture2D takes a target and cannot
// name a slice, so an array eye needs glFramebufferTextureLayer or it is not
// readable at all — which is not a degraded picture, it is an incomplete
// framebuffer and a black PNG.
static uint32_t klfb_read_from_texture_layer(uint32_t tex, int layer) {
    static void (*r_GenFramebuffers)(int32_t, uint32_t *);
    static void (*r_BindFramebuffer)(uint32_t, uint32_t);
    static void (*r_FramebufferTextureLayer)(uint32_t, uint32_t, uint32_t, int32_t, int32_t);
    static uint32_t (*r_CheckFramebufferStatus)(uint32_t);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        r_GenFramebuffers = asym("glGenFramebuffers");
        r_BindFramebuffer = asym("glBindFramebuffer");
        r_FramebufferTextureLayer = asym("glFramebufferTextureLayer");
        r_CheckFramebufferStatus = asym("glCheckFramebufferStatus");
    }
    if (!tex || !r_GenFramebuffers || !r_BindFramebuffer ||
        !r_FramebufferTextureLayer || !r_CheckFramebufferStatus) return 0;
    static void (*r_FramebufferTexture2D)(uint32_t, uint32_t, uint32_t,
                                          uint32_t, int32_t);
    static void (*r_GetFbAttachmentParam)(uint32_t, uint32_t, uint32_t, int32_t *);
    if (!r_FramebufferTexture2D) {
        r_FramebufferTexture2D = asym("glFramebufferTexture2D");
        r_GetFbAttachmentParam = asym("glGetFramebufferAttachmentParameteriv");
    }
    static uint32_t layer_fb;
    if (!layer_fb) r_GenFramebuffers(1, &layer_fb);
    if (!layer_fb) return 0;
    r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, layer_fb);
    // The attach that CANNOT silently succeed on the wrong texture. A layered
    // attach only works on an ARRAY or 3D texture; on a plain 2D texture
    // glFramebufferTextureLayer is INVALID_OPERATION and a NO-OP — it neither
    // attaches the new texture NOR detaches whatever was on this reused FBO from
    // the last call. The old code then saw the STALE attachment still complete
    // and returned it, so every 2D-texture probe read the first texture ever
    // attached (a bright eye) instead of its own. So: pick the attach that
    // matches the layer, then VERIFY the colour attachment is actually `tex`
    // before trusting COMPLETE — a stale-but-complete FBO must read as a miss.
    // OpenXR states layer=-1 ("not an array") for the per-eye-swapchain case, so
    // any NEGATIVE layer is a whole-2D-texture attach. layer 0 is a real ARRAY
    // slice (an array swapchain's left eye / imageArrayIndex 0) and MUST use
    // glFramebufferTextureLayer — the old `layer > 0` sent slice 0 down the 2D
    // path, where glFramebufferTexture2D on an array texture is INVALID and the
    // read came back "no framebuffer" for every left eye (Klepton array probe).
    if (layer >= 0) {
        r_FramebufferTextureLayer(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */, tex, 0,
                                  layer);
    } else if (r_FramebufferTexture2D) {
        r_FramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1 /* TEXTURE_2D */, tex, 0);
    } else {
        return 0;
    }
    uint32_t attach_err = a_glGetError ? a_glGetError() : 0;
    uint32_t status = r_CheckFramebufferStatus(0x8CA8);
    if (status != 0x8CD5) {
        static int said;
        if (kl_env_on("KL_GLFB_PROBE_VIDEO", 0) && said++ < 12) {
            uint32_t f = 0; int32_t tw = 0, th = 0; klfb_tex_info(tex, &f, &tw, &th);
            fprintf(stderr, "  [glfb] read_from_texture_layer: tex=%u layer=%d attach "
                            "FAILED status=0x%x attachErr=0x%x texinfo=%dx%d fmt=0x%x\n",
                    tex, layer, status, attach_err, tw, th, f);
        }
        return 0;
    }
    if (r_GetFbAttachmentParam) {
        int32_t oname = 0;
        r_GetFbAttachmentParam(0x8CA8, 0x8CE0, 0x8CD1 /* OBJECT_NAME */, &oname);
        if ((uint32_t)oname != tex) {
            static int said2;
            if (kl_env_on("KL_GLFB_PROBE_VIDEO", 0) && said2++ < 12)
                fprintf(stderr, "  [glfb] read_from_texture_layer: tex=%u layer=%d STALE "
                                "attach (fb color0 name=%d, not %u)\n", tex, layer, oname, tex);
            return 0;  // stale attach, not ours
        }
    }
    return layer_fb;
}

static uint32_t klfb_read_from_texture(uint32_t tex) {
    static void (*r_GenFramebuffers)(int32_t, uint32_t *);
    static void (*r_BindFramebuffer)(uint32_t, uint32_t);
    static void (*r_FramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t,
                                          int32_t);
    static uint32_t (*r_CheckFramebufferStatus)(uint32_t);
    if (!r_GenFramebuffers) {
        r_GenFramebuffers = asym("glGenFramebuffers");
        r_BindFramebuffer = asym("glBindFramebuffer");
        r_FramebufferTexture2D = asym("glFramebufferTexture2D");
        r_CheckFramebufferStatus = asym("glCheckFramebufferStatus");
    }
    if (!tex || !r_GenFramebuffers || !r_BindFramebuffer ||
        !r_FramebufferTexture2D || !r_CheckFramebufferStatus) return 0;
    static uint32_t probe_fb;
    if (!probe_fb) r_GenFramebuffers(1, &probe_fb);
    if (!probe_fb) return 0;
    r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, probe_fb);
    r_FramebufferTexture2D(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */,
                           0x0DE1 /* TEXTURE_2D */, tex, 0);
    if (r_CheckFramebufferStatus(0x8CA8) != 0x8CD5) return 0;
    return probe_fb;
}

// Is `fb` complete, and is its colour attachment one of the eye textures? The
// answer is the FBO itself (0 for no), and on a hit the eye's own size, which
// is not the pbuffer's — reading a 2748x2880 eye at the pbuffer's 4000x3200 is
// an out-of-bounds read that comes back black.
static uint32_t klfb_eye_fbo_take(uint32_t fb, int32_t *w, int32_t *h) {
    static uint32_t (*r_CheckFramebufferStatus)(uint32_t);
    static void (*r_BindFramebuffer)(uint32_t, uint32_t);
    static void (*r_GetFbAttachmentParam)(uint32_t, uint32_t, uint32_t, int32_t *);
    if (!r_CheckFramebufferStatus) {
        r_CheckFramebufferStatus = asym("glCheckFramebufferStatus");
        r_BindFramebuffer = asym("glBindFramebuffer");
        r_GetFbAttachmentParam = asym("glGetFramebufferAttachmentParameteriv");
    }
    if (!r_CheckFramebufferStatus || !r_BindFramebuffer || !r_GetFbAttachmentParam)
        return 0;
    r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, fb);
    if (r_CheckFramebufferStatus(0x8CA8) != 0x8CD5) return 0;
    int32_t otype = 0, oname = 0;
    r_GetFbAttachmentParam(0x8CA8, 0x8CE0, 0x8CD0, &otype);
    r_GetFbAttachmentParam(0x8CA8, 0x8CE0, 0x8CD1, &oname);
    if (otype != 0x1702 /* TEXTURE */ ||
        ((uint32_t)oname != g_eye_tex[0] && (uint32_t)oname != g_eye_tex[1]))
        return 0;
    int32_t tw = 0, th = 0;
    if (klfb_tex_info((uint32_t)oname, NULL, &tw, &th) && tw > 0 && th > 0) {
        if (w) *w = tw;
        if (h) *h = th;
    }
    return fb;
}

static unsigned glfb_capture_now(const char *dir) {
    if (!g_ready || !a_glReadPixels) return 0;
    if (a_glFinish) a_glFinish();

    // Which framebuffer is the guest actually on at swap time? A black frame from
    // the default framebuffer means either nothing was drawn or it was drawn
    // somewhere else, and those need different fixes.
    int32_t draw_fb = -1, read_fb = -1, vp[4] = {0,0,0,0};
    int32_t fb_status = -1;
    static uint32_t (*a_glCheckFramebufferStatus)(uint32_t);
    static void (*a_BindFramebuffer)(uint32_t, uint32_t);
    static void (*a_GetFbAttachmentParam)(uint32_t, uint32_t, uint32_t, int32_t *);
    if (!a_glCheckFramebufferStatus) {
        a_glCheckFramebufferStatus = asym("glCheckFramebufferStatus");
        a_BindFramebuffer = asym("glBindFramebuffer");
        a_GetFbAttachmentParam = asym("glGetFramebufferAttachmentParameteriv");
    }
    if (a_glGetIntegerv) {
        a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &draw_fb);
        a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &read_fb);
        a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
    }
    if (a_glCheckFramebufferStatus)
        fb_status = (int32_t)a_glCheckFramebufferStatus(0x8CA9 /* GL_DRAW_FRAMEBUFFER */);

    // The frame lives in the FBO whose color attachment is an eye texture:
    // fb0 is black by construction (the VR frame goes to eye textures, not
    // the backbuffer — the old capture read fb0 and reported 0 lit on runs
    // full of real draws). Find that FBO by its attachment and read it its
    // own way; fall back to the legacy read of whatever is bound when no eye
    // texture exists yet.
    uint32_t src_fb = 0;
    int32_t src_w = g_w, src_h = g_h;
    if ((g_eye_tex[0] || g_eye_tex[1]) && a_BindFramebuffer &&
        a_GetFbAttachmentParam && a_glCheckFramebufferStatus) {
        // A STATED image beats every search. kl_glfb_set_live_eye_image is the
        // OpenXR path saying which swapchain image the guest just presented,
        // with its size and array layer — none of which can be recovered here,
        // because the swapchain is allocated through the real ANGLE entry
        // points and both eyes are layers of one texture. Everything below is
        // the search for a guest that never states it (Unity through OVRPlugin).
        int cap_eye = kl_env_int("KL_XR_CAPTURE_EYE", 0);
        if (cap_eye < 0 || cap_eye > 1) cap_eye = 0;
        const typeof(g_live_eye[0]) *le = &g_live_eye[cap_eye];
        if (!le->tex && g_live_eye[cap_eye ^ 1].tex) le = &g_live_eye[cap_eye ^ 1];
        if (le->tex && le->w > 0 && le->h > 0) {
            src_fb = le->layer >= 0 ? klfb_read_from_texture_layer(le->tex, le->layer)
                                    : klfb_read_from_texture(le->tex);
            if (src_fb) { src_w = le->w; src_h = le->h; }
        }
        // The guest's own answer next (g_last_eye_fbo): the scan matches on
        // texture NAME, and a name outlives the object it was issued for.
        uint32_t hint = src_fb ? 0 : __atomic_load_n(&g_last_eye_fbo, __ATOMIC_RELAXED);
        if (hint) src_fb = klfb_eye_fbo_take(hint, &src_w, &src_h);
        for (uint32_t i = 1; i <= g_fbomax && !src_fb; i++)
            src_fb = klfb_eye_fbo_take(i, &src_w, &src_h);
        if (!src_fb) {
            // No framebuffer of the guest's has it attached any more — the
            // OpenXR case above. Read the named image directly rather than
            // falling back to fb0, which is black whatever happened.
            uint32_t live = g_eye_tex[0] ? g_eye_tex[0] : g_eye_tex[1];
            src_fb = klfb_read_from_texture(live);
            if (src_fb) {
                int32_t tw = 0, th = 0;
                if (klfb_tex_info(live, NULL, &tw, &th) && tw > 0 && th > 0) {
                    src_w = tw;
                    src_h = th;
                }
            } else {   // not found either: put the guest's binding back
                a_BindFramebuffer(0x8CA8, (uint32_t)(read_fb >= 0 ? read_fb : 0));
            }
        }
    }
    // A multiview eye framebuffer (KL_OVRP_GL_ARRAY) is not readable as it is
    // (INVALID_FRAMEBUFFER_OPERATION): read the left eye's layer instead.
    if (a_GetFbAttachmentParam && (src_fb || read_fb > 0)) {
        int32_t views = 0, otype = 0, oname = 0;
        if (!src_fb) a_BindFramebuffer(0x8CA8, (uint32_t)read_fb);
        a_GetFbAttachmentParam(0x8CA8, 0x8CE0, 0x8CD0 /* OBJECT_TYPE */, &otype);
        if (otype == 0x1702) {
            a_GetFbAttachmentParam(0x8CA8, 0x8CE0, 0x9630 /* NUM_VIEWS_OVR */, &views);
            a_GetFbAttachmentParam(0x8CA8, 0x8CE0, 0x8CD1 /* OBJECT_NAME */, &oname);
        }
        if (a_glGetError) a_glGetError();
        if (views > 1 && oname) {
            uint32_t lf = klfb_read_from_texture_layer((uint32_t)oname, 0);
            if (lf) { src_fb = lf; if (src_w <= 0 || src_h <= 0 || src_w == g_w) { int32_t tw = 0, th = 0; if (klfb_tex_info((uint32_t)oname, NULL, &tw, &th) && tw > 0) { src_w = tw; src_h = th; } else if (vp[2] > 0 && vp[3] > 0) { src_w = vp[2]; src_h = vp[3]; } } }
        }
    }

    size_t   stride = (size_t)src_w * 4;
    uint8_t *px = malloc(stride * (size_t)src_h);
    if (!px) return 0;

    int32_t rfmt = 0, rtype = 0;
    if (a_glGetIntegerv) {
        a_glGetIntegerv(KLFB_IMPLEMENTATION_COLOR_READ_FORMAT, &rfmt);
        a_glGetIntegerv(KLFB_IMPLEMENTATION_COLOR_READ_TYPE, &rtype);
    }
    uint32_t pre_err = a_glGetError ? a_glGetError() : 0;
    int comps = (rfmt == 0x1907 /* GL_RGB */) ? 3 : 4;
    if (src_fb && (rtype == 0x1406 /* FLOAT */ || rtype == KLFB_HALF_FLOAT)) {
        // Float target: read in the implementation's own format/type and tone
        // map down to 8-bit (klfb_dbg_tone). A direct RGBA/UNSIGNED_BYTE read
        // here is the INVALID_ENUM the old capture died with. The half path
        // goes through a 64K-entry LUT: three powf per pixel per swap put the
        // tone map at the top of the profile (5M pixels at 2304x2198), and a
        // half float has only 16 bits of input to memoize.
        static uint8_t tone16[65536];
        static int tone16_ready;
        if (rtype == KLFB_HALF_FLOAT && !tone16_ready) {
            for (int i = 0; i < 65536; i++)
                tone16[i] = klfb_dbg_tone(klfb_half_to_float((uint16_t)i));
            tone16_ready = 1;
        }
        size_t esz = rtype == 0x1406 ? 4 : 2;
        void *rbuf = malloc((size_t)src_w * src_h * comps * esz);
        if (!rbuf) { free(px); return 0; }
        a_glReadPixels(0, 0, src_w, src_h, rfmt, rtype, rbuf);
        // Raw-value census, one line per 60 captures: is the eye texture dim
        // because the CONTENT is dim (fade still down — max/mean low) or
        // because the debug tone map undersells it (max near/over 1 while the
        // window shows murk)? KL_GLFB_RAWSTATS=0 turns the line off.
        {
            static int rawstats = -1;
            if (rawstats < 0) {
                rawstats = kl_env_int("KL_GLFB_RAWSTATS", 1);
            }
            static unsigned rs_n;
            if (rawstats && rs_n++ % 60 == 0) {
                float vmin = 1e30f, vmax = -1e30f; double vsum = 0;
                int32_t npx = src_w * src_h;
                for (int32_t i = 0; i < npx; i++) {
                    for (int j = 0; j < comps && j < 3; j++) {
                        float v = rtype == 0x1406
                                ? ((const float *)rbuf)[i * comps + j]
                                : klfb_half_to_float(((const uint16_t *)rbuf)[i * comps + j]);
                        if (v < vmin) vmin = v;
                        if (v > vmax) vmax = v;
                        vsum += v;
                    }
                }
                fprintf(stderr,
                        " [glfb] rawstats fb=%u %dx%d fmt=0x%x type=0x%x: "
                        "min=%.4f max=%.4f mean=%.5f\n",
                        src_fb, (int)src_w, (int)src_h, rfmt, rtype,
                        vmin, vmax, vsum / ((double)npx * (comps < 3 ? comps : 3)));
            }
        }
        if (rtype == KLFB_HALF_FLOAT && comps == 4) {
            const uint16_t *h = (const uint16_t *)rbuf;
            for (int32_t i = 0; i < src_w * src_h; i++) {
                px[i * 4 + 0] = tone16[h[i * 4 + 0]];
                px[i * 4 + 1] = tone16[h[i * 4 + 1]];
                px[i * 4 + 2] = tone16[h[i * 4 + 2]];
                px[i * 4 + 3] = 255;
            }
        } else {
            for (int32_t i = 0; i < src_w * src_h; i++) {
                float c[4] = {0, 0, 0, 1};
                for (int j = 0; j < comps; j++)
                    c[j] = rtype == 0x1406
                         ? ((float *)rbuf)[i * comps + j]
                         : klfb_half_to_float(((uint16_t *)rbuf)[i * comps + j]);
                px[i * 4 + 0] = klfb_dbg_tone(c[0]);
                px[i * 4 + 1] = klfb_dbg_tone(c[1]);
                px[i * 4 + 2] = klfb_dbg_tone(c[2]);
                px[i * 4 + 3] = 255;
            }
        }
        free(rbuf);
    } else {
        a_glReadPixels(0, 0, src_w, src_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        // OPAQUE, like the float path two branches up — and for the same
        // reason: this file is a PICTURE, not a copy of the buffer. An eye
        // layer is composited opaque by the runtime, so the guest has no reason
        // to author alpha and Beat Saber 1.40 writes 0 everywhere. Passing that
        // through wrote a fully transparent PNG over a correct RGB frame, which
        // every image viewer then showed as its own background — "mostly black"
        // or blank white, depending on the viewer, with the pixels right there.
        // The float path has always forced 255 and 1.28's eyes were RGBA16F, so
        // the difference arrived with the first 8-bit eye texture.
        for (size_t i = 0; i < (size_t)src_w * (size_t)src_h; i++)
            px[i * 4 + 3] = 255;
    }
    // The error could be the readback's own or one the guest left behind —
    // report both rather than blame the readback. Reading the pre-existing one
    // does clear it; this is a host-only diagnostic and the guest's errors on
    // the reference renderer are exactly what we want to see.
    uint32_t err = a_glGetError ? a_glGetError() : 0;
    char err_buf[64] = "";
    if (err) snprintf(err_buf, sizeof err_buf, " (GL error 0x%x%s)", err,
                      pre_err ? " — but pre-existing, not the readback's" : "");
    if (src_fb && read_fb >= 0 && a_BindFramebuffer)
        a_BindFramebuffer(0x8CA8, (uint32_t)read_fb);   // hand the binding back

    // Say whether anything landed. A frame that is uniformly one colour is a clear
    // with no draw, and that is a different outcome from a frame that drew — a
    // difference invisible in a thumbnail of a dark image.
    unsigned long sum = 0; unsigned lit = 0;
    for (size_t i = 0; i < (size_t)src_w * src_h; i++) {
        unsigned lum = px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2];
        sum += lum;
        if (lum > 12) lit++;
    }
    g_last_frame_lit = lit;

    // ...and WHERE the light is, in sixteenths, top row first. A picture that
    // fills a quarter of the target and a target with nothing in it are the
    // same single "0 lit" number otherwise, and the census that produced that
    // number was also reading a 1280x800 corner of a 1536x1536 eye. Cheap
    // enough to be unconditional: one pass over a buffer already in cache.
    char blocks[128] = "";
    {
        unsigned long bsum[16] = {0};
        for (int32_t y = 0; y < src_h; y++) {
            int by = (int)((int64_t)y * 4 / src_h);
            if (by > 3) by = 3;
            const uint8_t *row = px + (size_t)y * src_w * 4;
            for (int32_t x = 0; x < src_w; x++) {
                int bx = (int)((int64_t)x * 4 / src_w);
                if (bx > 3) bx = 3;
                bsum[by * 4 + bx] += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
            }
        }
        unsigned long n = (unsigned long)src_w * (unsigned long)src_h / 16 * 3;
        size_t at = 0;
        for (int by = 3; by >= 0 && at + 24 < sizeof blocks; by--) {   // top row
            at += (size_t)snprintf(blocks + at, sizeof blocks - at, " |");
            for (int bx = 0; bx < 4; bx++)                             // ...first
                at += (size_t)snprintf(blocks + at, sizeof blocks - at, " %lu",
                                       n ? bsum[by * 4 + bx] / n : 0UL);
        }
    }

    // The sink half of the readback/output split: with a frontend registered
    // the buffer IS the output — hand it over and count the frame presented.
    // The sink runs on this (GL) thread inside the guest's frame, so it must
    // be a memcpy, not a render. Everything below stays the default output:
    // PNG to KL_GLFB_OUT, unchanged when no sink is registered.
    if (g_frame_sink) {
        // KL_GLFB_DUMP_SINK=<dir>: write every Nth sink frame to a PNG
        // (KL_GLFB_DUMP_SINK_EVERY, default 100). The sink and the PNG path
        // share this px buffer, so a window that shows black while these files
        // show content is an SDL-side problem, not a capture-side one.
        // The interval is a knob because the default is ~17 s of a 2D UI at
        // the shell's frame rate — far too coarse to see what a click did.
        static const char *sink_dir;
        static int sink_dir_init, sink_every;
        if (!sink_dir_init) {
            sink_dir = kl_env_str("KL_GLFB_DUMP_SINK", NULL);
            sink_every = kl_env_int("KL_GLFB_DUMP_SINK_EVERY", 100);
            if (sink_every < 1) sink_every = 1;
            sink_dir_init = 1;
        }
        if (sink_dir && g_presented % (unsigned)sink_every == 0) {
            char spath[600];
            snprintf(spath, sizeof spath, "%s/sink_%05u.png", sink_dir, g_presented);
            klfb_write_png(spath, px, src_w, src_h);
        }
        g_frame_sink(px, src_w, src_h, g_frame_sink_ctx);
        free(px);
        return ++g_presented;
    }

    char path[512];
    snprintf(path, sizeof path, "%s/frame_%03u.png", dir, g_presented);
    size_t   raw_n = (stride + 1) * (size_t)src_h;
    uint8_t *raw = malloc(raw_n);
    uLongf   cn  = compressBound((uLong)raw_n);
    uint8_t *cb  = malloc(cn);
    if (!raw || !cb) { free(px); free(raw); free(cb); return 0; }
    for (int y = 0; y < src_h; y++) {              // GL bottom-up -> PNG top-down
        raw[(stride + 1) * (size_t)y] = 0;
        memcpy(raw + (stride + 1) * (size_t)y + 1,
               px + stride * (size_t)(src_h - 1 - y), stride);
    }
    if (compress(cb, &cn, raw, (uLong)raw_n) != Z_OK) {
        free(px); free(raw); free(cb); return 0;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { free(px); free(raw); free(cb); return 0; }
    static const uint8_t sig[8] = {0x89,'P','N','G',13,10,26,10};
    fwrite(sig, 1, 8, f);
    uint8_t ih[13]; be32(ih, (uint32_t)src_w); be32(ih + 4, (uint32_t)src_h);
    ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    chunk(f, "IDAT", cb, (uint32_t)cn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    klfb_report_draws();
    // fb0 has been measured black with every draw landing on FBOs instead
    // (the VR frame goes to eye textures, not the backbuffer). Census the
    // FBOs the guest actually created — format-correctly, see klfb_probe_fbo
    // — and the one with lit pixels names where the picture is.
    {
        static void (*a_glBindFramebuffer)(uint32_t, uint32_t);
        if (!a_glBindFramebuffer) a_glBindFramebuffer = asym("glBindFramebuffer");
        // KL_GLFB_DUMP_FBOS=1: the census also writes one PNG per FBO
        // (frame_NNN_fbM.png, tone-mapped like the capture). Intermediates —
        // the post-processing chain, the MSAA scene target — are otherwise
        // only lit-counts on a log line.
        static int dump_fbos = -1;
        if (dump_fbos < 0) dump_fbos = kl_env_on("KL_GLFB_DUMP_FBOS", 0);
        if (a_glBindFramebuffer) {
            int32_t save_fb = read_fb >= 0 ? read_fb : 0;
            float  *fbuf = malloc((size_t)g_w * g_h * 4 * sizeof(float));
            uint8_t *bbuf = malloc((size_t)g_w * g_h * 4);
            uint8_t *dbuf = dump_fbos ? malloc((size_t)g_w * g_h * 4) : NULL;
            if (fbuf && bbuf) {
                for (uint32_t i = 1; i <= g_fbomax; i++) {
                    char note[160] = "";
                    int32_t dw = 0, dh = 0;
                    // No hint here on purpose: this is a SWEEP over every FBO,
                    // and a size borrowed from elsewhere would read past a
                    // small attachment into undefined pixels. A census must
                    // say it could not look rather than invent a lit count
                    //. The targeted probes (blit, draw, clear) each
                    // know a real size and pass it.
                    unsigned long flit = klfb_probe_fbo(i, fbuf, bbuf,
                                                        note, sizeof note,
                                                        dbuf, &dw, &dh, 0, 0);
                    if (!strcmp(note, "incomplete")) continue;
                    fprintf(stderr, "  [glfb] census fbo%u: %lu lit — %s\n",
                            i, flit, note);
                    if (dbuf && dw > 0 && dh > 0) {
                        char fpath[600];
                        snprintf(fpath, sizeof fpath, "%s/frame_%03u_fb%u.png",
                                 dir, g_presented, i);
                        klfb_write_png(fpath, dbuf, dw, dh);
                    }
                }
            }
            free(fbuf); free(bbuf); free(dbuf);
            a_glBindFramebuffer(0x8CA8, (uint32_t)save_fb);
        }
    }
    uint64_t cap_tid = 0; pthread_threadid_np(NULL, &cap_tid);
    fprintf(stderr, "  [glfb] %s: %u/%u lit, mean luma %lu%s blocks%s "
                    "[draw_fb=%d read_fb=%d src=fb%u %dx%d fb_status=0x%x viewport %dx%d+%d+%d] (captured on t%llu)\n", path, lit,
            (unsigned)(src_w * src_h), sum / ((unsigned long)src_w * src_h * 3),
            err_buf, blocks, draw_fb, read_fb, src_fb, src_w, src_h, fb_status,
            vp[2], vp[3], vp[0], vp[1],
            (unsigned long long)cap_tid);
    free(px); free(raw); free(cb);
    return ++g_presented;
}
