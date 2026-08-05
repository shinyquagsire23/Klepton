// The one-eye compositor, on ANGLE.
//
// Opt-in with KL_GLFB=1; the null driver in kl_egl.c stays the default, so
// `make check` and the green path are unaffected by anything here.
//
// ---------------------------------------------------------------------------
// Why ANGLE, and what it deleted
//
// The first version of this file forwarded to Apple's desktop GL 4.1 (S0.7/S0.8).
// It worked, but every difference between GLES and desktop GL became a lie this
// file had to tell: a #version rewrite on every shader, an FBO standing in for the
// default framebuffer with GL_BACK -> GL_COLOR_ATTACHMENT0 translation on top, and
// finally a substitution of RGBA8 for ETC2, because Apple's desktop GL cannot
// allocate a format that is *mandatory* in the GLES 3.0 the guest speaks.
//
// S0.9 (`make angle`) measured the alternative on the guest's own artefacts:
//
//     ANGLE / GL backend      ETC2 0x9279 REJECTED   shaders unmodified
//     ANGLE / Metal backend   ETC2 0x9279 accepted   shaders unmodified
//
// So ANGLE speaks GLES for real, and every one of those workarounds is gone: no
// shader rewrite, no format substitution, no framebuffer impersonation. The
// guest's "default framebuffer" is a genuine EGL pbuffer, so binding 0 means what
// it says and GL_BACK is a valid buffer name again.
//
// Note the backend must be *asked for*: eglGetDisplay(EGL_DEFAULT_DISPLAY) selects
// ANGLE's OpenGL backend on macOS, which inherits every desktop-GL limitation and
// fails ETC2 identically. EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE is the whole point.
//
// ---------------------------------------------------------------------------
// Still host-only, and still borrowed
//
// This remains a *reference renderer*, not the shipping backend: it produces the
// known-good frame a real backend gets diffed against. ANGLE narrows the gap — it
// is the likely device path too — but the dylibs are borrowed out of a Chromium
// app rather than vendored. PLANNING M5 carries that TODO and why the build system
// makes vendoring a task of its own.
//
// ---------------------------------------------------------------------------
// The ABI hazard survives the move, because it is about Darwin, not about GL
//
// The guest is AAPCS64; ANGLE is a Darwin arm64 dylib. They agree on x0-x7 and
// disagree on the stack: AAPCS64 gives every stack argument an 8-byte slot, Darwin
// packs each to its natural size. So the first stack argument lands in the same
// place and everything after it can diverge. glBlitFramebuffer(8 ints, mask,
// filter) is the live case — the guest writes filter at sp+8, a Darwin callee
// reads it at sp+4. The wrappers below declare those parameters as 64-bit so
// Darwin reproduces AAPCS64's layout, then truncate. The int64_t parameters
// holding obviously-32-bit values are deliberate; removing the widening silently
// corrupts the argument after it.
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#include "kl_glfb.h"

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
#define EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE 0x320D

#define GL_NO_ERROR      0
#define GL_RGBA          0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RENDERER      0x1F01
#define GL_VERSION       0x1F02

#define ANGLE_DEFAULT_DIR \
    "/Applications/Google Chrome.app/Contents/Frameworks/" \
    "Google Chrome Framework.framework/Libraries"

static void *g_egl_lib, *g_gles_lib;
static void *g_dpy, *g_surf, *g_ctx, *g_cfg;
// Kept so per-thread contexts can be created later, after init has chosen a config.
static void *(*a_eglCreateContext)(void *, void *, void *, const int32_t *);
static void *(*a_eglCreatePbufferSurface)(void *, void *, const int32_t *);
static int   g_on = -1, g_ready;
static int   g_w = 1832, g_h = 1920;      // one eye, Quest 2 per-eye default
static unsigned g_presented;

int kl_glfb_enabled(void) {
    if (g_on < 0) g_on = getenv("KL_GLFB") != NULL;
    return g_on;
}

void kl_glfb_set_size(int w, int h) {
    if (w > 0 && h > 0 && !g_ready) { g_w = w; g_h = h; }
}

