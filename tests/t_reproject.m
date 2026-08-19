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
//  (Metal's shader compiler is an XPC service with its own failure modes, and make check already re-execs around one of them).
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
    // u.depth, NOT the compile-time default: the shader reads the uniform, and
    // a helper that hardcodes the constant silently places every quad at 500 m
    // however the pass was configured — which made the parallax assertion below
    // read as a correction that did not scale with depth.
    float d = u.depth;
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
                                                 matrix_identity_float4x4, P, 0, 0, 0);
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
                                                  matrix_identity_float4x4, P, 0, 0, 0);
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
                                                  matrix_identity_float4x4, Pd, 0, 0, 0);
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
//  - **The uniform layout.** The C struct and the MSL struct are two
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

// One encode of the real pass into `out`. Factored out when the chroma case
// arrived: two copies of an encoder is how one of them comes to bind a
// different sampler or skip the grid, and this test has already caught exactly
// that once.
static void klr_render(id<MTLDevice> dev, id<MTLCommandQueue> q,
                       id<MTLRenderPipelineState> ps, MTLSamplerDescriptor *sd,
                       id<MTLTexture> src, id<MTLTexture> dst,
                       const kl_reproject_uniforms *u, uint8_t out[16]) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLCommandBuffer> cmd = [q commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:ps];
    [enc setFragmentTexture:src atIndex:0];
    [enc setFragmentTexture:src atIndex:1];
    [enc setFragmentSamplerState:[dev newSamplerStateWithDescriptor:sd] atIndex:0];
    [enc setVertexBytes:u length:sizeof *u atIndex:0];
    [enc setFragmentBytes:u length:sizeof *u atIndex:0];
    klr_draw(enc);
    [enc endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    [dst getBytes:out bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
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
                                                 matrix_identity_float4x4, P, 0, 0, 0);

    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLCommandBuffer> cmd = [q commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:ps];
    [enc setFragmentTexture:src atIndex:0];
    [enc setFragmentTexture:src atIndex:1];
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

    // ---- the chroma key, through the real pass ------------------------------
    //
    // Ported from VisionOSALVRClient with its maths and its defaults intact, so
    // what is asserted is that the KEY COLOUR goes to alpha 0 while a colour
    // that is not the key survives. Both halves matter: a matte that keys
    // everything looks exactly like a matte that works, right up until there is
    // something in the scene you wanted to keep.
    {
        const float key[3] = { 16.0f / 255.0f, 124.0f / 255.0f, 16.0f / 255.0f };
        uint8_t ck[16] = { 16,124,16,255,   16,124,16,255,     // the key colour
                           220,30,30,255,   16,124,16,255 };   // ...and one red
        [src replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 slice:0
                 withBytes:ck bytesPerRow:8 bytesPerImage:16];

        kl_reproject_set_chroma(1, key, 0.35f, 0.7f);
        kl_reproject_uniforms cu =
            kl_reproject_build(&r, 0, matrix_identity_float4x4,
                               matrix_identity_float4x4, P, 0, 0, 0);
        uint8_t cout[16] = {0};
        klr_render(dev, q, ps, sd, src, dst, &cu, cout);
        // Output row 0 is the picture's TOP, which is SOURCE row 1 — so the red
        // texel (memory row 1, left) lands at the output's top left.
        int red_kept  = cout[3] == 255 && cout[0] > 180;
        int green_out = cout[8 + 3] == 0 && cout[12 + 3] == 0;
        ok(red_kept && green_out,
           "the key colour mattes to alpha 0 and a non-key colour survives");

        // ...and off is a true passthrough, not a mask that happens to be 1.
        // This is the assertion that keeps every other guest unaffected.
        kl_reproject_set_chroma(0, key, 0.35f, 0.7f);
        kl_reproject_uniforms nu =
            kl_reproject_build(&r, 0, matrix_identity_float4x4,
                               matrix_identity_float4x4, P, 0, 0, 0);
        uint8_t nout[16] = {0};
        klr_render(dev, q, ps, sd, src, dst, &nu, nout);
        ok(nout[3] == 255 && nout[7] == 255 && nout[11] == 255 && nout[15] == 255,
           "with the key off every pixel comes through opaque");
        [src replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 slice:0
                 withBytes:s0 bytesPerRow:8 bytesPerImage:16];
    }
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
    [enc setFragmentTexture:src atIndex:1];
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

    // ...and the same two answers through the BLIT, a separate shader with a
    // separate uv convention: it drives the viewer's liveness downsample and
    // stands in whenever the reprojection shader fails to compile.
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
            [enc setFragmentTexture:src atIndex:1];
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
    [enc setFragmentTexture:src atIndex:1];
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
        [enc setFragmentTexture:src atIndex:1];
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
    [enc setFragmentTexture:src atIndex:1];
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
                                                 matrix_identity_float4x4, P, 0, 0, 0);

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
    [enc setFragmentTexture:src atIndex:1];
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
    [enc setFragmentTexture:src atIndex:1];
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

    // TWO EYES, TWO CROPS, ONE GRID — the symmetric-projection case.
