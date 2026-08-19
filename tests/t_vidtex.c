// `make vidtex` — the video TEXTURE gate: a decoded frame all the way to
// sampled pixels, on both storages.
//
// `make hevc` proves frames come out of the decoder; this proves the guest
// could SEE them. The native path has four links the decoder gate cannot
// exercise — the CVMetalTextureCache wrap (kl_cvmtl.m), ANGLE accepting the
// private YCbCr pixel format as an EGLImage sibling (the vendored ANGLE's
// format-table patch), the glEGLImageTargetTexture2DOES retarget, and the
// sampler's own YUV->RGB conversion — and a regression in any of them is a
// black or garbage video pane in a live Steam Link session, which costs a
// fresh pairing to even look at. Here it is one draw.
//
// The assertion of record: the SAME picture decoded twice, once native
// (biplanar YCbCr, sampler converts) and once as BGRA (KL_VTDEC_BGRA=1,
// VideoToolbox converts), drawn with the same shader, reads back nearly equal.
// Neither path is the reference for the other in principle, but they only
// agree if the sampler's matrix, range and plane addressing are all right —
// a wrong range reads as a global contrast shift, a swapped chroma plane as
// hue inversion, a wrong stride as diagonal tearing; all far outside the
// tolerance here, which only forgives the two paths' different chroma
// upsampling filters at the test card's hard edges.
//
// Needs the PATCHED ANGLE (`make angle-debug`) and is not part of
// `make check` — a bare checkout has no vendor/ to load.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <CoreVideo/CoreVideo.h>

#include "../runtime/media/kl_vtdec.h"
#include "../runtime/gfx/kl_glfb.h"

// Two streams, one per YCbCr row in klfb_video_mtl_format: 8-bit (private
// format 500) and Main10 (packed, 508 — the row a live Steam Link session
// actually takes, which the 8-bit stream alone failed to prove once). Both
// ffmpeg-generated testsrc2 like the `make hevc` stream; the 10-bit one adds
// `-profile:v main10 -pix_fmt p010le`.
#define STREAM8  "tests/data/t_hevc.h265"
#define STREAM10 "tests/data/t_hevc10.h265"
#define W 320
#define H 240

