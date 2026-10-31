// Foveated rendering for a Vulkan guest, done underneath MoltenVK.
//
// A Vulkan engine foveates with a fragment density map: the attachment keeps
// its size, the rasterizer shades it coarsely where the map says so, and the
// implementation reconstructs. MoltenVK has no such extension, and Metal's
// counterpart is the wrong shape for one — an MTLRasterizationRateMap makes the
// stored picture physically SMALLER (it lands squeezed into the top-left of the
// attachment) and leaves the unwarp to whoever reads it. kl_glfb.h tells the
// same story for the GL guests, where the answer was to patch ANGLE and to
// foveate only pictures the compositor reads itself.
//
// Here the reader is the guest: Source 2 resolves its multisampled scene pass
// into a target that its own post-processing then samples with screen-space
// coordinates. So this does both halves around that one pass, and the engine
// never learns of either:
//
//   1. The scene pass — recognised as the render pass whose colour attachment
//      is a large multisampled array texture, which in a frame of ~20 passes is
//      exactly one — is given a rate map. That is where nearly all of a frame's
//      fill cost is (KL_VK_PROFILE: 59 of ~100 megasamples), and it now shades
//      only the physical area.
//   2. As that pass ends, its resolve target holds the squeezed picture. It is
//      copied aside and drawn back over the full target through the map's own
//      screen->physical mapping, so what the post chain reads is an ordinary
//      full-size picture that is merely softer toward the edge.
//
// There is no hook in MoltenVK for either, and none is needed: both are Metal
// calls MoltenVK makes on objects whose classes can be asked to do a little
// more (-renderCommandEncoderWithDescriptor: and -endEncoding). That is the
// Objective-C runtime doing what it is for; no private interface is named.
//
// What this cannot make right: a fragment's position inside the foveated pass
// is its PHYSICAL pixel, so a shader that derives a screen coordinate from it
// (a froxel fog lookup, a screen-space texture) reads a place that drifts from
// the true one toward the bottom right. Whether a title's scene shaders do that
// is a thing to look at, not to assume — KL_VK_FOVEATE=0 is the A/B.
//
// Knobs (DEBUG_ENV_VARS.md):
//   KL_VK_FOVEATE=<edge>     the rate at the widest edge of the cone, 0 < edge
//                            < 1. Unset or 0: off, nothing is installed.
//   KL_VK_FOVEATE_INNER=<t>  the tangent out to which the rate stays 1 (default
//                            0.30, about 17 degrees either side of straight ahead)
//   KL_VK_FOVEATE_OUTER=<t>  the tangent at which the edge rate is reached
//                            (default: the widest edge of the cone)
//   KL_VK_FOVEATE_TRACE=1    say what every candidate pass looked like
//
// Two instruments share the hooks, because they need the same vantage point —
// what Metal is actually asked to do, which is not what the guest asked Vulkan:
//   KL_MTL_PROFILE=<frames>  GPU time of every render pass, by shape, from
//                            Metal's own stage-boundary timestamps; the
//                            compositor's passes are in it too
//   KL_MTL_TEX_TRACE=1       every large texture the process creates
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <pthread.h>
#include <execinfo.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include "kl_env.h"
#include "kl_ovrp.h"
#include "kl_driver.h"

#if __has_feature(objc_arc)
#define KLF_RELEASE(x) ((void)0)
#define KLF_KEEP(x)    ((void)CFBridgingRetain(x))
#else
#define KLF_RELEASE(x) [(x) release]
#define KLF_KEEP(x)    ((void)[(x) retain])
#endif

#define KLF_ZONES 32

// kl_vulkan.c: the size of the picture the guest draws inside its scene pass,
// which may be a little smaller than the attachments the pass covers.
extern uint32_t g_klvk_scene_view_w, g_klvk_scene_view_h;

static float g_edge, g_inner, g_outer;
static int   g_trace, g_fov_on;
// Recursive: the unwarp opens and ends encoders of its own while it holds this,
// and those come back through the same two methods.
static pthread_mutex_t g_mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

static IMP   g_orig_begin;        // -[cmdbuf renderCommandEncoderWithDescriptor:]
static IMP   g_orig_end;          // -[encoder endEncoding]
static IMP   g_orig_store;        // -[encoder setColorStoreAction:atIndex:]
static Class g_enc_class;

// One rate map per render size, kept for the run (a guest has one eye size, or
// a few). Everything an unwarp needs hangs off it.
typedef struct {
    NSUInteger w, h;              // what the map covers: the pass's render target
    NSUInteger vw, vh;            // the picture inside it, which the fovea is centred in
    __unsafe_unretained id<MTLRasterizationRateMap> map;   // kept with KLF_KEEP
    __unsafe_unretained id<MTLBuffer>  params;             // the map, for the decoder
    __unsafe_unretained id<MTLTexture> aside;              // physical-size copy
    NSUInteger pw, ph;
    MTLPixelFormat aside_fmt;
} klf_map;
static klf_map g_maps[4];
static int     g_nmaps;

// The passes that are open right now and were given a map.
typedef struct {
    __unsafe_unretained id enc;
    __unsafe_unretained id<MTLCommandBuffer> cmd;
    __unsafe_unretained id<MTLTexture> resolve;
    NSUInteger slice, layers;
    int map;
    int resolves;       // the store action is, or was later made, a resolve
} klf_open;
static klf_open g_open[8];

static struct { MTLPixelFormat fmt; __unsafe_unretained id<MTLRenderPipelineState> pso; } g_pso[4];
static int g_npso;

static unsigned long g_passes, g_unwarps;

