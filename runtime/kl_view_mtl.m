// The viewer's hardware compositor. See kl_view_mtl.h for why it exists and
// what it replaces; this file is the pass.
//
// It is the macOS twin of visionos/Sources/KleptonCompositor.swift, and the
// shader and the matrices behind it are now literally the same code — both
// compile kl_reproject_msl() and both build their uniforms with
// kl_reproject_build(). The flip, the quad and the array-slice sampler are the
// bits that have to be right in both, and having them diverge would mean
// debugging the picture twice. What differs is only the destination — a
// CAMetalLayer drawable here, a cp_drawable there.
//
// It is also the only place the reprojection math can be *run* before the
// device is available: KL_VIEW_TIMEWARP=1 feeds the pass the pose the guest
// actually rendered with (kl_ovrp's stage-keyed record) instead of the current
// one, so mouse-look motion between the guest's frame and this composite is
// corrected here exactly as head motion will be corrected on device. Default
// off, so the viewer path that reached gameplay is untouched.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kl_glfb.h"
#include "kl_ovrp.h"
#include "kl_reproject.h"
#include "kl_view_mtl.h"
#include "kl_vulkan.h"
#include "kl_env.h"

// The liveness downsample. 64x64 is 16 KB read back in a completion handler —
// off the critical path and small enough not to matter, but enough resolution
// to tell a black frame from a lit one, which is the only question the HUD
// asks. Bigger buys nothing: this is a sparse sample of a 5-megapixel eye
// either way.
#define KLVM_STAT_DIM 64

static id<MTLDevice>              g_dev;
static id<MTLCommandQueue>        g_queue;
static id<MTLRenderPipelineState> g_pipe;      // the plain blit
static id<MTLRenderPipelineState> g_pipe_rp;   // the reprojection pass, KL_VIEW_TIMEWARP
static int                        g_timewarp;
static id<MTLSamplerState>        g_samp;
static id<MTLSharedEvent>         g_event;
static id<MTLTexture>             g_stat;
static CAMetalLayer              *g_layer;
static int                        g_started;
static unsigned                   g_frames;
static uint64_t                   g_last_value;      // fence value last composited
static unsigned long             g_lit;   // atomics below: written on a Metal completion queue

// Build one pipeline from one of kl_reproject.c's two shaders. Both are shared
// with the visionOS compositor; the only thing this file decides is which one
// runs and what it draws into.
static id<MTLRenderPipelineState> klvm_pipeline(const char *msl, const char *vfn,
                                                const char *ffn, const char *what) {
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:[NSString stringWithUTF8String:msl]
                                             options:nil error:&err];
    id<MTLFunction> vf = [lib newFunctionWithName:[NSString stringWithUTF8String:vfn]];
    id<MTLFunction> ff = [lib newFunctionWithName:[NSString stringWithUTF8String:ffn]];
    if (!vf || !ff) {
        fprintf(stderr, "  [vmtl] %s shader failed to compile: %s\n", what,
                err.localizedDescription.UTF8String ?: "?");
        return nil;
    }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vf;
    pd.fragmentFunction = ff;
    // sRGB, so the encode from the eye's linear RGBA16F is the display
    // hardware's job. The readback path did this in software with powf(1/2.2)
    // per channel per pixel — 15 million calls a frame at 2198x2304, which is
    // why it needed a 64K-entry LUT to be affordable at all.
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
    id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithDescriptor:pd
                                                                        error:&err];
    if (!p)
        fprintf(stderr, "  [vmtl] %s pipeline failed: %s\n", what,
                err.localizedDescription.UTF8String ?: "?");
    return p;
}

