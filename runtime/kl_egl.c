// M5, first cut — EGL, and the door to everything behind it.
//
// The 19 unresolved imports in libunity.so are all EGL and contain no GL entry
// point whatsoever, because `eglGetProcAddress` is one of them: Unity resolves
// the whole of GLES through that single function at runtime. So the entire
// graphics surface is reachable from one place, and the cheapest useful thing
// to build first is an EGL that is a *correct state machine* — real configs,
// real surfaces, consistent answers — wired to a GL gateway that names whatever
// the guest reaches for.
//
// That follows the rule the JNI surface already runs on:
//
//   * eglGetProcAddress is a LOOKUP. It never fails and never aborts; the guest
//     resolves plenty it never calls, and refusing there would only tell us
//     which symbol Unity's loader happened to reach first.
//   * calling the returned pointer is an ASSERTION. It aborts by name.
//     KL_PERMISSIVE=1 downgrades that to a zero return so one run collects the
//     whole GL surface instead of one function per run — which is the entire
//     point of this file right now, since nothing can size M5 until we know
//     what Unity actually calls.
//
// Nothing here draws anything. The configs describe the panel kl_jni.c already
// presents and the surfaces are sized from the ANativeWindow the guest hands
// over, so the numbers agree with the rest of the shim; binding them to a real
// Metal/MoltenVK drawable is the next step, not this one.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "klepton.h"
#include "kl_egl.h"
#include "kl_ndk.h"

// ---- the subset of EGL/egl.h we actually answer for ----
#define EGL_FALSE                 0
#define EGL_TRUE                  1
#define EGL_NONE             0x3038
#define EGL_SUCCESS          0x3000
#define EGL_BAD_DISPLAY      0x3008
#define EGL_BAD_PARAMETER    0x300C

#define EGL_ALPHA_SIZE       0x3021
#define EGL_BLUE_SIZE        0x3022
#define EGL_GREEN_SIZE       0x3023
#define EGL_RED_SIZE         0x3024
#define EGL_DEPTH_SIZE       0x3025
#define EGL_STENCIL_SIZE     0x3026
#define EGL_CONFIG_CAVEAT    0x3027
#define EGL_CONFIG_ID        0x3028
#define EGL_BUFFER_SIZE      0x3020
#define EGL_LEVEL            0x3029
#define EGL_MAX_PBUFFER_HEIGHT 0x302A
#define EGL_MAX_PBUFFER_PIXELS 0x302B
#define EGL_MAX_PBUFFER_WIDTH  0x302C
#define EGL_NATIVE_RENDERABLE  0x302D
#define EGL_NATIVE_VISUAL_ID   0x302E
#define EGL_NATIVE_VISUAL_TYPE 0x302F
#define EGL_SAMPLES          0x3031
#define EGL_SAMPLE_BUFFERS   0x3032
#define EGL_SURFACE_TYPE     0x3033
#define EGL_TRANSPARENT_TYPE 0x3034
#define EGL_COLOR_BUFFER_TYPE 0x303F
#define EGL_RGB_BUFFER       0x308E
#define EGL_RENDERABLE_TYPE  0x3040
#define EGL_CONFORMANT       0x3042
#define EGL_VENDOR           0x3053
#define EGL_VERSION          0x3054
#define EGL_EXTENSIONS       0x3055
#define EGL_HEIGHT           0x3056
#define EGL_WIDTH            0x3057
#define EGL_DRAW             0x3059
#define EGL_READ             0x305A
#define EGL_CLIENT_APIS      0x308D
#define EGL_SWAP_BEHAVIOR    0x3093
#define EGL_BUFFER_DESTROYED 0x3095
#define EGL_OPENGL_ES2_BIT   0x0004
#define EGL_OPENGL_ES3_BIT   0x0040
#define EGL_PBUFFER_BIT      0x0001
#define EGL_WINDOW_BIT       0x0004

typedef void *EGLDisplay, *EGLSurface, *EGLContext, *EGLConfig;

// Opaque handles the guest only ever compares and passes back. Distinct
// addresses, so a mismatched one is a wrong pointer rather than a silent alias.
static struct { int initialised; } g_display;
static struct { int id, es_bits, samples; } g_configs[] = {
    {1, EGL_OPENGL_ES3_BIT | EGL_OPENGL_ES2_BIT, 0},   // the one Unity will pick
    {2, EGL_OPENGL_ES3_BIT | EGL_OPENGL_ES2_BIT, 4},   // 4x MSAA
};
#define NCONFIGS ((int)(sizeof g_configs / sizeof g_configs[0]))

typedef struct { int32_t w, h; int pbuffer; } kl_egl_surface;
typedef struct { int client_version; } kl_egl_context;