// The rate along one axis. `neg` and `pos` are the cone's tangents either side
// of straight ahead, in the direction zones are counted (left then right, top
// then bottom); `widest` is the largest tangent of any side, so the falloff is
// the same number of rate a tangent in every direction.
// `span` is how much of the map's extent the picture takes (1 when they are the
// same size); zones beyond the picture are nobody's and get the edge rate.
static void klf_axis(float *q, float neg, float pos, float widest, float span) {
    for (int i = 0; i < KLF_ZONES; i++) {
        float u = ((float)i + 0.5f) / (float)KLF_ZONES / span;
        if (u > 1.0f) { q[i] = g_edge; continue; }
        float t = -neg + u * (neg + pos);
        if (t < 0) t = -t;
        float r = 1.0f;
        // The rate falls from 1 at the inner tangent to the edge rate at the
        // cone's widest edge — or, with KL_VK_FOVEATE_OUTER, already at that
        // tangent, and stays there to the edge. A fall over the whole cone
        // leaves a wide ring at middling rates: with 0.10 at the edge the
        // picture was still 48% of its fragments, and its player could not
        // tell that anything was on (2026-10-13).
        float far = (g_outer > g_inner + 0.01f && g_outer < widest) ? g_outer : widest;
        if (t > g_inner && far > g_inner + 0.01f)
            r = 1.0f - (1.0f - g_edge) * (t - g_inner) / (far - g_inner);
        q[i] = r < g_edge ? g_edge : r;
    }
}

static int klf_map_for(id<MTLDevice> dev, NSUInteger w, NSUInteger h) {
    NSUInteger vw = g_klvk_scene_view_w, vh = g_klvk_scene_view_h;
    if (!vw || vw > w) vw = w;
    if (!vh || vh > h) vh = h;
    for (int i = 0; i < g_nmaps; i++)
        if (g_maps[i].w == w && g_maps[i].h == h && g_maps[i].vw == vw && g_maps[i].vh == vh) return i;
    if (g_nmaps >= (int)(sizeof g_maps / sizeof *g_maps)) return -1;
    if (![dev supportsRasterizationRateMapWithLayerCount:2]) {
        static int said;
        if (!said++) fprintf(stderr, "  [fov] this GPU has no two-layer rasterization rate "
                                     "map — KL_VK_FOVEATE ignored\n");
        return -1;
    }
    float tan[2][4];
    kl_ovrp_eye_told_tangents(0, tan[0]);
    kl_ovrp_eye_told_tangents(1, tan[1]);
    float widest = 0;
    for (int e = 0; e < 2; e++) for (int i = 0; i < 4; i++) if (tan[e][i] > widest) widest = tan[e][i];
    MTLRasterizationRateMapDescriptor *d = [[MTLRasterizationRateMapDescriptor alloc] init];
    d.screenSize = MTLSizeMake(w, h, 0);
    d.label = @"klepton vulkan foveation";
    // Each eye's cone is wider outward than inward (60 / 45 degrees on the
    // Vision Pro), so a curve counted from its own straight-ahead point leaves
    // the outer side of each eye coarse and the two eyes disagree: the left
    // one blurred on its left, the right one on its right (kl_fove.m's
    // kf_map_for has the same reasoning; Alien: Rogue Incursion, toolchain
    // 565). Both layers get the envelope of the two eyes' curves, i.e. the
    // same map, symmetric for mirrored cones. KL_FOVE_PER_EYE=1: each eye's own.
    static int per_eye = -1;
    if (per_eye < 0) per_eye = kl_env_on("KL_FOVE_PER_EYE", 0);
    for (int e = 0; e < 2; e++) {
        float qx[KLF_ZONES], qy[KLF_ZONES];
        klf_axis(qx, tan[e][0], tan[e][1], widest, (float)vw / (float)w);     // left ... right
        klf_axis(qy, tan[e][2], tan[e][3], widest, (float)vh / (float)h);     // top ... bottom
        if (!per_eye) {
            float ox[KLF_ZONES], oy[KLF_ZONES];
            klf_axis(ox, tan[e ^ 1][0], tan[e ^ 1][1], widest, (float)vw / (float)w);
            klf_axis(oy, tan[e ^ 1][2], tan[e ^ 1][3], widest, (float)vh / (float)h);
            for (int i = 0; i < KLF_ZONES; i++) {
                if (ox[i] > qx[i]) qx[i] = ox[i];
                if (oy[i] > qy[i]) qy[i] = oy[i];
            }
        }
        MTLRasterizationRateLayerDescriptor *l = [[MTLRasterizationRateLayerDescriptor alloc]
            initWithSampleCount:MTLSizeMake(KLF_ZONES, KLF_ZONES, 0) horizontal:qx vertical:qy];
        [d setLayer:l atIndex:(NSUInteger)e];
        KLF_RELEASE(l);
    }
    id<MTLRasterizationRateMap> m = [dev newRasterizationRateMapWithDescriptor:d];
    KLF_RELEASE(d);
    if (!m) {
        fprintf(stderr, "  [fov] no rate map for %lux%lu\n", (unsigned long)w, (unsigned long)h);
        return -1;
    }
    klf_map *k = &g_maps[g_nmaps];
    memset(k, 0, sizeof *k);
    k->w = w; k->h = h; k->vw = vw; k->vh = vh; k->map = m;
    MTLSizeAndAlign sa = m.parameterBufferSizeAndAlign;
    id<MTLBuffer> b = [dev newBufferWithLength:sa.size options:MTLResourceStorageModeShared];
    [m copyParameterDataToBuffer:b offset:0];
    k->params = b;
    for (NSUInteger e = 0; e < 2; e++) {
        MTLSize p = [m physicalSizeForLayer:e];
        if (p.width > k->pw) k->pw = p.width;
        if (p.height > k->ph) k->ph = p.height;
    }
#if __has_feature(objc_arc)
    KLF_KEEP(m); KLF_KEEP(b);
#endif
    fprintf(stderr, "  [fov] foveation: a %lux%lu scene pass (picture %lux%lu) is rasterized into "
                    "%lux%lu — %.0f%% of its fragments (rate 1 within tan %.2f, %.2f at tan %.2f; "
                    "eye 0 cone l=%.2f r=%.2f t=%.2f b=%.2f)\n",
            (unsigned long)w, (unsigned long)h, (unsigned long)vw, (unsigned long)vh,
            (unsigned long)k->pw, (unsigned long)k->ph,
            100.0 * (double)(k->pw * k->ph) / (double)(w * h), (double)g_inner, (double)g_edge,
            (double)((g_outer > g_inner + 0.01f && g_outer < widest) ? g_outer : widest), (double)tan[0][0], (double)tan[0][1], (double)tan[0][2], (double)tan[0][3]);
    return g_nmaps++;
}

