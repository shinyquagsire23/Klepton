// EGL, and the door to everything behind it.
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
//     point of this file right now, since nothing can be sized until we know
//     what Unity actually calls.
//
// Nothing here draws anything. The configs describe the panel kl_jni.c already
// presents and the surfaces are sized from the ANativeWindow the guest hands
// over, so the numbers agree with the rest of the shim; binding them to a real
// Metal/MoltenVK drawable is the next step, not this one.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <zlib.h>
#include "klepton.h"
#include "kl_env.h"
#include "kl_egl.h"
#include "kl_ndk.h"
#include "kl_glfb.h"
#include "kl_mediandk.h"   // what an AHardwareBuffer is, for the image path
#include "kl_present.h"
#include "kl_openxr.h"   // XR_KHR_android_surface_swapchain: eglSwapBuffers presents it

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
// The configs we admit to having. Depth is per-config and not a constant, and
// that is not a detail: libvrlink_scene enumerates with eglGetConfigs and
// filters the list ITSELF, insisting on RGBA8888 + ES3 + WINDOW|PBUFFER + a
// depth of EXACTLY 16. With one depth-24 answer for every config it matched
// nothing, took its no-config branch, and android_main returned before its
// first GL call — no error, no log, just a thread that was there and then was
// not. A device that offers only depth-24 configs is an unusual device; every
// real GLES driver offers both.
//
// STENCIL is per-config for the same reason, and the guest that forced it is
// JKXR: the Oculus VrCubeWorld sample its front end is built from filters for
// an EXACT match on seven attributes, and the two it wants that no entry here
// offered are DEPTH 0 and STENCIL 0. That is not an odd request — an OpenXR app
// renders into swapchain images and needs no depth or stencil on the EGL config
// at all, so the sample asks for the cheapest one that exists — and its failure
// is the one this whole comment block is about: it logs
// `eglChooseConfig() failed` (its own message; it enumerates by hand), carries
// on with a null EGLContext, hands that to xrCreateSession, and dies inside the
// first glGenTextures of xrCreateSwapchain with nothing naming EGL.
//
// The attribute list is READ FROM THE BINARY rather than assumed
// (`libopenjk_ja.so+0x15cee0`: RGBA8888, depth 0, stencil 0, samples 0).
//
// Order matters only to the extent that eglChooseConfig picks by index, so
// every entry added after the first goes LAST and Unity's answer is unchanged.
static struct { int id, es_bits, samples, depth, stencil; } g_configs[] = {
    {1, EGL_OPENGL_ES3_BIT | EGL_OPENGL_ES2_BIT, 0, 24, 8},  // the one Unity will pick
    {2, EGL_OPENGL_ES3_BIT | EGL_OPENGL_ES2_BIT, 4, 24, 8},  // 4x MSAA
    {3, EGL_OPENGL_ES3_BIT | EGL_OPENGL_ES2_BIT, 0, 16, 8},  // what libvrlink_scene requires
    {4, EGL_OPENGL_ES3_BIT | EGL_OPENGL_ES2_BIT, 0,  0, 0},  // ...and what JKXR requires
};
#define NCONFIGS ((int)(sizeof g_configs / sizeof g_configs[0]))

typedef struct { int32_t w, h; int pbuffer; void *xr_swapchain; } kl_egl_surface;
typedef struct { int client_version; int config_id; } kl_egl_context;

static kl_egl_surface g_surfaces[8];
static kl_egl_context g_contexts[8];
static unsigned g_nsurf, g_nctx;
// EGL's "current" state is PER THREAD — eglMakeCurrent binds a context to the
// calling thread, and eglGetCurrentContext/Display/Surface answer for that
// thread alone. These were process-wide globals, so the last thread to call
// eglMakeCurrent decided what EVERY thread saw. Two threads ping-ponging one
// context (which is exactly how Unity drives GL: release on one, take on the
// next) meant a thread that had released still reported one as current.
//
// It matters because a guest may key resource ownership on the answer. Unity
// records the EGLContext each framebuffer object was bound under — framebuffer
// objects are container objects and are NOT shared between contexts, even
// within a share group — and re-checks it before every bind, binding
// (GLuint)-1 on a mismatch. On VRChat, which creates three contexts where Beat
// Saber creates two, that guard was firing: measured 140 -> 73
// GL_INVALID_FRAMEBUFFER_OPERATIONs over the same span of log with this fixed.
//
// It did NOT take them to zero, so this is not the whole of that bug (AVPro's
// third context was suspected there and measured innocent). This change stands
// on being what the spec says, not on that measurement.
//
// Nothing else in the tree reads these; the ANGLE context that actually backs
// them migrates separately, through kl_glfb_make_current/_release_current.
static __thread EGLSurface g_draw, g_read;
static __thread EGLContext g_current;
// The last REAL context (a g_contexts[] slot) this thread ever made current,
// remembered across an eglMakeCurrent(NULL) release. HL2Q3VR (hl2) needs it:
// its startup-video path releases the GL context to hand the Android Surface to
// a MediaPlayer, then relies on the post-video code to restore it — but our port
// finds no valve.avi, takes the "no video" branch, and never re-makes-current.
// The engine then reads eglGetCurrentContext at XR-graphics-binding time
// (libsourcevr's XrGraphicsBindingOpenGLESAndroidKHR setup), gets NULL, logs
// "Source EGL context is not current", and skips xrCreateSession entirely — so
// the whole VR render loop (and the multiview probe that lives inside it) is
// never reached, and the menu renders black. ANGLE keeps a live context the
// whole time (kl_glfb re-takes the root on the next GL call), so answering with
// the last real context is truthful, not a lie. Gated to hl2 so Unity — which
// deliberately keys FBO ownership on a NULL answer — is untouched.
static __thread EGLContext g_last_real;
// Process-wide companion to g_last_real: the last real context made current on ANY
// thread. The hl2 cinema eye-copy ("Point Insertion" g-man intro) reads
// eglGetCurrentContext on a VR submission thread that never made a context current
// there — so per-thread g_last_real is 0 and the guest would fall to its broken
// context-less copy path (Eye 1 fallback=1, see-through / stale per eye). All guest
// contexts alias the single ANGLE root on this backend, so answering with the
// process-wide last real context lets that thread take the NORMAL copy path; the
// klfb_GenFramebuffers host-bind (KL_HL2_CINEMA_CTX) makes its GL actually land.
static EGLContext g_last_real_any;
static int g_error = EGL_SUCCESS;
static unsigned long g_frames;

// Cached "is this the hl2 target" test — the only guest that needs the sticky
// current-context above. Computed once; kl_driver_target_name is stable.
static int klegl_sticky_ctx(void) {
    static int cached = -1;
    if (cached < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        cached = (t && strcmp(t, "hl2") == 0) ? 1 : 0;
    }
    return cached;
}

#define DISPLAY ((EGLDisplay)&g_display)

// ---------- the GL gateway ----------
#define KL_GL_MAX 512
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
    if (g_permissive < 0) g_permissive = kl_env_on("KL_PERMISSIVE", 0);
    return g_permissive;
}

// Reached through a per-name trampoline, so x0 is the function's own name. The
// return value becomes the GL call's return value: the stub tail-calls here, so
// returning 0 is exactly "the GL function returned 0".
// The last GL name that hit the unimplemented-abort. The pre-abort fprintf below
// races the device-console capture and is often lost, so this is stamped and
// re-emitted from kl_egl_report(), which rides the fault handler's report that
// DOES survive to the log — the reliable way to learn the culprit's name.
char kl_gl_last_unimpl[64];
static uint64_t klgl_called(const char *name) {
    int s = gl_slot(name);
    if (s >= 0) g_gl[s].calls++;
    if (permissive()) {
        if (s >= 0 && g_gl[s].calls == 1)
            fprintf(stderr, "  [egl] GL call (permissive, returning 0): %s\n", name);
        return 0;
    }
    snprintf(kl_gl_last_unimpl, sizeof kl_gl_last_unimpl, "%s", name ? name : "?");
    fprintf(stderr, "\n[klepton] fatal: guest called unimplemented GL entry point "
                    "'%s'\n", name);
    fflush(stderr);   // abort() below does not flush a fully-buffered stderr (log
                      // is a file, not a tty): without this the one line that names
                      // the culprit never reaches the captured log.
    kl_egl_report(stderr);
    kl_fatal_prepare();
    abort();
}

// The other half of the gateway: the calls a null driver can honestly do nothing
// for. Reached through the same per-name trampoline as klgl_called — x0 is the
// function's own name — so one handler serves all of them and kl_egl_report still
// counts each one separately. That count is the point: this list is the null
// driver's ledger of everything the Metal backend will eventually have to do.
static uint64_t klgl_noop(const char *name) {
    int s = gl_slot(name);
    if (s >= 0) g_gl[s].calls++;
    return 0;
}

// ---------- the GL calls that are already forced ----------
//
// glGetString is not optional in the way the rest of GL is. It is the first
// thing Unity calls, and a permissive zero is a NULL string it immediately
// parses — a SIGSEGV at address 0 three frames inside libunity.
//
// As with the display group in kl_jni.c, the capability set is answered as a
// whole rather than call by call: the version string, the GLSL version and the
// integer limits all have to describe the same device or Unity builds a renderer
// against one and validates it against another. What is described here is a
// plain, conformant GLES 3.2 with no extensions.
//
// 3.2 is a deliberate overstatement — ANGLE's Metal backend caps at ES 3.0 —
// and it is priced: Unity emits "#version 320 es" shaders and calls the ES 3.1
// program-interface queries, both of which an ES 3.0 context rejects, and
// kl_glfb.c repairs both at the GL boundary (the glShaderSource rewrite and the
// program-interface translation). An honest 3.0 description costs more: Unity
// gates B10G11R11_UFloatPack32 renderability on the version number itself (no
// extension parse — the string appears nowhere in libunity), so on 3.0 the
// post-processing stack falls back to format 'None' and aborts, and the VRDevice
// switches to the distortion-window path besides.
#define GL_NO_ERROR      0
#define GL_VENDOR   0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION  0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_NUM_EXTENSIONS 0x821D
#define GL_MAJOR_VERSION  0x821B
#define GL_MINOR_VERSION  0x821C

// The described GLES level. Default 3.2 for Beat Saber (the overstatement is
// deliberate and priced — see below); Steam Link is a GLES2 app and is told so
// via kl_egl_set_gles_version(2, 0) before any GL traffic.
static int g_es_major = 3, g_es_minor = 2;
void kl_egl_set_gles_version(int major, int minor) {
    g_es_major = major; g_es_minor = minor;
}