static kl_egl_surface g_surfaces[8];
static kl_egl_context g_contexts[8];
static unsigned g_nsurf, g_nctx;
static EGLSurface g_draw, g_read;
static EGLContext g_current;
static int g_error = EGL_SUCCESS;
static unsigned long g_frames;

#define DISPLAY ((EGLDisplay)&g_display)

// ---------- the GL gateway ----------
#define KL_GL_MAX 256
static struct { const char *name; unsigned calls; int resolved; } g_gl[KL_GL_MAX];
static unsigned g_ngl;

static int gl_slot(const char *name) {
    for (unsigned i = 0; i < g_ngl; i++)
        if (strcmp(g_gl[i].name, name) == 0) return (int)i;
    if (g_ngl >= KL_GL_MAX) return -1;
    g_gl[g_ngl].name = strdup(name);
    g_gl[g_ngl].calls = 0;
    g_gl[g_ngl].resolved = 0;
    return (int)g_ngl++;
}

static int g_permissive = -1;
static int permissive(void) {
    if (g_permissive < 0) g_permissive = getenv("KL_PERMISSIVE") != NULL;
    return g_permissive;
}

// Reached through a per-name trampoline, so x0 is the function's own name. The
// return value becomes the GL call's return value: the stub tail-calls here, so
// returning 0 is exactly "the GL function returned 0".
static uint64_t klgl_called(const char *name) {
    int s = gl_slot(name);
    if (s >= 0) g_gl[s].calls++;
    if (permissive()) {
        if (s >= 0 && g_gl[s].calls == 1)
            fprintf(stderr, "  [egl] GL call (permissive, returning 0): %s\n", name);
        return 0;
    }
    fprintf(stderr, "\n[klepton] fatal: guest called unimplemented GL entry point "
                    "'%s'\n", name);
    kl_egl_report(stderr);
    kl_fatal_prepare();
    abort();
}

// ---------- the GL calls that are already forced ----------
//
// glGetString is not optional in the way the rest of GL is. It is the first
// thing Unity calls, and a permissive zero is a NULL string that it immediately
// parses — which is a SIGSEGV at address 0 three frames inside libunity, i.e.
// exactly the "silent zeros are worse than errors" trap the shim has hit before.
//
// As with the display group in kl_jni.c, the capability set is answered as a
// whole rather than call by call: the version string, the GLSL version and the
// integer limits all have to describe the same device or Unity builds a renderer
// against one and validates it against another. What is described here is a
// plain, conformant GLES 3.2 with no extensions.
#define GL_NO_ERROR      0
#define GL_VENDOR   0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION  0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_NUM_EXTENSIONS 0x821D
#define GL_MAJOR_VERSION  0x821B
#define GL_MINOR_VERSION  0x821C

static const char *klgl_GetString(uint32_t name) {
    switch (name) {
    case GL_VENDOR:     return "Klepton";
    case GL_RENDERER:   return "Klepton GLES";
    // Unity parses this to decide the GLES level it drives, so the shape matters
    // as much as the number: it expects "OpenGL ES <major>.<minor> <vendor>".
    case GL_VERSION:    return "OpenGL ES 3.2 Klepton";
    case GL_SHADING_LANGUAGE_VERSION: return "OpenGL ES GLSL ES 3.20";
    // Empty for the same reason as the EGL extension string: naming one is a
    // promise, and a broken promise fails far from its cause. ES3 asks for these
    // through glGetStringi anyway.
    case GL_EXTENSIONS: return "";
    }
    fprintf(stderr, "  [gl] glGetString: unhandled name 0x%x\n", name);
    return "";
}

static const char *klgl_GetStringi(uint32_t name, uint32_t index) {
    (void)name; (void)index;
    return "";           // consistent with GL_NUM_EXTENSIONS = 0
}

static uint32_t klgl_GetError(void) { return GL_NO_ERROR; }

