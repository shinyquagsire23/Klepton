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
#include "kl_glfb.h"
#include "kl_egl.h"        // kl_gl_cap_* — the capability tables
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

// The vendored debug build (vendor/angle/out/Debug, see Makefile `angle-debug`)
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
    // KL_GLFB_SIZE=WxH overrides the eye size the guest asked for. Every spike that
    // failed to reproduce the AGX abort used a 256x256 pbuffer while the guest uses
    // 1832x1920, and each thread gets its own with depth and stencil — so surface
    // size is the one resource axis none of them varied. Shrinking it here tests
    // that without touching anything else.
    const char *env = getenv("KL_GLFB_SIZE");
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
    const char *size_env = getenv("KL_GLFB_SIZE");
    if (size_env) {
        int ew = 0, eh = 0;
        if (sscanf(size_env, "%dx%d", &ew, &eh) == 2 && ew > 0 && eh > 0) {
            g_w = ew; g_h = eh;
        }
    }

    const char *dir = kl_angle_dir();
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

    // KL_GLFB_DEBUG_CB=1: register a KHR_debug callback and print every
    // message. The vendored debug ANGLE speaks KHR_debug fluently, so a guest
    // call failing silently (the "0 lit" frame was one) comes with its reason
    // here instead of needing an error-code probe per call site.
    if (getenv("KL_GLFB_DEBUG_CB")) {
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
        if (!getenv("KL_GLFB_SHARED")) {
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
            void *share = getenv("KL_GLFB_NOSHARE_OBJ") ? EGL_NO_CONTEXT : g_ctx;
            t->ctx  = a_eglCreateContext(g_dpy, g_cfg, share, ctx_attrs);
        }
        pthread_setspecific(g_tls_key, t);
        fprintf(stderr, "  [glfb] thread %llu gets its own %s context\n",
                (unsigned long long)tid, t->ctx == g_ctx ? "root" : "shared");
    } else if (!getenv("KL_GLFB_SHARED") && !t->ctx &&
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
    if (getenv("KL_GLFB_PROBE") && !t->probed) {
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
    } else if (getenv("KL_GLFB_DEBUG_CB") && !t->debug_cb) {
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

// If the layout qualifier starting at `p` ("layout(...)") directly precedes a
// `uniform` declaration, remove it (and the spaces between) in place and
// return the new scan position; otherwise return NULL. Used to strip explicit
// uniform locations: layout(location=N) on a *uniform* is legal GLSL only
// from ES 3.1 on ("only valid on program inputs and outputs" here), and
// Unity resolves sampler locations with glGetUniformLocation regardless, so
// the pin is a no-op semantically. in/out declarations keep their qualifiers
// — those ARE program inputs/outputs and ES 3.0 wants them.
static char *klfb_strip_uniform_layout(char *p) {
    char *close = strchr(p, ')');
    if (!close) return NULL;
    char *q = close + 1;
    while (*q == ' ' || *q == '\t') q++;
    if (strncmp(q, "uniform", 7) != 0) return NULL;
    memmove(p, q, strlen(q) + 1);
    return p;
}

// Returns buf rewritten in place (it only ever shrinks), or NULL if no rule
// applied.
static char *klfb_rewrite_glsl(char *buf) {
    int changed = 0;
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
        char *q = klfb_strip_uniform_layout(p);
        if (q) { p = q; changed = 1; } else p += 5;
    }
    p = buf;
    while ((p = strstr(p, "layout(location = "))) {
        char *q = klfb_strip_uniform_layout(p);
        if (q) { p = q; changed = 1; } else p += 8;
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
            char *rewritten = klfb_rewrite_glsl(buf);
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
        const char *dir = getenv("KL_DUMP_SHADERS");
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
    // KL_GLFB_DUMP_PROGRAM=N: print the sources that were linked into program
    // N. The timeline names programs by number ("the frame's last draw is
    // program 7"); this turns the number into the shader text.
    static int dump_prog = -2;
    if (dump_prog == -2) {
        const char *d = getenv("KL_GLFB_DUMP_PROGRAM");
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

// KL_GLFB_NOSRGB=1 substitutes GL_RGBA8 for GL_SRGB8_ALPHA8 at allocation. The
// census found the guest's eye-sized render target is SRGB8_ALPHA8, and sRGB is a
// case Metal's fixed-function blit cannot always service — it falls back to a
// compute blit, which is the AGX family that aborts. Swapping the format is the
// one-line way to test that without touching anything else; the two are upload- and
// attachment-compatible, so only the colour transfer function changes. A frame
// captured with this set is wrong (un-decoded sRGB), which is fine for a probe.

static uint32_t klfb_maybe_unsrgb(uint32_t fmt) {
    static int on = -1;
    if (on < 0) on = getenv("KL_GLFB_NOSRGB") != NULL;
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
    if (on < 0) on = getenv("KL_GLFB_TRACE_TEX") != NULL;
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

// Highest FBO name handed out so far — the census scans 1..g_fbomax. Binding a
// name glGenFramebuffers never returned is INVALID_OPERATION in ES 3.0, so the
// scan needs a real upper bound, not a guess.
static uint32_t g_fbomax;

static int klfb_trace_fbo(void) {
    static int on = -1;
    if (on < 0) on = getenv("KL_GLFB_TRACE_FBO") != NULL;
    return on;
}
static uint64_t klfb_tid(void) { uint64_t t = 0; pthread_threadid_np(NULL, &t); return t; }

static void klfb_GenFramebuffers(int32_t n, uint32_t *ids) {
    if (g_real_GenFramebuffers) g_real_GenFramebuffers(n, ids);
    if (ids)
        for (int32_t i = 0; i < n; i++)
            if (ids[i] > g_fbomax) g_fbomax = ids[i];
    if (klfb_trace_fbo() && ids)
        fprintf(stderr, "  [glfb] t%llu glGenFramebuffers(%d) -> %u\n",
                (unsigned long long)klfb_tid(), n, ids[0]);
}
static void klfb_BindFramebuffer(uint32_t target, uint32_t fb) {
    if (klfb_trace_fbo())
        fprintf(stderr, "  [glfb] t%llu glBindFramebuffer(0x%x, %u)\n",
                (unsigned long long)klfb_tid(), target, fb);
    if (g_real_BindFramebuffer) g_real_BindFramebuffer(target, fb);
}
static void klfb_FramebufferTexture2D(uint32_t target, uint32_t attachment,
                                      uint32_t textarget, uint32_t texture,
                                      int32_t level) {
    if (klfb_trace_fbo())
        fprintf(stderr, "  [glfb] t%llu glFramebufferTexture2D(att=0x%x, tex=%u)\n",
                (unsigned long long)klfb_tid(), attachment, texture);
    if (g_real_FramebufferTexture2D)
        g_real_FramebufferTexture2D(target, attachment, textarget, texture, level);
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
    if (noswz < 0) noswz = getenv("KL_GLFB_NOSWIZZLE") != NULL;
    if (noswz && pname >= GL_TEXTURE_SWIZZLE_R && pname <= GL_TEXTURE_SWIZZLE_A) return;
    // TEXTURE_SRGB_DECODE_EXT writes: no EXT_texture_sRGB_decode on this ANGLE,
    // so the write can only raise INVALID_ENUM — and does, about once per
    // frame, leaving a sticky error that Unity then reports attributed to
    // innocent calls (and that error-bracketing probes read as everyone's
    // fault). Dropping it is semantics-preserving here: the decode behaviour
    // it would select does not exist on this driver either way.
    if (pname == 0x8A48 /* TEXTURE_SRGB_DECODE_EXT */) return;
    if (g_real_TexParameteri) g_real_TexParameteri(target, pname, param);
    if (getenv("KL_GLFB_ERRPROBE") && a_glGetError) {
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
        const char *e = getenv("KL_GLFB_TEX_LIMIT");
        n = e ? atoi(e) : -1;          // -1 means unlimited
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
        if (bound > 0) klfb_note_tex_storage((uint32_t)bound, fmt, w, h);
    }
    if (g_real_TexStorage2D) g_real_TexStorage2D(target, levels, fmt, w, h);
}
static void klfb_TexStorage3D(uint32_t target, int32_t levels, uint32_t fmt,
                              int32_t w, int32_t h, int32_t d) {
    klfb_note_format(fmt, w, h, d, "glTexStorage3D");
    fmt = klfb_maybe_unsrgb(fmt);
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
#define KLFB_MAX_TEX 512
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

void kl_glfb_note_eye_texture(int eye, uint32_t tex) {
    if (eye < 0 || eye > 1 || !tex) return;
    g_eye_tex[eye] = tex;
}

static void (*g_real_BlitFramebuffer)(int32_t, int32_t, int32_t, int32_t, int32_t,
                                      int32_t, int32_t, int32_t, uint32_t, uint32_t);

static void klfb_errprobe(const char *what, const char *detail);
// Defined with the capture below; the blit probe uses it. dump/dw/dh are an
// optional pixel out: when dump is non-NULL the probe tone-maps the readback
// into it (g_w*g_h*4 bytes, bottom-up rows) and reports the clipped size.
static unsigned long klfb_probe_fbo(uint32_t fb, float *fbuf, uint8_t *bbuf,
                                    char *note, size_t note_n,
                                    uint8_t *dump, int32_t *dw, int32_t *dh);

// glInvalidateFramebuffer matters here because ANGLE's Metal backend actually
// discards (memoryless attachments): an invalidate of the scene color
// renderbuffer placed BEFORE the eye-resolve blit reads as "the blit copied
// black" downstream. The thunk exists to put the call on the probe timeline.
static void (*g_real_InvalidateFramebuffer)(uint32_t, int32_t, const uint32_t *);

static void klfb_InvalidateFramebuffer(int64_t target, int32_t n,
                                       const uint32_t *attachments) {
    if (getenv("KL_GLFB_BLIT_PROBE") && a_glGetIntegerv) {
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
    if (no_inv < 0) no_inv = getenv("KL_GLFB_NO_INVALIDATE") != NULL;
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
    if (on < 0) on = getenv("KL_GLFB_BLIT_PROBE") != NULL;
    return on;
}

static void klfb_Clear(uint32_t mask) {
    if (klfb_timeline() && a_glGetIntegerv) {
        int32_t dfb = -1;
        a_glGetIntegerv(0x8CA6, &dfb);
        fprintf(stderr, "  [glfb] glClear(mask=0x%x) on draw_fb=%d\n", mask, dfb);
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
    if (g_real_RenderbufferStorage) g_real_RenderbufferStorage(target, fmt, w, h);
}

static void klfb_BlitFramebuffer(int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1,
                                 int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1,
                                 int64_t mask, int64_t filter) {
    klfb_errprobe("glBlitFramebuffer(before)", NULL);
    static int blit_log = -1;
    if (blit_log < 0) blit_log = getenv("KL_GLFB_ERRPROBE") != NULL;
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
    // KL_GLFB_BLIT_PROBE=1: probe the blit's source BEFORE the blit and its
    // destination AFTER — the swap-time capture found every FBO black, which
    // has two very different readings: the pixels were gone by then
    // (glInvalidateFramebuffer discards), or the draws themselves produce
    // black. Probing both sides of the blit puts the loss on the timeline.
    static int blit_probe = -1;
    if (blit_probe < 0) blit_probe = getenv("KL_GLFB_BLIT_PROBE") != NULL;
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
            unsigned long ls = klfb_probe_fbo((uint32_t)rfb, pfb, pbb, ns, sizeof ns,
                                              NULL, NULL, NULL);
            fprintf(stderr, "  [glfb] BLIT_PROBE before: read_fb=%d %lu lit (%s)\n",
                    rfb, ls, ns);
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
    if (blit_probe && a_glGetError) {
        uint32_t be = a_glGetError();
        if (be)
            fprintf(stderr, "  [glfb] BLIT_PROBE: guest blit fb%d -> fb%d "
                            "raised 0x%x\n", rfb, dfb, be);
    }
    if (blit_probe && pfb && pbb && bp_bind) {
        char nd[160] = "";
        unsigned long ld = klfb_probe_fbo((uint32_t)dfb, pfb, pbb, nd, sizeof nd,
                                          NULL, NULL, NULL);
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
    if (on < 0) on = getenv("KL_GLFB_ERRPROBE") != NULL;
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
    static int on = -1, said, quota;
    if (on < 0) {
        on = getenv("KL_GLFB_DRAW_PROBE") != NULL;
        // KL_GLFB_DRAW_PROBE_N overrides the 12-line default; scene frames
        // burn the quota on early frames otherwise. 0 means unlimited.
        const char *q = getenv("KL_GLFB_DRAW_PROBE_N");
        quota = q ? atoi(q) : 12;
    }
    if (!on || (quota && said >= quota) || !a_glReadPixels) return;
    // KL_GLFB_DRAW_PROBE_MIN lowers the 32-vert floor — the frame's last few
    // draws are small, and one of them is a suspect in the scene's erasure.
    static int minv = -1;
    if (minv < 0) {
        const char *m = getenv("KL_GLFB_DRAW_PROBE_MIN");
        minv = m ? atoi(m) : 32;
    }
    if (verts < minv) return;
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
    if (pfb && pbb)
        lit = klfb_probe_fbo((uint32_t)fb, pfb, pbb, note, sizeof note,
                             NULL, NULL, NULL);
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
static void klfb_DrawElementsBaseVertex(uint32_t mode, int32_t count, uint32_t type,
                                        const void *indices, int32_t basevertex) {
    klfb_note_draw();
    if (g_real_DrawElementsBaseVertex)
        g_real_DrawElementsBaseVertex(mode, count, type, indices, basevertex);
    klfb_draw_probe(count);
}
static void klfb_DrawArraysInstanced(uint32_t mode, int32_t first, int32_t count,
                                     int32_t instances) {
    klfb_note_draw();
    if (g_real_DrawArraysInstanced)
        g_real_DrawArraysInstanced(mode, first, count, instances);
    klfb_draw_probe(count * instances);
}

static void klfb_report_draws(void) {
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

static int32_t klfb_GetProgramResourceLocation(uint32_t program, uint32_t iface,
                                               const char *name) {
    klfb_res_resolve();
    if (name && iface == KLFB_IF_UNIFORM && r_GetUniformLocation)
        return r_GetUniformLocation(program, name);
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

static void klfb_service_capture(void) {
    if (!g_capture_pending) return;
    g_capture_pending = 0;
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
        if (logu < 0) logu = getenv("KL_GLFB_LOG_UNITS") != NULL;
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
    klfb_err_say("glUseProgram", p);
}
static void (*g_real_BindTexture)(uint32_t, uint32_t);
static void klfb_BindTexture(uint32_t t, uint32_t n) {
    if (a_glGetError) while (a_glGetError()) {}
    if (g_real_BindTexture) g_real_BindTexture(t, n);
    klfb_err_say("glBindTexture", n);
}
static void (*g_real_BindSampler)(uint32_t, uint32_t);
static void klfb_BindSampler(uint32_t u, uint32_t s) {
    if (a_glGetError) while (a_glGetError()) {}
    if (g_real_BindSampler) g_real_BindSampler(u, s);
    klfb_err_say("glBindSampler", s);
}
static void (*g_real_Uniform1i)(int32_t, int32_t);
static void klfb_Uniform1i(int32_t loc, int32_t v) {
    if (a_glGetError) while (a_glGetError()) {}
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
    {"glBindSampler", (void *)klfb_BindSampler, (void **)&g_real_BindSampler},
    {"glUniform1i",   (void *)klfb_Uniform1i,   (void **)&g_real_Uniform1i},
    {"glGenerateMipmap", (void *)klfb_GenerateMipmap, (void **)&g_real_GenerateMipmap},
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
    {"glFramebufferTexture2D", (void *)klfb_FramebufferTexture2D, (void **)&g_real_FramebufferTexture2D},
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
    const char *list = getenv("KL_GLFB_SKIP");
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
    if (!getenv("KL_GLFB_SELFTEST")) return;
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
void kl_gl_trace_log(const char *name) {
    unsigned n = __atomic_fetch_add(&g_trace_calls, 1, __ATOMIC_RELAXED);
    if (n >= g_trace_from)
        fprintf(stderr, "  [gl] #%u %s\n", n, name);
}

static int glfb_tracing(void) {
    static int on = -1;
    if (on < 0) {
        on = getenv("KL_GLFB_TRACE") != NULL;
        const char *from = getenv("KL_GLFB_TRACE_FROM");
        if (from) g_trace_from = (unsigned)strtoul(from, NULL, 0);
    }
    return on;
}

static int glfb_locking(void) {
    static int on = -1;
    if (on < 0) on = getenv("KL_GLFB_LOCK") != NULL;
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
    if (!g_ready || (!dir && !g_frame_sink)) return g_presented;
    static int every = -1;
    if (every < 0) {
        const char *e = getenv("KL_GLFB_OUT_EVERY");
        every = e ? atoi(e) : 1;
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
                                    uint8_t *dump, int32_t *dw, int32_t *dh) {
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
    if (otype == 0x1702 /* TEXTURE */) {
        uint32_t ufmt = 0;
        klfb_tex_info((uint32_t)oname, &ufmt, &aw, &ah);
        fmt = (int32_t)ufmt;
        if (ufmt == 0x8C3A /* R11F_G11F_B10F */) stage_want = 0x881A;
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
    snprintf(note, note_n, "%s=%d fmt=0x%x %dx%d read=0x%x/0x%x",
             otype == 0x1702 ? "tex" : otype == 0x8D41 ? "rb" : "type?",
             oname, fmt, aw, ah, rfmt, rtype);
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
    if (aw <= 0 || ah <= 0) return 0;
    if (dw) *dw = aw;
    if (dh) *dh = ah;

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
        const char *e = getenv("KL_GLFB_EXPOSURE");
        expo = e ? (float)atof(e) : 1.0f;
        e = getenv("KL_GLFB_GAMMA");
        gam = e ? (float)atof(e) : 1.0f / 2.2f;
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
        for (uint32_t i = 1; i <= g_fbomax && !src_fb; i++) {
            a_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, i);
            if (a_glCheckFramebufferStatus(0x8CA8) != 0x8CD5) continue;
            int32_t otype = 0, oname = 0;
            a_GetFbAttachmentParam(0x8CA8, 0x8CE0, 0x8CD0, &otype);
            a_GetFbAttachmentParam(0x8CA8, 0x8CE0, 0x8CD1, &oname);
            if (otype == 0x1702 /* TEXTURE */ &&
                ((uint32_t)oname == g_eye_tex[0] ||
                 (uint32_t)oname == g_eye_tex[1])) {
                src_fb = i;
                int32_t tw = 0, th = 0;
                if (klfb_tex_info((uint32_t)oname, NULL, &tw, &th) && tw > 0 && th > 0) {
                    src_w = tw;
                    src_h = th;
                }
            }
        }
        if (!src_fb)   // not found: put the guest's binding back
            a_BindFramebuffer(0x8CA8, (uint32_t)(read_fb >= 0 ? read_fb : 0));
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
                const char *e = getenv("KL_GLFB_RAWSTATS");
                rawstats = e ? atoi(e) : 1;
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

    // The sink half of the readback/output split: with a frontend registered
    // the buffer IS the output — hand it over and count the frame presented.
    // The sink runs on this (GL) thread inside the guest's frame, so it must
    // be a memcpy, not a render. Everything below stays the default output:
    // PNG to KL_GLFB_OUT, unchanged when no sink is registered.
    if (g_frame_sink) {
        // KL_GLFB_DUMP_SINK=<dir>: write every 100th sink frame to a PNG.
        // The sink and the PNG path share this px buffer, so a window that
        // shows black while these files show content is an SDL-side problem,
        // not a capture-side one.
        static const char *sink_dir;
        static int sink_dir_init;
        if (!sink_dir_init) { sink_dir = getenv("KL_GLFB_DUMP_SINK"); sink_dir_init = 1; }
        if (sink_dir && g_presented % 100 == 0) {
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
        if (dump_fbos < 0) dump_fbos = getenv("KL_GLFB_DUMP_FBOS") != NULL;
        if (a_glBindFramebuffer) {
            int32_t save_fb = read_fb >= 0 ? read_fb : 0;
            float  *fbuf = malloc((size_t)g_w * g_h * 4 * sizeof(float));
            uint8_t *bbuf = malloc((size_t)g_w * g_h * 4);
            uint8_t *dbuf = dump_fbos ? malloc((size_t)g_w * g_h * 4) : NULL;
            if (fbuf && bbuf) {
                for (uint32_t i = 1; i <= g_fbomax; i++) {
                    char note[160] = "";
                    int32_t dw = 0, dh = 0;
                    unsigned long flit = klfb_probe_fbo(i, fbuf, bbuf,
                                                        note, sizeof note,
                                                        dbuf, &dw, &dh);
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
    fprintf(stderr, "  [glfb] %s: %u/%u lit, mean luma %lu%s "
                    "[draw_fb=%d read_fb=%d src=fb%u fb_status=0x%x viewport %dx%d+%d+%d] (captured on t%llu)\n", path, lit,
            (unsigned)(src_w * src_h), sum / ((unsigned long)src_w * src_h * 3),
            err_buf, draw_fb, read_fb, src_fb, fb_status, vp[2], vp[3], vp[0], vp[1],
            (unsigned long long)cap_tid);
    free(px); free(raw); free(cb);
    return ++g_presented;
}
