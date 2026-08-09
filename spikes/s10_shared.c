// S1.0 — do shared ANGLE contexts across threads crash Apple's Metal driver?
//
// The reference renderer (runtime/kl_glfb.c, KL_GLFB=1) captures black frames
// because Unity drives GL from more than one thread and an EGL context is current
// on exactly one. The correct fix — a context per thread, all *sharing* with the
// first, which is what a multi-threaded GLES app does on Android — is implemented
// behind KL_GLFB_SHARED=1, and it dies below ANGLE inside AGX:
//
//   AGX::Device::findOrCreateDriverProgramVariant<BlitComputeProgramVariant, ...>
//     getComputePatchExecutionShader -> _os_crash_fmt -> abort
//
// This spike removes the guest entirely: two or more host threads, shared ANGLE
// contexts, nothing of Klepton linked. If the crash reproduces here, it is ANGLE's
// or AGX's and the shim is exonerated — and this is a far better bug report than
// one involving Beat Saber. If it does not reproduce, the difference is ours.
//
// It is a *matrix*, not a single repro, because "shared contexts crash" and
// "concurrent Metal use crashes" are different bugs with different fixes and the
// original symptom cannot tell them apart:
//
//   S10_SHARE=0    each thread gets an INDEPENDENT context (no sharing).
//                  Still crashes => concurrency, not sharing.
//   S10_SERIAL=1   threads hold a global mutex across all their GL work, so the
//                  contexts are shared but never used concurrently.
//                  Still crashes => sharing, not concurrency.
//   S10_SERIAL=2   the mutex is held ONLY around shader compile + link. The AGX
//                  frame is a shader-compile frame, so if this is enough, the fix
//                  in the shim is a lock around compilation rather than around
//                  every GL call — very much cheaper, and safe against deadlock.
//   S10_SERIAL=3   the mutex is held only around the draw/readback loop.
//   S10_THREADS=N  worker threads (default 3).
//   S10_ITERS=N    times each thread repeats the draw/readback loop (default 4).
//   S10_STAGE=N    stop each thread after stage N, to bisect which GL operation
//                  is the trigger. Stages are numbered and announced.
//
// Every stage prints before it runs, unbuffered, so the LAST LINE PRINTED names
// the operation that died. That is the whole diagnostic.
//
// Build: make s10     Run: ./build/s10_shared
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---- EGL/GLES constants, spelled out so there is nothing to include ----------
#define EGL_DEFAULT_DISPLAY  ((void *)0)
#define EGL_NO_CONTEXT       ((void *)0)
#define EGL_NO_SURFACE       ((void *)0)
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

#define GL_NO_ERROR            0
#define GL_FALSE               0
#define GL_TRIANGLES           0x0004
#define GL_DEPTH_BUFFER_BIT    0x0100
#define GL_COLOR_BUFFER_BIT    0x4000
#define GL_TEXTURE_2D          0x0DE1
#define GL_UNSIGNED_BYTE       0x1401
#define GL_FLOAT               0x1406
#define GL_RGBA                0x1908
#define GL_RENDERER            0x1F01
#define GL_VERSION             0x1F02
#define GL_NEAREST             0x2600
#define GL_LINEAR              0x2601
#define GL_TEXTURE_MAG_FILTER  0x2800
#define GL_TEXTURE_MIN_FILTER  0x2801
#define GL_RGBA8               0x8058
#define GL_TEXTURE0            0x84C0
#define GL_ARRAY_BUFFER        0x8892
#define GL_STATIC_DRAW         0x88E4
#define GL_FRAGMENT_SHADER     0x8B30
#define GL_VERTEX_SHADER       0x8B31
#define GL_COMPILE_STATUS      0x8B81
#define GL_LINK_STATUS         0x8B82
#define GL_READ_FRAMEBUFFER    0x8CA8
#define GL_DRAW_FRAMEBUFFER    0x8CA9
#define GL_COLOR_ATTACHMENT0   0x8CE0
#define GL_FRAMEBUFFER         0x8D40
#define GL_RENDERBUFFER        0x8D41
#define GL_STENCIL_BUFFER_BIT  0x0400
#define GL_DEPTH_COMPONENT24   0x81A6
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH24_STENCIL8    0x88F0
#define GL_DEPTH_ATTACHMENT    0x8D00
#define GL_COMPRESSED_RGBA8_ETC2_EAC 0x9278
#define GL_RED                 0x1903
#define GL_R8                  0x8229
#define GL_SRGB8_ALPHA8        0x8C43
#define GL_ZERO                0x0000
#define GL_ONE                 0x0001
#define GL_HALF_FLOAT          0x140B
#define GL_R16F                0x822D
#define GL_TEXTURE_BASE_LEVEL  0x813C
#define GL_TEXTURE_MAX_LEVEL   0x813D
#define GL_TEXTURE_SWIZZLE_R   0x8E42
#define GL_TEXTURE_SWIZZLE_G   0x8E43
#define GL_TEXTURE_SWIZZLE_B   0x8E44
#define GL_TEXTURE_SWIZZLE_A   0x8E45

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

