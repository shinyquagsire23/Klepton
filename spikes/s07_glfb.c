// S0.7 — is there a real, readable framebuffer available on the host at all?
//
// The null driver in kl_egl.c can tell us which GL calls the guest makes but not
// what they would produce. Before building any backend, the question worth
// answering first is the cheap one: can this process get an offscreen GL context
// and read pixels back out of it? Everything downstream depends on that and
// nothing else does.
//
// macOS OpenGL is deprecated but present, and CGL can hand out a 4.1 core profile
// with no drawable attached — which is exactly what an offscreen compositor wants.
// GLES 3.0 (what the guest speaks) is close enough to GL 4.1 core that the same
// calls mostly work; this spike only proves the context and the readback.
//
// Build: clang -o build/s07_glfb spikes/s07_glfb.c -framework OpenGL -lz
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLTypes.h>
#include <OpenGL/CGLCurrent.h>
#include <OpenGL/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>

#define W 256
#define H 128

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
static int png(const char *path, const uint8_t *px, int w, int h) {
    size_t stride = (size_t)w * 4, raw_n = (stride + 1) * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < h; y++) {                    // GL is bottom-up, PNG top-down
        raw[(stride + 1) * (size_t)y] = 0;
        memcpy(raw + (stride + 1) * (size_t)y + 1, px + stride * (size_t)(h - 1 - y), stride);
    }
    uLongf cn = compressBound((uLong)raw_n);
    uint8_t *cb = malloc(cn);
    if (compress(cb, &cn, raw, (uLong)raw_n) != Z_OK) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    static const uint8_t sig[8] = {0x89,'P','N','G',13,10,26,10};
    fwrite(sig, 1, 8, f);
    uint8_t ih[13]; be32(ih, (uint32_t)w); be32(ih + 4, (uint32_t)h);
    ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    chunk(f, "IDAT", cb, (uint32_t)cn);
    chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(cb);
    return 1;
}

static const char *VS =
    "#version 410 core\n"
    "layout(location=0) in vec4 vertex;\n"
    "out vec2 texCoord;\n"
    "void main(){ gl_Position = vec4(vertex.xy,0.0,1.0); texCoord = vertex.zw; }\n";
static const char *FS =
    "#version 410 core\n"
    "in vec2 texCoord;\n"
    "out vec4 fragColor;\n"
    "void main(){ fragColor = vec4(texCoord, 0.25, 1.0); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log);
               fprintf(stderr, "shader: %s\n", log); return 0; }
    return s;
}

int main(void) {
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
        kCGLPFAAccelerated,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        kCGLPFADepthSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0,
    };
    CGLPixelFormatObj pix = NULL; GLint npix = 0;
    if (CGLChoosePixelFormat(attrs, &pix, &npix) != kCGLNoError || !pix) {
        fprintf(stderr, "FAIL: no GL4 core pixel format\n"); return 1;
    }
    CGLContextObj ctx = NULL;
    if (CGLCreateContext(pix, NULL, &ctx) != kCGLNoError || !ctx) {
        fprintf(stderr, "FAIL: CGLCreateContext\n"); return 1;
    }
    CGLSetCurrentContext(ctx);
    printf("  GL_VERSION  = %s\n", glGetString(GL_VERSION));
    printf("  GL_RENDERER = %s\n", glGetString(GL_RENDERER));
    printf("  GLSL        = %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));

    // No drawable is attached, so all rendering goes to an FBO — which is what a
    // headless compositor wants anyway.
    GLuint fbo = 0, tex = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FAIL: framebuffer incomplete\n"); return 1;
    }
    glViewport(0, 0, W, H);
    glClearColor(0.10f, 0.12f, 0.16f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // One textured-quad-shaped draw, the same shape as the guest's splash: a
    // triangle strip of vec4(x, y, u, v).
    GLuint prog = glCreateProgram();
    GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) return 1;
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) { char log[1024]; glGetProgramInfoLog(prog, sizeof log, NULL, log);
                   fprintf(stderr, "FAIL: link: %s\n", log); return 1; }
    glUseProgram(prog);

    const float quad[] = {
        -0.8f, -0.8f, 0.0f, 0.0f,
         0.8f, -0.8f, 1.0f, 0.0f,
        -0.8f,  0.8f, 0.0f, 1.0f,
         0.8f,  0.8f, 1.0f, 1.0f,
    };
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    uint8_t *px = malloc((size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) { fprintf(stderr, "FAIL: GL error 0x%x\n", err); return 1; }

    // Did anything actually land? A readback of the clear colour alone would mean
    // the context works but the draw did not, which is a different answer.
    unsigned drawn = 0;
    for (int i = 0; i < W * H; i++)
        if (px[i * 4] > 40 || px[i * 4 + 1] > 40) drawn++;
    printf("  non-background pixels: %u / %u\n", drawn, W * H);

    const char *out = "build/s07_glfb.png";
    if (!png(out, px, W, H)) { fprintf(stderr, "FAIL: png\n"); return 1; }
    printf("  wrote %s\n", out);
    printf("%s\n", drawn > 1000 ? "=== S0.7 PASS: offscreen GL context renders and reads back ==="
                                : "FAIL: context works but the draw produced nothing");
    return drawn > 1000 ? 0 : 1;
}
