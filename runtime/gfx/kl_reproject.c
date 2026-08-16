// The reprojection pass — see kl_reproject.h for what it corrects and why the
// two compositors share it.
//
// Nothing here touches Metal or Compositor Services: it is matrices and a
// string, so it builds for the host and the device out of the same source list
// and can be reasoned about (and, on the host, run) without a headset.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kl_reproject.h"
#include "kl_env.h"

// KL_REPROJECT_NOCANT=1 — treat device_from_view as having no rotation.
//
// The guest is told where each eye *is* (kl_ovrp_set_eye_offset pushes
// view.transform's translation) and what each eye's frustum *shape* is (the
// tangents, recovered from the drawable's projection). It is never told that
// the eye is also TURNED: nodes 0 and 1 report the head's orientation. So a
// picture the guest rendered has no cant in it, while this pass applies
// view.transform's rotation to place it — and that rotation is equal and
// opposite in the two eyes, which is the one error shape the visual system
// cannot merge. Set this if the composite is logging a non-zero per-eye view
// rotation; the real fix is to give the guest the cant, which is a change to
// kl_ovrp's eye nodes rather than to this file.
static int klr_nocant(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_REPROJECT_NOCANT", 0);
    return on;
}

// Whether the composite decodes its sample from sRGB. See kl_reproject.h for
// the full argument; the short version is that ANGLE forces an sRGB encode the
// guest explicitly asked to be without, and the extra decode undoes it.
//
// A plain int, written from the GL thread the first time the guest says so and
// read from the render thread every frame. Not atomic on purpose: it is one
// word, it only ever moves 0 -> 1, and the worst a torn read could do is
// composite one more frame with the old value.
//
// `KL_SRGB_DECODE` overrides it in both directions and is the A/B — the whole
// question is a judgement about brightness and the only instrument is a person
// looking at it, so being able to flip it inside a single session (rather than
// across two pairings) is the difference between settling it and arguing.
static int g_srgb_decode;

static int klr_srgb_forced(int *out) {
    const char *e = getenv("KL_SRGB_DECODE");
    if (!e || !*e) return 0;
    *out = kl_env_on("KL_SRGB_DECODE", 0);
    return 1;
}

void kl_reproject_set_srgb_decode(int on) {
    on = on ? 1 : 0;
    if (g_srgb_decode == on) return;
    g_srgb_decode = on;
    int forced;
    fprintf(stderr, "  [reproject] sRGB decode %s%s\n", on ? "ON" : "off",
            klr_srgb_forced(&forced) ? " (overridden by KL_SRGB_DECODE)" : "");
}

int kl_reproject_srgb_decode(void) {
    int forced;
    if (klr_srgb_forced(&forced)) return forced;
    return g_srgb_decode;
}