static int fails;
static void check(int ok, const char *what) {
    printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

// ---- the slice of GL this test speaks ----
#define GL_TEXTURE_2D_          0x0DE1
#define GL_RGBA_                0x1908
#define GL_UNSIGNED_BYTE_       0x1401
#define GL_COLOR_ATTACHMENT0_   0x8CE0
#define GL_FRAMEBUFFER_         0x8D40
#define GL_FRAMEBUFFER_COMPLETE_ 0x8CD5
#define GL_TRIANGLES_           0x0004
#define GL_VERTEX_SHADER_       0x8B31
#define GL_FRAGMENT_SHADER_     0x8B30
#define GL_COMPILE_STATUS_      0x8B81
#define GL_LINK_STATUS_         0x8B82

static void (*p_glGenTextures)(int32_t, uint32_t *);
static void (*p_glBindTexture)(uint32_t, uint32_t);
static void (*p_glTexImage2D)(uint32_t, int32_t, int32_t, int32_t, int32_t,
                              int32_t, uint32_t, uint32_t, const void *);
static void (*p_glGenFramebuffers)(int32_t, uint32_t *);
static void (*p_glBindFramebuffer)(uint32_t, uint32_t);
static void (*p_glFramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
static uint32_t (*p_glCheckFramebufferStatus)(uint32_t);
static void (*p_glViewport)(int32_t, int32_t, int32_t, int32_t);
static void (*p_glDrawArrays)(uint32_t, int32_t, int32_t);
static void (*p_glReadPixels)(int32_t, int32_t, int32_t, int32_t, uint32_t,
                              uint32_t, void *);
static uint32_t (*p_glCreateShader)(uint32_t);
static void (*p_glShaderSource)(uint32_t, int32_t, const char **, const int32_t *);
static void (*p_glCompileShader)(uint32_t);
static void (*p_glGetShaderiv)(uint32_t, uint32_t, int32_t *);
static uint32_t (*p_glCreateProgram)(void);
static void (*p_glAttachShader)(uint32_t, uint32_t);
static void (*p_glLinkProgram)(uint32_t);
static void (*p_glGetProgramiv)(uint32_t, uint32_t, int32_t *);
static void (*p_glUseProgram)(uint32_t);
static void (*p_glFinish)(void);
static uint32_t (*p_glGetError)(void);

static int gl_resolve(void) {
#define R(n) (*(void **)&p_##n = kl_glfb_sym(#n))
    return R(glGenTextures) && R(glBindTexture) && R(glTexImage2D) &&
           R(glGenFramebuffers) && R(glBindFramebuffer) &&
           R(glFramebufferTexture2D) && R(glCheckFramebufferStatus) &&
           R(glViewport) && R(glDrawArrays) && R(glReadPixels) &&
           R(glCreateShader) && R(glShaderSource) && R(glCompileShader) &&
           R(glGetShaderiv) && R(glCreateProgram) && R(glAttachShader) &&
           R(glLinkProgram) && R(glGetProgramiv) && R(glUseProgram) &&
           R(glFinish) && R(glGetError);
#undef R
}

static uint32_t make_program(void) {
    // gl_VertexID's fullscreen triangle: no buffers, no attributes, and the
    // sampler at its default unit 0 — the least GL that can sample a texture.
    static const char *vs_src =
        "#version 300 es\n"
        "out vec2 uv;\n"
        "void main(){ vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "  uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
    static const char *fs_src =
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform sampler2D t;\n"
        "in vec2 uv; out vec4 o;\n"
        "void main(){ o = texture(t, uv); }\n";
    int32_t ok = 0;
    uint32_t vs = p_glCreateShader(GL_VERTEX_SHADER_);
    p_glShaderSource(vs, 1, &vs_src, NULL);
    p_glCompileShader(vs);
    p_glGetShaderiv(vs, GL_COMPILE_STATUS_, &ok);
    if (!ok) return 0;
    uint32_t fs = p_glCreateShader(GL_FRAGMENT_SHADER_);
    p_glShaderSource(fs, 1, &fs_src, NULL);
    p_glCompileShader(fs);
    p_glGetShaderiv(fs, GL_COMPILE_STATUS_, &ok);
    if (!ok) return 0;
    uint32_t prog = p_glCreateProgram();
    p_glAttachShader(prog, vs);
    p_glAttachShader(prog, fs);
    p_glLinkProgram(prog);
    p_glGetProgramiv(prog, GL_LINK_STATUS_, &ok);
    return ok ? prog : 0;
}

// Decode the stream's first frame under whatever KL_VTDEC_* is in force.
static int is_vcl(const unsigned char *nal) { return ((nal[0] >> 1) & 0x3f) <= 31; }
static CVPixelBufferRef first_frame(const unsigned char *buf, long n) {
    kl_vtdec *d = kl_vtdec_create("video/hevc");
    if (!d) return NULL;
    CVPixelBufferRef out = NULL;
    long i = 0, prev = 0;
    int seen_vcl = 0;
    while (i + 3 < n && !out) {
        if (!(buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1)) { i++; continue; }
        long payload = i + 3;
        long sc = (i > 0 && buf[i-1] == 0) ? i - 1 : i;
        if (seen_vcl) {
            kl_vtdec_submit(d, buf + prev, (size_t)(sc - prev), 0);
            prev = sc;
            seen_vcl = 0;
            for (int spin = 0; spin < 400 && !(out = kl_vtdec_pull(d, NULL)); spin++)
                usleep(500);
        }
        if (payload < n && is_vcl(buf + payload)) seen_vcl = 1;
        i = payload;
    }
    kl_vtdec_destroy(d);        // `out` is the caller's reference, unaffected
    return out;
}

// Frame -> image -> texture -> draw -> readback. Returns 0 and says why not.
static int draw_frame(CVPixelBufferRef pb, uint32_t prog, unsigned char *rgba,
                      const char *label) {
    int w = 0, h = 0;
    void *img = kl_glfb_image_from_pixels(pb, &w, &h);
    if (!img) { printf("  %s: no image from the frame\n", label); return 0; }
    if (w != W || h != H) { printf("  %s: image is %dx%d\n", label, w, h); return 0; }

    uint32_t tex = 0, fb_tex = 0, fb = 0;
    p_glGenTextures(1, &tex);
    p_glBindTexture(GL_TEXTURE_2D_, tex);
    if (!kl_glfb_image_bind(img)) {
        printf("  %s: kl_glfb_image_bind refused\n", label);
        return 0;
    }

    p_glGenTextures(1, &fb_tex);
    p_glBindTexture(GL_TEXTURE_2D_, fb_tex);
    p_glTexImage2D(GL_TEXTURE_2D_, 0, GL_RGBA_, W, H, 0, GL_RGBA_,
                   GL_UNSIGNED_BYTE_, NULL);
    p_glGenFramebuffers(1, &fb);
    p_glBindFramebuffer(GL_FRAMEBUFFER_, fb);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                             GL_TEXTURE_2D_, fb_tex, 0);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER_) != GL_FRAMEBUFFER_COMPLETE_) {
        printf("  %s: framebuffer incomplete\n", label);
        return 0;
    }

    p_glViewport(0, 0, W, H);
    p_glUseProgram(prog);
    p_glBindTexture(GL_TEXTURE_2D_, tex);   // unit 0, where the sampler looks
    while (p_glGetError()) {}
    p_glDrawArrays(GL_TRIANGLES_, 0, 3);
    uint32_t e = p_glGetError();
    if (e) { printf("  %s: draw -> GL error 0x%x\n", label, e); return 0; }
    p_glFinish();
    p_glReadPixels(0, 0, W, H, GL_RGBA_, GL_UNSIGNED_BYTE_, rgba);
    e = p_glGetError();
    if (e) { printf("  %s: readback -> GL error 0x%x\n", label, e); return 0; }
    kl_glfb_image_destroy(img);
    return 1;
}

