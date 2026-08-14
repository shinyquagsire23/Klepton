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
#include <unistd.h>
#include <math.h>          // powf — the debug tone map in the capture path
#include <zlib.h>
// The decoded-video image path (SL-13) — an AHardwareBuffer here is a
// CVPixelBuffer, and what ANGLE can take is the IOSurface behind it.
#include <CoreVideo/CoreVideo.h>
#include "kl_glfb.h"
#include "kl_present.h"
#include "kl_egl.h"        // kl_gl_cap_* — the capability tables
#include "kl_reproject.h"  // kl_reproject_set_srgb_decode — see klfb_srgb_settle
#include "kl_fault.h"      // kl_fault_print_frames — who issued a GL call
#include "klepton.h"       // kl_trace_stub, for the per-name GL call trace

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
static unsigned g_presented;

int kl_glfb_enabled(void) {
    // Value-aware, not presence-aware. Default off, which keeps every host loop
    // and `make check` exactly as they were — but the visionOS app now turns
    // this on for itself (kl_app_configure), so `KL_GLFB=0` has to be a real way
    // back to the null driver rather than a second way to say "on".
    if (g_on < 0) g_on = kl_env_on("KL_GLFB", 0);
    return g_on;
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
// ERRSCAN's framebuffer report (below): what GL itself thinks of the target a
// draw just failed against.
static uint32_t (*a_glCheckFramebufferStatus)(uint32_t);
static void     (*a_glGetFramebufferAttachmentParameteriv)(uint32_t, uint32_t,
                                                           uint32_t, int32_t *);
static const uint8_t *(*a_glGetString)(uint32_t);
static unsigned (*a_eglMakeCurrent)(void *, void *, void *, void *);

static void klfb_selftest(void);

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
    // it every limitation this move exists to escape (S0.9).
    const char *want = kl_env_str("KL_ANGLE_BACKEND", "angle");
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
static void klfb_tls_init(void) { pthread_key_create(&g_tls_key, klfb_tls_free); }

// Owner of the root context in migration mode: the thread id that last took it
// and has not released it, 0 when free.
static uint64_t g_root_owner;

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
            } else {
                t->ctx = NULL; t->surf = NULL;
                static int warned;
                if (!warned) {
                    warned = 1;
                    fprintf(stderr, "  [glfb] thread %llu wants GL while thread %llu "
                                    "holds the context; its draws will do nothing\n",
                            (unsigned long long)tid, (unsigned long long)g_root_owner);
                }
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
    }
}

// ------------------------------------------------- the shared-context compile lock
//
// THIS DOES NOT FIX THE AGX ABORT. Read that first, because the numbers below are
// persuasive about a different bug and it would be easy to file them against the
// wrong one. Under KL_GLFB_SHARED the guest still dies in
// findOrCreateDriverProgramVariant<BlitComputeProgramVariant>, lock or no lock —
// measured after this went in. What the lock fixes is a *second*, independent
// defect that s10 turned up while hunting the first.
//
// That second defect: shared contexts are correct, and this build of ANGLE is not
// thread-safe across them. spikes/s10_shared.c reproduces it with no guest at all
// — three host threads, shared ANGLE contexts, nothing of Klepton linked — and the
// failure rate is the measurement that matters:
//
//   no lock                        10/30 runs crash
//   lock around compile + link      2/60      <- ~10x better, but NOT a fix
//   lock around the draws only      1/30
//   serialised entirely             0/40
//   one thread only                 0/40
//   concurrency without sharing     1/20      <- so it is not concurrency as such
//
// Neither sharing nor concurrency alone, then: it is the combination. The race
// presents two ways from one code path — a SIGSEGV inside libGLESv2, and a
// libmalloc "pointer being freed was not allocated" abort, i.e. heap corruption in
// ANGLE's own state. Both arrive via GL_DrawArrays and share interior frames.
//
// Serialising compile and link is where most of the risk lives, ANGLE doing its
// heavy shared-object work at link time, and the improvement is real rather than
// thread skew: it survives making the draws overlap heavily afterwards (30 draw
// iterations per thread, 7/15 crashes without the lock and 0/15 with).
//
// But read row two honestly. Its first samples came back 0/30 and then 0/15, which
// looked like a clean fix; a 60-run sample found two failures. An order of
// magnitude, not a cure. The only configurations measured clean at 40 runs are the
// ones that never use shared contexts concurrently — which is exactly the thing
// the renderer has to do.
//
// So this is kept as a mitigation, not a resolution: partial, fixing nothing
// currently visible, but guarding a real race that is latent on precisely the path
// the renderer needs and would surface the moment the AGX problem is out of the
// way. It is cheap, compilation not being hot. It is not a licence to call the
// sharing path safe.
//
// And it is a workaround for the host driver, not a translation decision — nothing
// about the guest needs it. It lives here, in the host-only reference renderer,
// and must not be mistaken for something a real backend would inherit.
static pthread_mutex_t g_compile_lock = PTHREAD_MUTEX_INITIALIZER;

static void (*g_real_CompileShader)(uint32_t);
static void (*g_real_LinkProgram)(uint32_t);
static void (*g_real_ShaderSource)(uint32_t, int32_t, const char *const *,
                                   const int32_t *);
static void (*g_real_GetShaderiv)(uint32_t, uint32_t, int32_t *);
static void (*g_real_GetShaderInfoLog)(uint32_t, int32_t, int32_t *, char *);