static void *asym(const char *n) {
    void *p = g_gles_lib ? dlsym(g_gles_lib, n) : NULL;
    if (!p && g_egl_lib) p = dlsym(g_egl_lib, n);
    return p;
}

// The few ANGLE entry points this file drives itself, as opposed to the ones it
// hands straight to the guest.
static uint32_t (*a_glGetError)(void);
static void     (*a_glReadPixels)(int32_t, int32_t, int32_t, int32_t, uint32_t,
                                  uint32_t, void *);
static void     (*a_glFinish)(void);
static void     (*a_glGetIntegerv)(uint32_t, int32_t *);
static const uint8_t *(*a_glGetString)(uint32_t);
static unsigned (*a_eglMakeCurrent)(void *, void *, void *, void *);

int kl_glfb_init(void) {
    if (!kl_glfb_enabled() || g_ready) return g_ready;

    const char *dir = getenv("KL_ANGLE_DIR");
    if (!dir) dir = ANGLE_DEFAULT_DIR;
    char egl_path[1024], gles_path[1024];
    snprintf(egl_path,  sizeof egl_path,  "%s/libEGL.dylib", dir);
    snprintf(gles_path, sizeof gles_path, "%s/libGLESv2.dylib", dir);
    g_egl_lib  = dlopen(egl_path,  RTLD_NOW | RTLD_LOCAL);
    g_gles_lib = dlopen(gles_path, RTLD_NOW | RTLD_LOCAL);
    if (!g_egl_lib || !g_gles_lib) {
        fprintf(stderr, "  [glfb] cannot load ANGLE from %s\n         (%s)\n"
                        "         set KL_ANGLE_DIR to a Chromium app's Libraries "
                        "directory; staying on the null driver\n", dir, dlerror());
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
    // it every limitation this move exists to escape (S0.9).
    const char *want = getenv("KL_ANGLE_BACKEND");
    int use_gl = want && strcmp(want, "gl") == 0;
    const int32_t dpy_attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE,
        use_gl ? EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE
               : EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
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
    g_surf = a_eglCreatePbufferSurface(g_dpy, cfg, surf_attrs);
    const int32_t ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    g_ctx  = a_eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (!g_surf || !g_ctx || !a_eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
        fprintf(stderr, "  [glfb] could not make an ES3 context current\n");
        return 0;
    }

    a_glGetError   = asym("glGetError");
    a_glReadPixels = asym("glReadPixels");
    a_glFinish     = asym("glFinish");
    a_glGetIntegerv = asym("glGetIntegerv");
    a_glGetString  = asym("glGetString");
    g_ready = 1;
    fprintf(stderr, "  [glfb] ANGLE %dx%d — %s\n", g_w, g_h,
            a_glGetString ? (const char *)a_glGetString(GL_RENDERER) : "?");
    fprintf(stderr, "  [glfb] %s\n",
            a_glGetString ? (const char *)a_glGetString(GL_VERSION) : "?");

    // Release it. EGL binds a context to one thread at a time and *refuses* to
    // migrate it — unlike CGL, which silently allows it. Leaving it current on
    // whichever thread happened to resolve the first GL symbol meant the guest's
    // render thread could never claim it (EGL_BAD_ACCESS), so every guest GL call
    // ran with no context: silently, since that is not an error, just nothing.
    // Whoever asks next through kl_glfb_make_current gets it.
    a_eglMakeCurrent(g_dpy, NULL, NULL, NULL);
    return 1;
}

// A GL context is current per *thread*, and Unity drives GL from its own render
// thread rather than the one that created the context. Calling in from a thread
// with no current context dereferences a null dispatch table — which presented as
// a SIGSEGV at 0x2d8 inside glFlush when this was first wired up. eglMakeCurrent
// is the guest telling us which thread now owns the context, so it is the hook.
// One EGL context can be current on exactly one thread, and Unity drives GL from
// more than one — measured: the thread that won eglMakeCurrent was not the thread
// issuing draws, so the draws ran with no context at all and silently did nothing
// (the census reported them against framebuffer -1).
//
// The fix is what a multi-threaded GLES app does on Android: a context per thread,
// all *sharing* objects with the first one created. Textures, buffers and programs
// are shared; container objects such as FBOs and VAOs are not, which is fine here
// because each thread builds its own anyway.
//
// Each thread also gets its own eye-sized pbuffer, so whichever thread draws has a
// real default framebuffer of the right size — and the capture, which runs on that
// same thread, reads exactly what that thread drew.
static pthread_key_t g_tls_key;
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;

typedef struct { void *ctx, *surf; } klfb_thread_gl;

static void klfb_tls_free(void *p) { free(p); }
static void klfb_tls_init(void) { pthread_key_create(&g_tls_key, klfb_tls_free); }

void kl_glfb_make_current(void) {
    if (!g_ready || !a_eglMakeCurrent) return;
    pthread_once(&g_tls_once, klfb_tls_init);

    klfb_thread_gl *t = pthread_getspecific(g_tls_key);
    if (!t) {
        t = calloc(1, sizeof *t);
        if (!t) return;
        uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
        static int first = 1;
        if (first) {
            first = 0;
            t->ctx = g_ctx; t->surf = g_surf;      // the root context and eye surface
        } else if (getenv("KL_GLFB_SHARED")) {
            // Correct, and currently fatal. Giving each guest thread its own
            // sharing context is what a multi-threaded GLES app does, and it does
            // stop the draws running with no context — but it then crashes inside
            // Apple's *Metal driver* (AGX), building a blit compute program:
            //   AGX::Device::findOrCreateDriverProgramVariant<BlitComputeProgram...>
            // That is below ANGLE, so it is not ours to fix from here. Left behind
            // a knob rather than deleted, because it is the right shape and the
            // crash is the next thing to investigate.
            const int32_t surf_attrs[] = { EGL_WIDTH, g_w, EGL_HEIGHT, g_h, EGL_NONE };
            const int32_t ctx_attrs[]  = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
            t->surf = a_eglCreatePbufferSurface(g_dpy, g_cfg, surf_attrs);
            t->ctx  = a_eglCreateContext(g_dpy, g_cfg, g_ctx, ctx_attrs);  // shares
        } else {
            // Default: one context, first claimant wins. Later threads get nothing
            // and their GL calls do nothing — which is why frames are black, and is
            // recorded here rather than hidden.
            t->ctx = NULL; t->surf = NULL;
            static int warned;
            if (!warned) {
                warned = 1;
                fprintf(stderr, "  [glfb] thread %llu wants GL but the single context "
                                "is taken; its draws will do nothing. "
                                "KL_GLFB_SHARED=1 gives it its own sharing context "
                                "(see kl_glfb.c — that path currently crashes in AGX)\n",
                        (unsigned long long)tid);
            }
        }
        pthread_setspecific(g_tls_key, t);
        fprintf(stderr, "  [glfb] thread %llu gets its own %s context\n",
                (unsigned long long)tid, t->ctx == g_ctx ? "root" : "shared");
    }
    if (!t->ctx || !t->surf) return;
    if (!a_eglMakeCurrent(g_dpy, t->surf, t->surf, t->ctx)) {
        uint32_t (*eglGetError)(void) = asym("eglGetError");
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [glfb] eglMakeCurrent failed even with a per-thread "
                            "context (egl error 0x%x)\n", eglGetError ? eglGetError() : 0);
        }
    }
}