// The uniforms for this composite, in the viewer's degenerate case of it: one
// eye, no eye offset, and a display frustum defined to be the frustum the guest
// rendered with. That last one is what makes the reprojection pass reduce
// exactly to the blit when nothing has moved — the quad's corners land on
// NDC ±1 — so any difference in the picture is the pose delta and nothing else.
//
// `stage` is passed IN, not read here. kl_ovrp_last_complete_stage() advances
// whenever the guest finishes a frame, so reading it a second time inside one
// composite can return a different stage from the one whose texture is being
// sampled — and then this builds the pose of one picture to place another. It
// is a narrow window, but it is hit exactly when the guest is producing frames
// fastest, and the result is a picture reprojected by a delta that was never
// real. One read per composite, threaded through everything that needs it.
// The unwarp grid for the composite, cached.
//
// Rebuilt only when the rate map or the eye texture's size changes, which is
// the point of doing the unwarp this way: a rate map is static, so the squeeze
// is resolved once on the CPU into texture coordinates the rasterizer then
// interpolates for free. See kl_reproject.h.
static void klvm_s2p(void *ctx, float sx, float sy, float *px, float *py) {
    id<MTLRasterizationRateMap> m = (__bridge id<MTLRasterizationRateMap>)ctx;
    MTLSamplePosition p = [m mapScreenToPhysicalCoordinates:MTLSamplePositionMake(sx, sy)
                                                   forLayer:0];
    *px = p.x; *py = p.y;
}

static id<MTLBuffer> g_grid_buf;
static void         *g_grid_map;
static uint32_t      g_grid_w, g_grid_h, g_grid_verts;
static float         g_grid_vp[4];

// `vp` is the guest's render viewport for this frame in eye-texture pixels, or
// NULL for the whole texture — kl_ovrp_render_pose.viewport, read from the same
// frame record the uniforms come from so the crop and the pose describe one
// picture. It is part of the cache key: Beat Saber changes it on entering a
// map, and a grid built for the menu's viewport crops the map's picture (or
// fails to crop it) with nothing reporting either.
static id<MTLBuffer> klvm_grid_buffer(id<MTLTexture> src, const float *vp,
                                      uint32_t *out_verts) {
    void *map = kl_glfb_eye_rate_map();
    uint32_t tw = src ? (uint32_t)src.width : 0, th = src ? (uint32_t)src.height : 0;
    float want_vp[4] = { 0, 0, 0, 0 };
    if (vp && vp[2] > 0 && vp[3] > 0) memcpy(want_vp, vp, sizeof want_vp);
    if (!g_grid_buf || map != g_grid_map || tw != g_grid_w || th != g_grid_h ||
        memcmp(want_vp, g_grid_vp, sizeof want_vp) != 0) {
        g_grid_map = map; g_grid_w = tw; g_grid_h = th;
        memcpy(g_grid_vp, want_vp, sizeof want_vp);
        int cropped = want_vp[2] > 0 && want_vp[3] > 0 &&
                      ((uint32_t)want_vp[2] != tw || (uint32_t)want_vp[3] != th ||
                       want_vp[0] != 0 || want_vp[1] != 0);
        uint32_t nx = 1, ny = 1;
        if (map) {
            // One cell per ZONE, so every grid vertex lands on a breakpoint of
            // the piecewise-linear map and the unwarp is exact rather than a
            // sampling of it. The zone counts travel with the map because a
            // built MTLRasterizationRateMap does not report them.
            id<MTLRasterizationRateMap> m = (__bridge id<MTLRasterizationRateMap>)map;
            MTLSize scr = [m screenSize];
            int zx = 0, zy = 0;
            kl_glfb_eye_rate_zones(&zx, &zy);
            if (zx > 0 && zy > 0) {
                // The map's own zones. Exact: a rate map is piecewise linear
                // with breakpoints at the zone boundaries, which are uniform in
                // SCREEN space, so a screen-uniform grid of exactly this many
                // cells has a vertex on every breakpoint.
                nx = (uint32_t)zx; ny = (uint32_t)zy;
            } else {
                // A map we did not build does not report its zones — but the
                // GRANULARITY cell count is recoverable, which is what ALVR's
                // DummyMetalRenderer does to reconstruct a descriptor. Uniform
                // in physical rather than screen space, so it is not
                // zone-aligned and the unwarp is a dense sampling of the curve
                // rather than an exact reproduction of it; at ~32px granules
                // the residual is well under a texel.
                MTLSize gran = [m physicalGranularity];
                MTLSize phys = [m physicalSizeForLayer:0];
                nx = gran.width  ? (uint32_t)(phys.width  / gran.width)  : 16;
                ny = gran.height ? (uint32_t)(phys.height / gran.height) : 16;
            }
            if (nx < 1) nx = 1;
            if (ny < 1) ny = 1;
            if (nx > KL_REPROJECT_GRID_MAX) nx = KL_REPROJECT_GRID_MAX;
            if (ny > KL_REPROJECT_GRID_MAX) ny = KL_REPROJECT_GRID_MAX;
            uint32_t n = kl_reproject_grid_entries(nx, ny);
            simd_float2 *tmp = calloc(n, sizeof *tmp);
            kl_reproject_grid_build(tmp, nx, ny, cropped ? want_vp : NULL,
                                    (float)scr.width, (float)scr.height,
                                    (float)tw, (float)th, klvm_s2p, map);
            g_grid_buf = [g_dev newBufferWithBytes:tmp length:n * sizeof *tmp
                                           options:MTLResourceStorageModeShared];
            free(tmp);
            fprintf(stderr, "  [view] unwarp grid %ux%u for a %ux%u eye texture "
                            "(screen %lux%lu, viewport %.0f,%.0f %.0fx%.0f)\n", nx, ny, tw, th,
                    (unsigned long)scr.width, (unsigned long)scr.height,
                    cropped ? want_vp[0] : 0.f, cropped ? want_vp[1] : 0.f,
                    cropped ? want_vp[2] : (float)tw, cropped ? want_vp[3] : (float)th);
        } else if (cropped) {
            // No rate map, but the guest still drew into part of the texture.
            // One cell is enough — with no map the mapping is affine, so the
            // four corners carry it exactly.
            uint32_t n = kl_reproject_grid_entries(1, 1);
            simd_float2 tmp[8];
            kl_reproject_grid_build(tmp, 1, 1, want_vp, (float)tw, (float)th,
                                    (float)tw, (float)th, NULL, NULL);
            g_grid_buf = [g_dev newBufferWithBytes:tmp length:n * sizeof *tmp
                                           options:MTLResourceStorageModeShared];
            fprintf(stderr, "  [view] render viewport %.0f,%.0f %.0fx%.0f of a %ux%u eye "
                            "texture — compositing that sub-rect\n",
                    want_vp[0], want_vp[1], want_vp[2], want_vp[3], tw, th);
        } else {
            uint32_t n = kl_reproject_grid_entries(1, 1);
            simd_float2 tmp[8];
            kl_reproject_grid_identity(tmp);
            g_grid_buf = [g_dev newBufferWithBytes:tmp length:n * sizeof *tmp
                                           options:MTLResourceStorageModeShared];
        }
        g_grid_verts = kl_reproject_grid_vertices(nx, ny);
    }
    if (out_verts) *out_verts = g_grid_verts;
    return g_grid_buf;
}