// ---- ANGLE entry points ------------------------------------------------------
static void *g_egl_lib, *g_gles_lib;
static void *sym(const char *n) {
    void *p = g_gles_lib ? dlsym(g_gles_lib, n) : NULL;
    if (!p && g_egl_lib) p = dlsym(g_egl_lib, n);
    return p;
}

static void *(*eglGetPlatformDisplayEXT)(uint32_t, void *, const int32_t *);
static unsigned (*eglInitialize)(void *, int32_t *, int32_t *);
static unsigned (*eglChooseConfig)(void *, const int32_t *, void **, int32_t, int32_t *);
static void *(*eglCreatePbufferSurface)(void *, void *, const int32_t *);
static void *(*eglCreateContext)(void *, void *, void *, const int32_t *);
static unsigned (*eglMakeCurrent)(void *, void *, void *, void *);
static uint32_t (*eglGetError)(void);
static const uint8_t *(*glGetString)(uint32_t);
static uint32_t (*glGetError)(void);
static void (*glClearColor)(float, float, float, float);
static void (*glClear)(uint32_t);
static void (*glFinish)(void);
static void (*glFlush)(void);
static void (*glViewport)(int32_t, int32_t, int32_t, int32_t);
static void (*glReadPixels)(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void *);
static void (*glGenTextures)(int32_t, uint32_t *);
static void (*glBindTexture)(uint32_t, uint32_t);
static void (*glTexImage2D)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                            uint32_t, uint32_t, const void *);
static void (*glTexParameteri)(uint32_t, uint32_t, int32_t);
static void (*glActiveTexture)(uint32_t);
static uint32_t (*glCreateShader)(uint32_t);
static void (*glShaderSource)(uint32_t, int32_t, const char *const *, const int32_t *);
static void (*glCompileShader)(uint32_t);
static void (*glGetShaderiv)(uint32_t, uint32_t, int32_t *);
static void (*glGetShaderInfoLog)(uint32_t, int32_t, int32_t *, char *);
static uint32_t (*glCreateProgram)(void);
static void (*glAttachShader)(uint32_t, uint32_t);
static void (*glLinkProgram)(uint32_t);
static void (*glGetProgramiv)(uint32_t, uint32_t, int32_t *);
static void (*glUseProgram)(uint32_t);
static int32_t (*glGetAttribLocation)(uint32_t, const char *);
static int32_t (*glGetUniformLocation)(uint32_t, const char *);
static void (*glUniform1i)(int32_t, int32_t);
static void (*glGenBuffers)(int32_t, uint32_t *);
static void (*glBindBuffer)(uint32_t, uint32_t);
static void (*glBufferData)(uint32_t, intptr_t, const void *, uint32_t);
static void (*glVertexAttribPointer)(uint32_t, int32_t, uint32_t, uint8_t, int32_t, const void *);
static void (*glEnableVertexAttribArray)(uint32_t);
static void (*glDrawArrays)(uint32_t, int32_t, int32_t);
static void (*glGenFramebuffers)(int32_t, uint32_t *);
static void (*glBindFramebuffer)(uint32_t, uint32_t);
static void (*glFramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
static void (*glBlitFramebuffer)(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                                 int32_t, int32_t, uint32_t, uint32_t);
static void (*glGenVertexArrays)(int32_t, uint32_t *);
static void (*glBindVertexArray)(uint32_t);
static void (*glGenRenderbuffers)(int32_t, uint32_t *);
static void (*glBindRenderbuffer)(uint32_t, uint32_t);
static void (*glRenderbufferStorage)(uint32_t, uint32_t, int32_t, int32_t);
static void (*glFramebufferRenderbuffer)(uint32_t, uint32_t, uint32_t, uint32_t);
static void (*glCompressedTexImage2D)(uint32_t, int32_t, uint32_t, int32_t, int32_t,
                                      int32_t, int32_t, const void *);
static void (*glTexStorage2D)(uint32_t, int32_t, uint32_t, int32_t, int32_t);
static void (*glTexSubImage2D)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                               uint32_t, uint32_t, const void *);