// The limits Unity sizes its renderer from. Values are a conformant GLES 3.2
// implementation on tile-based mobile hardware, which is what the guest thinks
// it is talking to; nothing here is measured from a real driver yet because
// there is no real driver yet.
static const struct { uint32_t pname; int32_t value; } g_gl_int[] = {
    {GL_NUM_EXTENSIONS,   0},
    {GL_MAJOR_VERSION,    3},
    {GL_MINOR_VERSION,    2},
    {0x0D33 /* MAX_TEXTURE_SIZE            */, 16384},
    {0x851C /* MAX_CUBE_MAP_TEXTURE_SIZE   */, 16384},
    {0x8073 /* MAX_3D_TEXTURE_SIZE         */, 2048},
    {0x88FF /* MAX_ARRAY_TEXTURE_LAYERS    */, 2048},
    {0x84E8 /* MAX_RENDERBUFFER_SIZE       */, 16384},
    {0x8869 /* MAX_VERTEX_ATTRIBS          */, 32},
    {0x8872 /* MAX_TEXTURE_IMAGE_UNITS     */, 16},
    {0x8B4D /* MAX_COMBINED_TEXTURE_IMAGE_UNITS */, 32},
    {0x8B4C /* MAX_VERTEX_TEXTURE_IMAGE_UNITS   */, 16},
    {0x8DFB /* MAX_VERTEX_UNIFORM_VECTORS  */, 256},
    {0x8DFD /* MAX_FRAGMENT_UNIFORM_VECTORS*/, 224},
    {0x8DFC /* MAX_VARYING_VECTORS         */, 15},
    {0x8CDF /* MAX_COLOR_ATTACHMENTS       */, 8},
    {0x8824 /* MAX_DRAW_BUFFERS            */, 8},
    {0x8D57 /* MAX_SAMPLES                 */, 4},
    {0x80E8 /* MAX_ELEMENTS_VERTICES       */, 65536},
    {0x80E9 /* MAX_ELEMENTS_INDICES        */, 65536},
    {0x8A30 /* MAX_UNIFORM_BLOCK_SIZE      */, 65536},
    {0x8A2D /* MAX_FRAGMENT_UNIFORM_BLOCKS */, 12},
    {0x8A2B /* MAX_VERTEX_UNIFORM_BLOCKS   */, 12},
    {0x8A2F /* MAX_COMBINED_UNIFORM_BLOCKS */, 24},
    {0x8A34 /* UNIFORM_BUFFER_OFFSET_ALIGNMENT */, 256},
    {0x87FE /* NUM_PROGRAM_BINARY_FORMATS  */, 0},
    {0x8DF9 /* NUM_SHADER_BINARY_FORMATS   */, 0},
    {0x86A2 /* NUM_COMPRESSED_TEXTURE_FORMATS */, 0},
    {0x9122 /* MAX_VERTEX_OUTPUT_COMPONENTS*/, 64},
    {0x9125 /* MAX_FRAGMENT_INPUT_COMPONENTS */, 60},
    // The default framebuffer's actual depths. These must agree with the EGL
    // config eglGetConfigAttrib reports — Unity asks both and compares.
    {0x0D50 /* SUBPIXEL_BITS */, 4},
    {0x0D52 /* RED_BITS     */, 8},
    {0x0D53 /* GREEN_BITS   */, 8},
    {0x0D54 /* BLUE_BITS    */, 8},
    {0x0D55 /* ALPHA_BITS   */, 8},
    {0x0D56 /* DEPTH_BITS   */, 24},
    {0x0D57 /* STENCIL_BITS */, 8},
    {0x80A8 /* SAMPLE_BUFFERS */, 0},
    {0x80A9 /* SAMPLES        */, 0},
};

static void klgl_GetIntegerv(uint32_t pname, int32_t *params) {
    if (!params) return;
    for (size_t i = 0; i < sizeof g_gl_int / sizeof g_gl_int[0]; i++)
        if (g_gl_int[i].pname == pname) { params[0] = g_gl_int[i].value; return; }
    if (pname == 0x0D3A) {                       // MAX_VIEWPORT_DIMS: two values
        params[0] = 16384; params[1] = 16384; return;
    }
    // Unknown limits get the same treatment as everything else: named, not
    // guessed. A zero here is what makes Unity allocate nothing and fail later.
    fprintf(stderr, "  [gl] glGetIntegerv: unhandled pname 0x%x\n", pname);
    params[0] = 0;
}


// The indexed and typed state queries. Same rule as glGetIntegerv: answer what
// we know, and *say* when we do not rather than writing a zero the caller reads
// as a limit. A zero max-work-group-count is a renderer that silently declines
// to dispatch anything.
static void klgl_GetIntegeri_v(uint32_t target, uint32_t index, int32_t *data) {
    if (!data) return;
    switch (target) {
    case 0x91BE /* MAX_COMPUTE_WORK_GROUP_COUNT */: data[0] = 65535; return;
    case 0x91BF /* MAX_COMPUTE_WORK_GROUP_SIZE  */: data[0] = index == 2 ? 64 : 1024; return;
    case 0x8A28 /* UNIFORM_BUFFER_BINDING */:
    case 0x8A29 /* UNIFORM_BUFFER_START   */:
    case 0x8A2A /* UNIFORM_BUFFER_SIZE    */: data[0] = 0; return;
    case 0x8C8C /* TRANSFORM_FEEDBACK_BUFFER_START */:
    case 0x8C8D /* TRANSFORM_FEEDBACK_BUFFER_SIZE  */:
    case 0x8C8F /* TRANSFORM_FEEDBACK_BUFFER_BINDING */: data[0] = 0; return;
    case 0x8E51 /* SAMPLE_MASK_VALUE */: data[0] = ~0; return;
    }
    fprintf(stderr, "  [gl] glGetIntegeri_v: unhandled target 0x%x (index %u)\n",
            target, index);
    data[0] = 0;
}