// The extensions advertised here. Naming one is a promise and a broken promise
// fails far from its cause — but an EMPTY list is its own broken promise to a
// guest that reads capabilities from the string instead of from the version.
//
// The rule for adding here is narrow: an extension may be named only if the
// GLES level we already claim in GL_VERSION makes it CORE, so naming it states
// nothing new. Both entries below qualify -- colour-renderable float and
// half-float are core in ES 3.2, which is what we tell Beat Saber we are -- and
// the backend really does provide them (kl_glfb prints "renderable float:" at
// context creation, and the vendored ANGLE answers yes to both).
//
// Unity 2019.4 infers RGBA16F renderability from the ES level it is told and
// never looks at the string; Unity 2018.4 (Beat Saber 1.6.0) asks for the string
// and, finding nothing, refuses to create the eye textures --
// "RenderTexture.Create failed: format unsupported - RGBA16 SFloat (2)", then
// "Failure creating VR textures of size (4580, 2400)", then it tears the
// distortion window down. Nothing in that chain mentions an extension, and the
// ES 3.2 claim makes the capability look already granted.
static const char *const g_gl_extensions[] = {
    "GL_EXT_color_buffer_float",
    "GL_EXT_color_buffer_half_float",
    // Apple GPUs decode ASTC natively and kl_glfb uploads the ASTC LDR block
    // formats, so advertise it — UE4 reads this to decide whether to load its
    // ASTC-cooked textures (without it, it reports "Unsupported Texture Format:
    // ASTC / Supported: ETC2" and then dies uploading the assets it does have).
    "GL_KHR_texture_compression_astc_ldr",
    "GL_OES_texture_compression_astc",
    // Vertex array objects: CORE in ES 3.0, so at the 3.2 we claim in
    // GL_VERSION naming it states nothing new, and ANGLE carries the OES entry
    // points (aliases of the core ones).
    //
    // Leaving it out is a NULL function pointer inside the guest, and it is a
    // glad-loader hazard rather than a title quirk: glad fills
    // `glad_glBindVertexArrayOES` only when this string names the extension,
    // and a renderer that calls it with no null test (the Simpsons Hit & Run VR
    // port's Pure3D layer does) branches to address zero on its first draw.
    "GL_OES_vertex_array_object",
    // Framebuffer blit + multisample. These are CORE in GLES 3 (glBlitFramebuffer,
    // glRenderbufferStorageMultisample) and fully backed by ANGLE, but Source's
    // togl is a desktop-GL translator that gates its scene->eye RESOLVE on the old
    // EXT extension NAMES and, finding them absent from our (curated) string,
    // logged "This system DOES NOT support GL_EXT_framebuffer_blit" and never
    // copied its rendered scene into the eye swapchain image — a black display
    // despite ~29k draws a frame. Advertise them so that path runs. (Harmless to
    // GLES guests, which use the core entry points regardless.)
    "GL_EXT_framebuffer_object",
    "GL_EXT_framebuffer_blit",
    "GL_EXT_framebuffer_multisample",
    // Single-pass stereo. glGetString(GL_EXTENSIONS) is "kept ours", so a guest
    // that decides whether to use multiview by reading this string never saw it
    // even after ANGLE's Metal backend gained OVR_multiview support. Advertising
    // it makes the guest bake num_views shaders and use glFramebufferTextureMulti
    // viewOVR; ANGLE then translates them (instanced emulation -> per-view
    // render_target_array_index) and renders both eyes in one pass.
    //
    // But ONLY offer it to targets that genuinely render this way — ieytd (Unity)
    // and the HL2Q3VR Source-VR family (hl2, portal), which are multiview-only VR
    // ports: without the extension they can't build their stereo vertex variants
    // ("no usable vertex shader variant") and render black. A different engine that
    // isn't built for it is better left on its own per-eye path. klgl_want_multiview()
    // gates BOTH doors (this list and GL_MAX_VIEWS_OVR); klgl_ext_count() drops these
    // entries when it returns 0, so THESE TWO MULTIVIEW ENTRIES MUST STAY LAST.
    "GL_OVR_multiview",
    "GL_OVR_multiview2",
};

// Which targets actually render with GLES single-pass multiview. Advertise the
// extension ONLY to these; a non-multiview engine (cs1/Xash, hl1/lambda1vr) that
// saw it might take a multiview path it can't back. The ones here NEED it: hiding
// it makes HL2Q3VR (hl2/portal) report "no usable vertex shader variant" for every
// stereo shader and render black — it is a multiview-only VR port, same as ieytd.
// (The ANGLE side still only emits gl_Layer for shaders that enable the extension,
// so even a mistaken advertisement can't fault a per-eye draw — this door is about
// steering each engine to the path it actually supports.)
extern const char *kl_driver_target_name(void);
static int klgl_want_multiview(void) {
    const char *t = kl_driver_target_name();
    if (!t) return 0;
    // ieytd/ieytd2/ieytd-<n> (Unity), and the HL2Q3VR Source-VR family (hl2, portal
    // — both liblauncher) whose single-pass stereo is baked around GL_OVR_multiview.
    return strncmp(t, "ieytd", 5) == 0 ||
           strcmp(t, "hl2") == 0 ||
           strcmp(t, "portal") == 0;
}

// ...and a SECOND list, admitted by a different door: not "core at the level
// we claim", but "the backend in this process told us, this run, that it has
// it". Nothing here is named unless ANGLE's own glGetString(GL_EXTENSIONS)
// named it first (kl_gl_cap_adopt_backend_extensions, called from kl_glfb once
// the real context exists), so a promise made here is one ANGLE has already
// made to us.
//
//   * The DXT/BC family. Source-engine ports ship DXT1/DXT5 texture packs, and
//     libtogl, finding no compression extension in the string, takes the
//     engine's CPU-decompress path and uploads RGBA8888 — 4-8x the bytes, which
//     on a large pack is the difference between fitting in the memory budget
//     and not. ANGLE-on-Metal spells the capability per format (_dxt1 and the
//     two GL_ANGLE_ names) and not as the umbrella _s3tc, so all four are
//     candidates.
//   * GL_EXT_texture_filter_anisotropic: a sampler parameter, free in memory,
//     and without it a floor at a grazing angle samples from the mip chosen for
//     its worst axis.
//   * GL_EXT_texture_format_BGRA8888, which is not core at any GLES level. A
//     glad-bound renderer can build its upload pair from two places — the
//     internalformat from a table that says GL_BGRA_EXT unconditionally, the
//     format from a flag glad sets from this name — so an unnamed extension
//     selects a MIXED pair (GL_BGRA_EXT with GL_RGBA), invalid in every GLES
//     profile: every texture upload fails GL_INVALID_OPERATION and samples
//     black (the Simpsons Hit & Run VR port). Quest drivers tolerate the pair.
static const char *const g_gl_ext_candidates[] = {
    "GL_EXT_texture_compression_s3tc",
    "GL_EXT_texture_compression_dxt1",
    "GL_ANGLE_texture_compression_dxt3",
    "GL_ANGLE_texture_compression_dxt5",
    "GL_EXT_texture_filter_anisotropic",
    "GL_EXT_texture_format_BGRA8888",
};
#define KL_GL_EXT_CANDIDATES \
    ((int)(sizeof g_gl_ext_candidates / sizeof g_gl_ext_candidates[0]))

static const char *g_gl_ext_adopted[KL_GL_EXT_CANDIDATES];
static int g_gl_ext_adopted_n = 0;

// Called once, from kl_glfb, with whatever the real driver answers. A name the
// driver does not carry is simply not adopted — there is no override, because
// the failure this guards against (a guest uploading blocks to a driver that
// cannot decode them) is a black world, not a slow one.
void kl_gl_cap_adopt_backend_extensions(const char *backend_ext) {
    g_gl_ext_adopted_n = 0;
    if (!backend_ext) backend_ext = "";
    // The A/B back to the core-only description, and the escape hatch if a
    // guest turns out to mishandle a capability it now has.
    if (kl_env_on("KL_GL_NO_BACKEND_EXT", 0)) {
        fprintf(stderr, "  [gl] KL_GL_NO_BACKEND_EXT=1 — adopting nothing from "
                        "the backend\n");
        return;
    }
    for (int i = 0; i < KL_GL_EXT_CANDIDATES; i++) {
        const char *name = g_gl_ext_candidates[i];
        size_t len = strlen(name);
        // Whole-token match: "..._s3tc" is a prefix of "..._s3tc_srgb" in some
        // drivers, and strstr alone would adopt the shorter name off the longer.
        const char *q = backend_ext;
        int have = 0;
        while ((q = strstr(q, name)) != NULL) {
            char after = q[len];
            char before = (q == backend_ext) ? ' ' : q[-1];
            if ((after == 0 || after == ' ') && before == ' ') { have = 1; break; }
            q += len;
        }
        if (have) g_gl_ext_adopted[g_gl_ext_adopted_n++] = name;
    }
}

int kl_gl_cap_backend_has_dxt(void) {
    for (int i = 0; i < g_gl_ext_adopted_n; i++)
        if (strstr(g_gl_ext_adopted[i], "texture_compression_"))
            return 1;
    return 0;
}

int kl_gl_cap_adopted_extension_count(void) { return g_gl_ext_adopted_n; }
const char *kl_gl_cap_adopted_extension(int i) {
    return (i >= 0 && i < g_gl_ext_adopted_n) ? g_gl_ext_adopted[i] : NULL;
}

// ...and none of it is offered below ES 3. Steam Link is told GLES 2.0
// (kl_egl_set_gles_version) and has always run against an empty list; the
// argument above is an ES 3.2 argument and does not carry over, so that target
// stays exactly as it was rather than being changed by a fix it did not need.
static int klgl_ext_base_count(void) {
    if (g_es_major < 3) return 0;
    int n = (int)(sizeof g_gl_extensions / sizeof g_gl_extensions[0]);
    // Hide the two trailing OVR_multiview entries from targets that don't use it.
    if (!klgl_want_multiview()) n -= 2;
    return n;
}
static int klgl_ext_count(void) {
    int n = klgl_ext_base_count();
    return n ? n + g_gl_ext_adopted_n : 0;
}
static const char *klgl_ext_at(int i) {
    int base = klgl_ext_base_count();
    if (i < 0 || i >= base + g_gl_ext_adopted_n) return NULL;
    return i < base ? g_gl_extensions[i] : g_gl_ext_adopted[i - base];
}

static const char *klgl_GetString(uint32_t name) {
    // The extension string was already ~240 bytes of a 256-byte buffer, and
    // strlcat truncates silently, so any addition would have cut a name in half.
    static char ver[64], glsl[64], exts[768];
    switch (name) {
    case GL_VENDOR:     return "Klepton";
    case GL_RENDERER:   return "Klepton GLES";
    // Unity parses this to decide the GLES level it drives, so the shape matters
    // as much as the number: it expects "OpenGL ES <major>.<minor> <vendor>".
    case GL_VERSION:
        snprintf(ver, sizeof ver, "OpenGL ES %d.%d Klepton", g_es_major, g_es_minor);
        return ver;
    case GL_SHADING_LANGUAGE_VERSION:
        snprintf(glsl, sizeof glsl, "OpenGL ES GLSL ES %d.%d0",
                 g_es_major, g_es_minor);
        return glsl;
    // ES3 guests are supposed to ask through glGetStringi, and Unity 2018.4
    // asks BOTH ways. Built from the same list so the two doors cannot drift.
    case GL_EXTENSIONS: {
        int n = klgl_ext_count();
        exts[0] = 0;
        for (int i = 0; i < n; i++) {
            if (i) strlcat(exts, " ", sizeof exts);
            strlcat(exts, klgl_ext_at(i), sizeof exts);
        }
        return exts;
    }
    }
    fprintf(stderr, "  [gl] glGetString: unhandled name 0x%x\n", name);
    return "";
}

static const char *klgl_GetStringi(uint32_t name, uint32_t index) {
    if (name == GL_EXTENSIONS && (int)index < klgl_ext_count())
        return klgl_ext_at((int)index);
    return "";           // consistent with GL_NUM_EXTENSIONS
}

static uint32_t klgl_GetError(void) { return GL_NO_ERROR; }