//
// A guest using Oculus symmetric projection renders both eyes with one union
// frustum into one widened texture and submits a DIFFERENT sub-rect per eye
// (measured on BONELAB: eye 0 at x=0, eye 1 at x=609, both 2271 wide of 2880).
// The composite draws both eyes from one grid buffer, so the
// buffer carries a BLOCK PER EYE and kl_reproject_uniforms.grid_eye says which
// one a view reads.
//
// Nothing else can check this. Both blocks are built by code check_viewport
// already covers, both draws succeed, and reading the wrong block is a picture
// shifted by the offset between the two rects — which on a headset is a warped,
// displaced right eye and from inside the process is silence.
//
// The source is 4x1 columns of four distinct colours. Eye 0's rect is the left
// half, eye 1's the right; if the blocks are laid out or indexed wrongly the two
// draws produce the same two columns, which the failing-direction check below
// asserts really would be visible.
static void check_split_crop_pixels(void) {
    printf("=== two eyes, two crops, one grid ===\n");
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
    if (!ps) { ok(0, "pipeline for the split-crop check"); return; }

    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = MTLTextureType2DArray;
    td.pixelFormat = MTLPixelFormatRGBA8Unorm;
    td.width = 4; td.height = 2; td.arrayLength = 1;
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> src = [dev newTextureWithDescriptor:td];
// Four columns, four colours, both rows the same — so a HORIZONTAL shift is
    // the only thing this can see, which is the only thing the two rects differ
    // by.
    const uint8_t col[4][4] = { { 255,0,0,255 }, { 0,255,0,255 },
                                { 0,0,255,255 }, { 255,255,255,255 } };
    uint8_t s[4 * 2 * 4];
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 4; x++) memcpy(s + (y * 4 + x) * 4, col[x], 4);
    [src replaceRegion:MTLRegionMake2D(0,0,4,2) mipmapLevel:0 slice:0
             withBytes:s bytesPerRow:16 bytesPerImage:32];

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

    // One buffer, two blocks. The left half is eye 0's, the right half eye 1's.
    const float vp0[4] = { 0, 0, 2, 2 }, vp1[4] = { 2, 0, 2, 2 };
    simd_float2 grid[kl_reproject_grid_entries_n(1, 1, 2)];
    kl_reproject_grid_build_eye(grid, 0, 1, 1, vp0, 4, 2, 4, 2, NULL, NULL);
    kl_reproject_grid_build_eye(grid, 1, 1, 1, vp1, 4, 2, 4, 2, NULL, NULL);
    ok(grid[1].x == 0.0f && grid[2].x == 0.5f,
       "eye 0's block is still its own after eye 1's is written");
    ok(grid[5].x == 0.5f && grid[6].x == 1.0f,
       "eye 1's block starts one stride on and carries its own rect");

    uint8_t got[2][16];
    for (int eye = 0; eye < 2; eye++) {
        kl_reproject_uniforms u =
            kl_reproject_build(&r, eye, matrix_identity_float4x4,
                               matrix_identity_float4x4, P, 0, 0, /*grid_per_eye*/1);
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = dst;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        id<MTLCommandBuffer> cmd = [q commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:ps];
        [enc setFragmentTexture:src atIndex:0];
        [enc setFragmentTexture:src atIndex:1];
        [enc setFragmentSamplerState:samp atIndex:0];
        [enc setVertexBytes:&u length:sizeof u atIndex:0];
        [enc setFragmentBytes:&u length:sizeof u atIndex:0];
        [enc setVertexBytes:grid length:sizeof grid atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
                 vertexCount:kl_reproject_grid_vertices(1, 1)];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        [dst getBytes:got[eye] bytesPerRow:8
           fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
    }

    int e0 = memcmp(got[0] + 0, col[0], 3) == 0 && memcmp(got[0] + 4, col[1], 3) == 0;
    int e1 = memcmp(got[1] + 0, col[2], 3) == 0 && memcmp(got[1] + 4, col[3], 3) == 0;
    ok(e0, "eye 0 composites the left sub-rect the guest drew for it");
    ok(e1, "eye 1 composites its OWN sub-rect, not eye 0's");
    if (!e0 || !e1)
        printf("    eye0 %3u,%3u,%3u | %3u,%3u,%3u    eye1 %3u,%3u,%3u | %3u,%3u,%3u\n",
               got[0][0],got[0][1],got[0][2], got[0][4],got[0][5],got[0][6],
               got[1][0],got[1][1],got[1][2], got[1][4],got[1][5],got[1][6]);
    // The failing direction: the two eyes must not have produced the same
    // picture, or the assertions above would pass with the indexing removed.
    ok(memcmp(got[0], got[1], 16) != 0,
       "...and reading one block for both eyes really is a different picture");
}

    // Two eyes that are two TEXTURES, not two slices of one.
