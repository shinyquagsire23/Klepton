// Every GL limit we tell the guest, against what ANGLE will actually honour.
//
// **A limit stated above what the driver enforces is a lie with no error
// surface.** The guest validates against our answer, passes, calls GL, and GL
// refuses — and an engine that keeps a shadow of the state it just set writes
// that shadow anyway, indexed by the number it believed was legal. What that
// costs is not a slow path: MAX_UNIFORM_BLOCK_SIZE claimed at 65536 over
// ANGLE's 16384 made Unity size UnityInstancing_PerDraw0 for 64 KiB, so the
// shader failed to LINK and the effect using it was simply absent.
//
// So this is the GL twin of the ovrp ABI audit: it does not check that the
// numbers are nice, it checks that ours are ones the thing underneath will
// stand behind. Our side is asked through kl_gl_cap_integerv rather than a
// copied table, so the gate cannot drift from the answers the guest gets.
//
// Two directions, and they are not symmetric. For a BUDGET the guest may spend
// (sizes, counts, slots) ours must be <= ANGLE's. For a REQUIREMENT the guest
// must meet (buffer offset alignment) a larger number is stricter and therefore
// safe, so those are listed as such rather than exempted case by case.
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "kl_egl.h"

static void *g_egl, *g_gles;
static void *sym(const char *n) {
    void *p = g_gles ? dlsym(g_gles, n) : NULL;
    return p ? p : (g_egl ? dlsym(g_egl, n) : NULL);
}

// kind: BUDGET — ours must not exceed ANGLE's. STRICTER — ours may exceed it,
// because the guest satisfying our number satisfies ANGLE's too.
enum { BUDGET, STRICTER };

static const struct { uint32_t pname; const char *name; int kind; } g_limits[] = {
    {0x0D33, "MAX_TEXTURE_SIZE",                  BUDGET},
    {0x851C, "MAX_CUBE_MAP_TEXTURE_SIZE",         BUDGET},
    {0x8073, "MAX_3D_TEXTURE_SIZE",               BUDGET},
    {0x88FF, "MAX_ARRAY_TEXTURE_LAYERS",          BUDGET},
    {0x84E8, "MAX_RENDERBUFFER_SIZE",             BUDGET},
    {0x8869, "MAX_VERTEX_ATTRIBS",                BUDGET},
    {0x8872, "MAX_TEXTURE_IMAGE_UNITS",           BUDGET},
    {0x8B4D, "MAX_COMBINED_TEXTURE_IMAGE_UNITS",  BUDGET},
    {0x8B4C, "MAX_VERTEX_TEXTURE_IMAGE_UNITS",    BUDGET},
    {0x8CDF, "MAX_COLOR_ATTACHMENTS",             BUDGET},
    {0x8824, "MAX_DRAW_BUFFERS",                  BUDGET},
    {0x8D57, "MAX_SAMPLES",                       BUDGET},
    {0x8A30, "MAX_UNIFORM_BLOCK_SIZE",            BUDGET},
    {0x8A2D, "MAX_FRAGMENT_UNIFORM_BLOCKS",       BUDGET},
    {0x8A2B, "MAX_VERTEX_UNIFORM_BLOCKS",         BUDGET},
    {0x8A2F, "MAX_COMBINED_UNIFORM_BLOCKS",       BUDGET},
    {0x9122, "MAX_VERTEX_OUTPUT_COMPONENTS",      BUDGET},
    {0x9125, "MAX_FRAGMENT_INPUT_COMPONENTS",     BUDGET},
    {0x8A34, "UNIFORM_BUFFER_OFFSET_ALIGNMENT",   STRICTER},
};

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "vendor/out/Debug";
    char a[1024], b[1024];
    snprintf(a, sizeof a, "%s/libEGL.dylib", dir);
    snprintf(b, sizeof b, "%s/libGLESv2.dylib", dir);
    g_egl = dlopen(a, RTLD_NOW);
    g_gles = dlopen(b, RTLD_NOW);
    if (!g_egl || !g_gles) {
        fprintf(stderr, "SKIP: no debug ANGLE at %s (make angle-debug)\n", dir);
        return 0;                    // must pass on a bare checkout
    }
    void *(*getPlatformDisplay)(uint32_t, void *, const int32_t *) =
        sym("eglGetPlatformDisplayEXT");
    unsigned (*eglInitialize)(void *, int32_t *, int32_t *) = sym("eglInitialize");
    unsigned (*eglChooseConfig)(void *, const int32_t *, void **, int32_t, int32_t *) =
        sym("eglChooseConfig");
    void *(*eglCreatePbufferSurface)(void *, void *, const int32_t *) =
        sym("eglCreatePbufferSurface");
    void *(*eglCreateContext)(void *, void *, void *, const int32_t *) = sym("eglCreateContext");
    unsigned (*eglMakeCurrent)(void *, void *, void *, void *) = sym("eglMakeCurrent");

    const int32_t dattrs[] = { 0x3203, 0x3489 /* METAL */, 0x3038 };
    void *dpy = getPlatformDisplay ? getPlatformDisplay(0x3202, (void *)0, dattrs) : NULL;
    int32_t mj = 0, mn = 0;
    if (!dpy || !eglInitialize(dpy, &mj, &mn)) {
        fprintf(stderr, "SKIP: no Metal EGL display here\n");
        return 0;
    }
    const int32_t cattrs[] = { 0x3033, 0x0001, 0x3040, 0x0040, 0x3024, 8, 0x3023, 8,
                               0x3022, 8, 0x3021, 8, 0x3025, 24, 0x3026, 8, 0x3038 };
    void *cfg = NULL; int32_t n = 0;
    if (!eglChooseConfig(dpy, cattrs, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "FAIL: eglChooseConfig\n"); return 1;
    }
    const int32_t sattrs[] = { 0x3057, 64, 0x3056, 64, 0x3038 };
    void *surf = eglCreatePbufferSurface(dpy, cfg, sattrs);
    // ES 3.0 with no minor asked for, which is the context the guest gets — and
    // the ES version is load-bearing, because ANGLE applies some front-end caps
    // only below ES 3.2.
    const int32_t xattrs[] = { 0x3098, 3, 0x3038 };
    void *ctx = eglCreateContext(dpy, cfg, NULL, xattrs);
    if (!ctx || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "FAIL: no current ES3 context\n"); return 1;
    }
    void (*glGetIntegerv)(uint32_t, int32_t *) = sym("glGetIntegerv");
    uint32_t (*glGetError)(void) = sym("glGetError");

    int bad = 0, checked = 0;
    for (unsigned i = 0; i < sizeof g_limits / sizeof g_limits[0]; i++) {
        int32_t ours = 0;
        if (!kl_gl_cap_integerv(g_limits[i].pname, &ours)) continue;  // we do not claim it
        int32_t theirs = -1;
        while (glGetError()) {}
        glGetIntegerv(g_limits[i].pname, &theirs);
        if (glGetError()) continue;                                   // ANGLE will not say
        checked++;
        int over = g_limits[i].kind == BUDGET ? (ours > theirs) : 0;
        if (over) {
            bad++;
            printf("  %-34s we say %-8d ANGLE %-8d *** WE OVERCLAIM ***\n",
                   g_limits[i].name, ours, theirs);
        }
    }
    if (bad) {
        printf("FAIL: %d of %d limit(s) claim more than ANGLE honours\n", bad, checked);
        return 1;
    }
    printf("PASS: %d GL limit(s) checked, none claims more than ANGLE honours\n", checked);
    return 0;
}