// Which sample counts a format supports. Answered as 4x only, to agree with
// GL_MAX_SAMPLES and with the MSAA config eglChooseConfig hands out — three
// places that have to describe the same device.
static void klgl_GetInternalformativ(uint32_t target, uint32_t internalformat,
                                     uint32_t pname, int32_t bufSize, int32_t *params) {
    (void)target; (void)internalformat;
    if (!params || bufSize <= 0) return;
    switch (pname) {
    case 0x9380 /* NUM_SAMPLE_COUNTS */: params[0] = 1; return;
    case 0x80A9 /* SAMPLES */:           params[0] = 4; return;
    }
    fprintf(stderr, "  [gl] glGetInternalformativ: unhandled pname 0x%x\n", pname);
    params[0] = 0;
}

static void klgl_GetFloatv(uint32_t pname, float *data) {
    if (!data) return;
    switch (pname) {
    case 0x846D /* ALIASED_POINT_SIZE_RANGE */: data[0] = 1.0f; data[1] = 1024.0f; return;
    case 0x846E /* ALIASED_LINE_WIDTH_RANGE */: data[0] = 1.0f; data[1] = 1.0f; return;
    case 0x0B21 /* LINE_WIDTH */:               data[0] = 1.0f; return;
    case 0x84FF /* MAX_TEXTURE_MAX_ANISOTROPY */:data[0] = 16.0f; return;
    case 0x0B71 /* DEPTH_RANGE (2 values) */:    data[0] = 0.0f; data[1] = 1.0f; return;
    }
    fprintf(stderr, "  [gl] glGetFloatv: unhandled pname 0x%x\n", pname);
    data[0] = 0.0f;
}

static void klgl_GetBooleanv(uint32_t pname, uint8_t *data) {
    if (!data) return;
    // Every boolean piece of GL state we could be asked about starts false, and
    // nothing here has turned any of it on.
    fprintf(stderr, "  [gl] glGetBooleanv: unhandled pname 0x%x\n", pname);
    data[0] = 0;
}

static void klgl_GetInteger64v(uint32_t pname, int64_t *data) {
    if (!data) return;
    switch (pname) {
    case 0x9111 /* MAX_SERVER_WAIT_TIMEOUT */: data[0] = 0; return;
    case 0x821B /* MAJOR_VERSION */: data[0] = 3; return;
    case 0x821C /* MINOR_VERSION */: data[0] = 2; return;
    }
    fprintf(stderr, "  [gl] glGetInteger64v: unhandled pname 0x%x\n", pname);
    data[0] = 0;
}


// ---------------------------------------------------------- the null driver
//
// Names, not rendering. Nothing here draws: the point is to get the frame far
// enough to see the shader and draw calls, which is what actually sizes the
// Metal backend. Two things must be real for that to work at all.
//
// First, object names. GL reserves 0 for "no object", so a permissive zero from
// glGenTextures is not a harmless stub — it is every texture aliasing the
// default one. Names are handed out from a single counter because GL only
// requires them unique within a type and nothing here cares which type.
//
// Second, shader sources: they are captured rather than discarded, because they
// are the input to the GLSL ES -> SPIR-V -> MSL pipeline the backend will need
// (PLANNING M5), and this is the only place they exist in plain text.
static uint32_t g_gl_name = 1;

static void klgl_gen(int32_t n, uint32_t *names) {
    for (int32_t i = 0; i < n && names; i++) names[i] = g_gl_name++;
}
static void klgl_delete(int32_t n, const uint32_t *names) { (void)n; (void)names; }
static uint8_t klgl_is(uint32_t name) { return name && name < g_gl_name; }

static uint32_t klgl_CreateShader(uint32_t type) {
    (void)type;
    return g_gl_name++;
}
static uint32_t klgl_CreateProgram(void) { return g_gl_name++; }

#define KL_GL_MAX_SHADERS 512
static struct { uint32_t name; char *src; } g_shaders[KL_GL_MAX_SHADERS];
static unsigned g_nshaders;

static void klgl_ShaderSource(uint32_t shader, int32_t count,
                              const char *const *strings, const int32_t *lengths) {
    if (!strings || count <= 0 || g_nshaders >= KL_GL_MAX_SHADERS) return;
    size_t total = 0;
    for (int32_t i = 0; i < count; i++)
        total += lengths && lengths[i] >= 0 ? (size_t)lengths[i]
                                            : (strings[i] ? strlen(strings[i]) : 0);
    char *buf = malloc(total + 1);
    if (!buf) return;
    size_t off = 0;
    for (int32_t i = 0; i < count; i++) {
        size_t len = lengths && lengths[i] >= 0 ? (size_t)lengths[i]
                                                : (strings[i] ? strlen(strings[i]) : 0);
        if (strings[i] && len) { memcpy(buf + off, strings[i], len); off += len; }
    }
    buf[off] = 0;
    g_shaders[g_nshaders].name = shader;
    g_shaders[g_nshaders].src  = buf;
    g_nshaders++;
}