// The limits Unity sizes its renderer from.
//
// **Every number here must be one ANGLE will actually honour.** These began as
// a plausible GLES 3.2 tile-based mobile driver, written when nothing real was
// underneath; ANGLE-on-Metal is underneath now, and a limit we state above what
// it enforces is not optimism, it is a lie with no error surface. The guest
// validates against THESE, passes, calls GL, and GL refuses — and an engine
// that keeps a shadow of the state it just set writes that shadow anyway,
// indexed by the number it believed was legal. Measured overclaims and what
// each one did:
//
//   MAX_VERTEX_ATTRIBS       32 vs 16     attribute slots the driver refuses
//   MAX_UNIFORM_BLOCK_SIZE   65536 vs 16384
//                                         Unity sizes UnityInstancing_PerDraw0
//                                         for 64 KiB; the shader then fails to
//                                         LINK ("exceeds GL_MAX_UNIFORM_BLOCK_
//                                         SIZE (16384)"), which is a whole
//                                         effect missing rather than a slow one
//
// Stating a LARGER value is only safe where the number is a requirement the
// guest must meet rather than a budget it may spend — UNIFORM_BUFFER_OFFSET_
// ALIGNMENT is the one of those here, and 256 over ANGLE's 16 is deliberate.
static const struct { uint32_t pname; int32_t value; } g_gl_int[] = {
    // GL_NUM_EXTENSIONS is NOT here — it is answered from g_gl_extensions in
    // kl_gl_cap_integerv so the count and the strings cannot disagree.
    {GL_MAJOR_VERSION,    3},
    {GL_MINOR_VERSION,    2},
    {0x0D33 /* MAX_TEXTURE_SIZE            */, 16384},
    {0x851C /* MAX_CUBE_MAP_TEXTURE_SIZE   */, 16384},
    {0x8073 /* MAX_3D_TEXTURE_SIZE         */, 2048},
    {0x88FF /* MAX_ARRAY_TEXTURE_LAYERS    */, 2048},
    {0x84E8 /* MAX_RENDERBUFFER_SIZE       */, 16384},
    {0x8869 /* MAX_VERTEX_ATTRIBS          */, 16},
    // ---- texture units: three numbers that must agree ----------------------
    //
    // Unity's own "Invalid texture unit!" check reads the cap it was told HERE,
    // not ANGLE's, and then binds anyway — so these answers, the vendored
    // ANGLE's `kMaxGLSamplerBindings`, and its front-end
    // `IMPLEMENTATION_MAX_ES31_ACTIVE_TEXTURES` clamp have to state one number.
    // Per-stage stays 32 because ANGLE's front end clamps
    // `maxShaderTextureImageUnits` to 32 regardless, and promising more than
    // the driver will accept is a bind that silently does nothing.
    //
    // **Combined is 96 because 64 was measured to be too few.** A Unity 2019.4
    // title assigns sampler units up to 67 on one post-processing program
    // (`glUniform1i(loc, 64..67)`, GL_INVALID_VALUE against a limit of 64) from
    // unit numbers its Android build baked in, where the driver's limit is
    // higher — it never queries and cannot be made to adapt. The refusal is not
    // where it hurts: the engine believes the unit was set, indexes its own
    // unbounded GL state cache with it, and writes through the function-pointer
    // table it dispatches every GL call through, so the process dies in an
    // unrelated subsystem thousands of frames later.
    {0x8872 /* MAX_TEXTURE_IMAGE_UNITS          */, 32},
    {0x8B4D /* MAX_COMBINED_TEXTURE_IMAGE_UNITS */, 96},
    {0x8B4C /* MAX_VERTEX_TEXTURE_IMAGE_UNITS   */, 32},
    {0x8DFB /* MAX_VERTEX_UNIFORM_VECTORS  */, 256},
    {0x8DFD /* MAX_FRAGMENT_UNIFORM_VECTORS*/, 224},
    {0x8DFC /* MAX_VARYING_VECTORS         */, 15},
    {0x8CDF /* MAX_COLOR_ATTACHMENTS       */, 8},
    {0x8824 /* MAX_DRAW_BUFFERS            */, 8},
    {0x8D57 /* MAX_SAMPLES                 */, 4},
    {0x80E8 /* MAX_ELEMENTS_VERTICES       */, 65536},
    {0x80E9 /* MAX_ELEMENTS_INDICES        */, 65536},
    {0x8A30 /* MAX_UNIFORM_BLOCK_SIZE      */, 16384},
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

// The capability probe behind klgl_GetIntegerv, exported for kl_glfb: 1 when
// the pname is one of ours (and *params written), 0 when it is not — no
// print, no zero. Under the reference renderer the miss goes to ANGLE, which
// owns the dynamic state (READ_BUFFER & co.) this table never described.
int kl_gl_cap_integerv(uint32_t pname, int32_t *params) {
    if (!params) return 1;
    if (pname == GL_MAJOR_VERSION) { params[0] = g_es_major; return 1; }
    if (pname == GL_MINOR_VERSION) { params[0] = g_es_minor; return 1; }
    // Answered from the list rather than from the table below, for the same
    // reason the two glGetString doors are: a count that disagrees with what
    // glGetStringi will hand out makes the guest read past the end or stop
    // short, and either one is silent.
    if (pname == GL_NUM_EXTENSIONS) { params[0] = klgl_ext_count(); return 1; }
    // The compressed-format pair is described as EMPTY in the table below, which
    // is only true while no compression extension is named. Once one is adopted
    // from the backend, "s3tc, and zero compressed formats" is two different
    // answers; hand both questions to ANGLE, which owns the real one.
    if ((pname == 0x86A2 /* NUM_COMPRESSED_TEXTURE_FORMATS */ ||
         pname == 0x86A3 /* COMPRESSED_TEXTURE_FORMATS     */) &&
        g_gl_ext_adopted_n > 0)
        return 0;
    for (size_t i = 0; i < sizeof g_gl_int / sizeof g_gl_int[0]; i++)
        if (g_gl_int[i].pname == pname) {
            params[0] = g_gl_int[i].value;
            // The texture-unit caps, asked and answered: Unity's own
            // "Invalid texture unit" check reads a cap it computed at device
            // init — confirm what it was told.
            if (pname == 0x8872 || pname == 0x8B4D || pname == 0x8B4C)
                fprintf(stderr, "  [gl] glGetIntegerv(0x%x) -> %d\n",
                        pname, params[0]);
            return 1;
        }
    if (pname == 0x0D3A) {                       // MAX_VIEWPORT_DIMS: two values
        params[0] = 16384; params[1] = 16384; return 1;
    }
    // GL_MAX_VIEWS_OVR — how many views multiview supports. Answer 2 (stereo,
    // matching the ANGLE Metal backend's maxViews) ONLY for multiview targets; for
    // everyone else leave it unhandled (returns 0 -> "multiview unavailable") so it
    // matches the hidden extension string and no per-eye engine takes a multiview
    // path. See klgl_want_multiview().
    if (pname == 0x9631) { if (!klgl_want_multiview()) return 0; params[0] = 2; return 1; }
    return 0;
}

static void klgl_GetIntegerv(uint32_t pname, int32_t *params) {
    if (!params) return;
    if (kl_gl_cap_integerv(pname, params)) return;
    // Unknown limits get the same treatment as everything else: named, not
    // guessed. A zero here is what makes Unity allocate nothing and fail later.
    fprintf(stderr, "  [gl] glGetIntegerv: unhandled pname 0x%x\n", pname);
    params[0] = 0;
}

int kl_gl_cap_integeri_v(uint32_t target, uint32_t index, int32_t *data) {
    if (!data) return 1;
    switch (target) {
    case 0x91BE /* MAX_COMPUTE_WORK_GROUP_COUNT */: data[0] = 65535; return 1;
    case 0x91BF /* MAX_COMPUTE_WORK_GROUP_SIZE  */: data[0] = index == 2 ? 64 : 1024; return 1;
    case 0x8A28 /* UNIFORM_BUFFER_BINDING */:
    case 0x8A29 /* UNIFORM_BUFFER_START   */:
    case 0x8A2A /* UNIFORM_BUFFER_SIZE    */: data[0] = 0; return 1;
    case 0x8C8C /* TRANSFORM_FEEDBACK_BUFFER_START */:
    case 0x8C8D /* TRANSFORM_FEEDBACK_BUFFER_SIZE  */:
    case 0x8C8F /* TRANSFORM_FEEDBACK_BUFFER_BINDING */: data[0] = 0; return 1;
    case 0x8E51 /* SAMPLE_MASK_VALUE */: data[0] = ~0; return 1;
    }
    return 0;
}

// The indexed and typed state queries. Same rule as glGetIntegerv: answer what
// we know, and *say* when we do not rather than writing a zero the caller reads
// as a limit. A zero max-work-group-count is a renderer that silently declines
// to dispatch anything.
static void klgl_GetIntegeri_v(uint32_t target, uint32_t index, int32_t *data) {
    if (!data) return;
    if (kl_gl_cap_integeri_v(target, index, data)) return;
    fprintf(stderr, "  [gl] glGetIntegeri_v: unhandled target 0x%x (index %u)\n",
            target, index);
    data[0] = 0;
}

int kl_gl_cap_internalformativ(uint32_t target, uint32_t internalformat,
                               uint32_t pname, int32_t bufSize, int32_t *params) {
    (void)target; (void)internalformat;
    if (!params || bufSize <= 0) return 1;
    switch (pname) {
    case 0x9380 /* NUM_SAMPLE_COUNTS */: params[0] = 1; return 1;
    case 0x80A9 /* SAMPLES */:           params[0] = 4; return 1;
    }
    return 0;
}

// Which sample counts a format supports. Answered as 4x only, to agree with
// GL_MAX_SAMPLES and with the MSAA config eglChooseConfig hands out — three
// places that have to describe the same device.
static void klgl_GetInternalformativ(uint32_t target, uint32_t internalformat,
                                     uint32_t pname, int32_t bufSize, int32_t *params) {
    if (!params || bufSize <= 0) return;
    if (kl_gl_cap_internalformativ(target, internalformat, pname, bufSize, params)) return;
    fprintf(stderr, "  [gl] glGetInternalformativ: unhandled pname 0x%x\n", pname);
    params[0] = 0;
}

// GLES 3.1 program-interface enumeration (glGetProgramInterfaceiv family).
// Unity 2019.4 walks these when it reflects a linked program (reached deep in
// Beat Saber's loading, ~swap 1290). The null driver links no real programs,
// so the honest answer is "no active resources": writing 0 to GL_ACTIVE_RESOURCES
// makes the guest's enumeration loop terminate. A KL_PERMISSIVE zero does not
// serve here: the permissive stub *returns* 0 without writing params, and Unity
// then loops on the unwritten resource count — observed as 1.9e9 calls of
// glGetProgramResourceName spinning a render thread at 100% CPU.
static void klgl_GetProgramInterfaceiv(uint32_t program, uint32_t programInterface,
                                       uint32_t pname, int32_t *params) {
    (void)program; (void)programInterface; (void)pname;
    if (params) params[0] = 0;
}
static void klgl_GetProgramResourceiv(uint32_t program, uint32_t programInterface,
                                      uint32_t index, int32_t propsCount,
                                      const uint32_t *props, int32_t bufSize,
                                      int32_t *length, int32_t *params) {
    (void)program; (void)programInterface; (void)index; (void)props;
    if (length) *length = 0;
    for (int32_t i = 0; i < propsCount && params && i < bufSize; i++) params[i] = 0;
}
static void klgl_GetProgramResourceName(uint32_t program, uint32_t programInterface,
                                        uint32_t index, int32_t bufSize,
                                        int32_t *length, char *name) {
    (void)program; (void)programInterface; (void)index;
    if (length) *length = 0;
    if (name && bufSize > 0) name[0] = 0;
}

int kl_gl_cap_floatv(uint32_t pname, float *data) {
    if (!data) return 1;
    switch (pname) {
    case 0x846D /* ALIASED_POINT_SIZE_RANGE */: data[0] = 1.0f; data[1] = 1024.0f; return 1;
    case 0x846E /* ALIASED_LINE_WIDTH_RANGE */: data[0] = 1.0f; data[1] = 1.0f; return 1;
    case 0x0B21 /* LINE_WIDTH */:               data[0] = 1.0f; return 1;
    case 0x84FF /* MAX_TEXTURE_MAX_ANISOTROPY */:data[0] = 16.0f; return 1;
    case 0x0B71 /* DEPTH_RANGE (2 values) */:    data[0] = 0.0f; data[1] = 1.0f; return 1;
    }
    return 0;
}

static void klgl_GetFloatv(uint32_t pname, float *data) {
    if (!data) return;
    if (kl_gl_cap_floatv(pname, data)) return;
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

int kl_gl_cap_integer64v(uint32_t pname, int64_t *data) {
    if (!data) return 1;
    switch (pname) {
    case 0x9111 /* MAX_SERVER_WAIT_TIMEOUT */: data[0] = 0; return 1;
    case 0x821B /* MAJOR_VERSION */: data[0] = 3; return 1;
    case 0x821C /* MINOR_VERSION */: data[0] = 2; return 1;
    }
    return 0;
}

static void klgl_GetInteger64v(uint32_t pname, int64_t *data) {
    if (!data) return;
    if (kl_gl_cap_integer64v(pname, data)) return;
    fprintf(stderr, "  [gl] glGetInteger64v: unhandled pname 0x%x\n", pname);
    data[0] = 0;
}

// Texture object state. Every value here is the initial value the GLES 3.2 spec
// gives a freshly created texture, taken from the guest's own GLES3 headers
// rather than from memory — so this is a quotation, not a guess.
//
// The caveat, and it is a real one: glTexParameter* is in the no-op list, so a
// write is not recorded and a guest that sets a filter and reads it back gets the
// initial value instead of what it set. Unity caches its own texture state and has
// only made a single read so far, so nothing has needed the round trip yet. When
// something does, the fix is a per-name parameter block, not a wider default.
static void klgl_GetTexParameteriv(uint32_t target, uint32_t pname, int32_t *params) {
    (void)target;
    if (!params) return;
    switch (pname) {
    case 0x2800 /* TEXTURE_MAG_FILTER */:     params[0] = 0x2601 /* LINEAR */; return;
    case 0x2801 /* TEXTURE_MIN_FILTER */:     params[0] = 0x2702 /* NEAREST_MIPMAP_LINEAR */; return;
    case 0x2802 /* TEXTURE_WRAP_S */:
    case 0x2803 /* TEXTURE_WRAP_T */:
    case 0x8072 /* TEXTURE_WRAP_R */:         params[0] = 0x2901 /* REPEAT */; return;
    case 0x813C /* TEXTURE_BASE_LEVEL */:     params[0] = 0; return;
    case 0x813D /* TEXTURE_MAX_LEVEL */:      params[0] = 1000; return;
    case 0x884C /* TEXTURE_COMPARE_MODE */:   params[0] = 0 /* NONE */; return;
    case 0x884D /* TEXTURE_COMPARE_FUNC */:   params[0] = 0x0203 /* LEQUAL */; return;
    case 0x8E42 /* TEXTURE_SWIZZLE_R */:      params[0] = 0x1903 /* RED */; return;
    case 0x8E43 /* TEXTURE_SWIZZLE_G */:      params[0] = 0x1904 /* GREEN */; return;
    case 0x8E44 /* TEXTURE_SWIZZLE_B */:      params[0] = 0x1905 /* BLUE */; return;
    case 0x8E45 /* TEXTURE_SWIZZLE_A */:      params[0] = 0x1906 /* ALPHA */; return;
    case 0x90EA /* DEPTH_STENCIL_TEXTURE_MODE */: params[0] = 0x1902 /* DEPTH_COMPONENT */; return;
    // Nothing here is allocated with glTexStorage*, so no texture is immutable.
    case 0x912F /* TEXTURE_IMMUTABLE_FORMAT */: params[0] = 0; return;
    case 0x82DF /* TEXTURE_IMMUTABLE_LEVELS */: params[0] = 0; return;
    }
    fprintf(stderr, "  [gl] glGetTexParameteriv: unhandled pname 0x%x\n", pname);
    params[0] = 0;
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
//, and this is the only place they exist in plain text.
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

// Buffer objects with real backing store: a buffer owns storage sized by
// glBufferData, glBindBuffer tracks the current binding per target, and a map
// returns a pointer into the bound buffer's storage, so contents survive
// unmap/map cycles as the streaming paths require. A fresh malloc of exactly
// `length` per glMapBufferRange instead is overrun within a second — xzone
// malloc reports the freelist corruption at the NEXT map, so the crash site is
// klgl_MapBufferRange and the corruptor is the previous mapping's write.
#define KL_GL_MAX_BUFFERS 4096
#define KL_GL_MAX_BINDINGS 16
static struct { uint32_t name; uint8_t *data; size_t size; } g_gl_bufs[KL_GL_MAX_BUFFERS];
static unsigned g_gl_nbufs;
static struct { uint32_t target, name; } g_gl_binds[KL_GL_MAX_BINDINGS];
static unsigned g_gl_nbinds;

static uint32_t klgl_bound(uint32_t target) {
    for (unsigned i = 0; i < g_gl_nbinds; i++)
        if (g_gl_binds[i].target == target) return g_gl_binds[i].name;
    return 0;
}
static void klgl_BindBuffer(uint32_t target, uint32_t name) {
    for (unsigned i = 0; i < g_gl_nbinds; i++)
        if (g_gl_binds[i].target == target) { g_gl_binds[i].name = name; return; }
    if (g_gl_nbinds < KL_GL_MAX_BINDINGS) {
        g_gl_binds[g_gl_nbinds].target = target;
        g_gl_binds[g_gl_nbinds].name = name;
        g_gl_nbinds++;
    }
}
static typeof(g_gl_bufs[0]) *klgl_buf(uint32_t name) {
    for (unsigned i = 0; i < g_gl_nbufs; i++)
        if (g_gl_bufs[i].name == name) return &g_gl_bufs[i];
    if (!name || g_gl_nbufs == KL_GL_MAX_BUFFERS) return NULL;
    g_gl_bufs[g_gl_nbufs].name = name;
    g_gl_nbufs++;
    return &g_gl_bufs[g_gl_nbufs - 1];
}
static void klgl_BufferData(uint32_t target, intptr_t size, const void *data,
                            uint32_t usage) {
    (void)usage;
    typeof(g_gl_bufs[0]) *b = klgl_buf(klgl_bound(target));
    if (!b || size <= 0) return;
    uint8_t *p = realloc(b->data, (size_t)size);
    if (!p) return;
    b->data = p;
    b->size = (size_t)size;
    if (data) memcpy(b->data, data, (size_t)size);
}
static void klgl_BufferSubData(uint32_t target, intptr_t offset, intptr_t size,
                               const void *data) {
    typeof(g_gl_bufs[0]) *b = klgl_buf(klgl_bound(target));
    if (!b || !b->data || !data || offset < 0 || size < 0 ||
        (size_t)(offset + size) > b->size) return;
    memcpy(b->data + offset, data, (size_t)size);
}
static void *klgl_MapBufferRange(uint32_t target, intptr_t offset,
                                 intptr_t length, uint32_t access) {
    (void)access;
    typeof(g_gl_bufs[0]) *b = klgl_buf(klgl_bound(target));
    if (!b || offset < 0 || length <= 0) return NULL;
    if ((size_t)(offset + length) > b->size) {      // mapped before any data
        uint8_t *p = realloc(b->data, (size_t)(offset + length));
        if (!p) return NULL;
        b->data = p;
        b->size = (size_t)(offset + length);
    }
    return b->data + offset;
}
static uint8_t klgl_UnmapBuffer(uint32_t target) {
    (void)target;
    return 1;   // GL_TRUE: the storage is ours, nothing here can corrupt it
}

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
    // A synthetic location is a plausible-looking number that means nothing to a
    // real driver, so a guest that reaches this while ANGLE is running writes
    // one uniform into another's slot with no error until far away. Under the
    // null driver that is the whole arrangement and says nothing; under
    // KL_GLFB it is a bug, and this line should never print.
    static int said;
    if (kl_glfb_enabled() && said < 40) {
        said++;
        fprintf(stderr, "  [gl] null driver served glGetUniformLocation(program "
                        "%u, `%s`) -> synthetic %d, while ANGLE is running\n",
                program, name ? name : "(null)", g_gl_loc);
    }
    return g_gl_loc++;
}

// Sync objects. A fence marks a point in the command stream and the guest waits
// for the GPU to reach it; with no GPU behind this, that point is always already
// reached, so every wait reports ALREADY_SIGNALED and every query SIGNALED.
//
// The handle must be non-NULL and distinct: GLsync is an opaque pointer and 0 is
// "not a sync object", so a permissive zero would be a fence the guest could
// neither wait on nor delete. Values are from the guest's own GLES3 headers.
//
// glDeleteSync is already in the void list, which is correct — these handles are
// counters, not allocations, so there is nothing to release.
#define GL_ALREADY_SIGNALED 0x911A
#define GL_SIGNALED         0x9119
#define GL_SYNC_STATUS      0x9114
static uintptr_t g_gl_sync = 1;

static void *klgl_FenceSync(uint32_t condition, uint32_t flags) {
    (void)condition; (void)flags;
    return (void *)(g_gl_sync++);
}

static uint32_t klgl_ClientWaitSync(void *sync, uint32_t flags, uint64_t timeout) {
    (void)sync; (void)flags; (void)timeout;
    return GL_ALREADY_SIGNALED;     // nothing is ever outstanding
}

static void klgl_WaitSync(void *sync, uint32_t flags, uint64_t timeout) {
    (void)sync; (void)flags; (void)timeout;   // a server-side wait that returns at once
}

static void klgl_GetSynciv(void *sync, uint32_t pname, int32_t bufSize,
                           int32_t *length, int32_t *values) {
    (void)sync;
    if (length) *length = 0;
    if (!values || bufSize <= 0) return;
    if (pname == GL_SYNC_STATUS) { values[0] = GL_SIGNALED; if (length) *length = 1; return; }
    fprintf(stderr, "  [gl] glGetSynciv: unhandled pname 0x%x\n", pname);
    values[0] = 0;
}

// ---------------------------------------------------------------- texture dump
//
// There is no framebuffer to screenshot — nothing renders — but the pixels the
// guest *uploads* are real, and for a frame that is one textured fullscreen quad
// they are the frame's entire visible content. Dumping them is the only way to
// check that what the engine is feeding the pipeline is what we think it is,
// rather than inferring it from call counts.
//
// PNG rather than raw, so it can just be looked at. zlib is already linked (the
// APK is a zip), so this is a header, one deflate, and three CRCs.
#define GL_RGBA_FMT      0x1908
#define GL_RGB_FMT       0x1907
#define GL_UNSIGNED_BYTE 0x1401

static void png_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[4];
    png_be32(hdr, len);
    fwrite(hdr, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, (const Bytef *)data, len);
    png_be32(hdr, (uint32_t)crc);
    fwrite(hdr, 1, 4, f);
}

// channels is 3 or 4; `pixels` is tightly packed in GL upload order.
//
// The rows are emitted bottom-up. GL's texture origin is bottom-left and PNG's is
// top-left, so a straight copy comes out vertically mirrored — which is not a
// guess: the first dump produced Beat Saber's music-pack art with "MUSIC PACK /
// LINKIN PARK" upside down, which is what settled the orientation.
static int png_write(const char *path, const uint8_t *pixels,
                     int w, int h, int channels) {
    if (w <= 0 || h <= 0 || (channels != 3 && channels != 4)) return 0;
    size_t  stride = (size_t)w * channels;
    size_t  raw_n  = (stride + 1) * (size_t)h;      // one filter byte per row
    uint8_t *raw   = malloc(raw_n);
    if (!raw) return 0;
    for (int y = 0; y < h; y++) {
        raw[(stride + 1) * (size_t)y] = 0;          // filter type 0 (None)
        memcpy(raw + (stride + 1) * (size_t)y + 1,
               pixels + stride * (size_t)(h - 1 - y), stride);
    }
    uLongf comp_n = compressBound((uLong)raw_n);
    uint8_t *comp = malloc(comp_n);
    if (!comp || compress(comp, &comp_n, raw, (uLong)raw_n) != Z_OK) {
        free(raw); free(comp); return 0;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(comp); return 0; }
    static const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    png_be32(ihdr, (uint32_t)w);
    png_be32(ihdr + 4, (uint32_t)h);
    ihdr[8]  = 8;                                   // bit depth
    ihdr[9]  = channels == 4 ? 6 : 2;               // RGBA : RGB
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, sizeof ihdr);
    png_chunk(f, "IDAT", comp, (uint32_t)comp_n);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(comp);
    return 1;
}