static kl_reproject_uniforms klvm_uniforms(int stage, uint32_t slice, int flip_y) {
    kl_ovrp_render_pose r;
    int have = kl_ovrp_stage_render_pose(stage, &r);

    // Where the head is now. Without KL_VIEW_TIMEWARP this is *defined* to be
    // where the frame was rendered from, which makes the delta identically zero
    // — the A/B, and the reason the default path cannot regress.
    simd_float4x4 device = matrix_identity_float4x4;
    if (g_timewarp && have) {
        float px, py, pz, qx, qy, qz, qw;
        kl_ovrp_get_head_pose(&px, &py, &pz, &qx, &qy, &qz, &qw);
        device = simd_matrix4x4(simd_quaternion(qx, qy, qz, qw));
    } else if (have) {
        device = simd_matrix4x4(simd_quaternion(r.qx, r.qy, r.qz, r.qw));
    }

    const float *t = have ? r.tangents[0] : (const float[]){1, 1, 1, 1};
    simd_float4x4 proj = kl_reproject_projection(t[0], t[1], t[2], t[3], 0.03f);
    return kl_reproject_build(have ? &r : NULL, 0, device,
                              matrix_identity_float4x4, proj, slice, flip_y);
}

// Which frame of the guest's this composite would be showing, and 0 for "none
// yet". Two sources, because the two graphics APIs answer it in different
// places: ANGLE signals an MTLSharedEvent from inside its own command stream
// (kl_glfb), and a Vulkan guest has no such event — kl_vulkan waits for its
// queue at ovrp_EndFrame4 and publishes a serial instead. Both are monotonic and
// both mean "there is a NEW, COMPLETE picture", which is all this file wants of
// them.
//
// `*fenced` is the difference that matters at the encoder: a GL value is one the
// guest's queue really signals on our MTLSharedEvent, so the composite can WAIT
// on it, and a Vulkan serial is not — kl_vulkan already made the queue idle
// before publishing it. Waiting on the event for a Vulkan serial would be a
// command buffer that is committed and never executes: a presented drawable,
// healthy counters, and a black window (KL_CP_NOFENCE's failure, in the one
// place it cannot be turned off).
static uint64_t klvm_frame_value(int *fenced) {
    uint64_t v = kl_glfb_gpu_fence_value();
    if (fenced) *fenced = v != 0;
    return v ? v : (uint64_t)kl_vulkan_frame_serial();
}