static NSString *const kUnwarpSrc = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct V { float4 pos [[position]]; };\n"
    "struct U { uint layer; };\n"
    "vertex V klf_vs(uint vid [[vertex_id]]) {\n"
    "    V o; o.pos = float4(float2((vid << 1) & 2, vid & 2) * 2.0 - 1.0, 0.0, 1.0); return o;\n"
    "}\n"
    "fragment float4 klf_fs(V in [[stage_in]], texture2d_array<float> src [[texture(0)]],\n"
    "                       constant rasterization_rate_map_data &rrm [[buffer(0)]],\n"
    "                       constant U &u [[buffer(1)]]) {\n"
    "    constexpr sampler s(coord::pixel, filter::linear, address::clamp_to_edge);\n"
    "    rasterization_rate_map_decoder dec(rrm);\n"
    "    float2 p = dec.map_screen_to_physical_coordinates(in.pos.xy, u.layer);\n"
    "    return src.sample(s, p, u.layer);\n"
    "}\n";

static id<MTLRenderPipelineState> klf_pipeline(id<MTLDevice> dev, MTLPixelFormat fmt) {
    for (int i = 0; i < g_npso; i++) if (g_pso[i].fmt == fmt) return g_pso[i].pso;
    static int failed;                       // said once; a retry every frame says nothing new
    if (failed || g_npso >= (int)(sizeof g_pso / sizeof *g_pso)) return nil;
    failed = 1;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:kUnwarpSrc options:nil error:&err];
    if (!lib) {
        fprintf(stderr, "  [fov] the unwarp shader did not compile: %s\n",
                err.localizedDescription.UTF8String);
        return nil;
    }
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    id<MTLFunction> vs = [lib newFunctionWithName:@"klf_vs"], fs = [lib newFunctionWithName:@"klf_fs"];
    d.vertexFunction = vs; d.fragmentFunction = fs;
    d.colorAttachments[0].pixelFormat = fmt;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    KLF_RELEASE(vs); KLF_RELEASE(fs); KLF_RELEASE(d); KLF_RELEASE(lib);
    if (!pso) {
        fprintf(stderr, "  [fov] no unwarp pipeline for pixel format %lu: %s\n",
                (unsigned long)fmt, err.localizedDescription.UTF8String);
        return nil;
    }
#if __has_feature(objc_arc)
    KLF_KEEP(pso);
#endif
    g_pso[g_npso].fmt = fmt; g_pso[g_npso].pso = pso; g_npso++;
    failed = 0;
    return pso;
}

// The squeezed picture in `resolve` (slices slice..slice+layers-1), drawn back
// over itself at full size.
static void klf_unwarp(const klf_open *o) {
    klf_map *k = &g_maps[o->map];
    id<MTLTexture> dst = o->resolve;
    id<MTLDevice> dev = dst.device;
    id<MTLRenderPipelineState> pso = klf_pipeline(dev, dst.pixelFormat);
    if (!pso) return;
    if (!k->aside || k->aside_fmt != dst.pixelFormat) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:dst.pixelFormat width:k->pw height:k->ph mipmapped:NO];
        td.textureType = MTLTextureType2DArray;
        td.arrayLength = 2;
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        if (!t) return;
        t.label = @"klepton foveation aside";
#if __has_feature(objc_arc)
        KLF_KEEP(t);
#endif
        k->aside = t; k->aside_fmt = dst.pixelFormat;    // a replaced one is left to leak: once a run
    }
    NSUInteger cw = k->pw < dst.width ? k->pw : dst.width, ch = k->ph < dst.height ? k->ph : dst.height;
    id<MTLBlitCommandEncoder> blit = [o->cmd blitCommandEncoder];
    for (NSUInteger l = 0; l < o->layers && l < 2; l++)
        [blit copyFromTexture:dst sourceSlice:o->slice + l sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(cw, ch, 1)
                    toTexture:k->aside destinationSlice:l destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    for (NSUInteger l = 0; l < o->layers && l < 2; l++) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = dst;
        rp.colorAttachments[0].slice = o->slice + l;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [o->cmd renderCommandEncoderWithDescriptor:rp];
        NSUInteger vw = k->w < dst.width ? k->w : dst.width, vh = k->h < dst.height ? k->h : dst.height;
        [e setViewport:(MTLViewport){0, 0, (double)dst.width, (double)dst.height, 0, 1}];
        [e setScissorRect:(MTLScissorRect){0, 0, vw, vh}];
        [e setRenderPipelineState:pso];
        [e setFragmentTexture:k->aside atIndex:0];
        [e setFragmentBuffer:k->params offset:0 atIndex:0];
        uint32_t layer = (uint32_t)l;
        [e setFragmentBytes:&layer length:sizeof layer atIndex:1];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
    }
    g_unwarps++;
}

