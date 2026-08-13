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
                                                 matrix_identity_float4x4, P, 0, 0);
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
                                                  matrix_identity_float4x4, P, 0, 0);
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
                                                  matrix_identity_float4x4, Pd, 0, 0);
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
                                                 matrix_identity_float4x4, P, 0, 0);

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

    // ...and the same source read as a VULKAN guest's, which is the same
    // picture stored the other way up (kl_reproject.h, `flip_y`). The output
    // must then be the source in memory order, i.e. exactly the inverse of the
    // assertion above.
    //
    // This is worth a pass of its own rather than an inspection of the shader
    // because an upside-down composite is a *correct-looking* picture — every
    // Metal call succeeds, every counter is healthy, and the only instrument
    // that has ever caught it is a person looking at a loading screen. It is
    // also the one assertion here that would fail if the flip were applied
    // unconditionally, which is what both compositors did until this existed.
    u.flip_y = 1;
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
    uint8_t outf[16] = {0};
    [dst getBytes:outf bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
    ok(memcmp(outf, s0, 16) == 0,
       "flip_y = 1 reads a top-left-origin (Vulkan) picture the right way up");
    ok(memcmp(outf, want, 16) != 0,
       "...and that is NOT the same output as flip_y = 0");
    u.flip_y = 0;

    // ...and the same two answers through the BLIT, which is a separate shader
    // with a separate uv convention and is the viewer's DEFAULT path — the one
    // that actually ran when BONELAB's loading screen first appeared in a
    // window. Checking only the reprojection pass would leave the flip proven
    // in the shader nobody runs unless KL_VIEW_TIMEWARP is set.
    {
        NSError *berr = nil;
        id<MTLLibrary> blib = [dev newLibraryWithSource:
                                   [NSString stringWithUTF8String:kl_reproject_blit_msl()]
                                                options:nil error:&berr];
        MTLRenderPipelineDescriptor *bpd = [MTLRenderPipelineDescriptor new];
        bpd.vertexFunction = [blib newFunctionWithName:@"kl_blit_v"];
        bpd.fragmentFunction = [blib newFunctionWithName:@"kl_blit_f"];
        bpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> bps =
            [dev newRenderPipelineStateWithDescriptor:bpd error:&berr];
        if (!bps) ok(0, "pipeline for the blit's pixel check");
        for (int flip = 0; bps && flip < 2; flip++) {
            kl_blit_uniforms bu = { 0, (uint32_t)flip };
            cmd = [q commandBuffer];
            enc = [cmd renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:bps];
            [enc setFragmentTexture:src atIndex:0];
            [enc setFragmentSamplerState:[dev newSamplerStateWithDescriptor:sd] atIndex:0];
            [enc setFragmentBytes:&bu length:sizeof bu atIndex:0];
            // Three vertices, no grid: the blit is a full-screen triangle.
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];
            uint8_t b[16] = {0};
            [dst getBytes:b bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
            ok(memcmp(b, flip ? s0 : want, 16) == 0,
               flip ? "blit: flip_y = 1 reads a Vulkan picture the right way up"
                    : "blit: flip_y = 0 reads a GL picture the right way up");
        }
    }

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

    // The sRGB decode (kl_reproject.h). Checked on a MID grey, because the
    // source above is nothing but 0 and 255 — both fixed points of the curve —
    // so it would pass identically with the decode wired to nothing at all.
    // That is the failure this assertion exists for: an extra transfer function
    // in a composite has no error surface, produces a perfectly well-formed
    // frame, and is only visible to someone comparing brightness against the
    // machine that sent it.
    uint8_t grey[16];
    memset(grey, 128, sizeof grey);
    for (int i = 3; i < 16; i += 4) grey[i] = 255;
    [src replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 slice:1
             withBytes:grey bytesPerRow:8 bytesPerImage:16];
    // 128/255 = 0.5020 as an sRGB code value is 0.2159 in linear light, and the
    // render target here is a plain unorm, so the byte must land near 55.
    const int want_lin = (int)lroundf(255.0f *
        powf((128.0f / 255.0f + 0.055f) / 1.055f, 2.4f));
    for (int decode = 0; decode < 2; decode++) {
        u.srgb_decode = (uint32_t)decode;
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
        uint8_t g[16] = {0};
        [dst getBytes:g bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
        int want = decode ? want_lin : 128;
        int good = 1;
        for (int i = 0; i < 16; i += 4)
            if (abs((int)g[i] - want) > 1 || abs((int)g[i+1] - want) > 1 ||
                abs((int)g[i+2] - want) > 1) good = 0;
        printf("    srgb_decode=%d: 128 -> %u (want %d)\n", decode, g[0], want);
        ok(good, decode ? "srgb_decode = 1 takes the sample from sRGB to linear"
                        : "srgb_decode = 0 leaves the sample exactly alone");
    }
    u.srgb_decode = 0;
    [src replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 slice:1
             withBytes:s1 bytesPerRow:8 bytesPerImage:16];

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

// The crop, THROUGH THE PASS — not the grid arithmetic, which check_viewport
// above already covers on the CPU.
//
// The two are different assertions and only this one covers the wiring: a grid
// built perfectly and then bound to the wrong index, drawn with the vertex
// count of a different cell count, or read with the v axis running the other
// way all produce a correct table and a wrong picture. And that failure has no
// error surface anywhere — every Metal call succeeds and a plausible frame
// comes out — so the only instrument left is a person wearing the headset,
// which on this arc is the most expensive one there is.
//
// The source is 4x4 with the guest's picture in the BOTTOM-LEFT 2x2 (GL's
// origin, so the guest's {0,0,w/2,h/2} viewport lands there) and a marker
// colour everywhere else. If the crop is dropped, the marker is in the output;
// if the crop is applied to the wrong corner, the output is all marker. Both
// are the corner-of-the-eye bug, and this separates them.
static void check_crop_pixels(void) {
    printf("=== the crop, through the pass ===\n");
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
    if (!ps) { ok(0, "pipeline for the crop check"); return; }

    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = MTLTextureType2DArray;
    td.pixelFormat = MTLPixelFormatRGBA8Unorm;
    td.width = 4; td.height = 4; td.arrayLength = 2;
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> src = [dev newTextureWithDescriptor:td];

    // Memory row 0 is the picture's BOTTOM, exactly as in check_pixels and for
    // the same reason: this texture stands in for one GL wrote through an
    // EGLImage.
    const uint8_t M[4] = { 255, 0, 255, 255 };          // magenta: never drawn
    uint8_t s[4 * 4 * 4];
    for (int i = 0; i < 4 * 4; i++) memcpy(s + i * 4, M, 4);
    const uint8_t pic[4][4] = {                          // the guest's 2x2
        { 0, 0, 255, 255 }, { 255, 255, 255, 255 },      // row 0 = picture bottom
        { 255, 0, 0, 255 }, { 0, 255, 0, 255 },          // row 1 = picture top
    };
    memcpy(s + (0 * 4 + 0) * 4, pic[0], 4);
    memcpy(s + (0 * 4 + 1) * 4, pic[1], 4);
    memcpy(s + (1 * 4 + 0) * 4, pic[2], 4);
    memcpy(s + (1 * 4 + 1) * 4, pic[3], 4);
    [src replaceRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0 slice:0
             withBytes:s bytesPerRow:16 bytesPerImage:64];
    uint8_t decoy[4 * 4 * 4];
    for (int i = 0; i < 4 * 4; i++) { decoy[i*4]=0; decoy[i*4+1]=255; decoy[i*4+2]=255; decoy[i*4+3]=255; }
    [src replaceRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0 slice:1
             withBytes:decoy bytesPerRow:16 bytesPerImage:64];

    MTLTextureDescriptor *rd = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                     width:2 height:2 mipmapped:NO];
    rd.usage = MTLTextureUsageRenderTarget;
    rd.storageMode = MTLStorageModeShared;
    id<MTLTexture> dst = [dev newTextureWithDescriptor:rd];

    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterNearest;
    sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> samp = [dev newSamplerStateWithDescriptor:sd];

    kl_ovrp_render_pose r = {0};
    r.serial = 1; r.qw = 1;
    for (int e = 0; e < 2; e++)
        for (int i = 0; i < 4; i++) r.tangents[e][i] = 1.0f;
    simd_float4x4 P = kl_reproject_projection(1, 1, 1, 1, 0.03f);
    kl_reproject_uniforms u = kl_reproject_build(&r, 0, matrix_identity_float4x4,
                                                 matrix_identity_float4x4, P, 0, 0);

    // The guest's rect: the bottom-left quarter, as ovrp_CalculateEyeViewportRect
    // answers it and ovrp_EndFrame4 submits it.
    const float vp[4] = { 0, 0, 2, 2 };
    simd_float2 grid[kl_reproject_grid_entries(1, 1)];
    kl_reproject_grid_build(grid, 1, 1, vp, 4, 4, 4, 4, NULL, NULL);

    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLCommandBuffer> cmd = [q commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:ps];
    [enc setFragmentTexture:src atIndex:0];
    [enc setFragmentSamplerState:samp atIndex:0];
    [enc setVertexBytes:&u length:sizeof u atIndex:0];
    [enc setFragmentBytes:&u length:sizeof u atIndex:0];
    [enc setVertexBytes:grid length:sizeof grid atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
             vertexCount:kl_reproject_grid_vertices(1, 1)];
    [enc endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];

    uint8_t out[16] = {0};
    [dst getBytes:out bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];

    // Row-reversed, for the reason check_pixels spells out: output row 0 is the
    // top of the render target and the source's picture-top is its LAST row.
    uint8_t want[16];
    memcpy(want + 0, pic[2], 4); memcpy(want + 4,  pic[3], 4);
    memcpy(want + 8, pic[0], 4); memcpy(want + 12, pic[1], 4);
    int good = memcmp(out, want, 16) == 0;
    ok(good, "a cropped viewport composites the sub-rect the guest drew, "
             "full size and right way up");
    if (!good)
        printf("    got  %3u,%3u,%3u | %3u,%3u,%3u\n"
               "         %3u,%3u,%3u | %3u,%3u,%3u\n"
               "    want %3u,%3u,%3u | %3u,%3u,%3u\n"
               "         %3u,%3u,%3u | %3u,%3u,%3u\n",
               out[0],out[1],out[2],   out[4],out[5],out[6],
               out[8],out[9],out[10],  out[12],out[13],out[14],
               want[0],want[1],want[2],    want[4],want[5],want[6],
               want[8],want[9],want[10],   want[12],want[13],want[14]);

    // ...and the failing direction, so a pass here cannot be an accident of the
    // marker colour never being sampled: the identity grid over the SAME source
    // must show the magenta the crop excludes. If this one ever goes quiet, the
    // assertion above has stopped measuring anything.
    kl_reproject_grid_identity(grid);
    cmd = [q commandBuffer];
    enc = [cmd renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:ps];
    [enc setFragmentTexture:src atIndex:0];
    [enc setFragmentSamplerState:samp atIndex:0];
    [enc setVertexBytes:&u length:sizeof u atIndex:0];
    [enc setFragmentBytes:&u length:sizeof u atIndex:0];
    [enc setVertexBytes:grid length:sizeof grid atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
             vertexCount:kl_reproject_grid_vertices(1, 1)];
    [enc endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    uint8_t whole[16] = {0};
    [dst getBytes:whole bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
    int marker = 0;
    for (int i = 0; i < 16; i += 4)
        if (whole[i] == M[0] && whole[i+1] == M[1] && whole[i+2] == M[2]) marker++;
    ok(marker > 0, "...and dropping the crop puts the unwritten texels back on "
                   "screen — the corner-of-the-eye bug, reproduced");
}

// A stand-in rate map: screen -> physical, halving everything. Linear, so the
// grid reproduces it exactly at any cell count, which is what lets the
// composition below be asserted to the bit rather than to a tolerance.
static void half_s2p(void *ctx, float sx, float sy, float *px, float *py) {
    (void)ctx;
    *px = sx * 0.5f;
    *py = sy * 0.5f;
}

// The render viewport — the guest telling us it drew into only part of its eye
// texture, which is how a title lowers its render resolution without
// reallocating a swapchain (kl_ovrp.h). Ignoring it puts the picture in a
// corner of the eye with unwritten texels around it, and NOTHING reports that:
// every call succeeded and the guest rendered exactly what it was asked to.
// Beat Saber does this on entering a map.
//
// CPU only, because the whole failure lives in four texture coordinates.
static void check_viewport(void) {
    printf("=== the render viewport ===\n");
    const float W = 1000, H = 800;
    simd_float2 g[kl_reproject_grid_entries(2, 2)];

    // No crop, no map: the grid must be exactly what it was before any of this
    // existed, or every unscaled title regresses.
    kl_reproject_grid_build(g, 1, 1, NULL, W, H, W, H, NULL, NULL);
    ok(g[1].x == 0 && g[1].y == 0 && g[4].x == 1 && g[4].y == 1,
       "no viewport and no rate map is the identity grid");

    // A viewport of half the texture, unfoveated — the KL_VRR=0 case, and the
    // one the corner-of-the-eye bug lived on.
    const float vp[4] = { 0, 0, W * 0.5f, H * 0.5f };
    kl_reproject_grid_build(g, 1, 1, vp, W, H, W, H, NULL, NULL);
    ok(g[1].x == 0.0f && g[1].y == 0.0f &&
       g[4].x == 0.5f && g[4].y == 0.5f,
       "a half viewport samples the half of the texture the guest drew");

    // ...and composed with foveation, in that order: the viewport is in SCREEN
    // space, so the rate map maps what survives it. Half a screen through a
    // half-rate map is a quarter of the texture — which is also the arithmetic
    // that says the two corrections must not be applied to each other's output.
    kl_reproject_grid_build(g, 2, 2, vp, W, H, W, H, half_s2p, NULL);
    ok(g[1].x == 0.0f && g[1].y == 0.0f &&
       g[1 + 2 * 3 + 2].x == 0.25f && g[1 + 2 * 3 + 2].y == 0.25f,
       "viewport then rate map compose, in that order");
    // The middle vertex, which is what a 1x1 grid could not check: it must be
    // half way along the PHYSICAL span, not half way along the screen one.
    ok(fabsf(g[1 + 3 + 1].x - 0.125f) < 1e-6f &&
       fabsf(g[1 + 3 + 1].y - 0.125f) < 1e-6f,
       "the grid's interior follows the map inside the viewport");

    // A rect the guest could not have rendered into is refused rather than
    // handed to a rate map at coordinates it was not built for — Metal's answer
    // there is not defined by anything checkable.
    const float over[4] = { 0, 0, W * 4, H * 4 };
    kl_reproject_grid_build(g, 1, 1, over, W, H, W, H, NULL, NULL);
    ok(g[4].x == 1.0f && g[4].y == 1.0f,
       "a viewport past the screen is clamped, not trusted");

    // Zero is the "whole texture" spelling every guest that cannot answer uses
    // — 1.28's legacy VRDevice, the null driver, an OpenXR guest. It must not
    // read as an empty rect, which would draw nothing at all.
    const float zero[4] = { 0, 0, 0, 0 };
    kl_reproject_grid_build(g, 1, 1, zero, W, H, W, H, NULL, NULL);
    ok(g[4].x == 1.0f && g[4].y == 1.0f,
       "an unset viewport is the whole texture, not an empty one");
}

int main(void) {
    @autoreleasepool {
        check_math();
        check_viewport();
        check_shader();
        check_pixels();
        check_crop_pixels();
    }
    printf(g_fail ? "\n=== t_reproject FAILED ===\n"
                  : "\n=== t_reproject: the composite pass is a blit when nothing "
                    "has moved ===\n");
    return g_fail;
}