static const char *g_tex_dir;
static unsigned    g_tex_n;

// glTexSubImage2D is otherwise a no-op; this is the one place the guest's own
// image data is visible. Only the uncompressed 8-bit paths are written — the
// compressed uploads (ASTC/ETC2, 335 of them) would need a decoder, and guessing
// at their block format would produce a convincing-looking wrong picture.
static void klgl_TexSubImage2D(uint32_t target, int32_t level, int32_t xoff, int32_t yoff,
                               int32_t w, int32_t h, uint32_t format, uint32_t type,
                               const void *pixels) {
    (void)target; (void)xoff; (void)yoff;
    int s = gl_slot("glTexSubImage2D");
    if (s >= 0) g_gl[s].calls++;
    if (!g_tex_dir || !pixels || level != 0) return;
    if (type != GL_UNSIGNED_BYTE) return;
    int ch = format == GL_RGBA_FMT ? 4 : format == GL_RGB_FMT ? 3 : 0;
    if (!ch) return;
    char path[512];
    snprintf(path, sizeof path, "%s/tex_%03u_%dx%d.png", g_tex_dir, g_tex_n, w, h);
    if (png_write(path, pixels, w, h, ch)) {
        fprintf(stderr, "  [gl] wrote %s (%dx%d, %d channels)\n", path, w, h, ch);
        g_tex_n++;
    }
}