// KL_VK_FOVEATE_TRACE only: what else is encoded around the scene pass. A pass
// MoltenVK ends and re-opens (a compute or blit step in the middle of a Vulkan
// render pass) stores and reloads every sample of it, which no Vulkan-side
// count shows.
static IMP g_orig_compute, g_orig_blit;
static int g_trace_left;
static id klf_compute(id self, SEL _cmd) {
    if (g_trace_left > 0) { g_trace_left--; fprintf(stderr, "  [fov]   ... a compute encoder\n"); }
    return ((id (*)(id, SEL))g_orig_compute)(self, _cmd);
}
static id klf_blit(id self, SEL _cmd) {
    if (g_trace_left > 0) { g_trace_left--; fprintf(stderr, "  [fov]   ... a blit encoder\n"); }
    return ((id (*)(id, SEL))g_orig_blit)(self, _cmd);
}

// ---------------------------------------------------------------------------
// KL_MTL_PROFILE — where a frame's GPU time goes, pass by pass.
//
// Every render pass descriptor that comes by is given four timestamp samples
// (start and end of its vertex and of its fragment stage), in a counter sample
// buffer that is read back when its command buffer completes. Passes are
// summed by shape, and a table is printed every <frames> scene passes. A pass
// MoltenVK already instruments (a guest timestamp query) is left alone.
#define KLP_PASSES 120
#define KLP_POOL   6
typedef struct { uint32_t w, h; uint8_t samples, layers, depth_only, scene; } klp_kind;
typedef struct { klp_kind k; double vert, frag; unsigned long n; } klp_acc;
typedef struct { int slot, n, has_scene; klp_kind kinds[KLP_PASSES]; } klp_cb;
static klp_acc g_klp_acc[64];
static int     g_klp_nacc;
static __unsafe_unretained id<MTLCounterSampleBuffer> g_klp_buf[KLP_POOL];
static int     g_klp_busy[KLP_POOL];
static int     g_klp_every;
static double  g_klp_ns_per_tick = 1.0;
static uint64_t g_klp_cal_wall, g_klp_cal_gpu;
static unsigned long g_klp_frames, g_klp_cbs_scene, g_klp_cbs_other, g_klp_unsampled;
static double  g_klp_span_scene, g_klp_span_other;
static char    kKlpKey;

static int klp_acc_cmp(const void *a, const void *b) {
    const klp_acc *x = a, *y = b;
    double tx = x->vert + x->frag, ty = y->vert + y->frag;
    return tx < ty ? 1 : tx > ty ? -1 : 0;
}

static void klp_report(id<MTLDevice> dev) {
    // Ticks to nanoseconds, from two readings of both clocks some frames apart.
    MTLTimestamp cpu = 0, gpu = 0;
    [dev sampleTimestamps:&cpu gpuTimestamp:&gpu];
    uint64_t wall = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (g_klp_cal_gpu && gpu > g_klp_cal_gpu)
        g_klp_ns_per_tick = (double)(wall - g_klp_cal_wall) / (double)(gpu - g_klp_cal_gpu);
    double f = (double)(g_klp_frames ? g_klp_frames : 1), k = g_klp_ns_per_tick / 1e6 / f;
    double sum = 0;
    for (int i = 0; i < g_klp_nacc; i++) sum += (g_klp_acc[i].vert + g_klp_acc[i].frag) * k;
    fprintf(stderr, "  [mtl] GPU time by render pass over %lu frames (%.2f ns a tick): %.1f ms a frame "
                    "in render passes; command buffers with the scene pass span %.1f ms each, the "
                    "%.1f others a frame %.1f ms each%s\n", g_klp_frames, g_klp_ns_per_tick, sum,
            g_klp_cbs_scene ? g_klp_span_scene * 1000.0 / (double)g_klp_cbs_scene : 0.0,
            (double)g_klp_cbs_other / f,
            g_klp_cbs_other ? g_klp_span_other * 1000.0 / (double)g_klp_cbs_other : 0.0,
            g_klp_unsampled ? " (some command buffers were not sampled)" : "");
    qsort(g_klp_acc, (size_t)g_klp_nacc, sizeof *g_klp_acc, klp_acc_cmp);
    for (int i = 0; i < g_klp_nacc && i < 14; i++) {
        klp_acc *a = &g_klp_acc[i];
        if ((a->vert + a->frag) * k < 0.05) break;
        fprintf(stderr, "  [mtl]   %5.1f x a frame: %4ux%-4u x%u layer(s), %ux samples%s%s — vertex "
                        "%.2f ms, fragment %.2f ms a frame\n", (double)a->n / f, a->k.w, a->k.h,
                a->k.layers, a->k.samples, a->k.depth_only ? ", depth only" : "",
                a->k.scene ? "  <- the scene pass" : "", a->vert * k, a->frag * k);
    }
    g_klp_nacc = 0; g_klp_frames = 0; g_klp_cbs_scene = g_klp_cbs_other = g_klp_unsampled = 0;
    g_klp_span_scene = g_klp_span_other = 0;
    g_klp_cal_wall = wall; g_klp_cal_gpu = gpu;
}