//
// Every other case in this file composites from a single 2-slice array, because
// until Open Brush that is all anything produced: the eye provider allocates
// exactly that by construction, and a Vulkan guest whose eye IS the layer hands
// over the same shape. An OpenXR guest on Vulkan gets ONE SWAPCHAIN PER EYE, so
// the two eyes are two unrelated MTLTextures — and the composite bound one
// texture and dropped any view that did not match it, which took the right eye
// out of the picture for the whole run.
//
// That failure has no error surface at all. Every call succeeds, the counters
// stay healthy, the left eye is perfect, and the only instrument that can see it
// is a person wearing the headset. So it is gated here, in the shape
// KleptonCompositor really encodes: two views, ONE draw, per-view render-target
// slices, and the source texture selected by [[amplification_id]].
//
// The sources are FLAT colours on purpose. Every other case in this file is
// about where a texel lands; this one is only ever about WHICH TEXTURE was
// read, and a flat source means a wrong answer cannot hide in a flip or a
// crop.
static void check_two_textures(void) {
    printf("=== two eyes, two TEXTURES, one amplified pass ===\n");
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q = [dev newCommandQueue];
    if (!dev || !q) { printf("  no Metal device — skipped\n"); return; }
    if (![dev supportsVertexAmplificationCount:2]) {
        printf("  no vertex amplification on this device — skipped\n");
        return;
    }

    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:
                              [NSString stringWithUTF8String:kl_reproject_msl()]
                                           options:nil error:&err];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"kl_reproject_v"];
    pd.fragmentFunction = [lib newFunctionWithName:@"kl_reproject_f"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
// The whole point of the pass: both eyes in one draw.
    pd.maxVertexAmplificationCount = 2;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
                                                                       error:&err];
    if (!ps) { ok(0, "pipeline for the two-texture check"); return; }

    // One 2x2 array texture per eye, one layer each — which is exactly what
    // MoltenVK hands back for a per-eye Vulkan swapchain image, once
    // klvm_array_view has made an array view of it.
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = MTLTextureType2DArray;
    td.pixelFormat = MTLPixelFormatRGBA8Unorm;
    td.width = 2; td.height = 2; td.arrayLength = 1;
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> eye0 = [dev newTextureWithDescriptor:td];
    id<MTLTexture> eye1 = [dev newTextureWithDescriptor:td];
    uint8_t red[16], green[16];
    for (int i = 0; i < 4; i++) {
        red[i*4+0] = 255; red[i*4+1] = 0;   red[i*4+2] = 0; red[i*4+3] = 255;
        green[i*4+0] = 0; green[i*4+1] = 255; green[i*4+2] = 0; green[i*4+3] = 255;
    }
    [eye0 replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 slice:0
              withBytes:red bytesPerRow:8 bytesPerImage:16];
    [eye1 replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 slice:0
              withBytes:green bytesPerRow:8 bytesPerImage:16];

    // The destination is the drawable: one texture, one layer per view.
    MTLTextureDescriptor *rd = [MTLTextureDescriptor new];
    rd.textureType = MTLTextureType2DArray;
    rd.pixelFormat = MTLPixelFormatRGBA8Unorm;
    rd.width = 2; rd.height = 2; rd.arrayLength = 2;
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
    // One uniform per amplified view, in the pass's own order — the array the
    // vertex shader indexes with [[amplification_id]]. Slice 0 for both, which
    // is the point: with a texture each there is no slice left to tell them
    // apart, and that is precisely what the old pass had no way to express.
    kl_reproject_uniforms u[2];
    for (int e = 0; e < 2; e++)
        u[e] = kl_reproject_build(&r, e, matrix_identity_float4x4,
                                  matrix_identity_float4x4, P, 0, 0, 0);

    // Bound in the order the views were amplified: slot 0 is view 0's, slot 1 is
    // view 1's. `swap` reruns the whole pass with ONE texture in both slots,
    // which is what every guest before Open Brush looks like — and it is also
    // the failing direction for the assertions below.
    uint8_t got[2][2][16];
    for (int swap = 0; swap < 2; swap++) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = dst;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
    // The array LENGTH, not a slice — this is what lets amplification reach
        // layer 1 at all (KleptonCompositor.encodeViews).
        rp.renderTargetArrayLength = 2;
        id<MTLCommandBuffer> cmd = [q commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:ps];
        [enc setViewport:(MTLViewport){ 0, 0, 2, 2, 0, 1 }];
        // One viewport, so both views index it; the render-target slice is what
        // differs. The shipping compositor gives each view its own viewport out
        // of the drawable's textureMap, which is a different number and the same
        // mechanism.
        MTLVertexAmplificationViewMapping maps[2] = {
            { .viewportArrayIndexOffset = 0, .renderTargetArrayIndexOffset = 0 },
            { .viewportArrayIndexOffset = 0, .renderTargetArrayIndexOffset = 1 },
        };
        [enc setVertexAmplificationCount:2 viewMappings:maps];
        [enc setFragmentTexture:eye0 atIndex:0];
        [enc setFragmentTexture:(swap ? eye0 : eye1) atIndex:1];
        [enc setFragmentSamplerState:samp atIndex:0];
        [enc setVertexBytes:u length:sizeof u atIndex:0];
        klr_draw(enc);
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        for (int layer = 0; layer < 2; layer++)
            [dst getBytes:got[swap][layer] bytesPerRow:8 bytesPerImage:16
               fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 slice:layer];
    }

    ok(memcmp(got[0][0], red, 16) == 0,
       "view 0 samples texture slot 0");
    ok(memcmp(got[0][1], green, 16) == 0,
       "view 1 samples slot 1 — the right eye is its OWN texture, not eye 0's");
    if (memcmp(got[0][1], green, 16) != 0)
        printf("    eye 1 came out %u,%u,%u — %s\n",
               got[0][1][0], got[0][1][1], got[0][1][2],
               got[0][1][2] == 255 ? "the CLEAR, so the view was dropped"
                                   : "eye 0's picture");
        // The failing direction, which is also every earlier guest: one texture in
    // both slots really does put eye 0's picture in both eyes. Without this the
    // assertion above would pass just as well with the second binding removed
    // and the shader reading slot 0 unconditionally.
    ok(memcmp(got[1][1], red, 16) == 0,
       "...and binding one texture to both slots puts eye 0 in both, as it did before");
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

