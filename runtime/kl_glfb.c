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

static void klfb_CompileShader(uint32_t shader) {
    if (!g_real_CompileShader) return;
    pthread_mutex_lock(&g_compile_lock);
    g_real_CompileShader(shader);
    pthread_mutex_unlock(&g_compile_lock);
}
static void klfb_LinkProgram(uint32_t program) {
    if (!g_real_LinkProgram) return;
    pthread_mutex_lock(&g_compile_lock);
    g_real_LinkProgram(program);
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

static int klfb_trace_fbo(void) {
    static int on = -1;
    if (on < 0) on = getenv("KL_GLFB_TRACE_FBO") != NULL;
    return on;
}
static uint64_t klfb_tid(void) { uint64_t t = 0; pthread_threadid_np(NULL, &t); return t; }

static void klfb_GenFramebuffers(int32_t n, uint32_t *ids) {
    if (g_real_GenFramebuffers) g_real_GenFramebuffers(n, ids);
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
static void (*g_real_BlitFramebuffer)(int32_t, int32_t, int32_t, int32_t, int32_t,
                                      int32_t, int32_t, int32_t, uint32_t, uint32_t);

static void klfb_errprobe(const char *what, const char *detail);

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
    if (g_real_BlitFramebuffer)
        g_real_BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                               (uint32_t)mask, (uint32_t)filter);
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
// middle 64x64 and say whether the draw emitted anything. Distinguishes
// "draws run but output black" from "content drawn but lost before the
// capture" — the readback happens inside the same command stream, so there
// is no timing or thread question left.
static void klfb_draw_probe(int verts) {
    static int on = -1, said;
    if (on < 0) on = getenv("KL_GLFB_DRAW_PROBE") != NULL;
    if (!on || said >= 12 || !a_glReadPixels) return;
    if (verts < 32) return;   // skip the post-processing passes, keep scene draws
    int32_t fb = -1;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
    if (a_glFinish) a_glFinish();
    said++;
    uint8_t px[64 * 64 * 4];
    memset(px, 0, sizeof px);
    a_glReadPixels(g_w / 2 - 32, g_h / 2 - 32, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px);
    unsigned long lit = 0, sum = 0;
    for (int i = 0; i < 64 * 64; i++) {
        unsigned lum = px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2];
        sum += lum;
        if (lum > 12) lit++;
    }
    uint32_t err = a_glGetError ? a_glGetError() : 0;
    fprintf(stderr, "  [glfb] DRAW_PROBE fb=%d: %lu/4096 lit, mean luma %lu, err 0x%x\n",
            fb, lit, sum / (4096 * 3), err);
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

static const struct { const char *name; void *thunk; void **real; } g_thunks[] = {
    {"glFlush",  (void *)klfb_Flush,  (void **)&g_real_Flush},
    {"glDrawElements", (void *)klfb_DrawElements, (void **)&g_real_DrawElements},
    {"glDrawArrays",   (void *)klfb_DrawArrays,   (void **)&g_real_DrawArrays},
    {"glDrawElementsInstanced", (void *)klfb_DrawElementsInstanced, (void **)&g_real_DrawElementsInstanced},
    {"glDrawElementsBaseVertex", (void *)klfb_DrawElementsBaseVertex, (void **)&g_real_DrawElementsBaseVertex},
    {"glDrawArraysInstanced", (void *)klfb_DrawArraysInstanced, (void **)&g_real_DrawArraysInstanced},
    {"glFinish", (void *)klfb_Finish, (void **)&g_real_Finish},
    {"glBlitFramebuffer",         (void *)klfb_BlitFramebuffer,         (void **)&g_real_BlitFramebuffer},
    {"glTexSubImage3D",           (void *)klfb_TexSubImage3D,           (void **)&g_real_TexSubImage3D},
    {"glCompressedTexSubImage3D", (void *)klfb_CompressedTexSubImage3D, (void **)&g_real_CompressedTexSubImage3D},
    // the shared-context compile lock — see the block above s10's numbers
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
// pump otherwise writes 300 full-size PNGs.
unsigned kl_glfb_present(const char *dir) {
    if (!g_ready || !dir) return g_presented;
    static int every = -1;
    if (every < 0) {
        const char *e = getenv("KL_GLFB_OUT_EVERY");
        every = e ? atoi(e) : 1;
        if (every < 1) every = 1;
    }
    static unsigned swap_n;
    if (swap_n++ % (unsigned)every) return g_presented;
    snprintf(g_capture_dir, sizeof g_capture_dir, "%s", dir);
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
    int32_t fb_status = -1;
    static uint32_t (*a_glCheckFramebufferStatus)(uint32_t);
    if (!a_glCheckFramebufferStatus)
        a_glCheckFramebufferStatus = asym("glCheckFramebufferStatus");
    if (a_glGetIntegerv) {
        a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &draw_fb);
        a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &read_fb);
        a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
    }
    if (a_glCheckFramebufferStatus)
        fb_status = (int32_t)a_glCheckFramebufferStatus(0x8CA9 /* GL_DRAW_FRAMEBUFFER */);
    uint32_t pre_err = a_glGetError ? a_glGetError() : 0;
    a_glReadPixels(0, 0, g_w, g_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    // The error could be the readback's own or one the guest left behind —
    // report both rather than blame the readback. Reading the pre-existing one
    // does clear it; this is a host-only diagnostic and the guest's errors on
    // the reference renderer are exactly what we want to see.
    uint32_t err = a_glGetError ? a_glGetError() : 0;
    char err_buf[64] = "";
    if (err) snprintf(err_buf, sizeof err_buf, " (GL error 0x%x%s)", err,
                      pre_err ? " — but pre-existing, not the readback's" : "");

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
    // fb0 has been measured black with every draw landing on FBOs instead
    // (the VR frame goes to eye textures, not the backbuffer). Census the
    // FBOs: bind each as READ, and if complete, count lit pixels — this names
    // the framebuffer the picture is actually in.
    {
        static void (*a_glBindFramebuffer)(uint32_t, uint32_t);
        if (!a_glBindFramebuffer) a_glBindFramebuffer = asym("glBindFramebuffer");
        if (a_glBindFramebuffer) {
            int32_t save_fb = read_fb >= 0 ? read_fb : 0;
            for (uint32_t i = 1; i <= 8; i++) {
                a_glBindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, i);
                if (a_glCheckFramebufferStatus &&
                    a_glCheckFramebufferStatus(0x8CA8) != 0x8CD5) continue;
                memset(px, 0, stride * 4);
                a_glReadPixels(0, 0, g_w, g_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
                unsigned long fsum = 0, flit = 0;
                for (size_t p2 = 0; p2 < (size_t)g_w * g_h; p2++) {
                    unsigned lum = px[p2 * 4] + px[p2 * 4 + 1] + px[p2 * 4 + 2];
                    fsum += lum;
                    if (lum > 12) flit++;
                }
                if (flit || i <= 4)
                    fprintf(stderr, "  [glfb] census fbo%u: %lu/%u lit, mean luma %lu\n",
                            i, flit, (unsigned)(g_w * g_h),
                            fsum / ((unsigned long)g_w * g_h * 3));
            }
            a_glBindFramebuffer(0x8CA8, (uint32_t)save_fb);
        }
    }
    uint64_t cap_tid = 0; pthread_threadid_np(NULL, &cap_tid);
    fprintf(stderr, "  [glfb] %s: %u/%u lit, mean luma %lu%s "
                    "[draw_fb=%d read_fb=%d fb_status=0x%x viewport %dx%d+%d+%d] (captured on t%llu)\n", path, lit,
            (unsigned)(g_w * g_h), sum / ((unsigned long)g_w * g_h * 3),
            err_buf, draw_fb, read_fb, fb_status, vp[2], vp[3], vp[0], vp[1],
            (unsigned long long)cap_tid);
    free(px); free(raw); free(cb);
    return ++g_presented;
}