// ---------------------------------------------------------- ABI thunks
static void (*g_real_BlitFramebuffer)(int32_t, int32_t, int32_t, int32_t, int32_t,
                                      int32_t, int32_t, int32_t, uint32_t, uint32_t);

static void klfb_BlitFramebuffer(int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1,
                                 int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1,
                                 int64_t mask, int64_t filter) {
    if (g_real_BlitFramebuffer)
        g_real_BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                               (uint32_t)mask, (uint32_t)filter);
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
static struct { int32_t fb; unsigned draws; } g_draw_fbs[KLFB_MAX_FBS];
static unsigned g_ndraw_fbs;

static void klfb_note_draw(void) {
    int32_t fb = -1;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
    for (unsigned i = 0; i < g_ndraw_fbs; i++)
        if (g_draw_fbs[i].fb == fb) { g_draw_fbs[i].draws++; return; }
    if (g_ndraw_fbs < KLFB_MAX_FBS) {
        g_draw_fbs[g_ndraw_fbs].fb = fb;
        g_draw_fbs[g_ndraw_fbs].draws = 1;
        g_ndraw_fbs++;
    }
}

static void (*g_real_DrawElements)(uint32_t, int32_t, uint32_t, const void *);
static void (*g_real_DrawArrays)(uint32_t, int32_t, int32_t);