// ---------------------------------------------------------------------------
// The OVERLAY pass — a guest's non-eye layer, which for a guest in cinematic
// mode is the whole picture (JKXR submits one quad and nothing else).
//
// It is a different pass from the reprojection above and it differs in exactly
// the two ways that have no error surface at all:
//
//   * it KEEPS translation. The eye quad is eye-centred on purpose, so a
//     placement bug there is invisible; an overlay is somewhere, and a
//     model-view that dropped the translation would put every panel dead ahead
//     no matter where the guest asked for it — which reads as "the menu follows
//     you" rather than as a matrix error.
//   * its `flip_y` means the same thing as the reprojection pass's. It used to
//     mean the opposite (the base mapping was top-left there and bottom-left
//     here), and both records are read out of one struct — so one field, two
//     polarities, and the only instrument is a person seeing an upside-down
//     menu.
static simd_float4 overlay_corner_ndc(kl_overlay_uniforms u, float cx, float cy) {
    simd_float4 p = simd_mul(simd_mul(u.projection, u.model_view),
                             simd_make_float4(cx * u.half_size.x,
                                              cy * u.half_size.y, 0, 1));
    return simd_make_float4(p.x / p.w, p.y / p.w, p.z / p.w, p.w);
}

// A head pose as a matrix, the way both compositors build one.
static simd_float4x4 head_at(float x, float y, float z) {
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[3] = simd_make_float4(x, y, z, 1);
    return m;
}