void kl_egl_dump_textures(const char *dir) { g_tex_dir = dir; }
unsigned kl_egl_texture_count(void) { return g_tex_n; }

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
    {"glGetProgramInterfaceiv", (void *)klgl_GetProgramInterfaceiv},
    {"glGetProgramResourceiv", (void *)klgl_GetProgramResourceiv},
    {"glGetProgramResourceName", (void *)klgl_GetProgramResourceName},
    {"glGetTexParameteriv", (void *)klgl_GetTexParameteriv},
    {"glTexSubImage2D",  (void *)klgl_TexSubImage2D},
    {"glFenceSync",      (void *)klgl_FenceSync},
    {"glClientWaitSync", (void *)klgl_ClientWaitSync},
    {"glWaitSync",       (void *)klgl_WaitSync},
    {"glGetSynciv",      (void *)klgl_GetSynciv},
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
    {"glMapBufferRange", (void *)klgl_MapBufferRange},
    {"glUnmapBuffer",    (void *)klgl_UnmapBuffer},
    {"glBindBuffer",     (void *)klgl_BindBuffer},
    {"glBufferData",     (void *)klgl_BufferData},
    {"glBufferSubData",  (void *)klgl_BufferSubData},

};

// The state setters and the draws: every GL entry point the guest resolved that
// returns void and whose only effect is on state this driver does not model. They
// share klgl_noop.
//
// This is the one place the "only implement what is forced" rule is applied to a
// *class* rather than a call, and the justification is the same as for the
// capability set above: there is nothing to invent. A void function that writes no
// out-parameter has no answer to get wrong, so doing nothing is the whole of the
// correct behaviour for a driver that does not draw. Contrast the 27 entry points
// deliberately left out — glCheckFramebufferStatus, glMapBufferRange, glIsEnabled,
// the glGet* family — each of which hands the guest a value it will act on, and so
// each of which still aborts by name until the trace shows what it needs.
//
// The list is exactly what the guest resolved, not the GLES 3.2 header. A name
// that shows up later aborts by name, which is a ten-second fix and a fact
// recorded, rather than a guess baked in ahead of the trace.
static const char *const g_gl_void[] = {
    // per-fragment and rasteriser state
    "glEnable", "glDisable", "glCullFace", "glFrontFace", "glDepthFunc", "glDepthMask",
    "glColorMask", "glColorMaski", "glStencilMask", "glStencilFuncSeparate",
    "glStencilOpSeparate", "glPolygonOffset", "glScissor", "glViewport", "glPixelStorei",
    // glPolygonMode is desktop-GL only (no GLES equivalent); gl4es passes it
    // through when the guest asks for it. GL_FILL is the default and only mode a
    // null/GLES driver can honour, so doing nothing is correct — wireframe
    // (GL_LINE) is the sole behaviour lost, which no gameplay path needs.
    "glPolygonMode",
    // glTexImage1D is desktop-GL only; gl4es passes it through when it can't fold
    // a 1D texture into a 2D one. A null/GLES driver has no 1D texture to fill, so
    // this is a no-op — the 1D texture stays unpopulated (a colour ramp / lookup
    // that samples black), which is a visual gap, not a crash.
    "glTexImage1D",
    "glBlendEquation", "glBlendEquationi", "glBlendEquationSeparate",
    "glBlendEquationSeparatei", "glBlendFuncSeparate", "glBlendFuncSeparatei",
    "glBlendBarrier",
    // clears
    "glClear", "glClearColor", "glClearDepthf", "glClearStencil",
    "glClearBufferfi", "glClearBufferfv", "glClearBufferuiv",
    // framebuffers and renderbuffers
    "glBindFramebuffer", "glBindRenderbuffer", "glFramebufferRenderbuffer",
    "glFramebufferTexture", "glFramebufferTexture2D", "glFramebufferTexture3D",
    "glFramebufferTextureLayer", "glRenderbufferStorage",
    "glRenderbufferStorageMultisample", "glInvalidateFramebuffer", "glBlitFramebuffer",
    "glDrawBuffers", "glReadBuffer",
    // buffers
    "glBindBuffer", "glBindBufferBase", "glBindBufferRange", "glBufferData",
    "glBufferSubData", "glCopyBufferSubData", "glFlushMappedBufferRange",
    // textures and samplers
    "glActiveTexture", "glBindTexture", "glBindSampler", "glBindImageTexture",
    "glSamplerParameteri", "glTexParameterf", "glTexParameteri", "glTexParameteriv",
    "glTexImage2D", "glTexImage3D", "glTexImage2DMultisample",
    "glTexStorage2D", "glTexStorage3D", "glTexStorage2DMultisample",
    "glTexStorage3DMultisample", "glTexSubImage3D",   // glTexSubImage2D has a real impl (texture dump)
    "glCompressedTexImage2D", "glCompressedTexSubImage2D", "glCompressedTexSubImage3D",
    "glCopyTexImage2D", "glCopyTexSubImage2D", "glCopyImageSubData", "glTexBuffer",
    "glGenerateMipmap",
    // shaders and programs
    "glCompileShader", "glAttachShader", "glDetachShader", "glDeleteShader",
    "glLinkProgram", "glUseProgram", "glValidateProgram", "glDeleteProgram",
    "glBindAttribLocation", "glProgramParameteri", "glProgramBinary",
    "glTransformFeedbackVaryings",
    // uniforms — the values are handed to a program that will never run
    "glUniform1i", "glUniform1fv", "glUniform2fv", "glUniform3fv", "glUniform4fv",
    "glUniform1iv", "glUniform2iv", "glUniform3iv", "glUniform4iv",
    "glUniform1uiv", "glUniform2uiv", "glUniform3uiv", "glUniform4uiv",
    "glUniformMatrix3fv", "glUniformMatrix4fv", "glUniformBlockBinding",
    "glProgramUniform1fv", "glProgramUniform2fv", "glProgramUniform3fv",
    "glProgramUniform4fv", "glProgramUniform1iv", "glProgramUniform2iv",
    "glProgramUniform3iv", "glProgramUniform4iv", "glProgramUniform1uiv",
    "glProgramUniform2uiv", "glProgramUniform3uiv", "glProgramUniform4uiv",
    "glProgramUniformMatrix2fv", "glProgramUniformMatrix3fv", "glProgramUniformMatrix4fv",
    "glProgramUniformMatrix2x3fv", "glProgramUniformMatrix3x2fv",
    "glProgramUniformMatrix2x4fv", "glProgramUniformMatrix4x2fv",
    "glProgramUniformMatrix3x4fv", "glProgramUniformMatrix4x3fv",
    // vertex state
    "glBindVertexArray", "glVertexAttribPointer", "glVertexAttribIPointer",
    "glEnableVertexAttribArray", "glDisableVertexAttribArray",
    "glVertexAttrib4f", "glVertexAttrib4fv",
    // the draws — the calls this whole milestone exists to reach
    "glDrawArrays", "glDrawArraysInstanced", "glDrawArraysIndirect",
    "glDrawElements", "glDrawElementsInstanced", "glDrawElementsBaseVertex",
    "glDrawElementsInstancedBaseVertex", "glDrawElementsIndirect",
    "glDispatchCompute", "glDispatchComputeIndirect", "glMemoryBarrier",
    "glPatchParameteri",
    // queries, transform feedback, sync
    "glBeginQuery", "glEndQuery", "glBeginTransformFeedback", "glEndTransformFeedback",
    "glBindTransformFeedback", "glDeleteSync",
    // submission — nothing is queued, so nothing has to be waited on
    "glFlush", "glFinish",
    // debug output
    "glDebugMessageCallback", "glDebugMessageControl", "glDebugMessageInsert",
    "glObjectLabel", "glPushDebugGroup", "glPopDebugGroup",
};

static int gl_is_void(const char *name) {
    for (size_t i = 0; i < sizeof g_gl_void / sizeof g_gl_void[0]; i++)
        if (strcmp(g_gl_void[i], name) == 0) return 1;
    return 0;
}

// ---------- EGL ----------
// ---------- KL_EGL_TRACE: which entry points the guest actually REACHES ----------
//
// There was no way to see this, and the gap has a shape: every EGL symbol binds
// at LOAD time (libEGL.so is a DT_NEEDED of libunity), so "resolved" says
// nothing about "called" — and a guest that fails a capability check *before*
// it ever calls EGL is indistinguishable, from every log this project prints,
// from a guest whose call we answered wrongly. BONELAB's `gles-api-check` is
// exactly that question, and reasoning about it from the disassembly is how an
// afternoon goes.
//
// Off by default; it is a census, not a diagnostic, so it prints the call and
// its interesting arguments and nothing about what we decided.
static uint64_t klegl_tid(void) {
    uint64_t t = 0; pthread_threadid_np(NULL, &t); return t;
}

static int klegl_tracing(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_EGL_TRACE", 0);
    return on;
}
#define KLEGL_TRACE(...) do { if (klegl_tracing()) { \
        fprintf(stderr, "  [egl] "); fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); fflush(stderr); } } while (0)

static EGLDisplay klegl_GetDisplay(void *native) {
    (void)native;
    KLEGL_TRACE("eglGetDisplay(%p) -> %p", native, (void *)DISPLAY);
    return DISPLAY;
}