// The shader. Compiled from source at runtime by whichever compositor is
// running rather than shipped as a .metal, so it lives beside the math that
// feeds it — the uniforms below and the struct in this string are one
// contract, and having them in one file is how they stay one.
//
// Drawn as a 4-vertex triangle strip with no vertex buffer.
static const char kl_msl_reproject[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"struct KLReproj {\n"
"    float4x4 projection;\n"
"    float4x4 modelView;\n"
"    float4   tangents;   // left, right, top, bottom — all positive\n"
"    uint     slice;\n"
"    float    depth;      // metres — see kl_reproject.h\n"
"    uint     visible;    // 0 = collapse the quad, this eye has no picture\n"
"    uint     srgbDecode; // 1 = the sample is an sRGB code value, not linear\n"
"    uint     flipY;      // 1 = the picture's origin is the TOP left (Vulkan)\n"
"    uint     gridEye;    // which grid BLOCK this view reads — kl_reproject.h\n"
"};\n"
"\n"
// The sRGB EOTF, piecewise as the spec has it rather than a 2.2 power. The
// difference is confined to the bottom of the range, which is precisely where a
// wrong curve is most visible — a near-black that lifts is the artefact this
// whole path exists to remove, and approximating it would leave a smaller
// version of the same complaint.
"static float3 kl_srgb_to_linear(float3 c)\n"
"{\n"
"    float3 lo = c * (1.0 / 12.92);\n"
"    float3 hi = pow(max(c + 0.055, 0.0) * (1.0 / 1.055), 2.4);\n"
"    return select(lo, hi, c > 0.04045);\n"
"}\n"
"\n"
"static float4 kl_eye_sample(texture2d_array<float> tex, sampler samp,\n"
"                            float2 uv, uint slice, uint srgbDecode)\n"
"{\n"
"    float3 c = tex.sample(samp, uv, slice).rgb;\n"
"    return float4(srgbDecode != 0u ? kl_srgb_to_linear(c) : c, 1.0);\n"
"}\n"
"\n"
// `slice` travels as a flat varying because the fragment stage has no
// amplification_id: with both eyes drawn in one amplified pass, the uniform the
// fragment shader would have to read is not the one it can index to.
// `srgbDecode` rides along for the same reason `slice` does: it comes out of
// the per-view uniform, and with both eyes in one amplified pass the fragment
// stage cannot index the array it would have to read.
// `src` is the third, and it is there because the two eyes are not always two
// SLICES of one texture. They are for every guest that goes through the eye
// provider (it allocates one 2-slice array by construction) and for a Vulkan
// guest whose eye IS the layer (BONELAB). They are NOT for an OpenXR guest on
// Vulkan, which has one swapchain per eye and therefore two unrelated
// MTLTextures — Open Brush is the first, and there is no view, no slice and no
// binding that can make one texture argument reach both. So the pass binds up
// to two and each view says which is its own; when they really do share one,
// the same texture is bound twice and this reduces to exactly what it was.
"struct VOut { float4 pos [[position]]; float2 uv; uint slice [[flat]];\n"
"              uint srgb [[flat]]; uint src [[flat]]; };\n"
"\n"
// One place, so a rung of the probe ladder cannot disagree with the real pass
// about which eye it is looking at — which is the failure this whole file keeps
// paying for — an instrument answering confidently about the wrong subject. A
// texture argument cannot be selected into a variable in MSL, so
// this is a branch and not an index.
"static float4 kl_eye_sample2(texture2d_array<float> t0,\n"
"                             texture2d_array<float> t1, sampler samp,\n"
"                             float2 uv, uint slice, uint srgbDecode, uint src)\n"
"{\n"
"    return src == 0u ? kl_eye_sample(t0, samp, uv, slice, srgbDecode)\n"
"                     : kl_eye_sample(t1, samp, uv, slice, srgbDecode);\n"
"}\n"
"\n"
// Off-screen on every axis (x, y and z all exceed w), so the clipper drops the
// primitive whole. This is how an eye with no texture yet is expressed now that
// the two eyes share one encoder and one draw call.
"static constant float4 kl_offscreen = float4(2.0, 2.0, 2.0, 1.0);\n"
"\n"
"// The guest's picture as a quad at KL_REPROJECT_DEPTH metres, sized by the\n"
"// tangents it was RENDERED with, in the space its render pose defines. The\n"
"// model-view then looks at it from where the head is now, and the projection\n"
"// is the one this drawable actually wants — so the stale pose and the\n"
"// frustum mismatch are corrected by the same two matrices.\n"
"//\n"
"// buffer(0) is an array — one entry per amplified view. A caller that draws a\n"
"// single view binds a single struct and lands on index 0.\n"
"vertex VOut kl_reproject_v(uint vid [[vertex_id]],\n"
"                           ushort amp [[amplification_id]],\n"
"                           constant KLReproj *ua [[buffer(0)]],\n"
"                           constant float2 *grid [[buffer(1)]])\n"
"{\n"
"    constant KLReproj &u = ua[amp];\n"
"    // The unwarp grid (kl_reproject.h): grid[0] is (nx, ny) and the texture\n"
"    // coordinates follow, one per grid vertex, ALREADY UNWARPED on the CPU.\n"
"    // A foveated eye texture is stored squeezed, and undoing that here — at a\n"
"    // few thousand vertices — rather than in the fragment shader is the whole\n"
"    // point: the rasterizer's interpolation does the work for free, and\n"
"    // because a rate map is piecewise linear between its zone boundaries, a\n"
"    // grid built on those boundaries is EXACT rather than an approximation.\n"
"    //\n"
"    // An unfoveated pass binds the 1x1 identity grid, and everything below\n"
"    // reduces to the two triangles that used to be a 4-vertex strip.\n"
"    //\n"
"    // The coordinates may start past 1: the two eyes can have been rendered\n"
"    // into DIFFERENT sub-rects of one texture (kl_reproject.h, `grid_per_eye`),\n"
"    // so a grid can carry one BLOCK per eye and u.gridEye says which is this\n"
"    // view's. Derived from the nx/ny read out of the header rather than passed\n"
"    // in, so a grid that fell back to the identity moves both the header and\n"
"    // every block's base together and no view can be left pointing past it.\n"
"    uint nx = uint(grid[0].x), ny = uint(grid[0].y);\n"
"    uint base = 1u + u.gridEye * (nx + 1u) * (ny + 1u);\n"
"    uint cell = vid / 6u, k = vid % 6u;\n"
"    uint2 g = uint2(cell % nx, cell / nx);\n"
"    // Two triangles a cell: (0,0)(1,0)(0,1) and (1,0)(1,1)(0,1).\n"
"    g.x += (k == 1u || k == 3u || k == 4u) ? 1u : 0u;\n"
"    g.y += (k == 2u || k == 4u || k == 5u) ? 1u : 0u;\n"
"    float2 c = float2(g) / float2(float(nx), float(ny));\n"
"    float d = u.depth;          // KL_REPROJECT_DEPTH\n"
"    float x = mix(-u.tangents.x, u.tangents.y, c.x) * d;\n"
"    float y = mix(-u.tangents.w, u.tangents.z, c.y) * d;\n"
"    VOut o;\n"
"    o.slice = u.slice;\n"
"    o.srgb = u.srgbDecode;\n"
"    // The amplification index IS the texture slot: the caller binds the views'\n"
"    // textures in the order it amplified them. A single-view pass amplifies\n"
"    // once, lands on 0, and binds one texture.\n"
"    o.src = uint(amp);\n"
"    o.pos = u.visible == 0u ? kl_offscreen\n"
"                            : u.projection * u.modelView * float4(x, y, -d, 1.0);\n"
"    // Same mapping as the plain blit below, and for the same reason. Derived\n"
"    // in the viewer against a frame known to be the right way up — do not\n"
"    // re-derive it from first principles about GL's origin, because ANGLE's\n"
"    // Metal backend has already had its say by the time we see the texture.\n"
"    //\n"
"    // The blit is the authority and this must track it. It was corrected to\n"
"    // uv.y = p.y (the picture was upside down); this quad's c.y = 1 is UP,\n"
"    // where the blit's p.y = 2 is also up, so the same convention written for\n"
"    // this parameterisation is uv.y = c.y. The two disagreed for exactly as\n"
"    // long as the reprojection pass had never run anywhere.\n"
"    //\n"
"    // Read from the grid rather than computed, which is what carries the\n"
"    // unwarp. The identity grid holds exactly float2(c.x, c.y), so the\n"
"    // convention derived above is preserved rather than re-derived — and the\n"
"    // table is built in this same uv space, so nothing here flips.\n"
"    float2 uv = grid[base + g.y * (nx + 1u) + g.x];\n"
// ...except for a guest whose framebuffer origin is the TOP left. That is
// Vulkan, and it is the one thing about the picture that the API the guest drew
// with decides rather than anything here (kl_reproject.h, `flip_y`). Applied
// AFTER the grid, which is exact today because the Vulkan path has no
// foveation: with a rate map in play the unwarp and the flip are both in the
// texture's own space and this composition would want checking against a
// foveated Vulkan eye, which nothing has produced yet.
"    if (u.flipY != 0u) uv.y = 1.0 - uv.y;\n"
"    o.uv = uv;\n"
"    return o;\n"
"}\n"
"\n"
"fragment float4 kl_reproject_f(VOut in [[stage_in]],\n"
"                               texture2d_array<float> tex [[texture(0)]],\n"
"                               texture2d_array<float> tex1 [[texture(1)]],\n"
"                               sampler samp [[sampler(0)]])\n"
"{\n"
"    return kl_eye_sample2(tex, tex1, samp, in.uv, in.slice, in.srgb, in.src);\n"
"}\n"
"\n"
// The probe ladder (KL_CP_PROBE). Same library, same vertex function, so each
// rung differs from the real pass in exactly one respect and nothing else moves.
// A picture that is black tells you nothing about WHICH link is dark; these do.
"fragment float4 kl_probe_solid_f(VOut in [[stage_in]])\n"
"{\n"
"    return float4(1.0, 0.0, 1.0, 1.0);            // magenta: geometry only\n"
"}\n"
"\n"
// The real sample with alpha forced opaque. If this is visible and the real pass
// is not, the guest's picture is arriving with alpha 0 and the fix is here.
"fragment float4 kl_probe_opaque_f(VOut in [[stage_in]],\n"
"                                  texture2d_array<float> tex [[texture(0)]],\n"
"                                  texture2d_array<float> tex1 [[texture(1)]],\n"
"                                  sampler samp [[sampler(0)]])\n"
"{\n"
"    return kl_eye_sample2(tex, tex1, samp, in.uv, in.slice, in.srgb, in.src);\n"
"}\n"
"\n"
// The eye texture over the whole viewport, ignoring the quad, the poses and the
// projection entirely — the viewer's proven path, on device. Visible here but
// not at rung 2 means the picture is fine and the REPROJECTION GEOMETRY is what
// is dark. Uses the same uv convention as kl_blit_v below.
"vertex VOut kl_probe_full_v(uint vid [[vertex_id]],\n"
"                            ushort amp [[amplification_id]],\n"
"                            constant KLReproj *ua [[buffer(0)]])\n"
"{\n"
"    float2 p = float2((vid << 1) & 2, vid & 2);\n"
"    VOut o;\n"
"    o.slice = ua[amp].slice;\n"
"    o.srgb = ua[amp].srgbDecode;\n"
"    o.src = uint(amp);\n"
"    o.pos = ua[amp].visible == 0u ? kl_offscreen\n"
"                                  : float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"    o.uv  = float2(p.x, ua[amp].flipY != 0u ? 1.0 - p.y : p.y);\n"
"    return o;\n"
"}\n"
"\n"
"fragment float4 kl_probe_full_f(VOut in [[stage_in]],\n"
"                                texture2d_array<float> tex [[texture(0)]],\n"
"                                texture2d_array<float> tex1 [[texture(1)]],\n"
"                                sampler samp [[sampler(0)]])\n"
"{\n"
"    return kl_eye_sample2(tex, tex1, samp, in.uv, in.slice, in.srgb, in.src);\n"
"}\n";

// The pass that existed before reprojection: a full-screen triangle, three
// vertices, no buffer, no seam down the middle where two triangles meet. Kept
// as the A/B — it is the one known to produce the right picture, so a wrong
// reprojected frame can be told from a wrong *frame*.
static const char kl_msl_blit[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; float2 uv; };\n"
// Matches kl_blit_uniforms in kl_reproject.h. The flip lives in the FRAGMENT
// stage here rather than in the vertex shader, because this pass has no grid
// and no per-vertex table — one branch on a uniform is the whole difference
// between a GL guest's picture and a Vulkan guest's.
"struct KLBlit { uint slice; uint flipY; };\n"
"vertex VOut kl_blit_v(uint vid [[vertex_id]]) {\n"
"    float2 p = float2((vid << 1) & 2, vid & 2);\n"
"    VOut o;\n"
"    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"    o.uv  = float2(p.x, p.y);\n"
"    return o;\n"
"}\n"
// Opaque, exactly as kl_eye_sample is, and not for tidiness: an eye layer is
// composited opaque by the runtime, so a guest has no reason to author alpha —
// Beat Saber 1.40 leaves 0 across the whole eye texture. Sampling that straight
// into a drawable hands the window server a fully transparent frame over a
// correct picture, which is the same failure as a black one and looks identical
// from inside. The A/B for "is the alpha the problem" is the reprojection path,
// which has forced 1.0 since it was written.
"fragment float4 kl_blit_f(VOut in [[stage_in]],\n"
"                          texture2d_array<float> tex [[texture(0)]],\n"
"                          sampler samp [[sampler(0)]],\n"
"                          constant KLBlit &u [[buffer(0)]]) {\n"
"    float2 uv = float2(in.uv.x, u.flipY != 0u ? 1.0 - in.uv.y : in.uv.y);\n"
"    return float4(tex.sample(samp, uv, u.slice).rgb, 1.0);\n"
"}\n";

// The OVERLAY pass — see kl_reproject.h.
//
// Deliberately not a variant of the reprojection shader above. That one places
// an EYE-CENTRED quad at a chosen depth and corrects only rotation, because a
// flat picture with no per-pixel depth cannot be moved. An overlay is somewhere:
// it has a position in the guest's tracking space, a real size in metres, and
// its parallax between the two eyes is the whole point of it being a layer
// rather than pixels. So the geometry is built in world units and the model-view
// keeps its translation.
//
// It also BLENDS, which the eye pass never does — the caller sets that on the
// pipeline, and the alpha here is the guest's own rather than the forced 1.0
// two doors down: a UI quad's transparency is authored, unlike an eye texture's
// (see kl_msl_blit for why that one is forced).
static const char kl_msl_overlay[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; float2 uv; };\n"
// Matches kl_overlay_uniforms in kl_reproject.h.
"struct KLOv {\n"
"    float4x4 projection;\n"
"    float4x4 model_view;\n"
"    float2   half_size;\n"
"    float4   uv_rect;\n"      // x, y, w, h — the eye's ViewportRect as a fraction
"    uint     slice;\n"
"    uint     flip_y;\n"
"    uint     srgb_decode;\n"
"    uint     visible;\n"
"};\n"
"vertex VOut kl_ov_v(uint vid [[vertex_id]],\n"
"                    constant KLOv *us [[buffer(0)]],\n"
"                    ushort amp [[amplification_id]]) {\n"
"    constant KLOv &u = us[amp];\n"
"    VOut o;\n"
"    if (u.visible == 0u) { o.pos = float4(2.0, 2.0, 2.0, 1.0); o.uv = float2(0); return o; }\n"
// A triangle strip in the quad's own plane, facing -Z, which is the convention
// ovrpLayerSubmit's Pose states its orientation in.
"    float2 c = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? -1.0 : 1.0);\n"
"    float4 p = float4(c * u.half_size, 0.0, 1.0);\n"
"    o.pos = u.projection * (u.model_view * p);\n"
"    float2 t = float2((c.x + 1.0) * 0.5, (1.0 - c.y) * 0.5);\n"
"    if (u.flip_y != 0u) t.y = 1.0 - t.y;\n"
"    o.uv = u.uv_rect.xy + t * u.uv_rect.zw;\n"
"    return o;\n"
"}\n"
"fragment float4 kl_ov_f(VOut in [[stage_in]],\n"
"                        texture2d<float> tex [[texture(0)]],\n"
"                        sampler samp [[sampler(0)]],\n"
"                        constant KLOv *us [[buffer(0)]],\n"
"                        ushort amp [[amplification_id]]) {\n"
"    constant KLOv &u = us[amp];\n"
"    float4 c = tex.sample(samp, in.uv);\n"
"    if (u.srgb_decode != 0u) {\n"
"        float3 lo = c.rgb / 12.92;\n"
"        float3 hi = pow((c.rgb + 0.055) / 1.055, float3(2.4));\n"
"        c.rgb = select(hi, lo, c.rgb <= float3(0.04045));\n"
"    }\n"
"    return c;\n"
"}\n";

const char *kl_reproject_msl(void)      { return kl_msl_reproject; }
const char *kl_reproject_blit_msl(void) { return kl_msl_blit; }
const char *kl_reproject_overlay_msl(void) { return kl_msl_overlay; }


// The unwarp grid. See kl_reproject.h for the layout and for why the unwarp
// lives here — at grid vertices, precomputed — rather than in the fragment
// shader.
void kl_reproject_grid_identity(simd_float2 *out) {
    if (!out) return;
    out[0] = simd_make_float2(1.0f, 1.0f);      // 1 x 1 cells
    out[1] = simd_make_float2(0.0f, 0.0f);
    out[2] = simd_make_float2(1.0f, 0.0f);
    out[3] = simd_make_float2(0.0f, 1.0f);
    out[4] = simd_make_float2(1.0f, 1.0f);
}

void kl_reproject_grid_build(simd_float2 *out, uint32_t nx, uint32_t ny,
                             const float *vp,
                             float screen_w, float screen_h,
                             float tex_w, float tex_h,
                             kl_reproject_s2p fn, void *ctx) {
    kl_reproject_grid_build_eye(out, 0, nx, ny, vp, screen_w, screen_h,
                                tex_w, tex_h, fn, ctx);
}

void kl_reproject_grid_build_eye(simd_float2 *out, uint32_t eye,
                                 uint32_t nx, uint32_t ny,
                                 const float *vp,
                                 float screen_w, float screen_h,
                                 float tex_w, float tex_h,
                                 kl_reproject_s2p fn, void *ctx) {
    // A degenerate grid would be a pass that draws nothing, which is a black
    // eye rather than an unfoveated one. Fall back to the identity, which is
    // the correct picture for an unwarped, fully-drawn texture and merely the
    // wrong one otherwise — the strictly better failure of the two.
    if (!out) return;
    if (nx == 0 || ny == 0 || nx > KL_REPROJECT_GRID_MAX ||
        ny > KL_REPROJECT_GRID_MAX || !(tex_w > 0) || !(tex_h > 0) ||
        !(screen_w > 0) || !(screen_h > 0)) {
        // The identity, in this EYE's block of a 1x1 grid: the shader derives
        // every block's base from the header it reads, so collapsing the header
        // to 1x1 moves eye 1's base to 5 whether this call or the other one
        // took the fallback. Writing it at 1 unconditionally would leave a
        // second eye reading whatever followed.
        kl_reproject_grid_identity(out);
        if (eye) {
            out[5] = simd_make_float2(0.0f, 0.0f);
            out[6] = simd_make_float2(1.0f, 0.0f);
            out[7] = simd_make_float2(0.0f, 1.0f);
            out[8] = simd_make_float2(1.0f, 1.0f);
        }
        return;
    }
    // The span the guest drew, in screen pixels. Absent or nonsensical means it
    // used the whole screen, which is what every guest that cannot answer does.
    float vx = 0, vy = 0, vw = screen_w, vh = screen_h;
    if (vp && vp[2] > 0 && vp[3] > 0) {
        vx = vp[0]; vy = vp[1]; vw = vp[2]; vh = vp[3];
        // Clamped rather than trusted: a rect reaching past the screen would
        // send this through the rate map at coordinates it was not built for,
        // and Metal's answer there is not defined by anything we can check.
        if (vx < 0) vx = 0;
        if (vy < 0) vy = 0;
        if (vx + vw > screen_w) vw = screen_w - vx;
        if (vy + vh > screen_h) vh = screen_h - vy;
        if (!(vw > 0) || !(vh > 0)) { vx = vy = 0; vw = screen_w; vh = screen_h; }
    }
    out[0] = simd_make_float2((float)nx, (float)ny);
    // Where this eye's block starts — the same arithmetic kl_reproject_build
    // puts in the uniform, and the reason it is not a second convention: both
    // read kl_reproject_grid_stride, so a block written here and the base
    // handed to the shader cannot disagree.
    const uint32_t base = 1u + eye * kl_reproject_grid_stride(nx, ny);
    for (uint32_t y = 0; y <= ny; y++) {
        for (uint32_t x = 0; x <= nx; x++) {
            // Grid vertex -> uv -> SCREEN pixels, inside the viewport. Top-left
            // origin throughout, which is both Metal's texture convention and
            // the one mapScreenToPhysicalCoordinates speaks, so no axis is
            // flipped anywhere in this function. The shader's separate question
            // of which corner is "up" in clip space is about the POSITION and
            // does not reach the coordinates built here.
            //
            // With a full-screen viewport this is exactly what it was before
            // the viewport existed, so the foveation measurements stand.
            float sx = vx + vw * (float)x / (float)nx;
            float sy = vy + vh * (float)y / (float)ny;
            float px = sx, py = sy;
            // No map is the identity, not a reason to give up: an unfoveated
            // eye with a shrunken viewport still has to be corrected, and it is
            // the commonest case of the two (KL_VRR=0, and every host run).
            if (fn) fn(ctx, sx, sy, &px, &py);
            out[base + y * (nx + 1) + x] = simd_make_float2(px / tex_w, py / tex_h);
        }
    }
}

// The rotation of a pose, as a matrix, with the translation dropped. Dropping
// it is the rotation-only decision in the header, not an oversight: a
// positional correction without per-pixel depth smears disocclusions.
static simd_float4x4 klr_rotation_of(simd_float4x4 m) {
    m.columns[3] = simd_make_float4(0, 0, 0, 1);
    // The other three columns can carry translation only through the last row,
    // which a rigid transform leaves as (0,0,0,1) anyway.
    return m;
}

static simd_float4x4 klr_rotation_of_quat(float x, float y, float z, float w) {
    // A pose that was never written is all zeros, and a zero quaternion is not
    // a rotation — it would collapse the quad to a point. Treat it as identity.
    float n = x * x + y * y + z * z + w * w;
    if (!(n > 1e-6f)) return matrix_identity_float4x4;
    return simd_matrix4x4(simd_quaternion(x, y, z, w));
}

// A full rigid transform from an ovrpPosef laid out as {qx,qy,qz,qw,px,py,pz}.
// Unlike klr_rotation_of_quat above this KEEPS the translation, which is the
// whole difference between an eye quad and an overlay: one is a picture placed
// in front of the eye, the other is an object somewhere in the room.
static simd_float4x4 klr_pose_matrix(const float p[7]) {
    simd_float4x4 m = klr_rotation_of_quat(p[0], p[1], p[2], p[3]);
    m.columns[3] = simd_make_float4(p[4], p[5], p[6], 1.0f);
    return m;
}

kl_reproject_uniforms kl_reproject_build(const kl_ovrp_render_pose *rendered, int eye,
                                         simd_float4x4 origin_from_device,
                                         simd_float4x4 device_from_view,
                                         simd_float4x4 projection,
                                         uint32_t slice, int flip_y,
                                         int grid_per_eye) {
    kl_reproject_uniforms u;
    u.projection = projection;
    u.slice = slice;
    // Which grid block this eye reads. `eye` and `slice` are not the same
    // number and neither could stand in for this one: an Array-layout guest has
    // slice == eye, a Stereo-layout one has two textures and slice 0 for both,
    // and a caller that built a single shared block wants block 0 whatever the
    // eye is.
    u.grid_eye = (grid_per_eye && eye == 1) ? 1u : 0u;
    // Passed in rather than read from a global, unlike srgb_decode below: the
    // flip is a property of ONE (eye, stage) texture and its recorded answer
    // lives in kl_glfb's eye table, which this file cannot reach without taking
    // a dependency `make reproject` exists without.
    u.flip_y = flip_y ? 1u : 0u;
    // Visible unless the caller says otherwise: only the visionOS compositor
    // has an eye it may have no picture for, and it clears the flag itself.
    u.visible = 1;
    // Not a caller's decision: it is a property of how the guest wrote the
    // texture, and both compositors must reach the same answer. See
    // kl_reproject_set_srgb_decode.
    u.srgb_decode = (uint32_t)kl_reproject_srgb_decode();
    // **500 m, and the 2 m that sat here was a misattribution.**
    //
    // The quad is eye-centred (device_from_view loses its translation below), so
    // it fills the viewport at ANY distance and our own picture does not depend
    // on this at all. What depends on it is the *system's* reprojection:
    // visionOS warps the submitted frame using the depth buffer we hand it, so
    // this number tells it how far away our content is, and it applies parallax
    // for a plane at that distance when the head translates between our
    // submission and scanout.
    //
    // A head *turn* is a rotation about the neck, so the eyes translate several
    // centimetres. At 2 m that is over a degree of parallax the system adds and
    // the content does not have — applied on the frames it chooses to correct
    // and not on the others, which is two images in two places. Placing the quad
    // far away makes the system's correction rotational, which is the only
    // correction that is right for a picture with no per-pixel depth.
    //
    // A frame that vanishes on device is depth WRITES being off, not this
    // distance: a depth buffer that is never written says "no geometry here"
    // whatever the quad's z is, and the attachment must be stored rather than
    // discarded (`.dontCare` -> `.store`). Distance cannot clip it either — the
    // drawable reports `depthRange = (far inf, near 0.1)`, an INFINITE far
    // plane. The compositor logs the quad's actual NDC depth beside that range,
    // so this is a number in the log rather than an argument.
    static float s_depth = 0.0f;
    if (s_depth == 0.0f) {
        float v = kl_env_float("KL_REPROJECT_DEPTH", 0.0);
        s_depth = (v > 0.0f) ? v : KL_REPROJECT_DEPTH;
    }
    u.depth = s_depth;

    if (rendered && rendered->serial) {
        const float *t = rendered->tangents[(unsigned)eye > 1 ? 0 : eye];
        u.tangents = simd_make_float4(t[0], t[1], t[2], t[3]);
    } else {
        // No record: describe the default symmetric 90° frustum kl_ovrp reports
        // when nothing has overridden it, so the picture is placed at the size
        // it was drawn at rather than at an invented one.
        u.tangents = simd_make_float4(1, 1, 1, 1);
    }

    // The quad lives in the space the render pose defines. Take it to the
    // world with that pose, back to the head with the pose the display will
    // have, and then to the eye with the drawable's own view transform:
    //
    //     view <- device <- world <- render
    //
    // and the model-view is that whole chain, which is the inverse of the
    // chain written the other way round. Only the two head poses have their
    // translation dropped; device_from_view keeps its eye offset, because that
    // is a property of the display and not part of the delta being corrected.
    //
    // With no record, the render rotation is defined to be the *display's* and
    // not identity. That is not a detail: a missing record means "we do not
    // know when this was drawn", and the only safe answer to that is to correct
    // nothing. Calling it identity instead would apply the head's entire
    // orientation as if it were a one-frame delta, and the picture would swing
    // around the room — which is what happens for every frame before the guest
    // first reaches ovrp_BeginFrame.
    simd_float4x4 device_rot = klr_rotation_of(origin_from_device);
    simd_float4x4 render_rot = rendered && rendered->serial
        ? klr_rotation_of_quat(rendered->qx, rendered->qy, rendered->qz, rendered->qw)
        : device_rot;
    // device_from_view loses its translation too, and that is a correction of a
    // real double-count rather than a simplification. The guest rendered THIS
    // eye's picture from THIS eye's position — the offset is already in the
    // pixels. Viewing a head-centred quad from an offset eye applies it a second
    // time. Dropping the offset makes the quad eye-centred, so it fills the
    // viewport exactly at ANY distance and the depth is free to be chosen for
    // what the DISPLAY needs rather than for what the geometry needs. See
    // KL_REPROJECT_DEPTH above for what the display needs it for.
    // ...and the eye's own rotation, which the guest was never told about. See
    // klr_nocant: keeping it places the picture turned by an angle it was not
    // drawn with, oppositely in the two eyes.
    simd_float4x4 view_rot = klr_nocant() ? matrix_identity_float4x4
                                          : klr_rotation_of(device_from_view);
    simd_float4x4 chain = simd_mul(simd_mul(simd_inverse(render_rot), device_rot),
                                   view_rot);
    u.model_view = simd_inverse(chain);
    return u;
}

kl_overlay_uniforms kl_overlay_build(const kl_ovrp_overlay *ov, int eye,
                                     simd_float4x4 origin_from_device,
                                     simd_float4x4 device_from_view,
                                     simd_float4x4 projection) {
    kl_overlay_uniforms u;
    memset(&u, 0, sizeof u);
    u.projection = projection;
    u.model_view = matrix_identity_float4x4;
    u.uv_rect    = simd_make_float4(0, 0, 1, 1);
    u.slice      = 0;
    u.visible    = 0;
    if (!ov) return u;

    // Only the Quad is implemented, and an unimplemented shape is REFUSED
    // rather than drawn as a quad: a cylinder or an equirect placed as a flat
    // rectangle is a wrong picture with no error surface, where an absent one
    // is the failure that is already happening and is at least honest. The
    // caller names it (see kl_reproject.h).
    if (ov->shape != 0) return u;
    if (!(ov->size[0] > 0.0f) || !(ov->size[1] > 0.0f)) return u;

    u.half_size = simd_make_float2(ov->size[0] * 0.5f, ov->size[1] * 0.5f);

    // Which part of the layer texture this eye reads. The guest states it in
    // PIXELS of its own texture, so it is carried as a fraction for the reason
    // kl_ovrp_render_pose.viewport_of exists: the texture a compositor is
    // holding and the rect the guest filed can be one generation apart, and a
    // fraction is right for either.
    const int *r = ov->viewport[(unsigned)eye > 1 ? 0 : eye];
    if (ov->tex_w > 0 && ov->tex_h > 0 && r[2] > 0 && r[3] > 0)
        u.uv_rect = simd_make_float4((float)r[0] / (float)ov->tex_w,
                                     (float)r[1] / (float)ov->tex_h,
                                     (float)r[2] / (float)ov->tex_w,
                                     (float)r[3] / (float)ov->tex_h);

    // world <- quad, then view <- world. Unlike the eye pass NOTHING is dropped
    // here: the layer has a position and the display's eye offset is what gives
    // it the stereo disparity a quad at 7 m is supposed to have.
    simd_float4x4 world_from_quad = klr_pose_matrix(ov->pose);
    if (ov->head_locked)
        // Stated, not silently treated as world-locked: the two differ by the
        // whole head pose. Nothing in the corpus submits one yet, so this arm
        // has never run against a guest.
        world_from_quad = simd_mul(origin_from_device, world_from_quad);
    simd_float4x4 world_from_view = simd_mul(origin_from_device, device_from_view);
    u.model_view = simd_mul(simd_inverse(world_from_view), world_from_quad);
    u.srgb_decode = (uint32_t)kl_reproject_srgb_decode();
    u.visible = 1;
    return u;
}

float kl_reproject_ndc_depth(const kl_reproject_uniforms *u) {
    if (!u) return -1.0f;
    // The quad's centre, in the same space the vertex shader builds it in.
    simd_float4 p = simd_mul(simd_mul(u->projection, u->model_view),
                             simd_make_float4(0, 0, -u->depth, 1));
    return (p.w != 0.0f) ? p.z / p.w : -1.0f;
}

simd_float4x4 kl_reproject_projection(float left, float right, float top, float bottom,
                                      float near_z) {
    // Reverse-Z with an infinite far plane: near maps to 1, infinity to 0.
    // Metal clips to 0 <= z <= w, and the quad sits at 500 m where reverse-Z
    // has plenty of precision left. This exists for the viewer, which has no
    // Compositor Services to ask for a matrix.
    float w = right + left, h = top + bottom;
    simd_float4x4 p = (simd_float4x4){{
        { 2.0f / w, 0, 0, 0 },
        { 0, 2.0f / h, 0, 0 },
        { (right - left) / w, (top - bottom) / h, 0, -1 },
        { 0, 0, near_z, 0 },
    }};
    return p;
}

simd_float4 kl_reproject_tangents(simd_float4x4 p) {
    // Recovered from the two rows that carry x and y, so the depth convention
    // (reverse-Z, infinite far) does not enter into it. For a near plane at n:
    //   m00 = 2n/(r-l), m20 = (r+l)/(r-l)  =>  r/n = (1 + m20)/m00
    // and the tangents are r/n, l/n, t/n, b/n — so n cancels and never has to
    // be known.
    float m00 = p.columns[0][0], m11 = p.columns[1][1];
    float m20 = p.columns[2][0], m21 = p.columns[2][1];
    if (!(fabsf(m00) > 1e-9f) || !(fabsf(m11) > 1e-9f))
        return simd_make_float4(1, 1, 1, 1);
    float right  = ( 1.0f + m20) / m00;
    float left   = (-1.0f + m20) / m00;
    float top    = ( 1.0f + m21) / m11;
    float bottom = (-1.0f + m21) / m11;
    return simd_make_float4(fabsf(left), fabsf(right), fabsf(top), fabsf(bottom));
}

float kl_reproject_delta_degrees(const kl_ovrp_render_pose *rendered,
                                 simd_float4x4 origin_from_device) {
    if (!rendered || !rendered->serial) return 0.0f;
    simd_quatf a = simd_quaternion(rendered->qx, rendered->qy, rendered->qz, rendered->qw);
    simd_quatf b = simd_quaternion(klr_rotation_of(origin_from_device));
    simd_quatf d = simd_mul(simd_inverse(a), b);
    // The angle of the delta rotation, taken the short way round.
    float w = simd_real(d);
    if (w < 0) w = -w;
    if (w > 1.0f) w = 1.0f;
    return 2.0f * acosf(w) * (180.0f / 3.14159265358979f);
}
