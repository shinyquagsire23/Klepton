// The reprojection pass, checked without a headset.
//
// Two halves, and the second is the one that would otherwise wait for a device:
//
//   1. The matrices (kl_reproject_build / _projection / _tangents). The
//      property that matters is that reprojection **reduces exactly to a blit**
//      when nothing has moved: with the display frustum equal to the render
//      frustum and no pose delta, the quad's four corners must land on NDC ±1
//      to the last bit. If that holds, a wrong picture on device is a pose
//      problem or a texture problem and never the geometry.
//
//   2. The shader compiles and a pipeline links. kl_reproject_msl() is a string
//      that is only ever compiled at runtime, by a compositor, on a device we
//      cannot run here — so without this it is unproven until the moment it
//      matters. The Metal compiler on the host is the same one, and it is the
//      MSL front end that would reject a typo.
//
// `make reproject`. Deliberately not part of `make check`: it needs Metal's
// compiler service, and the gate should not acquire a dependency on that
// (PLANNING's AGX arc — Metal's shader compiler is an XPC service with its own
// failure modes, and make check already re-execs around one of them).
#import <Metal/Metal.h>
#include <stdio.h>
#include <math.h>
#include "kl_reproject.h"

static int g_fail;

static void ok(int cond, const char *what) {
    printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) g_fail = 1;
}

// The vertex shader's geometry, in C. Kept in step with the MSL by hand — which
// is exactly why the corner assertion below is worth having: it pins the one
// property the two have to agree on.
static simd_float4 corner_ndc(kl_reproject_uniforms u, float cx, float cy) {
    float d = KL_REPROJECT_DEPTH;
    float x = (-u.tangents.x + (u.tangents.y + u.tangents.x) * cx) * d;
    float y = (-u.tangents.w + (u.tangents.z + u.tangents.w) * cy) * d;
    simd_float4 p = simd_mul(simd_mul(u.projection, u.model_view),
                             simd_make_float4(x, y, -d, 1));
    return simd_make_float4(p.x / p.w, p.y / p.w, p.z / p.w, p.w);
}