// Compilation and linking "succeed" because there is nothing here to reject a
// shader — and a reported failure would send Unity down an error path over a
// verdict we did not actually reach.
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS    0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
static void klgl_GetShaderiv(uint32_t s, uint32_t pname, int32_t *p) {
    (void)s;
    if (!p) return;
    *p = (pname == GL_COMPILE_STATUS) ? 1 : 0;
}
static void klgl_GetProgramiv(uint32_t prog, uint32_t pname, int32_t *p) {
    (void)prog;
    if (!p) return;
    *p = (pname == GL_LINK_STATUS) ? 1 : 0;
}
static void klgl_GetInfoLog(uint32_t obj, int32_t bufSize, int32_t *length, char *log) {
    (void)obj;
    if (length) *length = 0;
    if (log && bufSize > 0) log[0] = 0;
}

// Uniform and attribute locations come from a per-name counter for the same
// reason object names do: -1 means "not found" and 0 is a legitimate location,
// so a zeroed stub would collapse every uniform onto slot 0.
static int32_t g_gl_loc;
static int32_t klgl_GetLocation(uint32_t program, const char *name) {
    (void)program; (void)name;
    return g_gl_loc++;
}

unsigned kl_egl_shader_count(void) { return g_nshaders; }

void kl_egl_dump_shaders(const char *dir) {
    if (!dir || !g_nshaders) return;
    for (unsigned i = 0; i < g_nshaders; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/shader_%03u_%u.glsl", dir, i, g_shaders[i].name);
        FILE *f = fopen(path, "w");
        if (!f) continue;
        fputs(g_shaders[i].src ? g_shaders[i].src : "", f);
        fclose(f);
    }
    fprintf(stderr, "  [gl] wrote %u shader sources to %s\n", g_nshaders, dir);
}

static const struct { const char *name; void *fn; } g_gl_impl[] = {
    {"glGetString",   (void *)klgl_GetString},
    {"glGetStringi",  (void *)klgl_GetStringi},
    {"glGetError",    (void *)klgl_GetError},
    {"glGetIntegerv", (void *)klgl_GetIntegerv},
    {"glGetIntegeri_v", (void *)klgl_GetIntegeri_v},
    {"glGetFloatv",     (void *)klgl_GetFloatv},
    {"glGetBooleanv",   (void *)klgl_GetBooleanv},
    {"glGetInteger64v", (void *)klgl_GetInteger64v},
    {"glGetInternalformativ", (void *)klgl_GetInternalformativ},
    {"glGenBuffers", (void *)klgl_gen},
    {"glGenTextures", (void *)klgl_gen},
    {"glGenFramebuffers", (void *)klgl_gen},
    {"glGenRenderbuffers", (void *)klgl_gen},
    {"glGenVertexArrays", (void *)klgl_gen},
    {"glGenSamplers", (void *)klgl_gen},
    {"glGenQueries", (void *)klgl_gen},
    {"glGenTransformFeedbacks", (void *)klgl_gen},
    {"glGenProgramPipelines", (void *)klgl_gen},
    {"glDeleteBuffers", (void *)klgl_delete},
    {"glDeleteTextures", (void *)klgl_delete},
    {"glDeleteFramebuffers", (void *)klgl_delete},
    {"glDeleteRenderbuffers", (void *)klgl_delete},
    {"glDeleteVertexArrays", (void *)klgl_delete},
    {"glDeleteSamplers", (void *)klgl_delete},
    {"glDeleteQueries", (void *)klgl_delete},
    {"glDeleteTransformFeedbacks", (void *)klgl_delete},
    {"glDeleteProgramPipelines", (void *)klgl_delete},
    {"glIsBuffer", (void *)klgl_is},
    {"glIsTexture", (void *)klgl_is},
    {"glIsFramebuffer", (void *)klgl_is},
    {"glIsRenderbuffer", (void *)klgl_is},
    {"glIsVertexArray", (void *)klgl_is},
    {"glIsSampler", (void *)klgl_is},
    {"glIsQuery", (void *)klgl_is},
    {"glIsProgram", (void *)klgl_is},
    {"glIsShader", (void *)klgl_is},
    {"glCreateShader",  (void *)klgl_CreateShader},
    {"glCreateProgram", (void *)klgl_CreateProgram},
    {"glShaderSource",  (void *)klgl_ShaderSource},
    {"glGetShaderiv",   (void *)klgl_GetShaderiv},
    {"glGetProgramiv",  (void *)klgl_GetProgramiv},
    {"glGetShaderInfoLog",  (void *)klgl_GetInfoLog},
    {"glGetProgramInfoLog", (void *)klgl_GetInfoLog},
    {"glGetUniformLocation", (void *)klgl_GetLocation},
    {"glGetAttribLocation",  (void *)klgl_GetLocation},
    {"glGetUniformBlockIndex", (void *)klgl_GetLocation},

};