static unsigned klegl_Initialize(EGLDisplay dpy, int32_t *major, int32_t *minor) {
    KLEGL_TRACE("eglInitialize(%p)", (void *)dpy);
    if (dpy != DISPLAY) { g_error = EGL_BAD_DISPLAY; return EGL_FALSE; }
    g_display.initialised = 1;
    if (major) *major = 1;
    if (minor) *minor = 5;
    return EGL_TRUE;
}

static unsigned klegl_Terminate(EGLDisplay dpy) {
    (void)dpy; g_display.initialised = 0; return EGL_TRUE;
}

// EGL_EXTENSIONS names exactly the extensions this runtime implements entry
// points (or, for the last, the attributes) for, and nothing else. Every name here is a promise, and Unity
// branches hard on this string — advertising one we do not serve buys a failure
// much further from its cause, which is why the list is the implemented set
// rather than a plausible one:
//
//   EGL_KHR_image_base                    eglCreateImageKHR / eglDestroyImageKHR
//   EGL_ANDROID_image_native_buffer       ...and EGL_NATIVE_BUFFER_ANDROID, the
//                                         only image target we accept
//   EGL_ANDROID_get_native_client_buffer  eglGetNativeClientBufferANDROID
//   EGL_KHR_create_context                no new entry point — it is the
//                                         eglCreateContext ATTRIBUTE set, and
//                                         the one that matters
//                                         (EGL_CONTEXT_MAJOR_VERSION_KHR) is
//                                         token 0x3098, the same number as
//                                         EGL_CONTEXT_CLIENT_VERSION, which
//                                         klegl_CreateContext already reads.
//
// EGL_KHR_create_context is not optional for an SDL2 guest. SDL_EGL_
// CreateContext will not ask for a GLES 3 context on a display that does not
// advertise it: it refuses BEFORE calling eglCreateContext ("Could not create
// EGL context (context attributes are not supported)"). The guest then runs on
// with no context at all — our GL entry points answer glGetString without one,
// so its renderer banner looks healthy — creates its XR session against context
// 0, and submits stereo layers from swapchain textures nothing ever drew into:
// an all-black immersive space with no crash and no complaint. (A GLES 2 SDL
// guest never hits it; that is the branch SDL takes without the extension.)
//
// The empty string is NOT the safe answer, legal as it is: AVPro Video's
// OpenGLESPlayerRenderer::setupEglContext strdup()s this string and walks it
// with strtok in a BOTTOM-tested loop — the first token is logged and strlen'd
// before the NULL check that ends the loop — so an empty list is a strlen(NULL)
// on the boot path. No conformant Android driver gives one. The symptom names
// EGL only in the guest's own last log line ("- (null)"); the frame it dies on
// is _platform_strlen.
//
// The empty string is one env-var away, to A/B against a guest that branches on
// a name here.
static const char *klegl_extensions(void) {
    static const char *s;
    if (!s) s = kl_env_str("KL_EGL_EXTENSIONS",
                           "EGL_KHR_image_base "
                           "EGL_ANDROID_image_native_buffer "
                           "EGL_ANDROID_get_native_client_buffer "
                           "EGL_KHR_create_context");
    return s;
}

static const char *klegl_QueryString(EGLDisplay dpy, int32_t name) {
    (void)dpy;
    KLEGL_TRACE("eglQueryString(%#x)", name);
    switch (name) {
    case EGL_VENDOR:  return "Klepton";
    case EGL_VERSION: return "1.5 Klepton";
    case EGL_EXTENSIONS:  return klegl_extensions();
    case EGL_CLIENT_APIS: return "OpenGL_ES";
    }
    g_error = EGL_BAD_PARAMETER;
    return NULL;
}