static void run_stream(const char *path, const char *tag, uint32_t prog) {
    printf("  --- %s (%s) ---\n", tag, path);
    char what[128];
    FILE *f = fopen(path, "rb");
    snprintf(what, sizeof what, "%s: stream opens (run from the repo root)", tag);
    check(f != NULL, what);
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return; }
    fclose(f);

    unsetenv("KL_VTDEC_BGRA");
    CVPixelBufferRef native = first_frame(buf, n);
    snprintf(what, sizeof what, "%s: a frame decodes in the native format", tag);
    check(native != NULL, what);
    unsigned pf = native ? CVPixelBufferGetPixelFormatType(native) : 0;
    snprintf(what, sizeof what, "%s: the native frame is biplanar", tag);
    check(native && CVPixelBufferGetPlaneCount(native) == 2, what);
    if (native)
        printf("  %s native format '%c%c%c%c'\n", tag, (char)(pf >> 24),
               (char)(pf >> 16), (char)(pf >> 8), (char)pf);

    setenv("KL_VTDEC_BGRA", "1", 1);
    CVPixelBufferRef bgra = first_frame(buf, n);
    unsetenv("KL_VTDEC_BGRA");
    snprintf(what, sizeof what, "%s: the same frame decodes as BGRA", tag);
    check(bgra != NULL, what);
    if (!native || !bgra) { free(buf); return; }

    size_t sz = (size_t)W * H * 4;
    unsigned char *px_native = malloc(sz), *px_bgra = malloc(sz);
    int drew = draw_frame(native, prog, px_native, tag);
    snprintf(what, sizeof what, "%s: native frame: image, bind, draw, readback", tag);
    check(drew, what);
    int drew_b = draw_frame(bgra, prog, px_bgra, tag);
    snprintf(what, sizeof what, "%s: BGRA frame: image, bind, draw, readback", tag);
    check(drew_b, what);

    if (drew && drew_b) {
        // The native readback carries a picture of its own...
        int varied = 0;
        for (size_t i = 4; i < sz && !varied; i += 64)
            if (px_native[i] != px_native[0]) varied = 1;
        snprintf(what, sizeof what, "%s: native readback is a picture, not a flat fill", tag);
        check(varied, what);
        int opaque = 1;
        for (size_t i = 3; i < sz; i += 4)
            if (px_native[i] != 255) { opaque = 0; break; }
        snprintf(what, sizeof what, "%s: native readback is opaque (YCbCr alpha answers 1)", tag);
        check(opaque, what);

        // ...and it is the SAME picture the BGRA path produces. Mean abs
        // difference over RGB: the two chroma upsamplers disagree only at the
        // test card's hard edges, and a matrix/range/plane mistake is tens of
        // levels everywhere.
        unsigned long long diff = 0;
        unsigned long count = 0;
        int worst = 0;
        for (size_t i = 0; i < sz; i += 4) {
            for (int c = 0; c < 3; c++) {
                int a = px_native[i + c], b = px_bgra[i + c];
                int d = a > b ? a - b : b - a;
                diff += (unsigned)d;
                if (d > worst) worst = d;
                count++;
            }
        }
        double mean = (double)diff / (double)count;
        printf("  %s native vs BGRA: mean abs diff %.2f, worst %d\n", tag, mean, worst);
        snprintf(what, sizeof what, "%s: the two storages agree (mean abs diff < 4)", tag);
        check(mean < 4.0, what);
    }

    CVPixelBufferRelease(native);
    CVPixelBufferRelease(bgra);
    free(px_native);
    free(px_bgra);
    free(buf);
}

int main(void) {
    printf("=== video texture (native YCbCr vs BGRA, sampled through ANGLE) ===\n");

    setenv("KL_GLFB", "1", 1);
    if (!kl_glfb_init()) {
        printf("  kl_glfb_init failed — this gate needs the PATCHED ANGLE "
               "(`make angle-debug`)\n");
        return 1;
    }
    kl_glfb_make_current();
    check(gl_resolve(), "GL entry points resolve through kl_glfb_sym");
    uint32_t prog = make_program();
    check(prog != 0, "the sampling program compiles and links");
    if (!prog) return 1;

    run_stream(STREAM8, "8-bit", prog);
    run_stream(STREAM10, "main10", prog);

    printf(fails ? "\nFAIL: %d check(s) failed\n" : "\nPASS: video textures sample\n",
           fails);
    return fails ? 1 : 0;
}