static void check_math(void) {
    printf("=== reprojection matrices ===\n");

    // An asymmetric frustum, because a symmetric one cannot tell left from
    // right or a transposed tangent order from a correct one.
    const float L = 1.1f, R = 0.9f, T = 1.3f, B = 1.2f;
    simd_float4x4 P = kl_reproject_projection(L, R, T, B, 0.03f);
    simd_float4 t = kl_reproject_tangents(P);
    ok(fabsf(t.x - L) < 1e-4f && fabsf(t.y - R) < 1e-4f &&
       fabsf(t.z - T) < 1e-4f && fabsf(t.w - B) < 1e-4f,
       "tangents survive a round trip through a projection");

    kl_ovrp_render_pose r = {0};
    r.serial = 1; r.qw = 1;
    for (int e = 0; e < 2; e++) {
        r.tangents[e][0] = L; r.tangents[e][1] = R;
        r.tangents[e][2] = T; r.tangents[e][3] = B;
    }

    // The identity case. This is the assertion the whole design rests on: the
    // pass is a blit when there is nothing to correct, so turning reprojection
    // on cannot change a picture that was already right.
    kl_reproject_uniforms u = kl_reproject_build(&r, 0, matrix_identity_float4x4,
                                                 matrix_identity_float4x4, P, 0);
    int exact = 1;
    for (int i = 0; i < 4; i++) {
        simd_float4 n = corner_ndc(u, (float)(i & 1), (float)(i >> 1));
        float wx = (i & 1) ? 1.0f : -1.0f, wy = (i >> 1) ? 1.0f : -1.0f;
        if (fabsf(n.x - wx) > 1e-4f || fabsf(n.y - wy) > 1e-4f) exact = 0;
        if (n.z < 0 || n.z > 1) exact = 0;      // inside Metal's clip range
    }
    ok(exact, "no pose delta, matching frustum -> the quad IS the viewport");
    ok(kl_reproject_delta_degrees(&r, matrix_identity_float4x4) < 1e-3f,
       "no pose delta measures as 0 degrees");

    // The quad's depth, named. This is the property that was settled wrongly
    // once — the default was pulled in to 2 m on the belief that a far quad is
    // discarded, when what had actually been measured was a quad with no depth
    // WRITES. Reverse-Z with an infinite far plane puts 500 m at a small
    // positive z, which is "far away" and not "clipped", and the two are only
    // distinguishable by looking.
    float z = kl_reproject_ndc_depth(&u);
    ok(z > 0.0f && z < 1.0f,
       "the default quad depth is inside the clip range, not beyond far");

    // A head that has yawed +10 degrees since the frame was rendered. Positive
    // rotation about +Y turns the head to the LEFT in a right-handed, -Z-forward
    // space, so the stale picture must move RIGHT in the view. The sign is the
    // whole test: an inverted delta looks plausible and doubles the error.
    simd_float4x4 dev = simd_matrix4x4(simd_quaternion(10.0f * (float)M_PI / 180.0f,
                                                       simd_make_float3(0, 1, 0)));
    kl_reproject_uniforms uy = kl_reproject_build(&r, 0, dev,
                                                  matrix_identity_float4x4, P, 0);
    float shift = corner_ndc(uy, 0.5f, 0.5f).x - corner_ndc(u, 0.5f, 0.5f).x;
    ok(shift > 0.10f && shift < 0.25f,
       "a +10 deg yaw moves the picture right, by about tan(10 deg)");
    ok(fabsf(kl_reproject_delta_degrees(&r, dev) - 10.0f) < 0.01f,
       "the delta measures 10 degrees");

    // Nothing recorded yet — every frame before the guest first reaches
    // ovrp_BeginFrame. "We do not know when this was drawn" must mean "correct
    // nothing", **even with the head somewhere other than the origin**: treat
    // the unknown render pose as identity instead and the whole head
    // orientation is applied as if it were one frame of delta, so the picture
    // swings around the room. Checked against a rotated device for exactly that
    // reason — against an identity one it cannot fail. The projection here is
    // the one matching the no-record default frustum, so the only thing left
    // that could move the corners is the rotation under test.
    simd_float4x4 Pd = kl_reproject_projection(1, 1, 1, 1, 0.03f);
    kl_reproject_uniforms un = kl_reproject_build(NULL, 0, dev,
                                                  matrix_identity_float4x4, Pd, 0);
    int quiet = 1;
    for (int i = 0; i < 4; i++) {
        simd_float4 n = corner_ndc(un, (float)(i & 1), (float)(i >> 1));
        float wx = (i & 1) ? 1.0f : -1.0f, wy = (i >> 1) ? 1.0f : -1.0f;
        if (!(fabsf(n.x - wx) < 1e-4f) || !(fabsf(n.y - wy) < 1e-4f)) quiet = 0;
    }
    ok(quiet, "no recorded pose reprojects by nothing, not by the head pose");
}

static void check_shader(void) {
    printf("=== shaders compile ===\n");
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { printf("  no Metal device — skipped\n"); return; }
    printf("  device: %s\n", dev.name.UTF8String);

    struct { const char *src, *vf, *ff, *what; } pass[] = {
        { kl_reproject_msl(),      "kl_reproject_v", "kl_reproject_f", "reprojection" },
        { kl_reproject_blit_msl(), "kl_blit_v",      "kl_blit_f",      "blit" },
    };
    for (unsigned i = 0; i < sizeof pass / sizeof *pass; i++) {
        NSError *err = nil;
        id<MTLLibrary> lib =
            [dev newLibraryWithSource:[NSString stringWithUTF8String:pass[i].src]
                              options:nil error:&err];
        if (!lib) {
            printf("  %s: %s\n", pass[i].what, err.localizedDescription.UTF8String);
            ok(0, pass[i].what); continue;
        }
        id<MTLFunction> vf = [lib newFunctionWithName:
                                 [NSString stringWithUTF8String:pass[i].vf]];
        id<MTLFunction> ff = [lib newFunctionWithName:
                                 [NSString stringWithUTF8String:pass[i].ff]];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = vf; pd.fragmentFunction = ff;
        // The eye textures are RGBA16F and so is the visionOS layer, so this is
        // the format the pipeline will really be linked against.
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        id<MTLRenderPipelineState> ps =
            [dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!ps) printf("  %s: %s\n", pass[i].what,
                        err.localizedDescription.UTF8String ?: "?");
        char msg[64];
        snprintf(msg, sizeof msg, "%s: vertex + fragment link", pass[i].what);
        ok(vf && ff && ps != nil, msg);
    }
}