// The per-layer projection composite: several projection layers, each placed by
// its OWN frustum against the display and never against another layer.
//
// This is the case the flattening it replaced could not state. To lay a narrow
// layer over a wide one, that model had to compute where the narrow frustum
// falls inside the wide one as a fraction, decide which layer was "the eye",
// and decide by threshold which of the others counted as an inset. Here the
// placement is the same arithmetic the eye quad has always used, so the
// assertion is that a layer's picture lands at exactly the angular size it was
// drawn with — which is what makes the seam a geometric identity rather than a
// number to tune.
static void check_proj_layers(void) {
    printf("=== per-layer projection composite ===\n");
    // The display's frustum, i.e. the topmost layer's: symmetric 45 degrees.
    simd_float4x4 P = kl_reproject_projection(1, 1, 1, 1, 0.03f);
    simd_float4x4 head = matrix_identity_float4x4;

    kl_ovrp_proj_layer base = {0};
    base.slot[0] = 3; base.slot[1] = 4;
    base.pose[6] = 1.0f;                       // identity orientation
    for (int e = 0; e < 2; e++) {
        base.tangents[e][0] = base.tangents[e][1] = 1.0f;
        base.tangents[e][2] = base.tangents[e][3] = 1.0f;
    }
    kl_reproject_uniforms u =
        kl_proj_layer_build(&base, 0, head, matrix_identity_float4x4, P, 0);
    simd_float4 bl = corner_ndc(u, 0, 0), tr = corner_ndc(u, 1, 1);
    ok(u.visible &&
       fabsf(bl.x + 1) < 1e-4f && fabsf(bl.y + 1) < 1e-4f &&
       fabsf(tr.x - 1) < 1e-4f && fabsf(tr.y - 1) < 1e-4f,
       "a layer drawn with the display's frustum fills the viewport exactly");

    // The inset: HALF the tangents, so half the angular size. Its corners land
    // at +-0.5 in NDC — a quarter of the area, dead centre — with no rect, no
    // containment test and no threshold anywhere in the arithmetic.
    kl_ovrp_proj_layer inset = base;
    inset.slot[0] = 5; inset.slot[1] = 6;
    for (int e = 0; e < 2; e++)
        for (int k = 0; k < 4; k++) inset.tangents[e][k] = 0.5f;
    kl_reproject_uniforms ui =
        kl_proj_layer_build(&inset, 0, head, matrix_identity_float4x4, P, 0);
    simd_float4 ibl = corner_ndc(ui, 0, 0), itr = corner_ndc(ui, 1, 1);
    ok(fabsf(ibl.x + 0.5f) < 1e-4f && fabsf(ibl.y + 0.5f) < 1e-4f &&
       fabsf(itr.x - 0.5f) < 1e-4f && fabsf(itr.y - 0.5f) < 1e-4f,
       "a half-tangent layer lands on exactly the middle half of the viewport");

    // ...and an ASYMMETRIC inset, because a symmetric one cannot tell a
    // correct placement from one that centres everything it is given. This is
    // the shape the guest actually submits: the streamed inset's frustum is
    // asymmetric and it MOVES while running.
    kl_ovrp_proj_layer off = base;
    for (int e = 0; e < 2; e++) {
        off.tangents[e][0] = 0.2f; off.tangents[e][1] = 0.6f;   // left, right
        off.tangents[e][2] = 0.5f; off.tangents[e][3] = 0.1f;   // top, bottom
    }
    kl_reproject_uniforms uo =
        kl_proj_layer_build(&off, 0, head, matrix_identity_float4x4, P, 0);
    simd_float4 obl = corner_ndc(uo, 0, 0), otr = corner_ndc(uo, 1, 1);
    ok(fabsf(obl.x + 0.2f) < 1e-4f && fabsf(otr.x - 0.6f) < 1e-4f &&
       fabsf(obl.y + 0.1f) < 1e-4f && fabsf(otr.y - 0.5f) < 1e-4f,
       "an asymmetric layer lands off-centre, exactly where its tangents say");

    // Each layer keeps its own placement: building one does not disturb the
    // other, which is the property a shared destination slot destroyed.
    kl_reproject_uniforms u2 =
        kl_proj_layer_build(&base, 0, head, matrix_identity_float4x4, P, 0);
    simd_float4 b2 = corner_ndc(u2, 1, 1);
    ok(fabsf(b2.x - tr.x) < 1e-6f && fabsf(b2.y - tr.y) < 1e-6f,
       "compositing the inset leaves the base's placement untouched");

    // An eye the layer does not name is REFUSED, not drawn with the other
    // eye's picture: a one-view layer says so with slot -1.
    kl_ovrp_proj_layer mono = base;
    mono.slot[1] = -1;
    ok(kl_proj_layer_build(&mono, 1, head, matrix_identity_float4x4, P, 0).visible == 0 &&
       kl_proj_layer_build(&mono, 0, head, matrix_identity_float4x4, P, 0).visible == 1,
       "a layer that names one eye is drawn in that eye only");

    // The pose is per LAYER, so the reprojection delta is too: a layer rendered
    // against a head that has since turned comes back rotated, and one rendered
    // against the current head does not move at all.
    simd_float4x4 turned = simd_matrix4x4(simd_quaternion(0.0f, 0.2588f, 0.0f, 0.9659f));
    kl_reproject_uniforms us =
        kl_proj_layer_build(&base, 0, turned, matrix_identity_float4x4, P, 0);
    simd_float4 sbl = corner_ndc(us, 0, 0);
    ok(fabsf(sbl.x - bl.x) > 0.1f,
       "a 30-degree head turn since the layer was drawn moves that layer's quad");

    // ---- the head TRANSLATING is parallax, and only at a finite depth -------
    //
    // A head turn rotates about the neck, so the eyes translate several
    // centimetres; uncorrected, that is the lateral swim measured on device.
    // The correction is a shift of the quad by the head's displacement, which
    // at depth d is an angular shift of |dP| / d — so it MUST scale with the
    // depth, and at the 500 m default it must be nothing at all. Both halves
    // are asserted: a correction that fired at 500 m would be a correction
    // nobody could turn off.
    simd_float4x4 moved = head;
    moved.columns[3] = simd_make_float4(0.10f, 0, 0, 1);   // 10 cm to the right
    simd_float4 far_x = corner_ndc(
        kl_proj_layer_build(&base, 0, moved, matrix_identity_float4x4, P, 0), 0, 0);
    ok(fabsf(far_x.x - bl.x) < 1e-3f,
       "at 500 m a 10 cm head translation moves the quad by nothing");

    setenv("KL_REPROJECT_DEPTH", "2.0", 1);
    kl_reproject_reset_depth();
    simd_float4 near_still = corner_ndc(
        kl_proj_layer_build(&base, 0, head, matrix_identity_float4x4, P, 0), 0, 0);
    simd_float4 near_moved = corner_ndc(
        kl_proj_layer_build(&base, 0, moved, matrix_identity_float4x4, P, 0), 0, 0);
    // Right by 10 cm at 2 m is 0.05 rad of parallax; the picture must move
    // LEFT, which is -x, and by a distance a person would see.
    ok(near_moved.x - near_still.x < -0.01f,
       "at 2 m the same translation moves the quad the other way, as parallax");
    unsetenv("KL_REPROJECT_DEPTH");
    kl_reproject_reset_depth();

    // The flip travels with the record, not with the caller: a layer Vulkan
    // drew has its origin at the top left and the shader is told so.
    kl_ovrp_proj_layer vk = base;
    vk.origin_top_left = 1;
    ok(kl_proj_layer_build(&vk, 0, head, matrix_identity_float4x4, P, 0).flip_y == 1 &&
       u.flip_y == 0,
       "origin_top_left reaches the shader from the layer's own record");
}