static void klp_done(klp_cb *p, id<MTLCommandBuffer> cb) {
    @autoreleasepool {
        NSData *data = p->n ? [g_klp_buf[p->slot] resolveCounterRange:NSMakeRange(0, (NSUInteger)p->n * 4)] : nil;
        const MTLCounterResultTimestamp *ts = data ? (const MTLCounterResultTimestamp *)data.bytes : NULL;
        pthread_mutex_lock(&g_mu);
        for (int i = 0; ts && i < p->n && data.length >= (NSUInteger)(i + 1) * 4 * sizeof *ts; i++) {
            uint64_t v0 = ts[i * 4].timestamp, v1 = ts[i * 4 + 1].timestamp;
            uint64_t f0 = ts[i * 4 + 2].timestamp, f1 = ts[i * 4 + 3].timestamp;
            if (v0 == MTLCounterErrorValue || v1 == MTLCounterErrorValue ||
                f0 == MTLCounterErrorValue || f1 == MTLCounterErrorValue) continue;
            int a = -1;
            for (int j = 0; j < g_klp_nacc; j++)
                if (!memcmp(&g_klp_acc[j].k, &p->kinds[i], sizeof(klp_kind))) { a = j; break; }
            if (a < 0 && g_klp_nacc < (int)(sizeof g_klp_acc / sizeof *g_klp_acc)) {
                a = g_klp_nacc++;
                memset(&g_klp_acc[a], 0, sizeof g_klp_acc[a]);
                g_klp_acc[a].k = p->kinds[i];
            }
            if (a < 0) continue;
            if (v1 > v0) g_klp_acc[a].vert += (double)(v1 - v0);
            if (f1 > f0) g_klp_acc[a].frag += (double)(f1 - f0);
            g_klp_acc[a].n++;
        }
        double span = cb.GPUEndTime - cb.GPUStartTime;
        if (p->has_scene) { g_klp_frames++; g_klp_cbs_scene++; g_klp_span_scene += span; }
        else              { g_klp_cbs_other++; g_klp_span_other += span; }
        g_klp_busy[p->slot] = 0;
        if (g_klp_frames >= (unsigned long)g_klp_every) klp_report(cb.device);
        pthread_mutex_unlock(&g_mu);
    }
    free(p);
}

// Give this pass its four samples. Called with the descriptor still ours to edit.
static void klp_pass(id cmd, MTLRenderPassDescriptor *d, int scene) {
    if (!g_klp_every || !d) return;
    MTLRenderPassSampleBufferAttachmentDescriptor *sa = d.sampleBufferAttachments[0];
    if (sa.sampleBuffer) return;                       // MoltenVK's own; not ours to replace
    NSValue *v = objc_getAssociatedObject(cmd, &kKlpKey);
    klp_cb *p = v ? (klp_cb *)v.pointerValue : NULL;
    if (!v) {
        int slot = -1;
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < KLP_POOL; i++) if (!g_klp_busy[i] && g_klp_buf[i]) { slot = i; g_klp_busy[i] = 1; break; }
        if (slot < 0) g_klp_unsampled++;
        pthread_mutex_unlock(&g_mu);
        if (slot >= 0 && (p = calloc(1, sizeof *p))) {
            p->slot = slot;
            klp_cb *q = p;
            [(id<MTLCommandBuffer>)cmd addCompletedHandler:^(id<MTLCommandBuffer> cb) { klp_done(q, cb); }];
        }
        // NULL is remembered too: a command buffer that got no buffer stays unsampled.
        objc_setAssociatedObject(cmd, &kKlpKey, [NSValue valueWithPointer:p], OBJC_ASSOCIATION_RETAIN);
    }
    if (!p || p->n >= KLP_PASSES) return;
    id<MTLTexture> c = d.colorAttachments[0].texture, z = d.depthAttachment.texture;
    id<MTLTexture> t = c ? c : z;
    klp_kind *k = &p->kinds[p->n];
    memset(k, 0, sizeof *k);
    if (t) {
        NSUInteger lvl = c ? d.colorAttachments[0].level : d.depthAttachment.level;
        k->w = (uint32_t)(t.width >> lvl ? t.width >> lvl : 1);
        k->h = (uint32_t)(t.height >> lvl ? t.height >> lvl : 1);
        k->samples = (uint8_t)t.sampleCount;
    }
    k->layers = (uint8_t)(d.renderTargetArrayLength ? d.renderTargetArrayLength : 1);
    k->depth_only = !c;
    k->scene = (uint8_t)scene;
    if (scene) p->has_scene = 1;
    NSUInteger b = (NSUInteger)p->n * 4;
    sa.sampleBuffer = g_klp_buf[p->slot];
    sa.startOfVertexSampleIndex = b;       sa.endOfVertexSampleIndex = b + 1;
    sa.startOfFragmentSampleIndex = b + 2; sa.endOfFragmentSampleIndex = b + 3;
    p->n++;
}

static void klp_install(id<MTLDevice> dev) {
    g_klp_every = kl_env_int("KL_MTL_PROFILE", 0);
    if (g_klp_every <= 0) { g_klp_every = 0; return; }
    id<MTLCounterSet> set = nil;
    for (id<MTLCounterSet> cs in dev.counterSets)
        if ([cs.name isEqualToString:MTLCommonCounterSetTimestamp]) set = cs;
    if (!set || ![dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
        fprintf(stderr, "  [mtl] this GPU does not sample timestamps at stage boundaries — "
                        "KL_MTL_PROFILE ignored\n");
        g_klp_every = 0;
        return;
    }
    MTLCounterSampleBufferDescriptor *d = [[MTLCounterSampleBufferDescriptor alloc] init];
    d.counterSet = set;
    d.storageMode = MTLStorageModeShared;
    d.sampleCount = KLP_PASSES * 4;
    int made = 0;
    for (int i = 0; i < KLP_POOL; i++) {
        NSError *err = nil;
        id<MTLCounterSampleBuffer> b = [dev newCounterSampleBufferWithDescriptor:d error:&err];
        if (!b) break;
#if __has_feature(objc_arc)
        KLF_KEEP(b);
#endif
        g_klp_buf[i] = b; made++;
    }
    KLF_RELEASE(d);
    MTLTimestamp cpu = 0, gpu = 0;
    [dev sampleTimestamps:&cpu gpuTimestamp:&gpu];
    g_klp_cal_wall = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); g_klp_cal_gpu = gpu;
    if (!made) g_klp_every = 0;
    fprintf(stderr, "  [mtl] KL_MTL_PROFILE=%d: %d counter sample buffer(s) of %d passes each\n",
            g_klp_every, made, KLP_PASSES);
}