// ---------- EGL ----------
static EGLDisplay klegl_GetDisplay(void *native) { (void)native; return DISPLAY; }

static unsigned klegl_Initialize(EGLDisplay dpy, int32_t *major, int32_t *minor) {
    if (dpy != DISPLAY) { g_error = EGL_BAD_DISPLAY; return EGL_FALSE; }
    g_display.initialised = 1;
    if (major) *major = 1;
    if (minor) *minor = 5;
    return EGL_TRUE;
}

static unsigned klegl_Terminate(EGLDisplay dpy) {
    (void)dpy; g_display.initialised = 0; return EGL_TRUE;
}

static const char *klegl_QueryString(EGLDisplay dpy, int32_t name) {
    (void)dpy;
    switch (name) {
    case EGL_VENDOR:  return "Klepton";
    case EGL_VERSION: return "1.5 Klepton";
    // Deliberately empty. Every extension named here is a promise, and Unity
    // branches hard on this string — advertising one we do not implement buys a
    // failure much further from its cause.
    case EGL_EXTENSIONS:  return "";
    case EGL_CLIENT_APIS: return "OpenGL_ES";
    }
    g_error = EGL_BAD_PARAMETER;
    return NULL;
}

static unsigned klegl_ChooseConfig(EGLDisplay dpy, const int32_t *attribs,
                                   EGLConfig *configs, int32_t size, int32_t *num) {
    (void)dpy;
    // The attribute list is honoured only far enough to be self-consistent:
    // report the MSAA config when samples were asked for, otherwise the plain
    // one. Unity re-queries everything it cares about with eglGetConfigAttrib.
    int want_samples = 0;
    for (const int32_t *a = attribs; a && *a != EGL_NONE; a += 2)
        if (a[0] == EGL_SAMPLES) want_samples = a[1];

    int pick = want_samples > 0 ? 1 : 0;
    if (configs && size > 0) configs[0] = (EGLConfig)&g_configs[pick];
    if (num) *num = (configs && size > 0) ? 1 : NCONFIGS;
    return EGL_TRUE;
}

static unsigned klegl_GetConfigAttrib(EGLDisplay dpy, EGLConfig cfg,
                                      int32_t attr, int32_t *value) {
    (void)dpy;
    if (!value) { g_error = EGL_BAD_PARAMETER; return EGL_FALSE; }
    const typeof(g_configs[0]) *c = cfg ? (const typeof(g_configs[0]) *)cfg : &g_configs[0];
    int32_t w, h;
    kl_ndk_window_size(NULL, &w, &h);
    switch (attr) {
    case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE:
    case EGL_ALPHA_SIZE:        *value = 8; break;
    case EGL_BUFFER_SIZE:       *value = 32; break;
    case EGL_DEPTH_SIZE:        *value = 24; break;
    case EGL_STENCIL_SIZE:      *value = 8; break;
    case EGL_SAMPLES:           *value = c->samples; break;
    case EGL_SAMPLE_BUFFERS:    *value = c->samples ? 1 : 0; break;
    case EGL_CONFIG_ID:         *value = c->id; break;
    case EGL_RENDERABLE_TYPE:
    case EGL_CONFORMANT:        *value = c->es_bits; break;
    case EGL_SURFACE_TYPE:      *value = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; break;
    case EGL_COLOR_BUFFER_TYPE: *value = EGL_RGB_BUFFER; break;
    case EGL_CONFIG_CAVEAT:     *value = EGL_NONE; break;
    case EGL_TRANSPARENT_TYPE:  *value = EGL_NONE; break;
    case EGL_NATIVE_RENDERABLE: *value = EGL_TRUE; break;
    case EGL_NATIVE_VISUAL_ID:  *value = 1; break;      // RGBA_8888, as kl_ndk reports
    case EGL_NATIVE_VISUAL_TYPE:*value = 0; break;
    case EGL_LEVEL:             *value = 0; break;
    case EGL_MAX_PBUFFER_WIDTH: *value = w; break;
    case EGL_MAX_PBUFFER_HEIGHT:*value = h; break;
    case EGL_MAX_PBUFFER_PIXELS:*value = w * h; break;
    case 0x303B /* EGL_MIN_SWAP_INTERVAL */: *value = 0; break;
    case 0x303C /* EGL_MAX_SWAP_INTERVAL */: *value = 1; break;
    case 0x303D /* EGL_LUMINANCE_SIZE */:
    case 0x303E /* EGL_ALPHA_MASK_SIZE */:   *value = 0; break;
    case 0x3039 /* EGL_BIND_TO_TEXTURE_RGB  */:
    case 0x303A /* EGL_BIND_TO_TEXTURE_RGBA */: *value = EGL_FALSE; break;
    default:
        // An unknown attribute is a measurement, not a failure: say so and give
        // EGL's own "no" rather than inventing a number.
        fprintf(stderr, "  [egl] eglGetConfigAttrib: unhandled attribute 0x%x\n", attr);
        g_error = 0x3004 /* EGL_BAD_ATTRIBUTE */;
        return EGL_FALSE;
    }
    return EGL_TRUE;
}

