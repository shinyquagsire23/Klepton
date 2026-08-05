// s11_draw: does a real DRAW work on an ANGLE/Metal pbuffer's default
// framebuffer? The guest's draws to fb0 all fail with GL_INVALID_FRAMEBUFFER_OPERATION
// (0x506) even though glCheckFramebufferStatus says COMPLETE and glClear works
// (KL_GLFB_PROBE). This spike reproduces that with no guest linked: ES3 context,
// 1832x1920 pbuffer (the guest's eye size), trivial shader, one triangle,
// readback. s09 and the probe establish dlopen/context/clear; this adds the draw.
//
// Build: clang -o build/s11_draw spikes/s11_draw.c
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#define EGL_PLATFORM_ANGLE_ANGLE          0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE     0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define EGL_DEFAULT_DISPLAY               ((void *)0)
#define EGL_NO_CONTEXT                    ((void *)0)
#define EGL_NONE                          0x3038
#define EGL_SURFACE_TYPE                  0x3033
#define EGL_PBUFFER_BIT                   0x0001
#define EGL_RENDERABLE_TYPE               0x3040
#define EGL_OPENGL_ES3_BIT                0x00000040
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_2D 0x0DE1
#define GL_TRIANGLES 0x0004
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FLOAT 0x1406
#define GL_FALSE 0

#define ANGLE_VENDORED_DIR "vendor/out/Debug"
#define ANGLE_DEFAULT_DIR \
    "/Applications/Google Chrome.app/Contents/Frameworks/" \
    "Google Chrome Framework.framework/Libraries"

static const char *angle_dir(void) {
    const char *d = getenv("KL_ANGLE_DIR");
    if (d) return d;
    if (access(ANGLE_VENDORED_DIR "/libEGL.dylib", R_OK) == 0) return ANGLE_VENDORED_DIR;
    return ANGLE_DEFAULT_DIR;
}

static void *g_egl, *g_gles;
static void *sym(const char *n) {
    void *p = g_gles ? dlsym(g_gles, n) : NULL;
    if (!p && g_egl) p = dlsym(g_egl, n);
    if (!p) { fprintf(stderr, "missing %s\n", n); exit(1); }
    return p;
}
#define S(n) n##_p