// ---------------------------------------------------------------------------
// KL_MTL_TEX_TRACE — every large texture this process asks Metal for, whoever
// asks: the guest through MoltenVK, the OpenXR runtime, the compositor. The
// Vulkan-side census (KL_VK_PROFILE) sees only the first of those.
static IMP g_orig_newtex;
static unsigned klt_bits(MTLPixelFormat f) {
    switch ((unsigned long)f) {
    case 10: case 11: case 13: case 14: return 8;          // R8, A8
    case 20: case 22: case 23: case 24: case 25: case 30: case 31: case 250: return 16;
    case 110: case 112: case 113: case 114: case 115: case 105: return 64;   // RGBA16, RG32F
    case 123: case 124: case 125: return 128;              // RGBA32
    case 260: case 255: return 40;                         // Depth32Float_Stencil8, D24S8
    default: return 32;
    }
}
static id klt_newtex(id self, SEL _cmd, MTLTextureDescriptor *d) {
    id t = ((id (*)(id, SEL, id))g_orig_newtex)(self, _cmd, d);
    if (d) {
        double b = (double)d.width * (double)d.height * (double)(d.depth ? d.depth : 1) *
                   (double)(d.arrayLength ? d.arrayLength : 1) * (double)(d.sampleCount ? d.sampleCount : 1) *
                   klt_bits(d.pixelFormat) / 8.0;
        if (d.mipmapLevelCount > 1) b *= 4.0 / 3.0;
        static double total_mb; static unsigned long n;
        if (b >= 16.0 * 1048576.0) {
            pthread_mutex_lock(&g_mu);
            total_mb += b / 1048576.0; n++;
            fprintf(stderr, "  [mtl] texture %lux%lu x%lu slice(s), depth %lu, %lux samples, %lu mip(s), "
                            "format %lu, usage %#lx, storage %lu — about %.0f MB (large ones so far: "
                            "%lu, %.0f MB, counting none as freed)\n",
                    (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.arrayLength,
                    (unsigned long)d.depth, (unsigned long)d.sampleCount,
                    (unsigned long)d.mipmapLevelCount, (unsigned long)d.pixelFormat,
                    (unsigned long)d.usage, (unsigned long)d.storageMode, b / 1048576.0, n, total_mb);
            pthread_mutex_unlock(&g_mu);
        }
    }
    return t;
}

static int klf_is_resolve(MTLStoreAction a) {
    return a == MTLStoreActionMultisampleResolve || a == MTLStoreActionStoreAndMultisampleResolve;
}

// MoltenVK opens its passes with the store action UNKNOWN and names it as the
// pass ends — which is when it knows whether this encoder is the subpass's last
// or one it is about to restart. So that is where a resolve is learnt of.
static void klf_store(id self, SEL _cmd, MTLStoreAction a, NSUInteger index) {
    if (index == 0) {
        pthread_mutex_lock(&g_mu);
        for (unsigned i = 0; i < sizeof g_open / sizeof *g_open; i++)
            if (g_open[i].enc == self) { g_open[i].resolves = klf_is_resolve(a); break; }
        pthread_mutex_unlock(&g_mu);
    }
    ((void (*)(id, SEL, MTLStoreAction, NSUInteger))g_orig_store)(self, _cmd, a, index);
}

static void klf_end(id self, SEL _cmd) {
    klf_open o; int found = 0;
    pthread_mutex_lock(&g_mu);
    for (unsigned i = 0; i < sizeof g_open / sizeof *g_open; i++)
        if (g_open[i].enc == self) { o = g_open[i]; g_open[i].enc = nil; found = 1; break; }
    pthread_mutex_unlock(&g_mu);
    ((void (*)(id, SEL))g_orig_end)(self, _cmd);
    if (found && g_trace_left > 0) {
        g_trace_left--;
        fprintf(stderr, "  [fov]   scene encoder ended, %s — by:\n",
                o.resolves ? "resolving" : "NOT resolving (stored, to be re-opened)");
        void *bt[14];
        int n = backtrace(bt, 14);
        backtrace_symbols_fd(bt + 1, n - 1, 2);
    }
    if (found && o.resolve && o.resolves) {
        @autoreleasepool {
            pthread_mutex_lock(&g_mu);
            klf_unwarp(&o);
            pthread_mutex_unlock(&g_mu);
        }
    }
}

// ---------------------------------------------------------------------------
// KL_MTL_GPU_TIME — the GPU time of the game's frame, the cheap way.
//
// Every command buffer knows when the GPU started and finished it. The ones
// that carry the scene pass — one a frame — are given a completed handler, and
// the mean of their spans since it was last asked for is what kl_app.c prints
// in its [perf] line and the compositor shows in its readout. That is not the
// whole frame (the engine's other command buffers, a millisecond or two, run
// beside it; KL_MTL_PROFILE has the full account at the price of sampling every
// pass), but it is the part that decides whether the frame makes a refresh.
// On for Half-Life: Alyx; KL_MTL_GPU_TIME=0 switches it off.
static int g_gt_on, g_ml_guard;
static double g_gt_sum;
static unsigned long g_gt_n;
static char kGtKey;

double kl_mtl_scene_gpu_ms(void) {
    pthread_mutex_lock(&g_mu);
    double ms = g_gt_n ? g_gt_sum * 1000.0 / (double)g_gt_n : -1.0;
    g_gt_sum = 0; g_gt_n = 0;
    pthread_mutex_unlock(&g_mu);
    return ms;
}

// Two bins beside the running mean, for a comparison made inside one run
// (KL_VK_METER_STORE_AB, kl_vulkan.c): whoever alternates between two ways of
// drawing says which one is on, and each scene command buffer is counted in the
// bin that was current when it was begun.
static int g_gt_phase;
static double g_gtp_sum[2];
static unsigned long g_gtp_n[2];
void kl_mtl_gpu_phase(int phase) {
    pthread_mutex_lock(&g_mu);
    g_gt_phase = phase & 1;
    pthread_mutex_unlock(&g_mu);
}
// The mean of one bin in ms and how many command buffers it holds; emptied by the call.
double kl_mtl_gpu_phase_take(int phase, unsigned long *n) {
    pthread_mutex_lock(&g_mu);
    phase &= 1;
    double ms = g_gtp_n[phase] ? g_gtp_sum[phase] * 1000.0 / (double)g_gtp_n[phase] : -1.0;
    if (n) *n = g_gtp_n[phase];
    g_gtp_sum[phase] = 0; g_gtp_n[phase] = 0;
    pthread_mutex_unlock(&g_mu);
    return ms;
}

static void klg_watch(id cmd) {
    if (objc_getAssociatedObject(cmd, &kGtKey)) return;
    objc_setAssociatedObject(cmd, &kGtKey, @YES, OBJC_ASSOCIATION_RETAIN);
    pthread_mutex_lock(&g_mu);
    int phase = g_gt_phase;
    pthread_mutex_unlock(&g_mu);
    [(id<MTLCommandBuffer>)cmd addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        double span = cb.GPUEndTime - cb.GPUStartTime;
        if (!(span > 0) || span > 1.0) return;
        pthread_mutex_lock(&g_mu);
        g_gt_sum += span; g_gt_n++;
        g_gtp_sum[phase] += span; g_gtp_n[phase]++;
        pthread_mutex_unlock(&g_mu);
    }];
}