// Compiling is not the same as producing the right pixels. Run the pass over a
// 2x2 source with a distinct colour in each texel and one decoy slice, and read
// the result back.
//
// This is what pins the three things that are invisible until a device shows
// them, and that no amount of matrix checking can reach:
//
//   - **The uniform layout.** The C struct and the MSL struct are two
//     declarations of one thing. If they disagree, the projection reads as
//     garbage and nothing lands on screen — on device that presents as "the
//     composite pass does nothing", which is indistinguishable from a dozen
//     other causes.
//   - **The orientation.** The v flip is the one piece of this that was derived
//     empirically rather than from first principles (see kl_reproject.c), so it
//     needs a test that would notice it inverting.
//   - **The slice.** Both eyes share one array texture, and sampling the wrong
//     slice shows the other eye — which looks like broken stereo, not like a
//     wrong constant.
// Bind the identity unwarp grid and draw the mesh.
//
// EVERY draw of this pass needs both — the vertex shader reads the grid's cell
// counts out of buffer(1), so a draw that binds nothing and asks for a 4-vertex
// strip (which is what these were before the grid existed) is degenerate rather
// than merely unfoveated. One helper, so a new case cannot get half of it: the
// slice-1 case did exactly that and the test caught it.
static void klr_draw(id<MTLRenderCommandEncoder> enc) {
    simd_float2 grid[kl_reproject_grid_entries(1, 1)];
    kl_reproject_grid_identity(grid);
    [enc setVertexBytes:grid length:sizeof grid atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
             vertexCount:kl_reproject_grid_vertices(1, 1)];
}

