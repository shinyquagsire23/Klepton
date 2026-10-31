// kl_mtl_busy.m: the GPU's working time for this process, from the spans of
// the command buffers it commits (the metrics panel's GPU numbers).
//
// What can be said for certain on any machine: before the install nothing is
// counted; after it every committed buffer is, whichever queue it came from;
// the time is positive for buffers that did work and cannot exceed the wall
// clock they ran in; and a take empties the count. Skips itself without a
// Metal device.
#import <Metal/Metal.h>
#include <stdio.h>
#include <time.h>
#include "kl_mtl_busy.h"

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// One command buffer that clears a large target: real work, however little.
static id<MTLCommandBuffer> clear(id<MTLCommandQueue> q, id<MTLTexture> t) {
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = t;
    d.colorAttachments[0].loadAction = MTLLoadActionClear;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    [[cb renderCommandEncoderWithDescriptor:d] endEncoding];
    return cb;
}

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); return 1; } } while (0)

int main(void) {
    printf("=== mtlbusy: GPU time of the process's command buffers ===\n");
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("mtlbusy: no Metal device — skipped\n"); return 0; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:4096 height:4096 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> q1 = [dev newCommandQueue], q2 = [dev newCommandQueue];

        unsigned long n = 99;
        CHECK(kl_mtl_busy_take(&n) < 0 && n == 0, "a take before the install must say 'not counting'");
        id<MTLCommandBuffer> before = clear(q1, tex);
        [before commit];
        [before waitUntilCompleted];

        CHECK(kl_mtl_busy_install() == 1, "the install found no command buffer class to extend");
        CHECK(kl_mtl_busy_install() == 1, "a second install must answer as the first did");
        double s = kl_mtl_busy_take(&n);
        CHECK(s == 0 && n == 0, "the buffer committed before the install was counted (%.6f s, %lu)", s, n);

        // Two queues, as in the app: the game's and the compositor's.
        double t0 = now_s();
        id<MTLCommandBuffer> last = nil;
        for (int i = 0; i < 40; i++) {
            last = clear(i & 1 ? q2 : q1, tex);
            [last commit];
        }
        [last waitUntilCompleted];
        id<MTLCommandBuffer> other = clear(q1, tex);
        [other commit];
        [other waitUntilCompleted];
        // Completed handlers run on Metal's own queue; the last one may still be on its way.
        struct timespec nap = { 0, 50000000 };
        nanosleep(&nap, NULL);
        double wall = now_s() - t0;
        s = kl_mtl_busy_take(&n);
        printf("  41 buffers on two queues: %lu counted, GPU worked %.3f ms of %.1f ms\n", n, s * 1000.0, wall * 1000.0);
        CHECK(n == 41, "41 buffers were committed, %lu were counted", n);
        CHECK(s > 0, "buffers that cleared a 4096x4096 target took no GPU time");
        CHECK(s <= wall, "the GPU worked %.3f ms in %.3f ms of wall clock — overlapping spans were counted twice",
              s * 1000.0, wall * 1000.0);

        s = kl_mtl_busy_take(&n);
        CHECK(s == 0 && n == 0, "a take must empty the count (%.6f s, %lu left)", s, n);

        // The shaders. A source nobody has compiled before (the constant in it is this run's own) and a
        // pipeline state from it have to be compiled; the same two asked for again are in the system's
        // store. Asked through the blocking forms first and the completion-handler forms second, that is
        // two compiled and two cached, however the device builds one form on another.
        unsigned long fresh = 9, cached = 9;
        kl_mtl_compiles_take(&fresh, &cached);
        CHECK(fresh == 0 && cached == 0, "something was compiled before anything was asked for");
        NSString *msl = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
                         "struct V { float4 p [[position]]; float3 n; };\n"
                         "vertex V v(uint i [[vertex_id]]) { V o; o.p = float4(float(i), %f, 0, 1);"
                         " o.n = normalize(float3(float(i), 1, 2)); return o; }\n"
                         "fragment float4 f(V in [[stage_in]]) { float3 c = abs(in.n);"
                         " for (int k = 0; k < 4; k++) c = sin(c * 3.0 + float(k)); return float4(c, 1); }\n",
                         now_s()];
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:nil error:&err];
        CHECK(lib, "the test's own shader does not compile: %s", err.localizedDescription.UTF8String);
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"v"];
        pd.fragmentFunction = [lib newFunctionWithName:@"f"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
        CHECK(ps, "the test's own pipeline cannot be made: %s", err.localizedDescription.UTF8String);
        kl_mtl_compiles_take(&fresh, &cached);
        printf("  a new shader and its pipeline state: %lu compiled, %lu cached\n", fresh, cached);
        CHECK(fresh == 2 && cached == 0, "a new shader and its pipeline: %lu compiled, %lu cached", fresh, cached);

        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block int answered = 0;
        [dev newLibraryWithSource:msl options:nil completionHandler:^(id<MTLLibrary> l, NSError *e) {
            answered += l != nil;
            dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        [dev newRenderPipelineStateWithDescriptor:pd completionHandler:^(id<MTLRenderPipelineState> p, NSError *e) {
            answered += p != nil;
            dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        CHECK(answered == 2, "the completion handlers were not handed their results (%d of 2)", answered);
        kl_mtl_compiles_take(&fresh, &cached);
        printf("  the same two again: %lu compiled, %lu cached\n", fresh, cached);
        CHECK(fresh == 0 && cached == 2, "the same shader and pipeline again: %lu compiled, %lu cached", fresh, cached);
        kl_mtl_compiles_take(&fresh, &cached);
        CHECK(fresh == 0 && cached == 0, "a take must empty the counts of compiles");
    }
    printf("mtlbusy: PASS\n");
    return 0;
}