static void check_overlay_math(void) {
    printf("=== overlay (quad layer) placement ===\n");
    simd_float4x4 P = kl_reproject_projection(1, 1, 1, 1, 0.03f);

    // A 2x2 m panel one metre in front of a head at the origin: through a
    // 90-degree frustum its corners land exactly on the viewport edges, which
    // is the same "reduces to a blit" property the eye pass is pinned by and
    // for the same reason — every other number here is checked against it.
    kl_ovrp_overlay ov = {0};
    ov.shape = 0;
    ov.tex_w = 100; ov.tex_h = 100;
    for (int e = 0; e < 2; e++) {
        ov.viewport[e][2] = 100; ov.viewport[e][3] = 100;
    }
    ov.pose[3] = 1;                       // identity orientation
    ov.pose[6] = -1.0f;                   // one metre down -Z
    ov.size[0] = 2.0f; ov.size[1] = 2.0f;

    kl_overlay_uniforms u = kl_overlay_build(&ov, 0, matrix_identity_float4x4,
                                             matrix_identity_float4x4, P);
    ok(u.visible != 0, "a quad with a size is placed");
    simd_float4 tl = overlay_corner_ndc(u, -1, 1), br = overlay_corner_ndc(u, 1, -1);
    ok(fabsf(tl.x + 1) < 1e-5f && fabsf(tl.y - 1) < 1e-5f &&
       fabsf(br.x - 1) < 1e-5f && fabsf(br.y + 1) < 1e-5f,
       "a 2x2 m quad at 1 m fills a 90-degree viewport exactly");

    // ...and the whole difference from the eye pass: MOVE THE HEAD. A metre to
    // the right, and the panel must move a full viewport to the left, because
    // it is at a place and the head is not there any more.
    kl_overlay_uniforms moved = kl_overlay_build(&ov, 0, head_at(1, 0, 0),
                                                 matrix_identity_float4x4, P);
    simd_float4 c = overlay_corner_ndc(moved, 0, 0);
    ok(fabsf(c.x + 1) < 1e-5f && fabsf(c.y) < 1e-5f,
       "the head moving 1 m right moves the panel a full viewport left "
       "(translation is KEPT, unlike the eye quad)");

    // The eye's own offset counts too — that is what gives a panel at a few
    // metres the disparity that makes it look like it is there.
    kl_overlay_uniforms right_eye =
        kl_overlay_build(&ov, 1, matrix_identity_float4x4, head_at(0.032f, 0, 0), P);
    simd_float4 rc = overlay_corner_ndc(right_eye, 0, 0);
    ok(rc.x < -0.03f && rc.x > -0.04f,
       "the display's eye offset shifts the panel, which is its stereo");

    // eyeVisibility. LEFT and RIGHT are how a guest shows different pixels to
    // each eye, and drawing one of those in both eyes is the other eye's
    // picture on top of this one's.
    ov.eye_visibility = 2;                                   // RIGHT only
    ok(kl_overlay_build(&ov, 0, matrix_identity_float4x4,
                        matrix_identity_float4x4, P).visible == 0 &&
       kl_overlay_build(&ov, 1, matrix_identity_float4x4,
                        matrix_identity_float4x4, P).visible != 0,
       "eyeVisibility RIGHT draws in the right eye only");
    ov.eye_visibility = 0;

    // A shape this pass cannot draw is REFUSED rather than drawn as a flat
    // rectangle — see kl_overlay_build. Cylinder is the one a guest is most
    // likely to submit next.
    ov.shape = 1;
    ok(kl_overlay_build(&ov, 0, matrix_identity_float4x4,
                        matrix_identity_float4x4, P).visible == 0,
       "a cylinder is refused, not drawn as a quad");
    ov.shape = 0;

    // The flip, stated by the record and meaning what it means everywhere else.
    ov.origin_top_left = 1;
    ok(kl_overlay_build(&ov, 0, matrix_identity_float4x4,
                        matrix_identity_float4x4, P).flip_y == 1,
       "a top-left-origin (Vulkan) layer asks the shader to flip");
    ov.origin_top_left = 0;
    ok(kl_overlay_build(&ov, 0, matrix_identity_float4x4,
                        matrix_identity_float4x4, P).flip_y == 0,
       "...and a GL one does not");
}