// The Bloom diagnostic, and the fix it bought. Unity fell back to the 1-pass
// error shader on "Hidden/PostProcessing/Bloom" — i.e. a variant failed to
// compile — and neither the null driver (which never sees these calls under
// KL_GLFB, the guest holding ANGLE's real pointers) nor KHR_debug said why.
// So capture every glShaderSource, keyed by shader name the way kl_egl's null
// driver does, and when a compile fails print the info log and the source
// that produced it, and write both to KL_DUMP_SHADERS/<dir> if one is set.
// That turned "Invalid pass number (13)" into the actual compiler verdict
// without a standalone replay step, and the verdict was:
//
//   ERROR: 0:1: '' : unsupported shader version 320
//
// The version gap. kl_egl describes the device as GLES 3.2 / GLSL ES 3.20
// (a deliberate group answer — Unity cross-checks the description against
// itself, and gates B10G11R11 renderability on the version number itself,
// so dropping the description to the 3.0 the driver actually is kills
// post-processing anyway). Unity therefore emits "#version 320 es" variants,
// and the ANGLE context behind this renderer is ES *3.0* — its Metal backend
// caps there — and an ES 3.0 context accepts only #version 100/300 es. Every
// 320 es source failed: 7016 compiles in one run, all two distinct texts
// (an instancing VS with a layout(binding=) uniform block, and a trivial
// constant FS), retried forever.
//
// The rewrite below repairs the text at the boundary between what we
// describe and what ANGLE implements. Three mechanical moves, all verified
// against the vendored ANGLE with the captured Bloom-era sources via
// build/s09_angle:
//
//   1. "#version 3xx es" with xx > 00 -> "#version 300 es". Nothing in the
//      failing corpus uses a real 3.1/3.2 feature; if a later shader does,
//      it fails loudly here with the true error, which is what the failure
//      reporter is for.
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
#define KLFB_PINS        32
static struct { uint32_t shader; unsigned n;
                struct { int32_t at; char name[40]; } p[KLFB_PINS]; }
    g_shader_pins[KLFB_PIN_SHADERS];
static unsigned g_shader_pins_n;