static unsigned klegl_ChooseConfig(EGLDisplay dpy, const int32_t *attribs,
                                   EGLConfig *configs, int32_t size, int32_t *num) {
    (void)dpy;
    KLEGL_TRACE("eglChooseConfig(size=%d)", size);
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

// The other way round from eglChooseConfig: hand over the whole list and let
// the caller filter it itself with eglGetConfigAttrib. libvrlink_scene does it
// this way rather than describing what it wants in an attribute list, so both
// entry points have to agree about what configs exist. They read the same
// g_configs.
//
// Passing configs == NULL is the standard "how many are there?" query, and it
// must not be confused with a zero-sized buffer.
static unsigned klegl_GetConfigs(EGLDisplay dpy, EGLConfig *configs,
                                 int32_t size, int32_t *num) {
    KLEGL_TRACE("eglGetConfigs");
    (void)dpy;
    if (!num) { g_error = EGL_BAD_PARAMETER; return EGL_FALSE; }
    if (!configs) { *num = NCONFIGS; return EGL_TRUE; }
    int n = size < NCONFIGS ? size : NCONFIGS;
    for (int i = 0; i < n; i++) configs[i] = (EGLConfig)&g_configs[i];
    *num = n;
    return EGL_TRUE;
}

static unsigned klegl_GetConfigAttrib(EGLDisplay dpy, EGLConfig cfg,
                                      int32_t attr, int32_t *value) {
    KLEGL_TRACE("eglGetConfigAttrib");
    (void)dpy;
    if (!value) { g_error = EGL_BAD_PARAMETER; return EGL_FALSE; }
    const typeof(g_configs[0]) *c = cfg ? (const typeof(g_configs[0]) *)cfg : &g_configs[0];
    int32_t w, h;
    kl_ndk_window_size(NULL, &w, &h);
    switch (attr) {
    case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE:
    case EGL_ALPHA_SIZE:        *value = 8; break;
    case EGL_BUFFER_SIZE:       *value = 32; break;
    case EGL_DEPTH_SIZE:        *value = c->depth; break;
    case EGL_STENCIL_SIZE:      *value = c->stencil; break;
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
    s->w = w; s->h = h; s->pbuffer = pbuffer; s->xr_swapchain = NULL;
    return (EGLSurface)s;
}

static EGLSurface klegl_CreateWindowSurface(EGLDisplay dpy, EGLConfig cfg,
                                            void *win, const int32_t *attribs) {
    KLEGL_TRACE("eglCreateWindowSurface");
    (void)dpy; (void)cfg; (void)attribs;
    int32_t w, h;
    kl_ndk_window_size(win, &w, &h);   // the size Unity will build its target from
    fprintf(stderr, "  [egl] window surface %dx%d\n", w, h);
    // A guest asking for a window surface has a flat picture to put in it. That
    // is only half the story for a VR guest — Unity creates one of these too,
    // because it is an Android app — so kl_present treats an eye texture as the
    // stronger signal and this as the fallback. See kl_present.h.
    kl_present_note_window_surface(w, h);
    // ...and under the real GL path, ANGLE's own surface has to be at least this
    // big or the guest draws into a target smaller than the viewport it set. The
    // setter is a no-op once ANGLE is up, and honours KL_GLFB_SIZE, so this
    // neither fights an explicit override nor resizes a live surface.
    kl_glfb_set_size(w, h);
    EGLSurface es = new_surface(w, h, 0);
    // XR_KHR_android_surface_swapchain: if this window came from
    // ANativeWindow_fromSurface on an XR game-surface, bind the EGL surface to
    // that swapchain. eglSwapBuffers then presents the guest's frame INTO the
    // swapchain image (see klegl_SwapBuffers) rather than only into the flat
    // door — which is what routes a guest's Android-surface screen to its VR quad layers.
    if (es) {
        void *owner = kl_ndk_window_owner(win);
        void *sc = owner ? kl_xr_swapchain_for_surface(owner) : NULL;
        if (sc) {
            ((kl_egl_surface *)es)->xr_swapchain = sc;
            fprintf(stderr, "  [egl] window surface %dx%d is an XR game surface "
                            "-> bound to swapchain %p; eglSwapBuffers presents it\n",
                    w, h, sc);
        }
    }
    return es;
}

static EGLSurface klegl_CreatePbufferSurface(EGLDisplay dpy, EGLConfig cfg,
                                             const int32_t *attribs) {
    KLEGL_TRACE("eglCreatePbufferSurface");
    (void)dpy; (void)cfg;
    int32_t w = 1, h = 1;
    for (const int32_t *a = attribs; a && *a != EGL_NONE; a += 2) {
        if (a[0] == EGL_WIDTH)  w = a[1];
        if (a[0] == EGL_HEIGHT) h = a[1];
    }
    return new_surface(w, h, 1);
}

static unsigned klegl_DestroySurface(EGLDisplay dpy, EGLSurface s) {
    KLEGL_TRACE("eglDestroySurface");
    (void)dpy; (void)s; return EGL_TRUE;      // storage is never reclaimed
}

static unsigned klegl_QuerySurface(EGLDisplay dpy, EGLSurface surf,
                                   int32_t attr, int32_t *value) {
    KLEGL_TRACE("eglQuerySurface");
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

// eglSurfaceAttrib — the setter whose getter is right above. BONELAB's libunity
// imports it (it was the only unresolved EGL name in that guest), and an
// unresolved import is an abort BY NAME the first time it is called, so a guest
// that merely states a preference about its back buffer would have died in it.
//
// EGL_SWAP_BEHAVIOR is the one that carries meaning here and it is ACCEPTED
// rather than acted on: our surfaces are destroyed-on-swap already, which is
// what klegl_QuerySurface has always answered, so a guest asking for that is
// asking for what it has. Asking to PRESERVE the buffer is refused rather than
// silently agreed with — that is a promise about contents surviving a swap, and
// nothing here keeps it.
static unsigned klegl_SurfaceAttrib(EGLDisplay dpy, EGLSurface surf,
                                    int32_t attr, int32_t value) {
    KLEGL_TRACE("eglSurfaceAttrib(%#x, %d)", attr, value);
    (void)dpy;
    if (!surf) { g_error = EGL_BAD_PARAMETER; return EGL_FALSE; }
    switch (attr) {
    case EGL_SWAP_BEHAVIOR:
        if (value == EGL_BUFFER_DESTROYED) return EGL_TRUE;
        fprintf(stderr, "  [egl] eglSurfaceAttrib: EGL_SWAP_BEHAVIOR %#x is not "
                        "EGL_BUFFER_DESTROYED — refusing rather than promising "
                        "the back buffer survives a swap\n", value);
        g_error = 0x3004 /* EGL_BAD_ATTRIBUTE */;
        return EGL_FALSE;
    default:
        fprintf(stderr, "  [egl] eglSurfaceAttrib: unhandled attribute 0x%x\n", attr);
        g_error = 0x3004;
        return EGL_FALSE;
    }
}

static EGLContext klegl_CreateContext(EGLDisplay dpy, EGLConfig cfg,
                                      EGLContext share, const int32_t *attribs) {
    KLEGL_TRACE("eglCreateContext");
    (void)dpy; (void)share;
    if (g_nctx >= sizeof g_contexts / sizeof g_contexts[0]) return NULL;
    kl_egl_context *c = &g_contexts[g_nctx++];
    c->client_version = 2;
    // Remember which config this context was created against so eglQueryContext
    // can answer EGL_CONFIG_ID with an id that round-trips through eglGetConfigs
    // + eglGetConfigAttrib. HL2Q3VR's XR-graphics-binding matches the two to find
    // the EGLConfig for XrGraphicsBindingOpenGLESAndroidKHR; without a real id it
    // finds no config and refuses the session.
    c->config_id = cfg ? ((const typeof(g_configs[0]) *)cfg)->id : g_configs[0].id;
    // 0x3098 is EGL_CONTEXT_CLIENT_VERSION and EGL_CONTEXT_MAJOR_VERSION_KHR —
    // one token under two names. The minor version is read for the log line
    // only: ANGLE hands kl_glfb whatever ES 3.x context it has. The rest of the
    // KHR list (flags, profile mask, robustness) is ignored on purpose.
    int minor = 0;
    for (const int32_t *a = attribs; a && *a != EGL_NONE; a += 2) {
        if (a[0] == 0x3098 /* EGL_CONTEXT_CLIENT_VERSION / MAJOR_VERSION_KHR */)
            c->client_version = a[1];
        else if (a[0] == 0x30FB /* EGL_CONTEXT_MINOR_VERSION_KHR */)
            minor = a[1];
    }
    fprintf(stderr, "  [egl] context %u = %p, GLES %d.%d (created on t%llu)\n",
            g_nctx - 1, (void *)c, c->client_version, minor, klegl_tid());
    return (EGLContext)c;
}

// eglQueryContext — core EGL 1.0, and the getter for what eglCreateContext was
// told. VRChat's AVPro asks; nothing before it did, so it was an abort by name.
//
// Three of the four attributes are answerable from state we keep or from facts
// about this runtime, and the fourth is refused rather than guessed:
//
//   EGL_CONTEXT_CLIENT_VERSION  the attribute the guest itself passed in
//   EGL_CONTEXT_CLIENT_TYPE     always EGL_OPENGL_ES_API — klegl_BindAPI serves
//                               no other, so this cannot be anything else
//   EGL_RENDER_BUFFER           EGL_BACK_BUFFER. Every surface here is a window
//                               or pbuffer we double-buffer; a single-buffered
//                               answer would be a promise that a draw is visible
//                               without a swap.
//
// EGL_CONFIG_ID is the refusal: configs here are two fixed picks (see
// klegl_ChooseConfig) and are not identified by id anywhere, so an id we
// invented would not round-trip through eglGetConfigs — and a guest that used
// it to re-find its config would get the wrong one, silently.
#define EGL_OPENGL_ES_API         0x30A0   // also klegl_BindAPI, below
#define EGL_CONFIG_ID             0x3028
#define EGL_CONTEXT_CLIENT_TYPE   0x3097
#define EGL_CONTEXT_CLIENT_VER    0x3098
#define EGL_RENDER_BUFFER         0x3086
#define EGL_BACK_BUFFER           0x3084

static unsigned klegl_QueryContext(EGLDisplay dpy, EGLContext ctx,
                                   int32_t attr, int32_t *value) {
    KLEGL_TRACE("eglQueryContext(%#x)", attr);
    (void)dpy;
    const kl_egl_context *c = (const kl_egl_context *)ctx;
    if (!c || !value) { g_error = EGL_BAD_PARAMETER; return EGL_FALSE; }
    switch (attr) {
    case EGL_CONTEXT_CLIENT_VER:  *value = c->client_version; break;
    case EGL_CONTEXT_CLIENT_TYPE: *value = EGL_OPENGL_ES_API; break;
    case EGL_RENDER_BUFFER:       *value = EGL_BACK_BUFFER;   break;
    // The config the context was created against (0 for the pre-tracking default,
    // which still round-trips: g_configs[0].id). Now answerable because the
    // context remembers its config — see klegl_CreateContext. HL2Q3VR needs it to
    // build its XR OpenGL-ES graphics binding.
    case EGL_CONFIG_ID:           *value = c->config_id ? c->config_id
                                                        : g_configs[0].id; break;
    default:
        fprintf(stderr, "  [egl] eglQueryContext: unhandled attribute 0x%x\n", attr);
        g_error = 0x3004 /* EGL_BAD_ATTRIBUTE */;
        return EGL_FALSE;
    }
    return EGL_TRUE;
}

static unsigned klegl_DestroyContext(EGLDisplay dpy, EGLContext c) {
    KLEGL_TRACE("eglDestroyContext");
    // Only this thread's binding can be cleared here: a context current on
    // ANOTHER thread stays current until that thread releases it, which is what
    // EGL's deferred destruction says too.
    (void)dpy; if (c == g_current) g_current = NULL; return EGL_TRUE;
}

static unsigned klegl_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                  EGLSurface read, EGLContext ctx) {
    KLEGL_TRACE("eglMakeCurrent");
    // Which context is current on which thread is a fact a GL trace cannot
    // recover afterwards, and a guest that keys FBO ownership on it (Unity does)
    // fails in a way that names neither. Rare enough to report unconditionally:
    // a handful of contexts and one line per change, per thread.
    if (ctx != g_current)
        fprintf(stderr, "  [egl] t%llu current context %p -> %p\n",
                klegl_tid(), (void *)g_current, (void *)ctx);
    (void)dpy; g_draw = draw; g_read = read; g_current = ctx;
    // Remember the last context that is one of ours (a g_contexts[] slot) so the
    // hl2 sticky path below can answer with it after a release. The startup-video
    // teardown makes a bogus (EGLContext)0x1 current for an instant; excluding
    // anything outside the array keeps that out of the sticky value.
    if (ctx && (kl_egl_context *)ctx >= g_contexts &&
        (kl_egl_context *)ctx < g_contexts + (sizeof g_contexts / sizeof g_contexts[0])) {
        g_last_real = ctx;
        g_last_real_any = ctx;   // cross-thread sticky for the cinema eye-copy
    }
    // Whichever thread this is, it is the one that will now issue GL — or, when
    // the guest releases the context (NULL), the one giving it up; migration
    // mode needs both halves.
    if (kl_glfb_enabled()) {
        if (ctx) kl_glfb_make_current();
        else     kl_glfb_release_current();
    }
    return EGL_TRUE;
}

static EGLContext klegl_GetCurrentContext(void) {
    KLEGL_TRACE("eglGetCurrentContext");
    // Resolve the answer FIRST — including the hl2 sticky restore — so the
    // log-dedup below cannot short-circuit it (it used to `return g_current`
    // early for a repeated call site, which bypassed the sticky path entirely and
    // is why the cinema eye-copy kept seeing 0x0). Per-thread g_last_real first;
    // fall back to the process-wide last real context so a submission thread that
    // never held one (the cinema eye-copy) still gets a valid context instead of
    // the guest's broken context-less path.
    EGLContext ans = g_current;
    if (!g_current && klegl_sticky_ctx()) {
        EGLContext sticky = g_last_real ? g_last_real : g_last_real_any;
        if (sticky) ans = sticky;
    }
    // A NULL answer is the interesting one and it is not an error: it means the
    // asking thread holds no context. Unity latches this into the "active
    // context" it keys framebuffer-object ownership on, and two of its three
    // writers store the answer UNCONDITIONALLY — a NULL there becomes a sentinel
    // that never matches any framebuffer again. Named by call site + answer, once
    // each (dedup only — never gates the return value).
    {
        static struct { const void *ret; const void *ctx; } said[16];
        static unsigned nsaid;
        const void *ret = __builtin_return_address(0);
        unsigned n = nsaid, seen = 0;
        for (unsigned i = 0; i < n && i < 16; i++)
            if (said[i].ret == ret && said[i].ctx == ans) { seen = 1; break; }
        if (!seen && n < 16) {
            said[n].ret = ret; said[n].ctx = ans; nsaid = n + 1;
            size_t off = 0; const char *img = kl_addr_image((void *)ret, &off);
            fprintf(stderr, "  [egl] t%llu eglGetCurrentContext -> %p <- %s+0x%zx%s\n",
                    klegl_tid(), (void *)ans, img ? img : "?", off,
                    ans != g_current ? " (sticky)" : "");
        }
    }
    return ans;
}

void *kl_egl_current_context(void) { return (void *)g_current; }
static EGLDisplay klegl_GetCurrentDisplay(void) {
    KLEGL_TRACE("eglGetCurrentDisplay");
    // The EGL 1.5 companion to the two above: the display for the current
    // context, or EGL_NO_DISPLAY when nothing is current — that is the spec's
    // answer for a thread with no context, and it is what 1.40's
    // libOculusXRPlugin asks for as it spins up its own EGL context.
    if (g_current) return DISPLAY;
    // hl2 sticky (see g_last_real): the XR-binding check reads display AND context
    // and bails if either is NULL, so the released-context restore must cover both.
    // Process-wide fallback too, so the cinema eye-copy thread's display matches the
    // context restored in klegl_GetCurrentContext.
    if ((g_last_real || g_last_real_any) && klegl_sticky_ctx()) return DISPLAY;
    return (EGLDisplay)0;   /* EGL_NO_DISPLAY */
}
static EGLSurface klegl_GetCurrentSurface(int32_t which) {
    KLEGL_TRACE("eglGetCurrentSurface");
    return which == EGL_READ ? g_read : g_draw;
}

// How many times the guest has presented through EGL. Read by the frame-complete
// paths that are the ONLY presentation signal some guests give: Beat Saber 1.28's
// legacy VRDevice swaps, and 1.40's XR-SDK display provider does not (measured:
// `eglSwapBuffers: 0` across a whole 58-frame XR run), exactly as an OpenXR guest
// does not. A capture seam that hangs off the swap alone is silently inert there,
// and the tell is a run with a live frame loop and an empty output directory.
unsigned long kl_egl_swap_count(void) { return g_frames; }

static unsigned klegl_SwapBuffers(EGLDisplay dpy, EGLSurface s) {
    KLEGL_TRACE("eglSwapBuffers");
    (void)dpy;
    g_frames++;
    // XR_KHR_android_surface_swapchain: for a swapchain-bound window this swap is
    // the "release" — the finished frame is in the default framebuffer right now
    // and the guest's context is current, the one moment a GL copy into the
    // swapchain image is legal. Do it BEFORE kl_glfb_present's own FBO-0 capture
    // below so both read the same finished frame.
    {
        kl_egl_surface *swp = (kl_egl_surface *)s;
        if (swp && swp->xr_swapchain)
            kl_xr_android_surface_present(swp->xr_swapchain);
    }
    // The GL object census, on the frame clock: a class whose live count climbs
    // at every loading transition is the leak, and a flat one exonerates the GL
    // path. No-op unless KL_GL_CENSUS names an interval (kl_glfb.h).
    {
        static int every = -1;
        if (every < 0) every = kl_env_int("KL_GL_CENSUS", 0);
        if (every > 0 && g_frames % (unsigned long)every == 0)
            kl_glfb_gl_census(stderr);
    }
    // A swap is where a frame is finished, so it is where the reference renderer
    // captures. No-op unless KL_GLFB_OUT names a directory — or a frontend is
    // attached: a frame sink (the readback path) or a GPU fence (KL_VIEW=1's
    // hardware compositor, which wants the *timing* of the swap and none of
    // its pixels).
    const char *out = kl_env_str("KL_GLFB_OUT", NULL);
    if (kl_glfb_enabled() &&
        (out || kl_glfb_has_frame_sink() || kl_glfb_has_gpu_fence()))
        kl_glfb_present(out);
    return EGL_TRUE;
}

static unsigned klegl_SwapInterval(EGLDisplay dpy, int32_t interval) {
    KLEGL_TRACE("eglSwapInterval");
    (void)dpy; (void)interval; return EGL_TRUE;
}

// ---------------------------------------------------------------------------
// The decoded-video image. Three entry points, and between them they are
// the whole path from a frame VideoToolbox produced to a texture the guest's
// shader samples.
//
// This half is only the ABI: what an AHardwareBuffer is (kl_mediandk.h — a
// CVPixelBuffer) and what an EGLImage becomes (kl_glfb.c — an IOSurface pbuffer)
// are the other two files' business, and neither is anything EGL has a name for.
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#define EGL_NO_IMAGE_KHR          ((void *)0)

// The identity function, with a check. Android's contract is that this hands
// back an opaque EGLClientBuffer for a buffer we recognise, and a buffer we do
// not recognise has no pixels we could ever find — so answer NULL rather than
// let it reach eglCreateImageKHR as something that looks valid.
static void *klegl_GetNativeClientBufferANDROID(void *ahb) {
    KLEGL_TRACE("eglGetNativeClientBufferANDROID");
    if (!kl_mediandk_buffer_pixels(ahb)) {
        fprintf(stderr, "  [egl] eglGetNativeClientBufferANDROID(%p): not one of our "
                        "AHardwareBuffers — nothing else allocates them here\n", ahb);
        g_error = EGL_BAD_PARAMETER;
        return NULL;
    }
    return ahb;
}

static void *klegl_CreateImageKHR(EGLDisplay dpy, void *ctx, uint32_t target,
                                  void *buffer, const int32_t *attrs) {
    KLEGL_TRACE("eglCreateImageKHR");
    (void)dpy; (void)ctx;
    if (target != EGL_NATIVE_BUFFER_ANDROID) {
        fprintf(stderr, "  [egl] eglCreateImageKHR target 0x%x is not "
                        "EGL_NATIVE_BUFFER_ANDROID — the only source of images here "
                        "is the video decoder\n", target);
        g_error = EGL_BAD_PARAMETER;
        return EGL_NO_IMAGE_KHR;
    }
    void *pixels = kl_mediandk_buffer_pixels(buffer);
    if (!pixels) {
        fprintf(stderr, "  [egl] eglCreateImageKHR: client buffer %p is not a decoded "
                        "frame\n", buffer);
        g_error = EGL_BAD_PARAMETER;
        return EGL_NO_IMAGE_KHR;
    }
    // Every attribute is reported once rather than ignored silently. The measured
    // list is empty; EGL_IMAGE_PRESERVED_KHR would be the one that means
    // something, and it is what our pbuffer does anyway (we never render into it).
    if (attrs && attrs[0] != EGL_NONE) {
        static int said;
        if (!said++) {
            fprintf(stderr, "  [egl] eglCreateImageKHR attributes (ignored):");
            for (int i = 0; attrs[i] != EGL_NONE; i += 2)
                fprintf(stderr, " 0x%x=0x%x", attrs[i], attrs[i + 1]);
            fprintf(stderr, "\n");
        }
    }
    int w = 0, h = 0;
    void *img = kl_glfb_image_from_pixels(pixels, &w, &h);
    if (!img) { g_error = EGL_BAD_PARAMETER; return EGL_NO_IMAGE_KHR; }
    return img;
}

static unsigned klegl_DestroyImageKHR(EGLDisplay dpy, void *image) {
    KLEGL_TRACE("eglDestroyImageKHR");
    (void)dpy;
    if (!kl_glfb_is_image(image)) {
        fprintf(stderr, "  [egl] eglDestroyImageKHR(%p): not one of ours\n", image);
        g_error = EGL_BAD_PARAMETER;
        return EGL_FALSE;
    }
    kl_glfb_image_destroy(image);
    return EGL_TRUE;
}

// SDL3 calls this before creating a context; Unity never did, because it only
// ever wanted the default. EGL_OPENGL_ES_API (0x30A0) is that default and is the
// only API this runtime serves — there is no desktop GL and no OpenVG behind us.
// Refusing anything else is the honest answer and keeps a guest that wanted a
// different API from proceeding as though it had got one.
static unsigned klegl_BindAPI(unsigned api) {
    KLEGL_TRACE("eglBindAPI");
    if (api == EGL_OPENGL_ES_API) return EGL_TRUE;
    fprintf(stderr, "  [egl] eglBindAPI(0x%x): only EGL_OPENGL_ES_API is served\n", api);
    g_error = EGL_BAD_PARAMETER;
    return EGL_FALSE;
}
static unsigned klegl_QueryAPI(void) { return EGL_OPENGL_ES_API; }

static int32_t klegl_GetError(void) {
    KLEGL_TRACE("eglGetError"); int e = g_error; g_error = EGL_SUCCESS; return e; }

// The gateway. Never fails, never aborts — see the header comment.
void *kl_egl_sym(const char *name) {
    if (!name) return NULL;
    void *own = kl_egl_lookup(name);              // eglXxx resolved through here too
    if (own) return own;
    // With KL_GLFB=1 the host's real GL answers everything except the capability
    // queries, which stay ours so the guest keeps believing it drives GLES 3.2.
    // Still slotted, so the report counts what was resolved either way.
    void *host = kl_glfb_sym(name);
    if (host) {
        int hs = gl_slot(name);
        if (hs >= 0) g_gl[hs].resolved = 1;
        return host;
    }
    for (size_t i = 0; i < sizeof g_gl_impl / sizeof g_gl_impl[0]; i++)
        if (strcmp(g_gl_impl[i].name, name) == 0) return g_gl_impl[i].fn;
    // Desktop-GL suffix aliases: the ARB/EXT/OES/KHR name of a function that was
    // promoted to core is the SAME entry point (the buffer-object family —
    // glGenBuffersARB etc. — is exactly glGenBuffers). gl4es and the Source/Xash
    // GL compat shims call the old names. Retry the base name against the real
    // backends (host GL, our egl/gl impls) before giving up to a stub; only take
    // it if the base actually resolves, so a genuinely-absent call still aborts by
    // its real name.
    {
        static const char *const sfx[] = { "ARB", "EXT", "OES", "KHR" };
        size_t len = strlen(name);
        for (size_t i = 0; i < sizeof sfx / sizeof sfx[0]; i++) {
            size_t sl = strlen(sfx[i]);
            if (len <= sl || strcmp(name + len - sl, sfx[i]) != 0) continue;
            char base[128];
            if (len - sl >= sizeof base) break;
            snprintf(base, sizeof base, "%.*s", (int)(len - sl), name);
            void *b = kl_egl_lookup(base);
            if (!b) b = kl_glfb_sym(base);
            if (!b) for (size_t j = 0; j < sizeof g_gl_impl / sizeof g_gl_impl[0]; j++)
                        if (strcmp(g_gl_impl[j].name, base) == 0) { b = g_gl_impl[j].fn; break; }
            if (b) {
                int bs = gl_slot(name);
                if (bs >= 0) g_gl[bs].resolved = 1;
                return b;
            }
        }
    }
    int s = gl_slot(name);
    if (s >= 0) g_gl[s].resolved = 1;
    if (gl_is_void(name)) return kl_named_stub(name, (void *)klgl_noop);
    return kl_named_stub(name, (void *)klgl_called);
}

static void *klegl_GetProcAddress(const char *name) {
    KLEGL_TRACE("eglGetProcAddress"); return kl_egl_sym(name); }

// The second door. Unity does dlopen("libGLESv2.so") + dlsym rather than going
// through eglGetProcAddress for the core entry points; a failed dlopen there
// produced a NULL it called anyway.
static const char g_gl_handle[] = "klepton-gles";

int kl_egl_claims(const char *soname) {
    if (!soname) return 0;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    return strcmp(b, "libGLESv2.so") == 0 || strcmp(b, "libGLESv3.so") == 0 ||
           strcmp(b, "libGLESv1_CM.so") == 0 || strcmp(b, "libEGL.so") == 0;
}

void *kl_egl_dlopen(const char *soname) {
    if (!kl_egl_claims(soname)) return NULL;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    fprintf(stderr, "  [egl] guest dlopen(\"%s\") -> synthetic GL handle\n", b);
    return (void *)g_gl_handle;
}

int kl_egl_is_handle(const void *h) { return h == (const void *)g_gl_handle; }

unsigned long kl_egl_frames(void) { return g_frames; }

void kl_egl_report(FILE *f) {
    // Outside the idempotency guard so it prints on whichever report call reaches
    // the log: the name of the GL entry point that aborted, when one did.
    if (kl_gl_last_unimpl[0])
        fprintf(f, "\n[klepton] the abort was an unimplemented GL entry point: '%s'\n",
                kl_gl_last_unimpl);
    // Idempotent: this is reachable both from a caller that knows it is dying
    // and from kl_fatal_prepare(), which every fatal path goes through. Printing
    // the graphics surface twice is noise; printing it never is how the first
    // sem_wait abort came back with no GL information at all.
    static int done;
    if (done) return;
    done = 1;
    fprintf(f, "\n=== EGL / GL surface ===\n");
    fprintf(f, "  eglSwapBuffers: %lu\n", g_frames);
    // A run that dies of memory wants this in the report, not only every Nth
    // swap: the last census before the abort is the one that names the class.
    kl_glfb_gl_census(f);
    kl_glfb_draw_census(f);
    kl_glfb_program_binary_census(f);
    unsigned called = 0;
    for (unsigned i = 0; i < g_ngl; i++) if (g_gl[i].calls) called++;
    // `calls` counts the NULL driver's own stubs. Under KL_GLFB=1 the guest gets
    // ANGLE's entry point directly and there is no seam left to count at, so a
    // zero here means "the host driver served it", not "the guest never called
    // it" — the same distinction kl_openxr_report carries, and the heading must
    // not assert the second.
    fprintf(f, "  GL entry points resolved: %u, of which counted through the "
               "null driver: %u\n", g_ngl, called);
    if (!g_ngl) return;
    if (called) {
        fprintf(f, "  --- called on the null driver (the work list) ---\n");
        for (unsigned i = 0; i < g_ngl; i++)
            if (g_gl[i].calls) fprintf(f, "    %-40s x%u\n", g_gl[i].name, g_gl[i].calls);
    }
    fprintf(f, "  --- resolved%s ---\n",
            kl_glfb_enabled() ? " (served by ANGLE; no per-call count)"
                              : " but never called");
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
    E("eglGetConfigs",          klegl_GetConfigs),
    E("eglGetConfigAttrib",     klegl_GetConfigAttrib),
    E("eglCreateWindowSurface", klegl_CreateWindowSurface),
    E("eglCreatePbufferSurface",klegl_CreatePbufferSurface),
    E("eglDestroySurface",      klegl_DestroySurface),
    E("eglQuerySurface",        klegl_QuerySurface),
    E("eglSurfaceAttrib",       klegl_SurfaceAttrib),
    E("eglCreateContext",       klegl_CreateContext),
    E("eglDestroyContext",      klegl_DestroyContext),
    E("eglQueryContext",        klegl_QueryContext),
    E("eglMakeCurrent",         klegl_MakeCurrent),
    E("eglGetCurrentContext",   klegl_GetCurrentContext),
    E("eglGetCurrentDisplay",   klegl_GetCurrentDisplay),
    E("eglGetCurrentSurface",   klegl_GetCurrentSurface),
    E("eglSwapBuffers",         klegl_SwapBuffers),
    E("eglSwapInterval",        klegl_SwapInterval),
    E("eglGetError",            klegl_GetError),
    E("eglGetProcAddress",      klegl_GetProcAddress),
    E("eglBindAPI",             klegl_BindAPI),
    E("eglQueryAPI",            klegl_QueryAPI),
    // The decoded-video image. glEGLImageTargetTexture2DOES is the
    // fourth member of this family and is intercepted in kl_glfb.c instead,
    // because the guest resolves it through eglGetProcAddress and it has to
    // fall through to ANGLE for anything that is not one of our images.
    E("eglGetNativeClientBufferANDROID", klegl_GetNativeClientBufferANDROID),
    E("eglCreateImageKHR",      klegl_CreateImageKHR),
    E("eglDestroyImageKHR",     klegl_DestroyImageKHR),
};

void *kl_egl_lookup(const char *name) {
    for (size_t i = 0; i < sizeof g_egl / sizeof g_egl[0]; i++)
        if (strcmp(g_egl[i].name, name) == 0) return g_egl[i].fn;
    return NULL;
}