int kl_viewmtl_start(void *metal_layer) {
    if (g_started) return 1;
    if (!metal_layer) return 0;

    // Nothing to composite until the guest has taken its eye textures, which
    // happens inside nativeRecreateGfxState. Returning 0 here is not a failure,
    // it is "not yet" — kl_view.c retries every frame.
    void *eyep = kl_glfb_eye_mtl_texture(0, 0, NULL);
    if (!eyep) return 0;

    // **The device is the EYE TEXTURE's, not MTLCreateSystemDefaultDevice() and
    // no longer ANGLE's either.** A drawable from a different device cannot be
    // the destination of a pass that samples this texture, so the only device
    // that can possibly be right is the one that owns the thing being sampled —
    // and asking the texture is the one phrasing that is right for both guests.
    // On GL that is ANGLE's device by construction (the extension requires the
    // eye texture to belong to the display's device, PLANNING §12.9); on the
    // Vulkan path it is MoltenVK's, and `kl_glfb_mtl_device()` answers NULL
    // there because ANGLE was never brought up at all.
    g_dev = ((__bridge id<MTLTexture>)eyep).device;
    if (!g_dev) return 0;
    g_queue = [g_dev newCommandQueue];
    g_event = [g_dev newSharedEvent];
    if (!g_queue || !g_event) {
        // WHICH of the two, and only once: this is retried every frame until it
        // takes, so an unnamed failure is a thousand identical lines that say
        // the compositor is down and not what is down about it.
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [vmtl] %s on %s — the hardware compositor cannot "
                            "start, so the window stays black\n",
                    !g_queue && !g_event ? "no command queue and no shared event"
                    : !g_queue          ? "newCommandQueue returned nil"
                                        : "newSharedEvent returned nil",
                    g_dev.name.UTF8String);
        }
        // Not half-started: a queue kept across a failed retry is a queue
        // leaked per frame.
        g_queue = nil; g_event = nil;
        return 0;
    }

    // The blit is built either way: the liveness downsample uses it (its
    // geometry is irrelevant to counting lit pixels) and it is the A/B for a
    // reprojected picture that comes out wrong.
    g_pipe = klvm_pipeline(kl_reproject_blit_msl(), "kl_blit_v", "kl_blit_f", "blit");
    if (!g_pipe) return 0;

    g_timewarp = kl_env_on("KL_VIEW_TIMEWARP", 0);
    if (g_timewarp) {
        g_pipe_rp = klvm_pipeline(kl_reproject_msl(), "kl_reproject_v",
                                  "kl_reproject_f", "reprojection");
        if (!g_pipe_rp) return 0;
    }

    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    g_samp = [g_dev newSamplerStateWithDescriptor:sd];

    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB
                                     width:KLVM_STAT_DIM height:KLVM_STAT_DIM
                                 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;      // so the completion handler can read it
    g_stat = [g_dev newTextureWithDescriptor:td];

    g_layer = (__bridge CAMetalLayer *)metal_layer;
    g_layer.device = g_dev;
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
    g_layer.framebufferOnly = YES;

    // Registering the fence is what switches kl_glfb's swap handling from
    // "read the eye back" to "signal that the frame is done". From here on
    // nothing copies pixels.
    //
    // Only when ANGLE is the renderer. On the Vulkan path kl_glfb has no
    // context, no swap and nothing to signal, and registering an event it will
    // never touch would make kl_glfb_has_gpu_fence() answer yes for a fence
    // that does not exist — which is a claim the end-of-run report and
    // kl_ovrp's present gate both read.
    int gl = kl_glfb_mtl_device() != NULL;
    if (gl) kl_glfb_set_gpu_fence((__bridge void *)g_event);

    g_started = 1;
    fprintf(stderr, "  [vmtl] hardware compositor on %s — no readback, "
                    "no CPU copies%s%s\n", g_dev.name.UTF8String,
            gl ? "" : ", VULKAN guest (the eye texture is MoltenVK's and the "
                      "frame seam is kl_vulkan's serial)",
            g_timewarp ? ", reprojecting against the guest's render pose" : "");
    return 1;
}