// A memoryless attachment (KL_VK_MSAA_MEMORYLESS, kl_vulkan.c) exists inside one
// render pass only: a pass may clear it and resolve it, and may neither load nor
// store it. The engine's scene pass does neither with its multisampled depth.
// Should any other pass ask for one of the two, Metal must not be handed it —
// the action is made the legal one next to it and the case is counted and said,
// because that pass then draws without the depth it wanted.
static unsigned long g_ml_passes, g_ml_loads, g_ml_stores;
static void klm_guard(MTLRenderPassAttachmentDescriptor *a) {
    id<MTLTexture> t = a ? a.texture : nil;
    if (!t || t.storageMode != MTLStorageModeMemoryless) return;
    int loads = 0, stores = 0;
    if (a.loadAction == MTLLoadActionLoad) { a.loadAction = MTLLoadActionDontCare; loads = 1; }
    if (a.storeAction == MTLStoreActionStore) { a.storeAction = MTLStoreActionDontCare; stores = 1; }
    else if (a.storeAction == MTLStoreActionStoreAndMultisampleResolve) {
        a.storeAction = MTLStoreActionMultisampleResolve; stores = 1;
    }
    pthread_mutex_lock(&g_mu);
    unsigned long n = ++g_ml_passes;
    g_ml_loads += (unsigned long)loads; g_ml_stores += (unsigned long)stores;
    unsigned long nl = g_ml_loads, ns = g_ml_stores;
    pthread_mutex_unlock(&g_mu);
    if ((loads && nl <= 4) || (stores && ns <= 4))
        fprintf(stderr, "  [mtl] memoryless: a pass wanted to %s a memoryless %lux%lu attachment — refused "
                        "(%lu load(s), %lu store(s) so far); that pass draws without it\n",
                loads ? "LOAD" : "STORE", (unsigned long)t.width, (unsigned long)t.height, nl, ns);
    if (n == 1 || n % 20000 == 0)
        fprintf(stderr, "  [mtl] memoryless: %lu pass(es) with a memoryless attachment, %lu load(s) and %lu "
                        "store(s) refused\n", n, nl, ns);
}