int main(void) {
    char p[1024];
    const char *d = angle_dir();
    snprintf(p, sizeof p, "%s/libEGL.dylib", d);    g_egl  = dlopen(p, RTLD_NOW|RTLD_LOCAL);
    snprintf(p, sizeof p, "%s/libGLESv2.dylib", d); g_gles = dlopen(p, RTLD_NOW|RTLD_LOCAL);
    if (!g_egl || !g_gles) { fprintf(stderr, "dlopen ANGLE: %s\n", dlerror()); return 1; }

    void *(*S(eglGetPlatformDisplayEXT))(uint32_t, void *, const int32_t *);
    uint32_t (*S(eglInitialize))(void *, int32_t *, int32_t *);
    uint32_t (*S(eglChooseConfig))(void *, const int32_t *, void **, int32_t, int32_t *);
    void *(*S(eglCreatePbufferSurface))(void *, void *, const int32_t *);
    void *(*S(eglCreateContext))(void *, void *, void *, const int32_t *);
    uint32_t (*S(eglMakeCurrent))(void *, void *, void *, void *);
    uint32_t (*S(glGetError))(void);
    const uint8_t *(*S(glGetString))(uint32_t);
    uint32_t (*S(glCheckFramebufferStatus))(uint32_t);
    void (*S(glGetIntegerv))(uint32_t, int32_t *);
    void (*S(glViewport))(int32_t, int32_t, int32_t, int32_t);
    void (*S(glClearColor))(float, float, float, float);
    void (*S(glClear))(uint32_t);
    uint32_t (*S(glCreateShader))(uint32_t);
    void (*S(glShaderSource))(uint32_t, int32_t, const char *const *, const int32_t *);
    void (*S(glCompileShader))(uint32_t);
    void (*S(glGetShaderiv))(uint32_t, uint32_t, int32_t *);
    void (*S(glGetShaderInfoLog))(uint32_t, int32_t, int32_t *, char *);
    uint32_t (*S(glCreateProgram))(void);
    void (*S(glAttachShader))(uint32_t, uint32_t);
    void (*S(glLinkProgram))(uint32_t);
    void (*S(glGetProgramiv))(uint32_t, uint32_t, int32_t *);
    void (*S(glUseProgram))(uint32_t);
    int32_t (*S(glGetAttribLocation))(uint32_t, const char *);
    void (*S(glGenBuffers))(int32_t, uint32_t *);
    void (*S(glBindBuffer))(uint32_t, uint32_t);
    void (*S(glBufferData))(uint32_t, intptr_t, const void *, uint32_t);
    void (*S(glEnableVertexAttribArray))(uint32_t);
    void (*S(glVertexAttribPointer))(uint32_t, int32_t, uint32_t, uint32_t, int32_t, const void *);
    void (*S(glDrawArrays))(uint32_t, int32_t, int32_t);
    void (*S(glFinish))(void);
    void (*S(glReadPixels))(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void *);

    eglGetPlatformDisplayEXT_p = (void *)sym("eglGetPlatformDisplayEXT");
    eglInitialize_p = (void *)sym("eglInitialize");
    eglChooseConfig_p = (void *)sym("eglChooseConfig");
    eglCreatePbufferSurface_p = (void *)sym("eglCreatePbufferSurface");
    eglCreateContext_p = (void *)sym("eglCreateContext");
    eglMakeCurrent_p = (void *)sym("eglMakeCurrent");
    glGetError_p = (void *)sym("glGetError");
    glGetString_p = (void *)sym("glGetString");
    glCheckFramebufferStatus_p = (void *)sym("glCheckFramebufferStatus");
    glGetIntegerv_p = (void *)sym("glGetIntegerv");
    glViewport_p = (void *)sym("glViewport");
    glClearColor_p = (void *)sym("glClearColor");
    glClear_p = (void *)sym("glClear");
    glCreateShader_p = (void *)sym("glCreateShader");
    glShaderSource_p = (void *)sym("glShaderSource");
    glCompileShader_p = (void *)sym("glCompileShader");
    glGetShaderiv_p = (void *)sym("glGetShaderiv");
    glGetShaderInfoLog_p = (void *)sym("glGetShaderInfoLog");
    glCreateProgram_p = (void *)sym("glCreateProgram");
    glAttachShader_p = (void *)sym("glAttachShader");
    glLinkProgram_p = (void *)sym("glLinkProgram");
    glGetProgramiv_p = (void *)sym("glGetProgramiv");
    glUseProgram_p = (void *)sym("glUseProgram");
    glGetAttribLocation_p = (void *)sym("glGetAttribLocation");
    glGenBuffers_p = (void *)sym("glGenBuffers");
    glBindBuffer_p = (void *)sym("glBindBuffer");
    glBufferData_p = (void *)sym("glBufferData");
    glEnableVertexAttribArray_p = (void *)sym("glEnableVertexAttribArray");
    glVertexAttribPointer_p = (void *)sym("glVertexAttribPointer");
    glDrawArrays_p = (void *)sym("glDrawArrays");
    glFinish_p = (void *)sym("glFinish");
    glReadPixels_p = (void *)sym("glReadPixels");

    const int32_t dpy_attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE };
    void *dpy = eglGetPlatformDisplayEXT_p(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, dpy_attrs);
    int32_t vmaj = 0, vmin = 0;
    if (!dpy) { fprintf(stderr, "eglGetPlatformDisplayEXT returned NULL\n"); return 1; }
    if (!eglInitialize_p(dpy, &vmaj, &vmin)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
    printf("  %s\n", glGetString_p(0x1F01 /* GL_RENDERER */));

    const int32_t cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    void *cfg = NULL; int32_t ncfg = 0;
    if (!eglChooseConfig_p(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1) { fprintf(stderr, "no config\n"); return 1; }
    int W = 1832, H = 1920;
    const char *sz = getenv("S11_SIZE");
    if (sz) sscanf(sz, "%dx%d", &W, &H);
    const int32_t surf_attrs[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    void *surf = eglCreatePbufferSurface_p(dpy, cfg, surf_attrs);
    const int32_t ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    void *ctx = eglCreateContext_p(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (!surf || !ctx || !eglMakeCurrent_p(dpy, surf, surf, ctx)) { fprintf(stderr, "make current failed\n"); return 1; }

    printf("  fb status: 0x%x\n", glCheckFramebufferStatus_p(0x8CA9));
    glViewport_p(0, 0, W, H);
    glClearColor_p(0.1f, 0.2f, 0.3f, 1.0f);
    glClear_p(GL_COLOR_BUFFER_BIT);
    printf("  after clear: error 0x%x\n", glGetError_p());

    const char *vs_src =
        "#version 300 es\n"
        "in vec2 aPos;\n"
        "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    const char *fs_src =
        "#version 300 es\n"
        "precision mediump float;\n"
        "out vec4 frag;\n"
        "void main() { frag = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    uint32_t vs = glCreateShader_p(GL_VERTEX_SHADER);
    glShaderSource_p(vs, 1, &vs_src, NULL);
    glCompileShader_p(vs);
    int32_t ok = 0; glGetShaderiv_p(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog_p(vs, sizeof log, NULL, log); fprintf(stderr, "vs: %s\n", log); return 1; }
    uint32_t fs = glCreateShader_p(GL_FRAGMENT_SHADER);
    glShaderSource_p(fs, 1, &fs_src, NULL);
    glCompileShader_p(fs);
    glGetShaderiv_p(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog_p(fs, sizeof log, NULL, log); fprintf(stderr, "fs: %s\n", log); return 1; }
    uint32_t prog = glCreateProgram_p();
    glAttachShader_p(prog, vs);
    glAttachShader_p(prog, fs);
    glLinkProgram_p(prog);
    glGetProgramiv_p(prog, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "link failed\n"); return 1; }
    glUseProgram_p(prog);
    printf("  program linked+used: error 0x%x\n", glGetError_p());

    float verts[] = { -0.9f, -0.9f, 0.9f, -0.9f, 0.0f, 0.9f };
    uint32_t buf = 0;
    glGenBuffers_p(1, &buf);
    glBindBuffer_p(GL_ARRAY_BUFFER, buf);
    glBufferData_p(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    int32_t loc = glGetAttribLocation_p(prog, "aPos");
    glEnableVertexAttribArray_p((uint32_t)loc);
    glVertexAttribPointer_p((uint32_t)loc, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
    printf("  before draw: error 0x%x\n", glGetError_p());

    glDrawArrays_p(GL_TRIANGLES, 0, 3);
    printf("  after draw:  error 0x%x   <-- the guest sees 0x506 here\n", glGetError_p());
    glFinish_p();

    int cw = W < 64 ? W : 64, ch = H < 64 ? H : 64;
    uint8_t *px = malloc((size_t)cw * ch * 4);
    glReadPixels_p(0, 0, cw, ch, GL_RGBA, GL_UNSIGNED_BYTE, px);
    unsigned long lit = 0;
    for (int i = 0; i < cw * ch; i++)
        if (px[i*4] + px[i*4+1] + px[i*4+2] > 30) lit++;
    printf("  readback: %lu/%d lit (want plenty: clear colour + red triangle)\n",
           lit, cw * ch);
    free(px);
    return 0;
}