// The completion handler's half of the HUD. Same definition of "lit" as
// kl_glfb's capture (the sum of the three tone-mapped channels above 12), read
// off a 64x64 render of the same source, then scaled to the eye's pixel count
// so the number is comparable to the one the readback path printed.
static void klvm_read_stats(int src_w, int src_h) {
    uint8_t px[KLVM_STAT_DIM * KLVM_STAT_DIM * 4];
    [g_stat getBytes:px bytesPerRow:KLVM_STAT_DIM * 4
          fromRegion:MTLRegionMake2D(0, 0, KLVM_STAT_DIM, KLVM_STAT_DIM)
         mipmapLevel:0];
    unsigned lit = 0;
    for (int i = 0; i < KLVM_STAT_DIM * KLVM_STAT_DIM; i++) {
        // BGRA8_sRGB: the stored bytes are already the encoded values, which is
        // the same domain kl_glfb's powf(c, 1/2.2) produced. Channel order does
        // not matter to a sum.
        unsigned lum = px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2];
        if (lum > 12) lit++;
    }
    double frac = (double)lit / (KLVM_STAT_DIM * KLVM_STAT_DIM);
    __atomic_store_n(&g_lit, (unsigned long)(frac * src_w * src_h), __ATOMIC_RELAXED);
}

// The measurement that says whether reprojection is doing anything: how far the
// head turned between the pose the guest rendered this picture with and the
// pose it is being shown at. Zero means the guest is keeping up and the pass is
// a blit; anything else is motion the composite is now hiding. Printed rarely,
// because it is a per-frame quantity and the interesting thing is its scale.
static void klvm_note_delta(int stage) {
    static unsigned n;
    static float worst;
    kl_ovrp_render_pose r;
    if (stage < 0 || !kl_ovrp_stage_render_pose(stage, &r)) return;
    float px, py, pz, qx, qy, qz, qw;
    kl_ovrp_get_head_pose(&px, &py, &pz, &qx, &qy, &qz, &qw);
    float d = kl_reproject_delta_degrees(&r, simd_matrix4x4(simd_quaternion(qx, qy, qz, qw)));
    if (d > worst) worst = d;
    if (++n % 120 == 0) {
        fprintf(stderr, "  [vmtl] timewarp: stage %d serial %llu, delta %.2f deg "
                        "(worst %.2f)\n", stage, (unsigned long long)r.serial,
                (double)d, (double)worst);
        worst = 0;
    }
}