static EGLSurface new_surface(int32_t w, int32_t h, int pbuffer) {
    if (g_nsurf >= sizeof g_surfaces / sizeof g_surfaces[0]) return NULL;
    kl_egl_surface *s = &g_surfaces[g_nsurf++];
    s->w = w; s->h = h; s->pbuffer = pbuffer;
    return (EGLSurface)s;
}

static EGLSurface klegl_CreateWindowSurface(EGLDisplay dpy, EGLConfig cfg,
                                            void *win, const int32_t *attribs) {
    (void)dpy; (void)cfg; (void)attribs;
    int32_t w, h;
    kl_ndk_window_size(win, &w, &h);   // the size Unity will build its target from
    fprintf(stderr, "  [egl] window surface %dx%d\n", w, h);
    return new_surface(w, h, 0);
}

static EGLSurface klegl_CreatePbufferSurface(EGLDisplay dpy, EGLConfig cfg,
                                             const int32_t *attribs) {
    (void)dpy; (void)cfg;
    int32_t w = 1, h = 1;
    for (const int32_t *a = attribs; a && *a != EGL_NONE; a += 2) {
        if (a[0] == EGL_WIDTH)  w = a[1];
        if (a[0] == EGL_HEIGHT) h = a[1];
    }
    return new_surface(w, h, 1);
}

static unsigned klegl_DestroySurface(EGLDisplay dpy, EGLSurface s) {
    (void)dpy; (void)s; return EGL_TRUE;      // storage is never reclaimed
}

static unsigned klegl_QuerySurface(EGLDisplay dpy, EGLSurface surf,
                                   int32_t attr, int32_t *value) {
    (void)dpy;
    const kl_egl_surface *s = (const kl_egl_surface *)surf;
    if (!s || !value) { g_error = EGL_BAD_PARAMETER; return EGL_FALSE; }
    switch (attr) {
    case EGL_WIDTH:         *value = s->w; break;
    case EGL_HEIGHT:        *value = s->h; break;
    case EGL_SWAP_BEHAVIOR: *value = EGL_BUFFER_DESTROYED; break;
    default:
        fprintf(stderr, "  [egl] eglQuerySurface: unhandled attribute 0x%x\n", attr);
        g_error = 0x3004;
        return EGL_FALSE;
    }
    return EGL_TRUE;
}

static EGLContext klegl_CreateContext(EGLDisplay dpy, EGLConfig cfg,
                                      EGLContext share, const int32_t *attribs) {
    (void)dpy; (void)cfg; (void)share;
    if (g_nctx >= sizeof g_contexts / sizeof g_contexts[0]) return NULL;
    kl_egl_context *c = &g_contexts[g_nctx++];
    c->client_version = 2;
    for (const int32_t *a = attribs; a && *a != EGL_NONE; a += 2)
        if (a[0] == 0x3098 /* EGL_CONTEXT_CLIENT_VERSION */) c->client_version = a[1];
    fprintf(stderr, "  [egl] context, GLES %d\n", c->client_version);
    return (EGLContext)c;
}

static unsigned klegl_DestroyContext(EGLDisplay dpy, EGLContext c) {
    (void)dpy; if (c == g_current) g_current = NULL; return EGL_TRUE;
}

static unsigned klegl_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                  EGLSurface read, EGLContext ctx) {
    (void)dpy; g_draw = draw; g_read = read; g_current = ctx; return EGL_TRUE;
}

static EGLContext klegl_GetCurrentContext(void) { return g_current; }
static EGLSurface klegl_GetCurrentSurface(int32_t which) {
    return which == EGL_READ ? g_read : g_draw;
}

static unsigned klegl_SwapBuffers(EGLDisplay dpy, EGLSurface s) {
    (void)dpy; (void)s;
    g_frames++;
    return EGL_TRUE;
}