static void klfb_DrawElements(uint32_t mode, int32_t count, uint32_t type,
                              const void *indices) {
    klfb_note_draw();
    if (g_real_DrawElements) g_real_DrawElements(mode, count, type, indices);
}
static void klfb_DrawArrays(uint32_t mode, int32_t first, int32_t count) {
    klfb_note_draw();
    if (g_real_DrawArrays) g_real_DrawArrays(mode, first, count);
}

static void klfb_report_draws(void) {
    if (!g_ndraw_fbs) { fprintf(stderr, "  [glfb] no draws seen at all\n"); return; }
    fprintf(stderr, "  [glfb] draws by target framebuffer:");
    for (unsigned i = 0; i < g_ndraw_fbs; i++)
        fprintf(stderr, " fb%d=%u", g_draw_fbs[i].fb, g_draw_fbs[i].draws);
    fprintf(stderr, "\n");
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

static void (*g_real_Flush)(void);
static void (*g_real_Finish)(void);

static void klfb_service_capture(void) {
    if (!g_capture_pending) return;
    g_capture_pending = 0;
    glfb_capture_now(g_capture_dir);
}
static void klfb_Flush(void)  { if (g_real_Flush)  g_real_Flush();  klfb_service_capture(); }
static void klfb_Finish(void) { if (g_real_Finish) g_real_Finish(); klfb_service_capture(); }

static const struct { const char *name; void *thunk; void **real; } g_thunks[] = {
    {"glFlush",  (void *)klfb_Flush,  (void **)&g_real_Flush},
    {"glDrawElements", (void *)klfb_DrawElements, (void **)&g_real_DrawElements},
    {"glDrawArrays",   (void *)klfb_DrawArrays,   (void **)&g_real_DrawArrays},
    {"glFinish", (void *)klfb_Finish, (void **)&g_real_Finish},
    {"glBlitFramebuffer",         (void *)klfb_BlitFramebuffer,         (void **)&g_real_BlitFramebuffer},
    {"glTexSubImage3D",           (void *)klfb_TexSubImage3D,           (void **)&g_real_TexSubImage3D},
    {"glCompressedTexSubImage3D", (void *)klfb_CompressedTexSubImage3D, (void **)&g_real_CompressedTexSubImage3D},
};

// ---------------------------------------------------------- the gateway
//
// Capability answers stay ours. kl_egl.c tells the guest it is driving a GLES 3.2
// device with no extensions, and Unity built its renderer against that answer;
// letting ANGLE answer instead would change the description underneath a decision
// already made. Everything operational goes to ANGLE.
static const char *const g_keep_ours[] = {
    "glGetString", "glGetStringi", "glGetIntegerv", "glGetIntegeri_v",
    "glGetFloatv", "glGetBooleanv", "glGetInteger64v", "glGetInternalformativ",
};

static int keep_ours(const char *name) {
    for (size_t i = 0; i < sizeof g_keep_ours / sizeof g_keep_ours[0]; i++)
        if (strcmp(g_keep_ours[i], name) == 0) return 1;
    return 0;
}

void *kl_glfb_sym(const char *name) {
    if (!kl_glfb_enabled() || !name) return NULL;
    if (!g_ready && !kl_glfb_init()) return NULL;
    if (keep_ours(name)) return NULL;              // caller keeps its own answer

    void *fn = asym(name);
    if (!fn) return NULL;                          // ANGLE has no such entry point
    for (size_t i = 0; i < sizeof g_thunks / sizeof g_thunks[0]; i++)
        if (strcmp(g_thunks[i].name, name) == 0) {
            *g_thunks[i].real = fn;
            return g_thunks[i].thunk;
        }
    return fn;
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

// Called from the guest's eglSwapBuffers — on whatever thread that happens to be,
// which is measurably *not* the one that owns the context. So this only records
// the request; klfb_service_capture does the work from a call the GL thread makes
// itself. Reading pixels with no current context is not an error, it is silence,
// which is exactly how this presented: a perfectly black frame with the
// framebuffer queries writing nothing at all.
unsigned kl_glfb_present(const char *dir) {
    if (!g_ready || !dir) return g_presented;
    snprintf(g_capture_dir, sizeof g_capture_dir, "%s", dir);
    g_capture_pending = 1;
    return g_presented;
}

static unsigned glfb_capture_now(const char *dir) {
    if (!g_ready || !a_glReadPixels) return 0;
    if (a_glFinish) a_glFinish();

    size_t   stride = (size_t)g_w * 4;
    uint8_t *px = malloc(stride * (size_t)g_h);
    if (!px) return 0;
    // Which framebuffer is the guest actually on at swap time? A black frame from
    // the default framebuffer means either nothing was drawn or it was drawn
    // somewhere else, and those need different fixes.
    int32_t draw_fb = -1, read_fb = -1, vp[4] = {0,0,0,0};
    if (a_glGetIntegerv) {
        a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &draw_fb);
        a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &read_fb);
        a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
    }
    a_glReadPixels(0, 0, g_w, g_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    uint32_t err = a_glGetError ? a_glGetError() : 0;
    char err_buf[48] = "";
    if (err) snprintf(err_buf, sizeof err_buf, " (GL error 0x%x)", err);

    // Say whether anything landed. A frame that is uniformly one colour is a clear
    // with no draw, and that is a different outcome from a frame that drew — a
    // difference invisible in a thumbnail of a dark image.
    unsigned long sum = 0; unsigned lit = 0;
    for (size_t i = 0; i < (size_t)g_w * g_h; i++) {
        unsigned lum = px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2];
        sum += lum;
        if (lum > 12) lit++;
    }

    char path[512];
    snprintf(path, sizeof path, "%s/frame_%03u.png", dir, g_presented);
    size_t   raw_n = (stride + 1) * (size_t)g_h;
    uint8_t *raw = malloc(raw_n);
    uLongf   cn  = compressBound((uLong)raw_n);
    uint8_t *cb  = malloc(cn);
    if (!raw || !cb) { free(px); free(raw); free(cb); return 0; }
    for (int y = 0; y < g_h; y++) {                // GL bottom-up -> PNG top-down
        raw[(stride + 1) * (size_t)y] = 0;
        memcpy(raw + (stride + 1) * (size_t)y + 1,
               px + stride * (size_t)(g_h - 1 - y), stride);
    }
    if (compress(cb, &cn, raw, (uLong)raw_n) != Z_OK) {
        free(px); free(raw); free(cb); return 0;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { free(px); free(raw); free(cb); return 0; }
    static const uint8_t sig[8] = {0x89,'P','N','G',13,10,26,10};
    fwrite(sig, 1, 8, f);
    uint8_t ih[13]; be32(ih, (uint32_t)g_w); be32(ih + 4, (uint32_t)g_h);
    ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    chunk(f, "IDAT", cb, (uint32_t)cn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    klfb_report_draws();
    fprintf(stderr, "  [glfb] %s: %u/%u lit, mean luma %lu%s "
                    "[draw_fb=%d read_fb=%d viewport %dx%d+%d+%d]\n", path, lit,
            (unsigned)(g_w * g_h), sum / ((unsigned long)g_w * g_h * 3),
            err_buf, draw_fb, read_fb, vp[2], vp[3], vp[0], vp[1]);
    free(px); free(raw); free(cb);
    return ++g_presented;
}