// The overlay shader, run: the same source the eye pass reads, through the
// other pipeline, so the two passes are pinned to ONE answer about which way up
// a GL-authored picture is.
static void check_overlay_pixels(void) {
    printf("=== the overlay pass, run ===\n");
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q = [dev newCommandQueue];
    if (!dev || !q) { printf("  no Metal device — skipped\n"); return; }

    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:
                              [NSString stringWithUTF8String:kl_reproject_overlay_msl()]
                                           options:nil error:&err];
    if (!lib) {
        printf("  %s\n", err.localizedDescription.UTF8String);
        ok(0, "the overlay shader compiles");
        return;
    }
    ok(1, "the overlay shader compiles");
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"kl_ov_v"];
    pd.fragmentFunction = [lib newFunctionWithName:@"kl_ov_f"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
                                                                       error:&err];
    if (!ps) { ok(0, "the overlay pipeline links"); return; }

    // A plain 2D source — the overlay fragment shader samples texture2d, not an
    // array, because a layer image is the guest's own and has no eye slices.
    // Authored bottom-up, as GL writes one: memory row 0 is the picture's
    // BOTTOM (see check_pixels).
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                     width:2 height:2 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> src = [dev newTextureWithDescriptor:td];
    uint8_t s0[16] = { 0,0,255,255,   255,255,255,255,      // row 0 = picture BOTTOM
                       255,0,0,255,   0,255,0,255 };        // row 1 = picture TOP
    [src replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0
             withBytes:s0 bytesPerRow:8];

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

    kl_ovrp_overlay ov = {0};
    ov.tex_w = 2; ov.tex_h = 2;
    for (int e = 0; e < 2; e++) { ov.viewport[e][2] = 2; ov.viewport[e][3] = 2; }
    ov.pose[3] = 1;
    ov.pose[6] = -1.0f;
    ov.size[0] = 2.0f; ov.size[1] = 2.0f;
    simd_float4x4 P = kl_reproject_projection(1, 1, 1, 1, 0.03f);

    uint8_t want[16];
    memcpy(want,     s0 + 8, 8);        // picture top    -> output row 0
    memcpy(want + 8, s0,     8);        // picture bottom -> output row 1

    for (int pass = 0; pass < 2; pass++) {
        ov.origin_top_left = pass;      // GL first, then Vulkan
        kl_overlay_uniforms u = kl_overlay_build(&ov, 0, matrix_identity_float4x4,
                                                 matrix_identity_float4x4, P);
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
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        uint8_t out[16] = {0};
        [dst getBytes:out bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
        // The panel covers the viewport exactly (proved above), so the output IS
        // the picture: rows reversed for a GL source, memory order for a Vulkan
        // one. Identical to the eye pass's two answers, which is the point.
        ok(memcmp(out, pass ? s0 : want, 16) == 0,
           pass ? "the overlay reads a top-left-origin (Vulkan) layer the right way up"
                : "the overlay reads a GL layer the right way up");
    }
}

int main(void) {
    @autoreleasepool {
        check_math();
        check_viewport();
        check_shader();
        check_pixels();
        check_crop_pixels();
        check_split_crop_pixels();
        check_two_textures();
        check_proj_layers();
        check_overlay_math();
        check_overlay_pixels();
    }
    printf(g_fail ? "\n=== t_reproject FAILED ===\n"
                  : "\n=== t_reproject: the composite pass is a blit when nothing "
                    "has moved ===\n");
    return g_fail;
}
