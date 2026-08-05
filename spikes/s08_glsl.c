// S0.8 — will the guest's own shaders compile on the host's GL?
//
// S0.7 proved there is a real offscreen framebuffer. This asks the question that
// decides whether a *forwarding* backend is viable at all: Unity ships GLSL ES 300
// and macOS offers GL 4.1 core / GLSL 4.10. If the guest's actual shader text
// compiles after a mechanical rewrite, then the cheapest possible compositor is to
// hand the guest the host's own GL entry points and let it drive them directly —
// no translation layer, no SPIR-V pipeline, no MSL.
//
// The shaders used here are the real ones, captured from a live run with
// KL_DUMP_SHADERS. Nothing is hand-written: a spike that compiles a shader we
// wrote ourselves would prove nothing about the guest's.
//
// Build: clang -o build/s08_glsl spikes/s08_glsl.c -framework OpenGL
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLTypes.h>
#include <OpenGL/CGLCurrent.h>
#include <OpenGL/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The rewrite under test. GLSL ES 300 -> GLSL 4.10 core, and the claim is that it
// is a *one line* change:
//
//   - "#version 300 es" -> "#version 410 core".
//   - `precision mediump float;` and friends are accepted verbatim by desktop
//     GLSL 1.30+ as no-ops, so they are deliberately left alone. If that is wrong
//     the compile fails here and says so, which is the point of the spike.
//   - in/out, texture(), and layout qualifiers are already spelled the same.
static char *rewrite(const char *src) {
    const char *ver = "#version 300 es";
    char *out = malloc(strlen(src) + 64);
    if (!out) return NULL;
    if (strncmp(src, ver, strlen(ver)) == 0)
        sprintf(out, "#version 410 core%s", src + strlen(ver));
    else
        strcpy(out, src);
    return out;
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "FAIL: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); return NULL; }
    b[n] = 0; fclose(f);
    return b;
}

static GLuint try_compile(GLenum type, const char *path, int *ok) {
    char *raw = slurp(path);
    if (!raw) { *ok = 0; return 0; }
    char *src = rewrite(raw);
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, (const GLchar *const *)&src, NULL);
    glCompileShader(s);
    GLint status = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log);
        printf("  %-28s FAILED\n%s\n", path, log);
        *ok = 0;
    } else {
        printf("  %-28s compiles\n", path);
    }
    free(raw); free(src);
    return s;
}

int main(int argc, char **argv) {
    const char *vs_path = argc > 1 ? argv[1] : "/tmp/sh2/shader_000_156.glsl";
    const char *fs_path = argc > 2 ? argv[2] : "/tmp/sh2/shader_001_157.glsl";

    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
        kCGLPFAAccelerated, (CGLPixelFormatAttribute)0,
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
    printf("  host GLSL = %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));

    int ok = 1;
    GLuint vs = try_compile(GL_VERTEX_SHADER, vs_path, &ok);
    GLuint fs = try_compile(GL_FRAGMENT_SHADER, fs_path, &ok);
    if (!ok) { printf("FAIL: the guest's shaders do not compile as written\n"); return 1; }

    // Linking is the second half of the question: two shaders that each compile
    // can still disagree about their varyings, and a forwarding backend needs the
    // whole program to survive, not each stage on its own.
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048]; glGetProgramInfoLog(prog, sizeof log, NULL, log);
        printf("  link FAILED\n%s\n", log);
        return 1;
    }
    printf("  program links\n");

    // And that the names the guest will ask for actually resolve, since it looks
    // uniforms up by name at runtime.
    printf("  uniform 'tex'              -> location %d\n", glGetUniformLocation(prog, "tex"));
    printf("  uniform 'uvOffsetAndScale' -> location %d\n",
           glGetUniformLocation(prog, "uvOffsetAndScale"));
    printf("  attrib  'vertex'           -> location %d\n", glGetAttribLocation(prog, "vertex"));

    printf("=== S0.8 PASS: the guest's GLSL ES 300 compiles and links as GL 4.10 core\n");
    printf("    (rewrite required: the #version line, and nothing else)\n");
    return 0;
}
