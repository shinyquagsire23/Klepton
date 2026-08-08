// The reprojection pass — see kl_reproject.h for what it corrects and why the
// two compositors share it.
//
// Nothing here touches Metal or Compositor Services: it is matrices and a
// string, so it builds for the host and the device out of the same source list
// and can be reasoned about (and, on the host, run) without a headset.
#include <math.h>
#include <stdlib.h>
#include "kl_reproject.h"

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
"};\n"
"\n"
"struct VOut { float4 pos [[position]]; float2 uv; };\n"
"\n"
"// The guest's picture as a quad at KL_REPROJECT_DEPTH metres, sized by the\n"
"// tangents it was RENDERED with, in the space its render pose defines. The\n"
"// model-view then looks at it from where the head is now, and the projection\n"
"// is the one this drawable actually wants — so the stale pose and the\n"
"// frustum mismatch are corrected by the same two matrices.\n"
"vertex VOut kl_reproject_v(uint vid [[vertex_id]],\n"
"                           constant KLReproj &u [[buffer(0)]])\n"
"{\n"
"    // Triangle-strip corners: (0,0) (1,0) (0,1) (1,1).\n"
"    float2 c = float2(float(vid & 1u), float((vid >> 1) & 1u));\n"
"    float d = u.depth;          // KL_REPROJECT_DEPTH\n"
"    float x = mix(-u.tangents.x, u.tangents.y, c.x) * d;\n"
"    float y = mix(-u.tangents.w, u.tangents.z, c.y) * d;\n"
"    VOut o;\n"
"    o.pos = u.projection * u.modelView * float4(x, y, -d, 1.0);\n"
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
"    o.uv = float2(c.x, c.y);\n"
"    return o;\n"
"}\n"
"\n"
"fragment float4 kl_reproject_f(VOut in [[stage_in]],\n"
"                               texture2d_array<float> tex [[texture(0)]],\n"
"                               sampler samp [[sampler(0)]],\n"
"                               constant KLReproj &u [[buffer(0)]])\n"
"{\n"
"    return float4(tex.sample(samp, in.uv, u.slice).rgb, 1.0);\n"
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
"                                  sampler samp [[sampler(0)]],\n"
"                                  constant KLReproj &u [[buffer(0)]])\n"
"{\n"
"    return float4(tex.sample(samp, in.uv, u.slice).rgb, 1.0);\n"
"}\n"
"\n"
// The eye texture over the whole viewport, ignoring the quad, the poses and the
// projection entirely — the viewer's proven path, on device. Visible here but
// not at rung 2 means the picture is fine and the REPROJECTION GEOMETRY is what
// is dark. Uses the same uv convention as kl_blit_v below.
"vertex VOut kl_probe_full_v(uint vid [[vertex_id]])\n"
"{\n"
"    float2 p = float2((vid << 1) & 2, vid & 2);\n"
"    VOut o;\n"
"    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"    o.uv  = float2(p.x, p.y);\n"
"    return o;\n"
"}\n"
"\n"
"fragment float4 kl_probe_full_f(VOut in [[stage_in]],\n"
"                                texture2d_array<float> tex [[texture(0)]],\n"
"                                sampler samp [[sampler(0)]],\n"
"                                constant KLReproj &u [[buffer(0)]])\n"
"{\n"
"    return float4(tex.sample(samp, in.uv, u.slice).rgb, 1.0);\n"
"}\n";

// The pass that existed before reprojection: a full-screen triangle, three
// vertices, no buffer, no seam down the middle where two triangles meet. Kept
// as the A/B — it is the one known to produce the right picture, so a wrong
// reprojected frame can be told from a wrong *frame*.
static const char kl_msl_blit[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; float2 uv; };\n"
"vertex VOut kl_blit_v(uint vid [[vertex_id]]) {\n"
"    float2 p = float2((vid << 1) & 2, vid & 2);\n"
"    VOut o;\n"
"    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"    o.uv  = float2(p.x, p.y);\n"
"    return o;\n"
"}\n"
"fragment float4 kl_blit_f(VOut in [[stage_in]],\n"
"                          texture2d_array<float> tex [[texture(0)]],\n"
"                          sampler samp [[sampler(0)]],\n"
"                          constant uint &slice [[buffer(0)]]) {\n"
"    return tex.sample(samp, in.uv, slice);\n"
"}\n";

const char *kl_reproject_msl(void)      { return kl_msl_reproject; }
const char *kl_reproject_blit_msl(void) { return kl_msl_blit; }

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

kl_reproject_uniforms kl_reproject_build(const kl_ovrp_render_pose *rendered, int eye,
                                         simd_float4x4 origin_from_device,
                                         simd_float4x4 device_from_view,
                                         simd_float4x4 projection,
                                         uint32_t slice) {
    kl_reproject_uniforms u;
    u.projection = projection;
    u.slice = slice;
    u.pad[0] = u.pad[1] = 0;
    // 2 m, not 500. The old value existed so the eye offset would be negligible,
    // and with that offset now dropped above the quad fills the viewport at any
    // distance — so the only thing left to choose it by is what the DISPLAY
    // needs, and the display reprojects using depth. Reverse-Z with near = 0.1
    // makes 500 m a depth of 0.0002, which is the "infinitely far" value a
    // discarded frame has; 2 m is 0.05, which is the value measured to appear.
    static float s_depth = 0.0f;
    if (s_depth == 0.0f) {
        const char *e = getenv("KL_REPROJECT_DEPTH");
        float v = e ? (float)atof(e) : 0.0f;
        s_depth = (v > 0.0f) ? v : 2.0f;
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
    // time. At 500 m that error is negligible, which is exactly why the quad was
    // put there; but 500 m is reverse-Z depth 0.1/500 = 0.0002, and the device
    // discards a frame whose depth reads as infinity (measured: a quad at 0 with
    // no depth write is invisible, the same quad at 0.05 appears). Dropping the
    // offset makes the quad eye-centred, so it fills the viewport exactly at ANY
    // distance and the depth becomes free to choose. See KL_REPROJECT_DEPTH.
    simd_float4x4 view_rot = klr_rotation_of(device_from_view);
    simd_float4x4 chain = simd_mul(simd_mul(simd_inverse(render_rot), device_rot),
                                   view_rot);
    u.model_view = simd_inverse(chain);
    return u;
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