int kl_viewmtl_present(int win_w, int win_h) {
    if (!g_started) return 0;

    // Re-read every frame rather than caching: Unity really does re-create its
    // eye textures at a different size partway through startup (1832x1920 ->
    // 2198x2304, measured), and a cached texture would silently become the
    // previous allocation.
    // ...and the STAGE has to be re-read too, now that there is more than one.
    // The eye swapchain is double-buffered (kl_ovrp.c, KLOVRP_STAGES_DEFAULT),
    // so stage 0 holds every other frame; sampling it unconditionally shows
    // half the frames and reads as a halved frame rate rather than as a bug.
    // The last *complete* stage is the same thing the visionOS compositor asks
    // for. It is read HERE and nowhere else in this composite: klvm_uniforms and
    // klvm_note_delta used to ask again, and two reads of a value the guest is
    // advancing pair one stage's picture with another stage's pose — the exact
    // failure this stage-keying exists to prevent.
    int stage = kl_ovrp_last_complete_stage();
    if (stage < 0) stage = 0;
    int slice = 0;
    void *texp = kl_glfb_eye_mtl_texture(0, stage, &slice);
    if (!texp) return 0;
    id<MTLTexture> src = (__bridge id<MTLTexture>)texp;
    // Which way up the guest drew it, recorded with the texture — see
    // kl_reproject.h. Read per (eye, stage) rather than decided once, because
    // it is a property of the storage and not of this compositor.
    int flip = kl_glfb_eye_mtl_origin_top_left(0, stage);

    int fenced = 0;
    uint64_t v = klvm_frame_value(&fenced);
    if (!v || v == g_last_value) return 0;   // no new guest frame

    if (win_w <= 0 || win_h <= 0) return 0;
    CGSize want = CGSizeMake(win_w, win_h);
    if (!CGSizeEqualToSize(g_layer.drawableSize, want)) g_layer.drawableSize = want;

    id<CAMetalDrawable> drawable = [g_layer nextDrawable];
    if (!drawable) return 0;
    id<MTLCommandBuffer> cmd = [g_queue commandBuffer];
    if (!cmd) return 0;

    // The whole point of the shared event: the guest's frame is in ANGLE's
    // command queue, this pass is in ours, and Metal orders nothing across
    // queues on its own. Without this the compositor samples a texture the
    // guest may still be rendering into — which reads as intermittent tearing
    // and is exactly the kind of bug that gets blamed on the guest.
    //
    // ...when the value is one ANGLE's queue signals. A Vulkan serial is
    // published only after kl_vulkan has made the guest's queue idle, so the
    // ordering has already happened on the CPU and there is nothing to wait for
    // — see klvm_frame_value.
    if (fenced) [cmd encodeWaitForEvent:g_event value:v];

    // Letterbox: the eye is ~1832x1920, the window is not.
    NSUInteger sw = src.width, sh = src.height;
    double scale = fmin((double)win_w / sw, (double)win_h / sh);
    double dw = sw * scale, dh = sh * scale;

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
    enc.label = @"klepton viewer composite";
    [enc setViewport:(MTLViewport){ (win_w - dw) * 0.5, (win_h - dh) * 0.5,
                                    dw, dh, 0.0, 1.0 }];
    [enc setFragmentTexture:src atIndex:0];
    [enc setFragmentSamplerState:g_samp atIndex:0];
    kl_blit_uniforms bu = { (uint32_t)slice, (uint32_t)(flip ? 1 : 0) };
    if (g_pipe_rp) {
        kl_reproject_uniforms u = klvm_uniforms(stage, bu.slice, flip);
        [enc setRenderPipelineState:g_pipe_rp];
        [enc setVertexBytes:&u length:sizeof u atIndex:0];
        [enc setFragmentBytes:&u length:sizeof u atIndex:0];
        // The unwarp grid. Identity until the guest's eye textures are
        // foveated, at which point it carries the squeeze this pass has to
        // undo (kl_reproject.h). A buffer rather than setVertexBytes: that
        // path caps at 4 KB and a grid dense enough to land on the map's zone
        // boundaries is bigger than that.
        uint32_t gverts = 0;
        // ...and the crop, from the same record the uniforms above came from.
        // Eye 0's, because this viewer composites eye 0 — the visionOS
        // compositor is the one that has to reconcile a pair.
        kl_ovrp_render_pose vr;
        float vp[4] = { 0, 0, 0, 0 };
        if (kl_ovrp_stage_render_pose(stage, &vr)) {
            for (int i = 0; i < 4; i++) vp[i] = (float)vr.viewport[0][i];
            // The same guard KleptonCompositor carries, and for the same reason:
            // a rect measured against another eye texture describes nothing
            // about this one, and cropping by it MAGNIFIES the picture by the
            // ratio rather than mis-placing it (kl_ovrp.h, viewport_of). Refuse
            // rather than rescale — with the two disagreeing, which texels the
            // guest wrote is not known.
            if (vp[2] > 0 && vr.viewport_of[0] > 0 && src &&
                ((uint32_t)vr.viewport_of[0] != (uint32_t)src.width ||
                 (uint32_t)vr.viewport_of[1] != (uint32_t)src.height)) {
                static int said_skew;
                if (!said_skew) {
                    said_skew = 1;
                    fprintf(stderr, "  [view] the render viewport is %dx%d of a %dx%d "
                                    "eye texture, but the texture in hand is %ux%u — "
                                    "NOT cropping, that rect describes another "
                                    "texture\n",
                            vr.viewport[0][2], vr.viewport[0][3],
                            vr.viewport_of[0], vr.viewport_of[1],
                            (unsigned)src.width, (unsigned)src.height);
                }
                vp[0] = vp[1] = vp[2] = vp[3] = 0;
            }
            // Once, and ALSO when it is zero — the same line KleptonCompositor
            // prints, so a host run and a device run answer "did the rect reach
            // the composite?" in the same words. `[ovrp] render viewport` is
            // the guest's own thread saying what it submitted; this is the
            // compositor saying what it read, and the two failures those
            // separate are indistinguishable by eye.
            static int said;
            if (!said) {
                said = 1;
                fprintf(stderr, "  [view] first render viewport read from the frame "
                                "record: %d,%d %dx%d (stage %d, serial %llu)%s\n",
                        vr.viewport[0][0], vr.viewport[0][1], vr.viewport[0][2],
                        vr.viewport[0][3], stage, (unsigned long long)vr.serial,
                        vr.viewport[0][2] <= 0
                            ? "  <- ZERO, so no crop is applied; compare against "
                              "the [ovrp] render viewport line" : "");
            }
        }
        id<MTLBuffer> gbuf = klvm_grid_buffer(src, vp[2] > 0 ? vp : NULL, &gverts);
        [enc setVertexBuffer:gbuf offset:0 atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:gverts];
        klvm_note_delta(stage);
    } else {
        [enc setRenderPipelineState:g_pipe];
        [enc setFragmentBytes:&bu length:sizeof bu atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [enc endEncoding];

    // The liveness downsample, same source, same shader, 64x64. It is a second
    // pass rather than a readback of the drawable because a drawable with
    // framebufferOnly=YES cannot be read at all, and dropping that would cost
    // more than this pass does.
    if (g_stat) {
        MTLRenderPassDescriptor *sp = [MTLRenderPassDescriptor renderPassDescriptor];
        sp.colorAttachments[0].texture = g_stat;
        sp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        sp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> se = [cmd renderCommandEncoderWithDescriptor:sp];
        se.label = @"klepton viewer liveness";
        [se setRenderPipelineState:g_pipe];
        [se setFragmentTexture:src atIndex:0];
        [se setFragmentSamplerState:g_samp atIndex:0];
        [se setFragmentBytes:&bu length:sizeof bu atIndex:0];
        [se drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [se endEncoding];
        int cw = (int)sw, ch = (int)sh;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer> b) {
            (void)b;
            klvm_read_stats(cw, ch);
        }];
    }

    [cmd presentDrawable:drawable];
    [cmd commit];
    g_last_value = v;
    g_frames++;
    return 1;
}

void kl_viewmtl_stop(void) {
    if (!g_started) return;
    // Unregister first: the guest may still be swapping, and a signal into a
    // released event is a crash rather than a missed frame.
    kl_glfb_set_gpu_fence(NULL);
    g_started = 0;
    g_pipe = nil; g_samp = nil; g_stat = nil; g_queue = nil;
    g_event = nil; g_layer = nil; g_dev = nil;
}

unsigned kl_viewmtl_frames(void) { return g_frames; }
unsigned long long kl_viewmtl_guest_frame(void) { return g_last_value; }
unsigned long kl_viewmtl_lit(void) {
    return __atomic_load_n(&g_lit, __ATOMIC_RELAXED);
}