static unsigned klegl_SwapInterval(EGLDisplay dpy, int32_t interval) {
    (void)dpy; (void)interval; return EGL_TRUE;
}

static int32_t klegl_GetError(void) { int e = g_error; g_error = EGL_SUCCESS; return e; }

// The gateway. Never fails, never aborts — see the header comment.
void *kl_egl_sym(const char *name) {
    if (!name) return NULL;
    void *own = kl_egl_lookup(name);              // eglXxx resolved through here too
    if (own) return own;
    for (size_t i = 0; i < sizeof g_gl_impl / sizeof g_gl_impl[0]; i++)
        if (strcmp(g_gl_impl[i].name, name) == 0) return g_gl_impl[i].fn;
    int s = gl_slot(name);
    if (s >= 0) g_gl[s].resolved = 1;
    return kl_named_stub(name, (void *)klgl_called);
}

static void *klegl_GetProcAddress(const char *name) { return kl_egl_sym(name); }

// The second door. Unity does dlopen("libGLESv2.so") + dlsym rather than going
// through eglGetProcAddress for the core entry points; a failed dlopen there
// produced a NULL it called anyway.
static const char g_gl_handle[] = "klepton-gles";

void *kl_egl_dlopen(const char *soname) {
    if (!soname) return NULL;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    if (strcmp(b, "libGLESv2.so") == 0 || strcmp(b, "libGLESv3.so") == 0 ||
        strcmp(b, "libGLESv1_CM.so") == 0 || strcmp(b, "libEGL.so") == 0) {
        fprintf(stderr, "  [egl] guest dlopen(\"%s\") -> synthetic GL handle\n", b);
        return (void *)g_gl_handle;
    }
    return NULL;
}

int kl_egl_is_handle(const void *h) { return h == (const void *)g_gl_handle; }

unsigned long kl_egl_frames(void) { return g_frames; }

void kl_egl_report(FILE *f) {
    // Idempotent: this is reachable both from a caller that knows it is dying
    // and from kl_fatal_prepare(), which every fatal path goes through. Printing
    // the graphics surface twice is noise; printing it never is how the first
    // sem_wait abort came back with no GL information at all.
    static int done;
    if (done) return;
    done = 1;
    fprintf(f, "\n=== EGL / GL surface ===\n");
    fprintf(f, "  eglSwapBuffers: %lu\n", g_frames);
    unsigned called = 0;
    for (unsigned i = 0; i < g_ngl; i++) if (g_gl[i].calls) called++;
    fprintf(f, "  GL entry points resolved: %u, of which called: %u\n", g_ngl, called);
    if (!g_ngl) return;
    fprintf(f, "  --- called (the M5 work list) ---\n");
    for (unsigned i = 0; i < g_ngl; i++)
        if (g_gl[i].calls) fprintf(f, "    %-40s x%u\n", g_gl[i].name, g_gl[i].calls);
    fprintf(f, "  --- resolved but never called ---\n");
    for (unsigned i = 0; i < g_ngl; i++)
        if (!g_gl[i].calls) fprintf(f, "    %s\n", g_gl[i].name);
}

// ---------- the table: exactly the 19 imports, nothing speculative ----------
#define E(n, f) { n, (void *)(f) }
static const struct { const char *name; void *fn; } g_egl[] = {
    E("eglGetDisplay",          klegl_GetDisplay),
    E("eglInitialize",          klegl_Initialize),
    E("eglTerminate",           klegl_Terminate),
    E("eglQueryString",         klegl_QueryString),
    E("eglChooseConfig",        klegl_ChooseConfig),
    E("eglGetConfigAttrib",     klegl_GetConfigAttrib),
    E("eglCreateWindowSurface", klegl_CreateWindowSurface),
    E("eglCreatePbufferSurface",klegl_CreatePbufferSurface),
    E("eglDestroySurface",      klegl_DestroySurface),
    E("eglQuerySurface",        klegl_QuerySurface),
    E("eglCreateContext",       klegl_CreateContext),
    E("eglDestroyContext",      klegl_DestroyContext),
    E("eglMakeCurrent",         klegl_MakeCurrent),
    E("eglGetCurrentContext",   klegl_GetCurrentContext),
    E("eglGetCurrentSurface",   klegl_GetCurrentSurface),
    E("eglSwapBuffers",         klegl_SwapBuffers),
    E("eglSwapInterval",        klegl_SwapInterval),
    E("eglGetError",            klegl_GetError),
    E("eglGetProcAddress",      klegl_GetProcAddress),
};

void *kl_egl_lookup(const char *name) {
    for (size_t i = 0; i < sizeof g_egl / sizeof g_egl[0]; i++)
        if (strcmp(g_egl[i].name, name) == 0) return g_egl[i].fn;
    return NULL;
}