static int klfb_pin_slot(uint32_t shader) {
    for (unsigned i = 0; i < g_shader_pins_n; i++)
        if (g_shader_pins[i].shader == shader) return (int)i;
    if (g_shader_pins_n >= KLFB_PIN_SHADERS) return -1;
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
static char *klfb_rewrite_glsl(char *buf, uint32_t shader) {
    int changed = 0;
    klfb_pin_reset(shader);
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

    // SL-10: external images, which ANGLE's Metal backend does not have.
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
    return changed ? buf : NULL;
}

static void klfb_ShaderSource(uint32_t shader, int32_t count,
                              const char *const *strings, const int32_t *lengths) {
    if (strings && count > 0) {
        // The buffer is built for the rewrite, not just the capture: the
        // first version of this tied the two together, and once the capture
        // table filled (1024 entries; a long run sources thousands) the
        // rewrite silently stopped applying and the 320 es failures came
        // straight back.
        size_t total = 0;
        for (int32_t i = 0; i < count; i++)
            total += lengths && lengths[i] >= 0 ? (size_t)lengths[i]
                                                : (strings[i] ? strlen(strings[i]) : 0);
        char *buf = malloc(total + 1);
        if (buf) {
            size_t off = 0;
            for (int32_t i = 0; i < count; i++) {
                size_t len = lengths && lengths[i] >= 0 ? (size_t)lengths[i]
                                                        : (strings[i] ? strlen(strings[i]) : 0);
                if (strings[i] && len) { memcpy(buf + off, strings[i], len); off += len; }
            }
            buf[off] = 0;
            char *rewritten = klfb_rewrite_glsl(buf, shader);
            int stored = 0;
            if (g_fb_nshaders < KLFB_MAX_SHADERS) {
                pthread_mutex_lock(&g_compile_lock);
                g_fb_shaders[g_fb_nshaders].name = shader;
                g_fb_shaders[g_fb_nshaders].src  = buf;
                g_fb_nshaders++;
                pthread_mutex_unlock(&g_compile_lock);
                stored = 1;
            }
            if (rewritten) {
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

static void klfb_CompileShader(uint32_t shader) {
    if (!g_real_CompileShader) return;
    // The status/info-log readers are not thunks (the guest must get ANGLE's
    // real answers), so they resolve lazily here rather than through g_thunks,
    // whose entries hand their thunk pointer to the guest.
    if (!g_real_GetShaderiv) g_real_GetShaderiv = (void *)asym("glGetShaderiv");
    if (!g_real_GetShaderInfoLog)
        g_real_GetShaderInfoLog = (void *)asym("glGetShaderInfoLog");
    pthread_mutex_lock(&g_compile_lock);
    g_real_CompileShader(shader);
    int32_t ok = 1;
    if (g_real_GetShaderiv)
        g_real_GetShaderiv(shader, KLFB_GL_COMPILE_STATUS, &ok);
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
static void klfb_LinkProgram(uint32_t program) {
    if (!g_real_LinkProgram) return;
    pthread_mutex_lock(&g_compile_lock);
    g_real_LinkProgram(program);
    // The pins the rewrite took out of the shader text become a translation
    // table here — this is the first moment a name has a location to translate
    // to. See the block above klfb_strip_uniform_layout.
    klfb_pins_link(program);
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

// The gen/delete pairs. Six near-identical thunks per family is worse than a
// macro here: the only thing that varies is which counter moves, and a typo in
// one hand-written copy is a census that lies about exactly one class.
#define KLFB_GENDEL(Name, KIND)                                               \
    static void (*g_real_Gen##Name)(int32_t, uint32_t *);                     \
    static void (*g_real_Delete##Name)(int32_t, const uint32_t *);            \
    static void klfb_Gen##Name(int32_t n, uint32_t *ids) {                    \
        if (g_real_Gen##Name) g_real_Gen##Name(n, ids);                       \
        klfb_census_made(KIND, n);                                            \
    }                                                                         \
    static void klfb_Delete##Name(int32_t n, const uint32_t *ids) {           \
        klfb_census_killed(KIND, n);                                          \
        if (klfb_census_every() && ids &&                                     \
            ((KIND) == KLC_TEX || (KIND) == KLC_RBO))                         \
            for (int32_t i = 0; i < n; i++) klfb_vram_forget(KIND, ids[i]);   \
        if (g_real_Delete##Name) g_real_Delete##Name(n, ids);                 \
    }

KLFB_GENDEL(Textures,     KLC_TEX)
KLFB_GENDEL(Renderbuffers, KLC_RBO)
KLFB_GENDEL(Buffers,      KLC_BUF)
KLFB_GENDEL(VertexArrays, KLC_VAO)
// glGenFramebuffers already has a thunk (the FBO trace); only the delete half
// is new, and the gen half calls klfb_census_made itself.
static void (*g_real_DeleteFramebuffers)(int32_t, const uint32_t *);
static void klfb_DeleteFramebuffers(int32_t n, const uint32_t *ids) {
    klfb_census_killed(KLC_FBO, n);
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
    // A MUTABLE allocation is an allocation: only glTexStorage2D/3D used to be
    // recorded, so a render target created this way was invisible to every
    // readback path — the same blindness as a full table, arriving by a
    // different door.
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
    return g_real_CreateShader ? g_real_CreateShader(type) : 0;
}
static void (*g_real_DeleteShader)(uint32_t);
static void klfb_DeleteShader(uint32_t s) {
    klfb_census_killed(KLC_SHADER, 1);
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
// The stage observation, defined with the eye-texture table further down.
static int  klfb_stage_of_tex(uint32_t tex);
static int  klfb_stage_of_fbo(uint32_t fbo);
static void klfb_map_fbo(uint32_t fbo, uint32_t tex);
static void klfb_note_render_stage(int stage);
static void klfb_note_eye_fbo(uint32_t fbo, uint32_t touched);
static uint32_t g_draw_fb;
// ...and the one before it, which is the only record of the framebuffer a guest
// was drawing into when it binds another one to blit INTO. See the read-binding
// experiment in klfb_BlitFramebuffer.
static uint32_t g_prev_draw_fb;
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
static uint32_t g_read_fb;

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
        if (fb != g_draw_fb) g_prev_draw_fb = g_draw_fb;
        g_draw_fb = fb;
        // Which stage this frame is going into. Sticky: a guest that binds an
        // eye FBO and then bounces through others (shadow maps, post) has still
        // last committed to this stage, and the next eye bind is what moves it.
        klfb_note_render_stage(klfb_stage_of_fbo(fb));
    }
    if (g_real_BindFramebuffer) g_real_BindFramebuffer(target, fb);
    // Did GL take the name? Only asked when ERRSCAN is already draining the
    // error queue every call — otherwise this would eat exactly the errors the
    // guest's own glGetError is looking for (trap 41).
    if (glfb_errscan() && a_glGetError) {
        unsigned i = g_bindlog_slot % KLFB_BINDLOG;
        g_bindlog_err[i] = a_glGetError();
    }
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
    // guest's blit source would be trap 41's shape with the sign flipped.
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
// eye render targets are among the LAST — a table that fills up drops exactly
// the entries the readback path is about, and every consumer then reads
// "never heard of it", which klfb_probe_fbo used to turn into a silent 0 lit.
// It still has a ceiling, so it says by name when it reaches one: a full table
// is a diagnostic going quiet, and going quiet is what made this expensive.
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

void kl_glfb_note_eye_texture(int eye, int stage, uint32_t tex) {
    if (eye < 0 || eye > 1 || !tex) return;
    g_eye_tex[eye] = tex;
    // An eye pair is what distinguishes a VR guest from a flat one, and it is
    // the stronger signal of the two kl_present watches: Unity creates an EGL
    // window surface as well, being an Android app, so the window alone cannot
    // tell them apart. See kl_present.h.
    kl_present_note_eye_texture();
    if ((unsigned)stage >= KLFB_MAX_STAGES) return;
    // GL recycles names, and Unity re-creates the whole eye swapchain on
    // resize. A name that used to mean some other (eye, stage) must stop
    // answering for it the moment it is re-registered: klfb_stage_of_tex
    // returns the FIRST match, so one stale entry files a frame's pose against
    // a different stage's picture for the rest of the run — permanently, and
    // silently, because every count still looks healthy.
    for (int e = 0; e < 2; e++)
        for (int s = 0; s < KLFB_MAX_STAGES; s++)
            if (g_eye_tex_stage[e][s] == tex) g_eye_tex_stage[e][s] = 0;
    g_eye_tex_stage[eye][stage] = tex;
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
// is the one place that knows it, and the capture stops searching. Trap 31 is
// the reason this is not a search: a GL name is a slot, not an identity.
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

static int klfb_stage_of_fbo(uint32_t fbo) {
    for (int i = 0; i < KLFB_FBO_MAP; i++)
        if (g_fbo_stage[i].fbo == fbo) return klfb_stage_of_tex(g_fbo_stage[i].tex);
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
// The Metal interop (P5). PLANNING §12.9 for why it is eglCreateImageKHR and
// not a pbuffer, and for the host/device measurements that came before this.
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
// One record per (eye, stage). Stage count is ovrp_GetEyeTextureStageCount's
// answer, which is 1 today — raising it for GPU pipelining is what makes
// §12.1(3)'s "key the pose to the stage" warning bite, so the array is indexed
// by stage from the start rather than retrofitted later.
#define KL_MTL_MAX_STAGES 4
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
int kl_glfb_foveation_wanted(int *zones, float *edge) {
    if (!kl_env_on("KL_VRR", 1)) return 0;
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
// kl_glfb_set_eye_rate_map for why it forbids foveation.
static int g_eye_mirroring;

void kl_glfb_set_eye_rate_map(int w, int h, int zones_x, int zones_y, void *rate_map) {
    if (!kl_glfb_init() || !mtl_resolve()) return;
    // **Not available on the copy path, and the reason is that there is nothing
    // there to save.** Foveation is a bargain with the GUEST's rasterizer: it
    // writes fewer fragments, and the compositor's grid unwarps what it wrote.
    // On the array-mirror path the guest rasterizes into a swapchain of its own
    // that carries no map (klxr_CreateSwapchain gives it plain glTexStorage3D
    // storage), and the only thing a map could attach to is the COPY — so the
    // full-resolution picture would be squeezed on the way in and stretched back
    // out on the way to the display, paying twice for a saving nobody made.
    //
    // Worse than pointless if the blit turns out not to rasterize at all: the
    // destination would then hold an unwarped picture that the compositor
    // unwarps anyway, which is a magnified centre and a correct-looking frame —
    // the failure this file has no instrument for. Refused rather than left to
    // the frontends, because both of them build a map from the display and
    // neither can know which guest is underneath.
    if (g_eye_mirroring && rate_map) {
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
    if (g_eye_rate_w && (g_eye_rate_w != w || g_eye_rate_h != h || !rate_map))
        a_SetRateMapForSize((uint32_t)g_eye_rate_w, (uint32_t)g_eye_rate_h,
                            KL_RATE_MIN_SAMPLES, NULL);

    g_eye_rate_map = rate_map;
    g_eye_rate_w   = rate_map ? w : 0;
    g_eye_rate_h   = rate_map ? h : 0;
    g_eye_rate_zx  = rate_map ? zones_x : 0;
    g_eye_rate_zy  = rate_map ? zones_y : 0;

    if (rate_map)
        a_SetRateMapForSize((uint32_t)w, (uint32_t)h, KL_RATE_MIN_SAMPLES, rate_map);

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

    fprintf(stderr, "  [glfb] eye rate map %p for %dx%d, %dx%d zones (multisampled "
                    "targets of that size, plus every bound eye texture)\n",
            rate_map, w, h, zones_x, zones_y);
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
    g_eye_mtl[eye][stage] = (typeof(g_eye_mtl[0][0])){0};
    fprintf(stderr, "  [glfb] eye=%d stage=%d released (GL texture %u, image %p)\n",
            eye, stage, gl_tex, img);
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
// perfectly well-formed. The note that stood here through SL-15..SL-20 — "an
// encode on write and a decode on sample cancel, so this is precision rather
// than gamma" — is true about the pair and wrong about the conclusion: what
// they cancel back to is the guest's sRGB code value, and the composite needs
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
    // itself: if a teardown ever goes missing, destroying only the EGLImage —
    // which is all this used to do — leaves ANGLE's texture holding the
    // MTLTexture, and the provider's release then reclaims nothing. It says so
    // when it fires, because a silent backstop is how a leak comes back.
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
    // Foveation, if a map is in force AND it was built for this size. Before the
    // guest renders into this texture, not after: ANGLE caches a framebuffer's
    // render pass descriptor and only rebuilds it on a GL state sync, so a map
    // bound afterwards misses the first pass with nothing reporting it
    // (notes/VISIONOS.md).
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
// The ARRAY swapchain's way onto the compositor — see kl_glfb.h for why a copy
// is the only route and what it costs.
//
// One destination per (eye, stage), allocated the ordinary way: a GL name of
// OURS, given provider storage by the call above. So everything downstream —
// the eye table, the rate map, the sRGB settle, the census, the release path —
// sees exactly what it sees for a Unity/OVRPlugin guest, and this function owns
// nothing but the blit and the name it created.
static struct { uint32_t tex; int w, h; uint32_t fmt; }
    g_eye_mirror[2][KL_MTL_MAX_STAGES];

int kl_glfb_mirror_eye_layer(int eye, int stage, uint32_t src_tex, int src_layer,
                             int w, int h, uint32_t internal_fmt) {
    if (!g_mtl_provider || !src_tex || w <= 0 || h <= 0) return 0;
    if (eye < 0 || eye > 1 || stage < 0 || stage >= KL_MTL_MAX_STAGES) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror eye=%d stage=%d is out of range "
                            "(%d stages) — this eye is not composited\n",
                    eye, stage, KL_MTL_MAX_STAGES);
        return 0;
    }
    if (!kl_glfb_init() || !mtl_resolve()) return 0;

    // Before the first bind below, because that is what asks the provider for
    // storage and both providers build a rate map from the size they are asked
    // for. A map already in force (KleptonCompositor builds one in primeDisplay,
    // before the guest has rendered anything) is retired here.
    if (!g_eye_mirroring) {
        g_eye_mirroring = 1;
        if (g_eye_rate_map) kl_glfb_set_eye_rate_map(0, 0, 0, 0, NULL);
    }

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
                            "the array eye cannot be composited\n");
        return 0;
    }
    if (src_layer >= 0 && !r_FramebufferTextureLayer) {
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
        // frame. Refusing costs a black eye; guessing costs the whole picture.
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror: no glGetIntegerv, so the guest's GL "
                            "state cannot be restored — refusing\n");
        return 0;
    }

    // (Re)allocate the destination. A size or format change means the guest
    // rebuilt its swapchain, and the old destination describes a picture that no
    // longer exists — release it rather than blit a mismatch, which the blit
    // would happily scale.
    __typeof__(g_eye_mirror[0][0]) *m = &g_eye_mirror[eye][stage];
    // ...and the eye table is the authority on whether our name still exists.
    // kl_glfb_release_eye_texture deletes it, and it has callers that know
    // nothing about this record — a swapchain teardown, a re-bind backstop — so
    // a destination remembered here and gone there would be a blit into a
    // deleted name: GL_INVALID_OPERATION, once, and a black eye for the rest of
    // the run.
    if (m->tex && g_eye_mtl[eye][stage].gl_tex != m->tex) *m = (__typeof__(*m)){0};
    if (m->tex && (m->w != w || m->h != h || m->fmt != internal_fmt)) {
        fprintf(stderr, "  [glfb] mirror eye=%d stage=%d: %dx%d fmt 0x%x -> %dx%d "
                        "fmt 0x%x, reallocating\n",
                eye, stage, m->w, m->h, m->fmt, w, h, internal_fmt);
        kl_glfb_release_eye_texture(eye, stage);   // drops the EGLImage AND our name
        *m = (__typeof__(*m)){0};
    }
    if (!m->tex) {
        uint32_t t = 0;
        r_GenTextures(1, &t);
        if (!t) return 0;
        // kl_glfb_bind_eye_mtl_texture leaves GL_TEXTURE_2D bound to whatever it
        // was given, which is fine where it is normally called from (the guest's
        // own graphics setup, in ovrp_SetupEyeTexture2) and is not fine here:
        // this runs inside the guest's frame, between its calls. Six times a run
        // rather than every frame, and the failure would be a texture binding
        // the guest never asked for surviving into its next draw.
        int32_t save_tex = 0;
        a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &save_tex);
        int ok = kl_glfb_bind_eye_mtl_texture(eye, stage, t, w, h, internal_fmt);
        if (a_glBindTexture_mtl) a_glBindTexture_mtl(0x0DE1, (uint32_t)save_tex);
        if (!ok) {
            // The provider declined, so the name is ours and nothing else took a
            // reference to it. kl_glfb_bind_eye_mtl_texture has already said why.
            if (r_DeleteTextures) r_DeleteTextures(1, &t);
            return 0;
        }
        m->tex = t; m->w = w; m->h = h; m->fmt = internal_fmt;
        fprintf(stderr, "  [glfb] mirror eye=%d stage=%d: %dx%d fmt 0x%x <- guest "
                        "tex %u layer %d (one blit a frame; the array swapchain "
                        "cannot be re-pointed)\n",
                eye, stage, w, h, internal_fmt, src_tex, src_layer);
    }

    // The guest's state is the guest's. A blit is affected by the scissor test
    // (and by nothing else in the fragment pipeline — the write masks do not
    // apply), so that is what has to come off, and the two framebuffer bindings
    // are read back rather than assumed: this runs inside the guest's frame,
    // between its own calls.
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
    r_FramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, m->tex, 0);

    // Both sides checked once. An incomplete framebuffer makes the blit a no-op
    // that raises INVALID_FRAMEBUFFER_OPERATION, i.e. a black eye and one error
    // in a log full of them — the same failure this whole path exists to end.
    static int checked;
    if (!checked && r_CheckFramebufferStatus) {
        checked = 1;
        uint32_t rs = r_CheckFramebufferStatus(0x8CA8);
        uint32_t ds = r_CheckFramebufferStatus(0x8CA9);
        if (rs != 0x8CD5 || ds != 0x8CD5)
            fprintf(stderr, "  [glfb] mirror: read fb status 0x%x, draw fb status 0x%x "
                            "(0x8CD5 is complete) — the eye will stay black\n", rs, ds);
    }

    if (a_glGetError) while (a_glGetError()) {}
    // Source and destination are the same format, so the sRGB decode on read and
    // encode on write are each other's inverse and this is a copy. NEAREST for
    // the same reason: the rectangles are identical, so no filter is consulted.
    r_BlitFramebuffer(0, 0, w, h, 0, 0, w, h,
                      0x4000 /* COLOR_BUFFER_BIT */, 0x2600 /* NEAREST */);
    uint32_t e = a_glGetError ? a_glGetError() : 0;

    r_BindFramebuffer(0x8CA8, (uint32_t)save_read);
    r_BindFramebuffer(0x8CA9, (uint32_t)save_draw);
    if (scissor && r_Enable) r_Enable(0x0C11);

    if (e) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror eye=%d stage=%d: glBlitFramebuffer -> "
                            "GL error 0x%x\n", eye, stage, e);
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// The decoded-video image (SL-13). An AHardwareBuffer — which is one of our
// CVPixelBuffers, see kl_mediandk.h — sampled as a GL texture.
//
// The guest's sequence is Android's, and every step of it is missing here:
//
//     buf = eglGetNativeClientBufferANDROID(ahardwarebuffer)
//     img = eglCreateImageKHR(dpy, NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, buf, ...)
//     glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex)
//     glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img)
//
// ANGLE's Metal backend has no external images at all (SL-10, DisplayMtl.mm:984)
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
// klfb_detarget did that in SL-10), and BGRA output means the YUV→RGB conversion
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
    void    *pbuf;              // the EGLSurface over the IOSurface
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

void *kl_glfb_image_from_pixels(void *pixels, int *out_w, int *out_h) {
    if (!pixels) return NULL;
    if (!kl_glfb_init() || !img_resolve()) {
        fprintf(stderr, "  [glfb] no ANGLE IOSurface entry points — a decoded frame "
                        "cannot be turned into a texture (is KL_GLFB=1?)\n");
        return NULL;
    }
    IOSurfaceRef surf = CVPixelBufferGetIOSurface((CVPixelBufferRef)pixels);
    if (!surf) {
        fprintf(stderr, "  [glfb] the decoded frame is not IOSurface-backed — "
                        "kCVPixelBufferIOSurfacePropertiesKey is what makes it so\n");
        return NULL;
    }
    // The attribute list below states BGRA/UNSIGNED_BYTE to ANGLE, and ANGLE only
    // WARNs when the IOSurface disagrees (IOSurfaceSurfaceMtl::ValidateAttributes
    // compares bytes-per-element, which every 4-byte format passes). So check the
    // pixel format here instead: a decoder that started handing back NV12 would
    // otherwise sample as garbage rather than fail.
    OSType pf = CVPixelBufferGetPixelFormatType((CVPixelBufferRef)pixels);
    if (pf != kCVPixelFormatType_32BGRA) {
        fprintf(stderr, "  [glfb] decoded frame is pixel format '%c%c%c%c', not BGRA — "
                        "refusing to describe it to ANGLE as something it is not\n",
                (char)(pf >> 24), (char)(pf >> 16), (char)(pf >> 8), (char)pf);
        return NULL;
    }
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

// The other half of SL-10's detarget, and it is the difference between a picture
// and a black rectangle.
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
    if (im->bound_tex) {
        a_eglReleaseTexImage(g_dpy, im->pbuf, EGL_BACK_BUFFER_);
        im->bound_tex = 0;
    }
    for (klfb_image *o = g_images; o; o = o->next)
        if (o != im && o->bound_tex == (uint32_t)tex) {
            a_eglReleaseTexImage(g_dpy, o->pbuf, EGL_BACK_BUFFER_);
            o->bound_tex = 0;
        }
    pthread_mutex_unlock(&g_images_lk);
    while (a_glGetError && a_glGetError()) {}
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
    return 1;
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
    if (im->bound_tex && a_eglReleaseTexImage)
        a_eglReleaseTexImage(g_dpy, im->pbuf, EGL_BACK_BUFFER_);
    if (a_eglDestroySurface_img) a_eglDestroySurface_img(g_dpy, im->pbuf);
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

static void klfb_BlitFramebuffer(int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1,
                                 int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1,
                                 int64_t mask, int64_t filter) {
    klfb_errprobe("glBlitFramebuffer(before)", NULL);
    static int blit_log = -1;
    if (blit_log < 0) blit_log = kl_env_on("KL_GLFB_ERRPROBE", 0);
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
    if (blit_log && a_glGetIntegerv) {
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
    // KL_GLFB_BLIT_READ_FIX=1 — an EXPERIMENT, not a fix, and it is here to
    // answer one question: when VRChat's Unity blits into its OpenXR eye
    // swapchain it passes framebuffer 0 as the source (confirmed in libunity's
    // own code — the read bind is skipped by the redundant-bind check, which is
    // only reachable with a requested name of 0), so the eye receives the
    // default framebuffer instead of the array texture Unity just rendered
    // thousands of draws into. Substituting the framebuffer the guest was
    // drawing into immediately before says whether that IS what it meant.
    // A wrong answer here is a picture; the right one is a cause.
    //
    // It runs BEFORE the probe below so that the probe's "before" line reports
    // the substituted source rather than the one being replaced.
    static int read_fix = -1;
    if (read_fix < 0) read_fix = kl_env_on("KL_GLFB_BLIT_READ_FIX", 0);
    int32_t fixed_from = -1;
    if (a_glGetIntegerv) {
        int32_t rfb2 = -1;
        a_glGetIntegerv(0x8CAA, &rfb2);
        g_blits++;
        if (rfb2 == 0) g_blits_read0++;
        if (read_fix && rfb2 == 0 && g_prev_draw_fb) {
            static void (*rf_bind)(uint32_t, uint32_t);
            if (!rf_bind) rf_bind = asym("glBindFramebuffer");
            if (rf_bind) { rf_bind(0x8CA8, g_prev_draw_fb); fixed_from = (int32_t)g_prev_draw_fb; }
        }
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
    if (blit_probe && a_glGetIntegerv) {
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
            // texture is named rather than guessed for the reason trap 32 and
            // trap 41 are both about: an instrument that assumes reports a
            // working pipeline as a broken one. Layer 1 on a non-array texture
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
    if (g_real_BlitFramebuffer)
        g_real_BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                               (uint32_t)mask, (uint32_t)filter);
    if (fixed_from >= 0) {
        static void (*rf_bind)(uint32_t, uint32_t);
        if (!rf_bind) rf_bind = asym("glBindFramebuffer");
        if (rf_bind) rf_bind(0x8CA8, 0);       // put back what the guest had
        static int said_fix;
        if (!said_fix++)
            fprintf(stderr, "  [glfb] BLIT_READ_FIX: the guest blitted with read "
                            "framebuffer 0; read it from fb %d instead\n", fixed_from);
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
// the *format* has already been ruled out (PLANNING §12.9): the emulated-format
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
    // a line in any run. The knobs add the clean ones, which is what makes the
    // host and the Simulator diffable.
    if (klfb_trace_tex() || klfb_probe_ctex() || *verdict)
        fprintf(stderr, "  [glfb] #%u glCompressedTexSubImage2D target=0x%04x "
                        "level=%d %dx%d at %d,%d fmt=0x%04x imageSize=%d "
                        "expect=%ld (tex=%d alloc 0x%04x %dx%d, level %dx%d)%s\n",
                g_texcalls++, target, level, w, h, xoff, yoff, fmt, sz, expect,
                bound, tfmt, tw, th, lw, lh, verdict);

    if (g_real_CompressedTexSubImage2D)
        g_real_CompressedTexSubImage2D(target, level, xoff, yoff, w, h, fmt, sz, data);
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
static struct { int32_t fb; unsigned draws; uint64_t tid; } g_draw_fbs[KLFB_MAX_FBS];
static unsigned g_ndraw_fbs;

static void klfb_note_draw(void) {
    int32_t fb = -1;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
    // The framebuffer the last DRAW CALL went to. g_prev_draw_fb is a different
    // thing — the binding before the current one — and at a blit the two are
    // usually equal, which silently hid the comparison that matters: this guest
    // draws into one framebuffer and blits out of another.
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
// M5 arc and never printed, so "where is the guest actually drawing?" — the
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
                snprintf(what, sizeof what, " (colour0 tex %u%s %dx%d fmt 0x%x)",
                         tex, lb, w, h, fmt);
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
    if (!err || said >= 20) return;
    said++;
    int32_t fb = -1, prog = -1, vp[4] = {0,0,0,0};
    if (a_glGetIntegerv) {
        a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
        a_glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &prog);
        a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
    }
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
// direct RGBA/UNSIGNED_BYTE readback cannot see (that was the first version
// of this probe, and its err 0x500 lines).
static void klfb_draw_probe(int verts) {
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
}

static void klfb_DrawElements(uint32_t mode, int32_t count, uint32_t type,
                              const void *indices) {
    // A pending capture first: it runs on THIS thread, which is the one that
    // draws, so it reads the pbuffer the frame actually landed in — not the
    // swap thread's, which is empty by construction.
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
    klfb_service_capture();
    klfb_note_draw();
    klfb_errprobe0("glDrawArrays(before)");
    if (g_real_DrawArrays) g_real_DrawArrays(mode, first, count);
    klfb_draw_probe(count);
    klfb_errprobe0("glDrawArrays(after)");
}
// The instanced/basevertex variants: Unity's actual scene geometry goes
// through these, not the plain two — the first census wrapped only those and
// was blind to the scene entirely.
static void klfb_DrawElementsInstanced(uint32_t mode, int32_t count, uint32_t type,
                                       const void *indices, int32_t instances) {
    klfb_note_draw();
    if (g_real_DrawElementsInstanced)
        g_real_DrawElementsInstanced(mode, count, type, indices, instances);
    klfb_draw_probe(count * instances);
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
}
static void klfb_DrawArraysInstanced(uint32_t mode, int32_t first, int32_t count,
                                     int32_t instances) {
    klfb_note_draw();
    if (g_real_DrawArraysInstanced)
        g_real_DrawArraysInstanced(mode, first, count, instances);
    klfb_draw_probe(count * instances);
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

static void klfb_GetProgramInterfaceiv(uint32_t program, uint32_t iface,
                                       uint32_t pname, int32_t *params) {
    klfb_res_resolve();
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
// table, and a fence that is created but never committed is worse than no
// fence at all — the compositor would wait on a value the GPU never reaches.
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
    // Drain first: the guest leaves errors unread, and the first version of
    // this bracket misreported a leftover as glActiveTexture's own. Errors
    // after this point are provably this call's.
    if (a_glGetError) while (a_glGetError()) {}
    if (g_real_ActiveTexture) g_real_ActiveTexture(unit);
    // Which units does the guest actually select? Unity's "Invalid texture
    // unit" check rejects above its own cap — if that preempts the bind, no
    // high unit ever reaches GL, and measuring the request stream proves it.
    {
        static int logu = -1, saidu;
        static uint8_t seen[64];
        if (logu < 0) logu = kl_env_on("KL_GLFB_LOG_UNITS", 0);
        int u = (int)(unit - 0x84C0);
        if (logu && u >= 0 && u < 64 && !seen[u]) {
            seen[u] = 1; saidu++;
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
static uint32_t klfb_detarget(uint32_t t) {
    return t == GL_TEXTURE_EXTERNAL_OES_ ? GL_TEXTURE_2D_ : t;
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
static void klfb_BindTexture(uint32_t t, uint32_t n) {
    if (a_glGetError) while (a_glGetError()) {}
    if (g_real_BindTexture) g_real_BindTexture(klfb_detarget(t), n);
    klfb_err_say("glBindTexture", n);
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
// `out`. Printing only the matching location was not enough: the answer to "why
// is the guest writing a float at location 1" turned out to be that it never
// asked for location 1 at all, and only the whole list says so.
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
static void (*g_real_BindSampler)(uint32_t, uint32_t);
static void klfb_BindSampler(uint32_t u, uint32_t s) {
    if (a_glGetError) while (a_glGetError()) {}
    if (g_real_BindSampler) g_real_BindSampler(u, s);
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
#define KLFB_UV(fn, T) KLFB_UNIFORM_N(fn, T, 0, (int32_t l, int32_t n, const T *v), \
                                      (klfb_remap_loc(l), n, v))
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

static void (*g_real_Uniform1i)(int32_t, int32_t);
static void klfb_Uniform1i(int32_t loc, int32_t v) {
    if (a_glGetError) while (a_glGetError()) {}
    loc = klfb_remap_loc(loc);
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
    {"glBindTexture", (void *)klfb_BindTexture, (void **)&g_real_BindTexture},
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
    {"glDrawElements", (void *)klfb_DrawElements, (void **)&g_real_DrawElements},
    {"glDrawArrays",   (void *)klfb_DrawArrays,   (void **)&g_real_DrawArrays},
    {"glDrawElementsInstanced", (void *)klfb_DrawElementsInstanced, (void **)&g_real_DrawElementsInstanced},
    {"glDrawElementsBaseVertex", (void *)klfb_DrawElementsBaseVertex, (void **)&g_real_DrawElementsBaseVertex},
    {"glDrawArraysInstanced", (void *)klfb_DrawArraysInstanced, (void **)&g_real_DrawArraysInstanced},
    {"glFinish", (void *)klfb_Finish, (void **)&g_real_Finish},
    {"glBlitFramebuffer",         (void *)klfb_BlitFramebuffer,         (void **)&g_real_BlitFramebuffer},
    {"glInvalidateFramebuffer",   (void *)klfb_InvalidateFramebuffer,   (void **)&g_real_InvalidateFramebuffer},
    {"glClear",                   (void *)klfb_Clear,                   (void **)&g_real_Clear},
    {"glClearColor",              (void *)klfb_ClearColor,              (void **)&g_real_ClearColor},
    {"glRenderbufferStorageMultisample", (void *)klfb_RenderbufferStorageMultisample, (void **)&g_real_RenderbufferStorageMultisample},
    {"glRenderbufferStorage",     (void *)klfb_RenderbufferStorage,     (void **)&g_real_RenderbufferStorage},
    {"glTexSubImage3D",           (void *)klfb_TexSubImage3D,           (void **)&g_real_TexSubImage3D},
    {"glCompressedTexSubImage2D", (void *)klfb_CompressedTexSubImage2D, (void **)&g_real_CompressedTexSubImage2D},
    {"glCompressedTexSubImage3D", (void *)klfb_CompressedTexSubImage3D, (void **)&g_real_CompressedTexSubImage3D},
    // the shared-context compile lock — see the block above s10's numbers
    {"glShaderSource", (void *)klfb_ShaderSource, (void **)&g_real_ShaderSource},
    {"glCompileShader", (void *)klfb_CompileShader, (void **)&g_real_CompileShader},
    {"glLinkProgram",   (void *)klfb_LinkProgram,   (void **)&g_real_LinkProgram},
    // the texture storage census — which format the AGX abort follows
    {"glTexStorage2D", (void *)klfb_TexStorage2D, (void **)&g_real_TexStorage2D},
    {"glTexStorage3D", (void *)klfb_TexStorage3D, (void **)&g_real_TexStorage3D},
    {"glTexSubImage2D", (void *)klfb_TexSubImage2D, (void **)&g_real_TexSubImage2D},
    {"glTexParameteri", (void *)klfb_TexParameteri, (void **)&g_real_TexParameteri},
    {"glGenFramebuffers", (void *)klfb_GenFramebuffers, (void **)&g_real_GenFramebuffers},
    {"glBindFramebuffer", (void *)klfb_BindFramebuffer, (void **)&g_real_BindFramebuffer},
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
// eye blit died GL_INVALID_FRAMEBUFFER_OPERATION every frame (M6).
// The query family is the hybrid, but the split is not "table vs rest":
// ANGLE's context here is ES 3.0 while we describe 3.2 to the guest, so a
// capability pname the table does not list (the SSBO family, 0x90d6..) must
// NOT go to ANGLE — there it is a real GL_INVALID_ENUM, and the null driver's
// named zero is exactly what keeps Unity off those paths. Only genuine
// dynamic *state* — Unity's save/restore set around its blits — belongs to
// ANGLE, which alone tracks it. The proof was 0x0C02 (READ_BUFFER): our 0
// became GL_INVALID_ENUM one glReadBuffer later, and the eye blit died
// GL_INVALID_FRAMEBUFFER_OPERATION every frame (M6).
static int glfb_state_pname(uint32_t p) {
    switch (p) {
    case 0x0C01: case 0x0C02:   // DRAW_BUFFER, READ_BUFFER
    case 0x8CA6: case 0x8CAA:   // DRAW/READ_FRAMEBUFFER_BINDING
    case 0x8B8D:                // CURRENT_PROGRAM
    case 0x84E0:                // ACTIVE_TEXTURE
    case 0x0BA2: case 0x0C10:   // VIEWPORT, SCISSOR_BOX
        return 1;
    }
    return 0;
}

static void klfb_GetIntegerv(uint32_t pname, int32_t *params) {
    if (glfb_state_pname(pname)) {
        if (g_real_GetIntegerv) g_real_GetIntegerv(pname, params);
        return;
    }
    if (kl_gl_cap_integerv(pname, params)) return;
    // Same contract as the null driver: named, not guessed — and crucially
    // no GL error set, which forwarding to the ES 3.0 context would do.
    fprintf(stderr, "  [glfb] glGetIntegerv: unhandled pname 0x%x (kept ours)\n", pname);
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
// census can reach. This is the instrument the AGX abort needs: the texture trace
// could only say the crash arrived somewhere after an R8 upload, and "somewhere
// after" spans every call the engine makes next.
//
// It is expensive by construction — a log line per GL call — so it is off unless
// asked for. KL_GLFB_TRACE_FROM=<n> starts printing only after the nth call, which
// is how a crash tens of thousands of calls in stays readable.
typedef struct { const char *name; void *real; } klfb_trace_desc;

extern void kl_gl_trace_tramp(void);       // runtime/kl_gl_trace.S
extern void kl_gl_lock_tramp(void);        // runtime/kl_gl_lock.S

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
// afterwards so this cannot become the thing the next scan reports (trap 41's
// rule: an instrument that perturbs what it measures is worse than none).
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
        static unsigned said;   // plain: __atomic_* reject _Atomic-qualified operands
        uint32_t e = a_glGetError();
        if (e && (!g_err_only || e == g_err_only) &&
            __atomic_load_n(&said, __ATOMIC_RELAXED) < 40) {
            __atomic_fetch_add(&said, 1, __ATOMIC_RELAXED);
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

void *kl_glfb_sym(const char *name) {
    if (!kl_glfb_enabled() || !name) return NULL;
    if (!g_ready && !kl_glfb_init()) return NULL;
    if (keep_ours(name)) return NULL;              // caller keeps its own answer
    if (glfb_skipped(name)) {
        fprintf(stderr, "  [glfb] KL_GLFB_SKIP: %s stays on the null driver\n", name);
        return NULL;
    }

    void *fn = asym(name);
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
        if (tg && tg->ctx) return klfb_gpu_frame_now();
        g_capture_pending = 1;
        return g_presented;
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
    // Finish first, for every caller. The draw probe used to do this and the
    // blit probe did not, and the two then disagreed about the same texture in
    // the same run — which is a difference between the INSTRUMENTS being read
    // as a difference in the pipeline. A debug readback can afford the stall;
    // being unable to trust it costs whole sessions.
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
            // rather than as whatever the staging texture held before. The
            // first version of this probe had no sentinel and produced
            // back-to-back contradictory readings of the same renderbuffer —
            // check the instrument before trusting it.
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
    static uint32_t layer_fb;
    if (!layer_fb) r_GenFramebuffers(1, &layer_fb);
    if (!layer_fb) return 0;
    r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, layer_fb);
    r_FramebufferTextureLayer(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */, tex, 0, layer);
    if (r_CheckFramebufferStatus(0x8CA8) == 0x8CD5) return layer_fb;
    // A layered attach only works on an ARRAY or 3D texture, and this guest's
    // eye targets stopped being arrays the moment it asked for one swapchain
    // per eye — so the probe built for the array case answered "no framebuffer"
    // for every plain 2D texture, which reads as "nothing to see" rather than
    // as "wrong attach call". Layer 0 of a 2D texture is the texture.
    if (layer == 0) {
        static void (*r_FramebufferTexture2D)(uint32_t, uint32_t, uint32_t,
                                              uint32_t, int32_t);
        if (!r_FramebufferTexture2D)
            r_FramebufferTexture2D = asym("glFramebufferTexture2D");
        if (r_FramebufferTexture2D) {
            r_FramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1 /* TEXTURE_2D */, tex, 0);
            if (r_CheckFramebufferStatus(0x8CA8) == 0x8CD5) return layer_fb;
        }
    }
    return 0;
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
                    // small attachment into undefined pixels — a census that
                    // invents lit counts is worse than one that says it could
                    // not look. The targeted probes (blit, draw, clear) each
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