#define RESOLVE(f) do {                                                        \
        *(void **)&f = sym(#f);                                                \
        if (!f) { fprintf(stderr, "FAIL: ANGLE has no %s\n", #f); return 1; }  \
    } while (0)

// ---- shared state ------------------------------------------------------------
static void *g_dpy, *g_cfg, *g_root_ctx, *g_root_surf;
static uint32_t g_shared_tex;          // created on the root context; workers sample it
static int g_w = 256, g_h = 256;
static int g_threads = 3, g_iters = 4, g_stage = 99, g_share = 1, g_serial = 0;
static pthread_mutex_t g_gl_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_failed;

// Stage announcements. stderr is unbuffered, so the last line printed is the
// operation that died — which is the entire point of this spike.
static void stage(int tid, int n, const char *what) {
    fprintf(stderr, "  [t%d] stage %d: %s\n", tid, n, what);
}

// The failure is intermittent, so a single clean run proves nothing — run it
// tens of times. When it does fire, this names the frames, which is what makes
// the standalone repro usable as a bug report: it should show AGX's
// findOrCreateDriverProgramVariant<BlitComputeProgramVariant> under MTLCompiler,
// the same signature KL_GLFB_SHARED produces inside t_boot.
static void on_fault(int sig) {
    void *fr[40];
    int n = backtrace(fr, 40);
    char msg[64];
    int len = snprintf(msg, sizeof msg, "\n!! signal %d — backtrace:\n", sig);
    write(2, msg, (size_t)len);
    backtrace_symbols_fd(fr, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}

// ---- stage 10: Beat Saber's own texture sequence, replayed verbatim -----------
//
// KL_GLFB_TRACE_TEX captured every texture call the guest makes before the AGX
// abort, and there are only 21 of them. This is that list. The abort follows the
// last entry — an R8 allocation and a GL_RED upload — immediately.
//
// Replaying it here is what turns "the guest crashes" into a minimal repro with no
// Unity, no IL2CPP and no Klepton in the picture at all.
static const struct {
    uint32_t ifmt;      // internalformat for glTexStorage2D
    int32_t  levels, w, h;
    uint32_t upfmt;     // format for glTexSubImage2D; 0 means no upload at all
} GUEST_TEXTURES[] = {
    { GL_SRGB8_ALPHA8, 1, 1832, 1920, 0 },        // the eye-sized render target
    { GL_RGBA8,        1,    4,    4, GL_RGBA },
    { GL_SRGB8_ALPHA8, 1,    4,    4, GL_RGBA },
    { GL_SRGB8_ALPHA8, 1,    4,    4, GL_RGBA },
    { GL_RGBA8,        1,   16,   16, GL_RGBA },
    { GL_SRGB8_ALPHA8, 3,    4,    4, GL_RGBA },  // three mip levels uploaded
    { GL_SRGB8_ALPHA8, 1,    4,    4, GL_RGBA },
    { GL_RGBA8,        1,    4,    4, GL_RGBA },
    { GL_RGBA8,        1,  256,    2, GL_RGBA },
    { GL_R8,           1,   64,   64, GL_RED  },  // <- the abort follows this one
};

static void replay_guest_textures(int tid) {
    unsigned char *buf = malloc(1832u * 1920u * 4u);
    if (!buf) return;
    memset(buf, 0x7f, 1832u * 1920u * 4u);

    for (size_t i = 0; i < sizeof GUEST_TEXTURES / sizeof GUEST_TEXTURES[0]; i++) {
        uint32_t tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, GUEST_TEXTURES[i].levels, GUEST_TEXTURES[i].ifmt,
                       GUEST_TEXTURES[i].w, GUEST_TEXTURES[i].h);
        uint32_t e = glGetError();
        if (e != GL_NO_ERROR)
            fprintf(stderr, "  [t%d]   storage 0x%04x %dx%d -> gl 0x%x\n", tid,
                    GUEST_TEXTURES[i].ifmt, GUEST_TEXTURES[i].w, GUEST_TEXTURES[i].h, e);
        if (!GUEST_TEXTURES[i].upfmt) continue;
        for (int lv = 0; lv < GUEST_TEXTURES[i].levels; lv++) {
            int32_t w = GUEST_TEXTURES[i].w >> lv, h = GUEST_TEXTURES[i].h >> lv;
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            fprintf(stderr, "  [t%d]   glTexSubImage2D %dx%d level %d fmt=0x%04x\n",
                    tid, w, h, lv, GUEST_TEXTURES[i].upfmt);
            glTexSubImage2D(GL_TEXTURE_2D, lv, 0, 0, w, h, GUEST_TEXTURES[i].upfmt,
                            GL_UNSIGNED_BYTE, buf);
        }
    }
    glFinish();
    free(buf);
}

// ---- the swizzled single-channel texture, which is what the guest was doing ----
//
// KL_GLFB_TRACE_TEX caught the guest setting, immediately before the allocation
// the AGX abort follows:
//
//   GL_TEXTURE_SWIZZLE_R = GL_RED    GL_TEXTURE_SWIZZLE_G = GL_ZERO
//   GL_TEXTURE_SWIZZLE_B = GL_ZERO   GL_TEXTURE_SWIZZLE_A = GL_ONE
//
// That is Unity reconstructing GL_LUMINANCE/GL_ALPHA, which GLES 3 removed, out of
// a one-channel format. Two different runs died on two different such textures —
// R8 64x64 and R16F 1024x1 — and the swizzle is what they have in common. A Metal
// backend cannot always set that natively and has to emulate it, and emulation is
// where a blit compute program would come from.
//
// S10_SWIZZLE=1 does only this, so a failure cannot be blamed on anything else.
static void swizzled_single_channel(int tid) {
    const struct {
        uint32_t ifmt, upfmt, uptype;
        int32_t  w, h;
        uint32_t sr, sg, sb, sa;
        const char *what;
    } CASES[] = {
        // The exact texture five runs died on, at call #312 every time: an R8 4x4
        // whose colour channels are all the *constant* zero and whose alpha comes
        // from red. That is GL_ALPHA rebuilt out of R8 — not the luminance shape
        // (RED/ZERO/ZERO/ONE) guessed at first, which is also in the guest's trace
        // but is not what it dies on.
        { GL_R8,   GL_RED, GL_UNSIGNED_BYTE,    4, 4, GL_ZERO, GL_ZERO, GL_ZERO, GL_RED,
          "R8 4x4, swizzle ZERO/ZERO/ZERO/RED (the guest's own)" },
        { GL_R8,   GL_RED, GL_UNSIGNED_BYTE,   64,64, GL_RED,  GL_ZERO, GL_ZERO, GL_ONE,
          "R8 64x64, swizzle RED/ZERO/ZERO/ONE" },
        { GL_R16F, GL_RED, GL_HALF_FLOAT,    1024, 1, GL_RED,  GL_ZERO, GL_ZERO, GL_ONE,
          "R16F 1024x1, swizzle RED/ZERO/ZERO/ONE" },
    };
    unsigned char *buf = calloc(1, 1024 * 64 * 8);
    if (!buf) return;

    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        uint32_t tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        // The full parameter set the guest applies, in its order: swizzle first,
        // then filters, then the mip range — all before storage is allocated.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, (int32_t)CASES[i].sr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, (int32_t)CASES[i].sg);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, (int32_t)CASES[i].sb);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, (int32_t)CASES[i].sa);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

        fprintf(stderr, "  [t%d]   %s\n", tid, CASES[i].what);
        glTexStorage2D(GL_TEXTURE_2D, 1, CASES[i].ifmt, CASES[i].w, CASES[i].h);
        uint32_t e = glGetError();
        if (e != GL_NO_ERROR)
            fprintf(stderr, "  [t%d]     storage -> gl 0x%x\n", tid, e);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CASES[i].w, CASES[i].h,
                        CASES[i].upfmt, CASES[i].uptype, buf);
        glFinish();
        fprintf(stderr, "  [t%d]     uploaded (gl 0x%x)\n", tid, glGetError());
    }
    free(buf);
}