static void check_pixels(void) {
    printf("=== the pass, run ===\n");
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q = [dev newCommandQueue];
    if (!dev || !q) { printf("  no Metal device — skipped\n"); return; }

    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:
                              [NSString stringWithUTF8String:kl_reproject_msl()]
                                           options:nil error:&err];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"kl_reproject_v"];
    pd.fragmentFunction = [lib newFunctionWithName:@"kl_reproject_f"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
                                                                       error:&err];
    if (!ps) { ok(0, "pipeline for the pixel check"); return; }

    // Source: 2x2, two slices. Slice 0 carries the corners we look for; slice 1
    // is a decoy, so sampling the wrong slice cannot pass by accident.
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = MTLTextureType2DArray;
    td.pixelFormat = MTLPixelFormatRGBA8Unorm;
    td.width = 2; td.height = 2; td.arrayLength = 2;
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> src = [dev newTextureWithDescriptor:td];
    // Row 0 first, in Metal's memory order — the row sampled at v = 0.
    //
    // The eye texture this pass really consumes is written by GL through an
    // EGLImage, and GL's framebuffer origin is bottom-left: memory row 0 holds
    // the BOTTOM of the picture, not the top. That is the entire reason the
    // pass flips at all, so the source here is authored the same way — row 0 is
    // the picture's bottom — and the flipped expectation below is what "right
    // way up" then means. Modelling a replaceRegion-authored (top-down) source
    // instead would test the shader against a texture the shipping path never
    // sees, and would assert precisely the opposite flip.
    uint8_t s0[16] = { 0,0,255,255,   255,255,255,255,   // row 0 = picture BOTTOM
                       255,0,0,255,   0,255,0,255 };     // row 1 = picture TOP
    uint8_t s1[16] = { 0,255,255,255, 0,255,255,255,
                       0,255,255,255, 0,255,255,255 };   // all cyan
    [src replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 slice:0
             withBytes:s0 bytesPerRow:8 bytesPerImage:16];
    [src replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 slice:1
             withBytes:s1 bytesPerRow:8 bytesPerImage:16];

    MTLTextureDescriptor *rd = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                     width:2 height:2 mipmapped:NO];
    rd.usage = MTLTextureUsageRenderTarget;
    rd.storageMode = MTLStorageModeShared;
    id<MTLTexture> dst = [dev newTextureWithDescriptor:rd];

    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterNearest;   // so a texel is a texel
    sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;

    kl_ovrp_render_pose r = {0};
    r.serial = 1; r.qw = 1;
    for (int e = 0; e < 2; e++)
        for (int i = 0; i < 4; i++) r.tangents[e][i] = 1.0f;
    simd_float4x4 P = kl_reproject_projection(1, 1, 1, 1, 0.03f);
    kl_reproject_uniforms u = kl_reproject_build(&r, 0, matrix_identity_float4x4,
                                                 matrix_identity_float4x4, P, 0);

    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLCommandBuffer> cmd = [q commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:ps];
    [enc setFragmentTexture:src atIndex:0];
    [enc setFragmentSamplerState:[dev newSamplerStateWithDescriptor:sd] atIndex:0];
    [enc setVertexBytes:&u length:sizeof u atIndex:0];
    [enc setFragmentBytes:&u length:sizeof u atIndex:0];
    // The identity unwarp grid: this test's source texture is not foveated, so
    // the pass reduces to the two triangles it always drew. Bound anyway
    // because there is one code path — see kl_reproject.h.
    klr_draw(enc);
    [enc endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];

    uint8_t out[16] = {0};
    [dst getBytes:out bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];

    // The render target's row 0 is its top. The quad covers the whole viewport
    // (proved above), so output row 0 must be whatever the shader samples at
    // the top of the picture — and in a GL-authored source that is the LAST
    // row, not the first. So the expectation is the source with its rows
    // reversed: that is what an un-inverted picture looks like coming out.
    uint8_t want[16];
    memcpy(want,     s0 + 8, 8);     // source row 1 (picture top)    -> output top
    memcpy(want + 8, s0,     8);     // source row 0 (picture bottom) -> output bottom
    ok(memcmp(out, want, 16) == 0,
       "the pass reproduces the source, right way up, from the right slice");
    if (memcmp(out, want, 16) != 0)
        printf("    got  %3u,%3u,%3u | %3u,%3u,%3u\n"
               "         %3u,%3u,%3u | %3u,%3u,%3u\n"
               "    want %3u,%3u,%3u | %3u,%3u,%3u\n"
               "         %3u,%3u,%3u | %3u,%3u,%3u\n",
               out[0],out[1],out[2],  out[4],out[5],out[6],
               out[8],out[9],out[10], out[12],out[13],out[14],
               want[0],want[1],want[2],    want[4],want[5],want[6],
               want[8],want[9],want[10],   want[12],want[13],want[14]);

    // The same picture through the other slice must NOT be the same picture.
    u.slice = 1;
    cmd = [q commandBuffer];
    enc = [cmd renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:ps];
    [enc setFragmentTexture:src atIndex:0];
    [enc setFragmentSamplerState:[dev newSamplerStateWithDescriptor:sd] atIndex:0];
    [enc setVertexBytes:&u length:sizeof u atIndex:0];
    [enc setFragmentBytes:&u length:sizeof u atIndex:0];
    klr_draw(enc);
    [enc endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    uint8_t out1[16] = {0};
    [dst getBytes:out1 bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
    ok(memcmp(out1, s1, 16) == 0, "slice 1 samples the other eye's picture");

    // visible = 0 must draw NOTHING. This is how the visionOS pass expresses
    // "this eye has no picture yet" now that both eyes share one encoder and
    // one draw call — it can no longer just skip the draw. A collapse that
    // does not actually collapse would smear one eye's quad with whatever the
    // uniforms happened to contain, which on device is a wrong picture rather
    // than a missing one.
    u.visible = 0;
    cmd = [q commandBuffer];
    enc = [cmd renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:ps];
    [enc setFragmentTexture:src atIndex:0];
    [enc setFragmentSamplerState:[dev newSamplerStateWithDescriptor:sd] atIndex:0];
    [enc setVertexBytes:&u length:sizeof u atIndex:0];
    klr_draw(enc);
    [enc endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    uint8_t out2[16] = {0};
    [dst getBytes:out2 bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
    uint8_t cleared[16] = { 0,0,0,255, 0,0,0,255, 0,0,0,255, 0,0,0,255 };
    ok(memcmp(out2, cleared, 16) == 0,
       "visible = 0 collapses the quad — the clear survives");
}

int main(void) {
    @autoreleasepool {
        check_math();
        check_shader();
        check_pixels();
    }
    printf(g_fail ? "\n=== t_reproject FAILED ===\n"
                  : "\n=== t_reproject: the composite pass is a blit when nothing "
                    "has moved ===\n");
    return g_fail;
}