static id klf_begin(id self, SEL _cmd, MTLRenderPassDescriptor *d) {
    if (d && g_ml_guard) {
        klm_guard(d.depthAttachment);
        klm_guard(d.stencilAttachment);
    }
    MTLRenderPassColorAttachmentDescriptor *c = d ? d.colorAttachments[0] : nil;
    id<MTLTexture> t = c ? c.texture : nil;
    int mi = -1;
    NSUInteger w = 0, h = 0;
    int scene = t && t.sampleCount > 1 && t.width >= 1024 && t.height >= 1024;
    klp_pass(self, d, scene);
    if (scene && g_gt_on) klg_watch(self);
    if (g_fov_on && scene && !d.rasterizationRateMap) {
        w = d.renderTargetWidth ? d.renderTargetWidth : t.width;
        h = d.renderTargetHeight ? d.renderTargetHeight : t.height;
        if (g_trace) {
            static int n;
            if (n++ < 12)
                fprintf(stderr, "  [fov] candidate pass: colour %lux%lu x%lu slice(s) type %lu, %lux "
                                "samples, slice %lu, target array length %lu, target %lux%lu, load %lu "
                                "store %lu, resolve %s slice %lu, depth %s resolve %s\n",
                        (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.arrayLength,
                        (unsigned long)t.textureType, (unsigned long)t.sampleCount,
                        (unsigned long)c.slice, (unsigned long)d.renderTargetArrayLength,
                        (unsigned long)d.renderTargetWidth, (unsigned long)d.renderTargetHeight,
                        (unsigned long)c.loadAction, (unsigned long)c.storeAction,
                        c.resolveTexture ? "yes" : "no", (unsigned long)c.resolveSlice,
                        d.depthAttachment.texture ? "yes" : "no",
                        d.depthAttachment.resolveTexture ? "yes" : "no");
        }
        // Layered only: a single-layer pass of an array would take the map's
        // layer 0 for either eye, and the right eye's fovea is not the left's.
        if (d.renderTargetArrayLength >= 2) {
            pthread_mutex_lock(&g_mu);
            mi = klf_map_for(t.device, w, h);
            pthread_mutex_unlock(&g_mu);
        }
        if (mi >= 0) d.rasterizationRateMap = g_maps[mi].map;
    }
    if (g_trace_left > 0) {
        g_trace_left--;
        fprintf(stderr, "  [fov]   render encoder %lux%lu %lux load %lu%s\n",
                (unsigned long)(t ? t.width : 0), (unsigned long)(t ? t.height : 0),
                (unsigned long)(t ? t.sampleCount : 0), (unsigned long)(c ? c.loadAction : 99),
                mi >= 0 ? "  <- the scene pass" : "");
    }
    id enc = ((id (*)(id, SEL, id))g_orig_begin)(self, _cmd, d);
    if (mi >= 0 && enc) {
        pthread_mutex_lock(&g_mu);
        if (!g_enc_class) {
            Class cls = object_getClass(enc);
            Method m = class_getInstanceMethod(cls, @selector(endEncoding));
            Method ms = class_getInstanceMethod(cls, @selector(setColorStoreAction:atIndex:));
            if (m && ms) {
                g_orig_end = method_getImplementation(m);
                g_orig_store = method_getImplementation(ms);
                class_replaceMethod(cls, @selector(endEncoding), (IMP)klf_end, method_getTypeEncoding(m));
                class_replaceMethod(cls, @selector(setColorStoreAction:atIndex:), (IMP)klf_store,
                                    method_getTypeEncoding(ms));
                g_enc_class = cls;
            }
        }
        if (g_enc_class == object_getClass(enc)) {
            for (unsigned i = 0; i < sizeof g_open / sizeof *g_open; i++)
                if (!g_open[i].enc) {
                    g_open[i].enc = enc; g_open[i].cmd = self; g_open[i].map = mi;
                    g_open[i].resolve = c.resolveTexture;
                    g_open[i].resolves = klf_is_resolve(c.storeAction);
                    g_open[i].slice = c.resolveSlice;
                    g_open[i].layers = d.renderTargetArrayLength;
                    break;
                }
        }
        if (g_trace && g_passes == 2400) g_trace_left = 120;      // well into a level, two frames' worth
        if (!(g_passes++ % 3000))
            fprintf(stderr, "  [fov] %lu foveated scene pass(es), %lu unwarp(s) so far\n",
                    g_passes, g_unwarps);
        pthread_mutex_unlock(&g_mu);
    }
    return enc;
}

// Called once a Metal device exists (kl_vulkan.c, at vkCreateDevice).
void kl_mtl_foveate_install(void) {
    static int done;
    if (done++) return;
    g_edge = kl_env_float("KL_VK_FOVEATE", 0.0f);
    g_fov_on = g_edge > 0.02f && g_edge < 0.999f;
    g_inner = kl_env_float("KL_VK_FOVEATE_INNER", 0.30f);
    if (!(g_inner >= 0.0f)) g_inner = 0.30f;
    g_outer = kl_env_float("KL_VK_FOVEATE_OUTER", 0.0f);
    g_trace = kl_env_on("KL_VK_FOVEATE_TRACE", 0);
    int textures = kl_env_on("KL_MTL_TEX_TRACE", 0);
    { const char *t = kl_driver_target_name();
      g_gt_on = kl_env_on("KL_MTL_GPU_TIME", t && strcmp(t, "alyx") == 0); }
    g_ml_guard = kl_env_on("KL_VK_MSAA_MEMORYLESS", 0);
    if (!g_fov_on && !g_trace && !textures && !g_gt_on && !g_ml_guard && kl_env_int("KL_MTL_PROFILE", 0) <= 0) return;
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        klp_install(dev);
        if (textures) {
            Class dc = object_getClass(dev);
            Method mt = class_getInstanceMethod(dc, @selector(newTextureWithDescriptor:));
            if (mt) {
                g_orig_newtex = method_getImplementation(mt);
                class_replaceMethod(dc, @selector(newTextureWithDescriptor:), (IMP)klt_newtex,
                                    method_getTypeEncoding(mt));
            }
        }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        Class cls = cb ? object_getClass(cb) : Nil;
        Method m = cls ? class_getInstanceMethod(cls, @selector(renderCommandEncoderWithDescriptor:)) : NULL;
        if (!m) {
            fprintf(stderr, "  [fov] no command buffer class to extend — KL_VK_FOVEATE and "
                            "KL_MTL_PROFILE ignored\n");
        } else if (g_fov_on || g_trace || g_klp_every || g_gt_on || g_ml_guard) {
            g_orig_begin = method_getImplementation(m);
            class_replaceMethod(cls, @selector(renderCommandEncoderWithDescriptor:),
                                (IMP)klf_begin, method_getTypeEncoding(m));
            if (g_trace) {
                Method mc = class_getInstanceMethod(cls, @selector(computeCommandEncoder));
                Method mb = class_getInstanceMethod(cls, @selector(blitCommandEncoder));
                if (mc && mb) {
                    g_orig_compute = method_getImplementation(mc);
                    g_orig_blit = method_getImplementation(mb);
                    class_replaceMethod(cls, @selector(computeCommandEncoder), (IMP)klf_compute,
                                        method_getTypeEncoding(mc));
                    class_replaceMethod(cls, @selector(blitCommandEncoder), (IMP)klf_blit,
                                        method_getTypeEncoding(mb));
                }
            }
            if (g_fov_on)
                fprintf(stderr, "  [fov] KL_VK_FOVEATE=%.2f: watching %s for the multisampled scene pass\n",
                        (double)g_edge, class_getName(cls));
        }
        [cb commit];
        KLF_RELEASE(q); KLF_RELEASE(dev);
    }
}