static const char *VS =
    "#version 300 es\n"
    "in vec2 aPos;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "  vUV = aPos * 0.5 + 0.5;\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

static const char *FS =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 oColor;\n"
    "void main() { oColor = texture(uTex, vUV) + vec4(0.2, 0.0, 0.0, 1.0); }\n";

static uint32_t build_program(int tid) {
    uint32_t vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &VS, NULL);
    glCompileShader(vs);
    uint32_t fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &FS, NULL);
    glCompileShader(fs);
    for (int i = 0; i < 2; i++) {
        uint32_t s = i ? fs : vs;
        int32_t st = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &st);
        if (!st) {
            char log[1024];
            glGetShaderInfoLog(s, sizeof log, NULL, log);
            fprintf(stderr, "  [t%d] shader %d failed: %s\n", tid, i, log);
            g_failed = 1;
            return 0;
        }
    }
    uint32_t p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    int32_t linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        fprintf(stderr, "  [t%d] program failed to link\n", tid);
        g_failed = 1;
        return 0;
    }
    return p;
}

// ---- the worker: everything kl_glfb.c makes a guest thread do ----------------
static void *worker(void *arg) {
    int tid = (int)(intptr_t)arg;

    if (g_serial == 1) pthread_mutex_lock(&g_gl_lock);

    // Stage 1 — a per-thread pbuffer and a context, sharing with the root (or not,
    // under S10_SHARE=0). This is exactly what kl_glfb_make_current does.
    stage(tid, 1, "eglCreatePbufferSurface + eglCreateContext");
    const int32_t surf_attrs[] = { EGL_WIDTH, g_w, EGL_HEIGHT, g_h, EGL_NONE };
    const int32_t ctx_attrs[]  = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    void *surf = eglCreatePbufferSurface(g_dpy, g_cfg, surf_attrs);
    void *ctx  = eglCreateContext(g_dpy, g_cfg, g_share ? g_root_ctx : EGL_NO_CONTEXT,
                                  ctx_attrs);
    if (!surf || !ctx) {
        fprintf(stderr, "  [t%d] FAIL: no surface/context (egl 0x%x)\n", tid, eglGetError());
        g_failed = 1;
        goto out;
    }
    if (g_stage < 2) goto out;

    stage(tid, 2, "eglMakeCurrent");
    if (!eglMakeCurrent(g_dpy, surf, surf, ctx)) {
        fprintf(stderr, "  [t%d] FAIL: eglMakeCurrent (egl 0x%x)\n", tid, eglGetError());
        g_failed = 1;
        goto out;
    }
    if (g_stage < 3) goto out;

    // ES3 core needs a bound VAO for attribute state to be legal.
    uint32_t vao = 0;
    if (glGenVertexArrays) { glGenVertexArrays(1, &vao); glBindVertexArray(vao); }

    // S10_REPLAY_ONLY=1 runs *only* the guest's texture sequence, skipping every
    // other stage. Without this the replay shares a run with the known ANGLE
    // draw race, and a failure cannot be attributed to either.
    if (getenv("S10_SWIZZLE")) {
        stage(tid, 12, "swizzled single-channel textures (the guest's actual shape)");
        swizzled_single_channel(tid);
        fprintf(stderr, "  [t%d]   swizzle survived (gl 0x%x)\n", tid, glGetError());
        goto out;
    }

    if (getenv("S10_REPLAY_ONLY")) {
        stage(tid, 10, "replay Beat Saber's texture sequence (isolated)");
        replay_guest_textures(tid);
        fprintf(stderr, "  [t%d]   replay survived (gl error 0x%x)\n", tid, glGetError());
        goto out;
    }

    // S10_EYE=1 — the guest's render target, which the replay above does not
    // exercise: an eye-sized SRGB8_ALPHA8 texture used as a colour attachment,
    // drawn into, and then blitted. Isolating the texture *calls* cleared them, so
    // what is left is the texture being a draw target rather than merely existing.
    // sRGB is the interesting part: Metal's fixed-function blit cannot always
    // service an sRGB surface and falls back to the compute blit that AGX aborts
    // compiling.
    if (getenv("S10_EYE")) {
        stage(tid, 11, "draw into an eye-sized SRGB8_ALPHA8 FBO, then blit it");
        uint32_t prog2 = build_program(tid);
        uint32_t etex = 0, efbo = 0;
        glGenTextures(1, &etex);
        glBindTexture(GL_TEXTURE_2D, etex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, 1832, 1920);
        glGenFramebuffers(1, &efbo);
        glBindFramebuffer(GL_FRAMEBUFFER, efbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, etex, 0);
        glViewport(0, 0, 1832, 1920);
        glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (prog2) {
            static const float quad[] = { -1, -1,  3, -1,  -1, 3 };
            uint32_t vbo2 = 0;
            glGenBuffers(1, &vbo2);
            glBindBuffer(GL_ARRAY_BUFFER, vbo2);
            glBufferData(GL_ARRAY_BUFFER, (intptr_t)sizeof quad, quad, GL_STATIC_DRAW);
            glUseProgram(prog2);
            int32_t l2 = glGetAttribLocation(prog2, "aPos");
            if (l2 >= 0) {
                glEnableVertexAttribArray((uint32_t)l2);
                glVertexAttribPointer((uint32_t)l2, 2, GL_FLOAT, GL_FALSE, 0, NULL);
            }
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_shared_tex);
            for (int it = 0; it < g_iters; it++) glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, efbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, 1832, 1920, 0, 0, g_w, g_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glFinish();
        fprintf(stderr, "  [t%d]   eye FBO survived (gl error 0x%x)\n", tid, glGetError());
        goto out;
    }

    stage(tid, 3, "glClear + glFinish");
    glViewport(0, 0, g_w, g_h);
    glClearColor(0.0f, 0.25f * (float)(tid + 1), 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glFinish();
    if (g_stage < 4) goto out;

    // Stage 4 — readback. ANGLE's Metal backend services some copies with a
    // *compute* blit, which is the family named in the AGX backtrace, so this is
    // the first real suspect.
    stage(tid, 4, "glReadPixels from the default framebuffer");
    unsigned char *px = malloc((size_t)g_w * g_h * 4);
    if (px) {
        glReadPixels(0, 0, g_w, g_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        fprintf(stderr, "  [t%d]   centre pixel = %u,%u,%u,%u\n", tid,
                px[(g_h / 2 * g_w + g_w / 2) * 4 + 0], px[(g_h / 2 * g_w + g_w / 2) * 4 + 1],
                px[(g_h / 2 * g_w + g_w / 2) * 4 + 2], px[(g_h / 2 * g_w + g_w / 2) * 4 + 3]);
    }
    if (g_stage < 5) { free(px); goto out; }

    stage(tid, 5, "compile + link a program on this context");
    if (g_serial == 2) pthread_mutex_lock(&g_gl_lock);
    uint32_t prog = build_program(tid);
    if (g_serial == 2) pthread_mutex_unlock(&g_gl_lock);
    if (!prog) { free(px); goto out; }
    if (g_stage < 6) { free(px); goto out; }

    // Stage 6 — draw, sampling the texture the ROOT context created. This is the
    // one stage that actually exercises object sharing rather than just coexistence.
    stage(tid, 6, "draw sampling the root context's shared texture");
    static const float quad[] = { -1, -1,  3, -1,  -1, 3 };   // oversized triangle
    uint32_t vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (intptr_t)sizeof quad, quad, GL_STATIC_DRAW);
    glUseProgram(prog);
    int32_t loc = glGetAttribLocation(prog, "aPos");
    if (loc >= 0) {
        glEnableVertexAttribArray((uint32_t)loc);
        glVertexAttribPointer((uint32_t)loc, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_shared_tex);
    int32_t utex = glGetUniformLocation(prog, "uTex");
    if (utex >= 0) glUniform1i(utex, 0);

    if (g_serial == 3) pthread_mutex_lock(&g_gl_lock);
    for (int it = 0; it < g_iters; it++) {
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        if (px) {
            glReadPixels(0, 0, g_w, g_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
            if (it == 0)
                fprintf(stderr, "  [t%d]   drew, centre pixel = %u,%u,%u,%u%s\n", tid,
                        px[(g_h / 2 * g_w + g_w / 2) * 4 + 0],
                        px[(g_h / 2 * g_w + g_w / 2) * 4 + 1],
                        px[(g_h / 2 * g_w + g_w / 2) * 4 + 2],
                        px[(g_h / 2 * g_w + g_w / 2) * 4 + 3],
                        px[(g_h / 2 * g_w + g_w / 2) * 4 + 0] ? "" : "  (BLACK)");
        }
    }
    if (g_serial == 3) pthread_mutex_unlock(&g_gl_lock);
    if (g_stage < 7) { free(px); goto out; }

    // Stage 7 — an FBO blit. glBlitFramebuffer is the most direct route to Metal's
    // blit path, and the AGX frame names a BlitComputeProgramVariant.
    stage(tid, 7, "FBO + glBlitFramebuffer");
    uint32_t tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_w, g_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, g_w, g_h, 0, 0, g_w, g_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glFinish();
    fprintf(stderr, "  [t%d]   colour blit ok (gl error 0x%x)\n", tid, glGetError());
    if (g_stage < 8) { free(px); goto out; }

    // Stage 8 — a DEPTH blit. This is the prime suspect: Metal cannot service a
    // depth/stencil blit through its ordinary blit encoder, so ANGLE's Metal
    // backend implements it as a *compute* pass — which is precisely the
    // BlitComputeProgramVariant family named in the AGX backtrace. A colour blit
    // (stage 7) does not go anywhere near that code path.
    stage(tid, 8, "glBlitFramebuffer with GL_DEPTH_BUFFER_BIT (the compute-blit path)");
    uint32_t drb = 0;
    glGenRenderbuffers(1, &drb);
    glBindRenderbuffer(GL_RENDERBUFFER, drb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, g_w, g_h);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, drb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, g_w, g_h, 0, 0, g_w, g_h,
                      GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    glFinish();
    fprintf(stderr, "  [t%d]   depth/stencil blit ok (gl error 0x%x)\n", tid, glGetError());
    if (g_stage < 9) { free(px); goto out; }

    // Stage 9 — an ETC2 upload, then sample it. The guest does 335 compressed
    // ASTC/ETC2 uploads (kl_egl.c), and a format the driver has to convert is the
    // other way to end up in a blit-compute program.
    stage(tid, 9, "ETC2 upload + sample");
    unsigned char etc[16 * 16 * 16];          // 64x64 RGBA8_ETC2_EAC, 16 B per 4x4 block
    memset(etc, 0x40, sizeof etc);
    uint32_t ctex = 0;
    glGenTextures(1, &ctex);
    glBindTexture(GL_TEXTURE_2D, ctex);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA8_ETC2_EAC, 64, 64, 0,
                           (int32_t)sizeof etc, etc);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    fprintf(stderr, "  [t%d]   ETC2 upload (gl error 0x%x)\n", tid, glGetError());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    fprintf(stderr, "  [t%d]   ETC2 sample ok (gl error 0x%x)\n", tid, glGetError());
    if (g_stage < 10) { free(px); goto out; }

    stage(tid, 10, "replay Beat Saber's own 21-call texture sequence");
    replay_guest_textures(tid);
    fprintf(stderr, "  [t%d]   replay survived (gl error 0x%x)\n", tid, glGetError());
    free(px);

out:
    if (ctx) eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    fprintf(stderr, "  [t%d] done\n", tid);
    if (g_serial == 1) pthread_mutex_unlock(&g_gl_lock);
    return NULL;
}

int main(void) {
    const char *e;
    signal(SIGSEGV, on_fault); signal(SIGBUS, on_fault);
    signal(SIGABRT, on_fault); signal(SIGTRAP, on_fault);
    if ((e = getenv("S10_THREADS"))) g_threads = atoi(e);
    if ((e = getenv("S10_ITERS")))   g_iters   = atoi(e);
    if ((e = getenv("S10_STAGE")))   g_stage   = atoi(e);
    if ((e = getenv("S10_SHARE")))   g_share   = atoi(e);
    if ((e = getenv("S10_SERIAL")))  g_serial  = atoi(e);
    if (g_threads < 1) g_threads = 1;

    const char *dir = kl_angle_dir();
    char egl_path[1024], gles_path[1024];
    snprintf(egl_path,  sizeof egl_path,  "%s/libEGL.dylib", dir);
    snprintf(gles_path, sizeof gles_path, "%s/libGLESv2.dylib", dir);
    g_egl_lib  = dlopen(egl_path,  RTLD_NOW | RTLD_LOCAL);
    g_gles_lib = dlopen(gles_path, RTLD_NOW | RTLD_LOCAL);
    if (!g_egl_lib || !g_gles_lib) {
        fprintf(stderr, "FAIL: dlopen ANGLE: %s\n  (set KL_ANGLE_DIR)\n", dlerror());
        return 1;
    }

    RESOLVE(eglGetPlatformDisplayEXT); RESOLVE(eglInitialize);
    RESOLVE(eglChooseConfig);          RESOLVE(eglCreatePbufferSurface);
    RESOLVE(eglCreateContext);         RESOLVE(eglMakeCurrent);
    RESOLVE(eglGetError);              RESOLVE(glGetString);
    RESOLVE(glGetError);               RESOLVE(glClearColor);
    RESOLVE(glClear);                  RESOLVE(glFinish);
    RESOLVE(glFlush);                  RESOLVE(glViewport);
    RESOLVE(glReadPixels);             RESOLVE(glGenTextures);
    RESOLVE(glBindTexture);            RESOLVE(glTexImage2D);
    RESOLVE(glTexParameteri);          RESOLVE(glActiveTexture);
    RESOLVE(glCreateShader);           RESOLVE(glShaderSource);
    RESOLVE(glCompileShader);          RESOLVE(glGetShaderiv);
    RESOLVE(glGetShaderInfoLog);       RESOLVE(glCreateProgram);
    RESOLVE(glAttachShader);           RESOLVE(glLinkProgram);
    RESOLVE(glGetProgramiv);           RESOLVE(glUseProgram);
    RESOLVE(glGetAttribLocation);      RESOLVE(glGetUniformLocation);
    RESOLVE(glUniform1i);              RESOLVE(glGenBuffers);
    RESOLVE(glBindBuffer);             RESOLVE(glBufferData);
    RESOLVE(glVertexAttribPointer);    RESOLVE(glEnableVertexAttribArray);
    RESOLVE(glDrawArrays);             RESOLVE(glGenFramebuffers);
    RESOLVE(glBindFramebuffer);        RESOLVE(glFramebufferTexture2D);
    RESOLVE(glBlitFramebuffer);        RESOLVE(glGenVertexArrays);
    RESOLVE(glBindVertexArray);        RESOLVE(glGenRenderbuffers);
    RESOLVE(glBindRenderbuffer);       RESOLVE(glRenderbufferStorage);
    RESOLVE(glFramebufferRenderbuffer);RESOLVE(glCompressedTexImage2D);
    RESOLVE(glTexStorage2D);           RESOLVE(glTexSubImage2D);

    // Metal by name — the default display would select ANGLE's OpenGL backend,
    // which is a different driver stack and would not reproduce anything (S0.9).
    const char *want = getenv("KL_ANGLE_BACKEND");
    int use_gl = want && strcmp(want, "gl") == 0;
    const int32_t dpy_attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE,
        use_gl ? EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE : EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
        EGL_NONE,
    };
    g_dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, dpy_attrs);
    int32_t major = 0, minor = 0;
    if (!g_dpy || !eglInitialize(g_dpy, &major, &minor)) {
        fprintf(stderr, "FAIL: eglInitialize\n");
        return 1;
    }

    const int32_t cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE,
    };
    int32_t ncfg = 0;
    if (!eglChooseConfig(g_dpy, cfg_attrs, &g_cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "FAIL: no ES3 pbuffer config\n");
        return 1;
    }

    // The root context, on the main thread — kl_glfb_init's shape exactly.
    const int32_t surf_attrs[] = { EGL_WIDTH, g_w, EGL_HEIGHT, g_h, EGL_NONE };
    const int32_t ctx_attrs[]  = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    g_root_surf = eglCreatePbufferSurface(g_dpy, g_cfg, surf_attrs);
    g_root_ctx  = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (!g_root_surf || !g_root_ctx ||
        !eglMakeCurrent(g_dpy, g_root_surf, g_root_surf, g_root_ctx)) {
        fprintf(stderr, "FAIL: no root ES3 context\n");
        return 1;
    }
    fprintf(stderr, "  %s\n  %s\n", (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VERSION));
    fprintf(stderr, "  config: %d worker threads, share=%d serial=%d iters=%d stage<=%d\n",
            g_threads, g_share, g_serial, g_iters, g_stage);

    // A texture the workers will sample, so stage 6 exercises real object sharing
    // and not merely two contexts coexisting.
    unsigned char *seed = malloc(64 * 64 * 4);
    for (int i = 0; i < 64 * 64; i++) {
        seed[i * 4 + 0] = 0; seed[i * 4 + 1] = 200;
        seed[i * 4 + 2] = 0; seed[i * 4 + 3] = 255;
    }
    glGenTextures(1, &g_shared_tex);
    glBindTexture(GL_TEXTURE_2D, g_shared_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, seed);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFinish();
    free(seed);
    fprintf(stderr, "  root context made shared texture %u (gl error 0x%x)\n",
            g_shared_tex, glGetError());

    // Release it, exactly as kl_glfb_init does — EGL refuses to migrate a current
    // context, so the root must let go before any worker can claim anything.
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    pthread_t *th = calloc((size_t)g_threads, sizeof *th);
    for (int i = 0; i < g_threads; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < g_threads; i++)
        pthread_join(th[i], NULL);
    free(th);

    if (g_failed)
        fprintf(stderr, "=== S1.0: threads finished, but a stage reported failure\n");
    else
        fprintf(stderr, "=== S1.0 SURVIVED: %d threads, share=%d serial=%d — no crash\n",
                g_threads, g_share, g_serial);
    return g_failed ? 1 : 0;
}
