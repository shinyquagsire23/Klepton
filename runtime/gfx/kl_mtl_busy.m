// The GPU's working time for this process, for the metrics panel
// (KL_STATS_CARD, visionos/Sources/KleptonMetrics.swift).
//
// kl_mtl_foveate.m times one command buffer of one kind of guest: the scene
// pass of a Vulkan game. This is every command buffer the process commits — the
// game's, whether ANGLE or MoltenVK made it, and the compositor's own. Each is
// given a completed handler as it is committed, and the spans between GPU start
// and GPU end, merged where they overlap, are the time the GPU worked for this
// app. Against the wall clock that is a utilization; less the compositor's
// share and over the game's frames it is the GPU time of a game frame — for a
// GL guest too, which has no other measure of it.
//
// There is no counter to read for this on the headset, so it is taken where
// Metal hands it out: -commit on the command buffer's class is asked to add the
// handler first (the Objective-C runtime, as in kl_mtl_foveate.m; no private
// interface is named). Nothing is installed until the panel asks, and what a
// frame then pays is one block and one lock per command buffer.
//
// Limits, so the number is read for what it is: a span is the time between a
// buffer's first and last work on the GPU, not the time the GPU was occupied
// by it alone — two apps' work interleaves — and completed handlers that arrive
// out of order lose the part an earlier-arriving span already covered.
//
// ...and the shaders, for the same panel. A game that stutters as something
// new comes into view is, more often than not, compiling: ANGLE and MoltenVK
// both hand Metal a shader's source when the guest first needs it and ask for a
// pipeline state when it is first drawn with, and either can take longer than a
// frame. Both are requests to the device, whichever of the two made them, so
// they are counted there — a library from source, a render pipeline state,
// through their blocking and their completion-handler forms — and told apart by
// how long the answer took: compiled, or only fetched from what the system had
// compiled before (klc_note).
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include "kl_mtl_busy.h"

static IMP g_orig_commit;
static int g_state;                         // 0 not tried, 1 counting, -1 not possible
static double g_busy_s, g_busy_end;
static unsigned long g_busy_n;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void klb_commit(id self, SEL _cmd) {
    [(id<MTLCommandBuffer>)self addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        double s = cb.GPUStartTime, e = cb.GPUEndTime;
        if (!(e > s) || e - s > 1.0) return;        // never ran, or not a span of one frame's work
        pthread_mutex_lock(&g_mu);
        if (s < g_busy_end) s = g_busy_end;         // the part another buffer's span already covers
        if (e > s) { g_busy_s += e - s; g_busy_end = e; }
        g_busy_n++;
        pthread_mutex_unlock(&g_mu);
    }];
    ((void (*)(id, SEL))g_orig_commit)(self, _cmd);
}

// The six ways a guest's shader reaches the device. One request is one count:
// where a form is built on another one (the plain pipeline call on the one that
// also returns reflection), the inner call is the same request.
//
// Compiled, or only fetched? Metal does not say; the time does. The system
// keeps what it has compiled — for this run and, on disk, for the next — and an
// answer out of that store comes back in a tenth of a millisecond, while the
// smallest shader there is takes four to compile and a real one tens (measured
// on an M-series Mac: 0.01-0.1 ms against 4-95). So a request answered in under
// two milliseconds is counted as cached and any other as compiled; for the
// completion-handler forms the time runs until the handler is called.
#define KLC_COMPILED_S 0.002
static _Atomic unsigned long g_compiled, g_cached;
static __thread int t_compiling;
static IMP g_o_lib, g_o_lib_cb, g_o_rps, g_o_rps_refl, g_o_rps_cb, g_o_rps_opt_cb;

static double klc_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static void klc_note(double since) {
    if (klc_now() - since >= KLC_COMPILED_S) g_compiled++; else g_cached++;
}

static id klc_lib(id self, SEL _cmd, id src, id opts, void *err) {
    double t0 = klc_now();
    t_compiling++;
    id r = ((id (*)(id, SEL, id, id, void *))g_o_lib)(self, _cmd, src, opts, err);
    if (!--t_compiling) klc_note(t0);
    return r;
}
static void klc_lib_cb(id self, SEL _cmd, id src, id opts, id handler) {
    if (t_compiling) { ((void (*)(id, SEL, id, id, id))g_o_lib_cb)(self, _cmd, src, opts, handler); return; }
    double t0 = klc_now();
    void (^h)(id, NSError *) = handler;
    ((void (*)(id, SEL, id, id, id))g_o_lib_cb)(self, _cmd, src, opts, ^(id lib, NSError *e) {
        klc_note(t0);
        if (h) h(lib, e);
    });
}
// Logic operations on the next render pipeline this thread asks Metal for
// (kl_mtl_logic_op_next). Metal has them only behind private methods of
// MTLRenderPipelineDescriptor, the ones MoltenVK calls when it is built to use
// private API; this build is not, so a VkPipeline's logicOp never reached
// Metal. HITMAN 3 VR: Reloaded ORs light bits into a tile mask with it
// (lightculling.frag). The enumeration is Apple's as MoltenVK declares it:
// Clear, Set, Copy, CopyInverted, Noop, Invert, And, Nand, Or, Nor, Xor,
// Equivalence, AndReverse, AndInverted, OrReverse, OrInverted.
// Not thread-local: MoltenVK may hand the descriptor to Metal from a thread of
// its own. The pending operation is applied only to a descriptor whose first
// colour attachment is an integer format, the only kind a logic operation is
// for, and stays set until the Vulkan side clears it.
static _Atomic int g_logic_op = -1;
void kl_mtl_logic_op_next(int metal_op) { atomic_store(&g_logic_op, metal_op); }
static int klc_int_format(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint:
    case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint: case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint:
    case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint:
    case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint:
    case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint:
        return 1;
    default: return 0;
    }
}
static void klc_logic(id desc) {
    int t_logic_op = atomic_load(&g_logic_op);
    if (t_logic_op < 0 || !desc || ![desc isKindOfClass:[MTLRenderPipelineDescriptor class]]) return;
    if (!klc_int_format(((MTLRenderPipelineDescriptor *)desc).colorAttachments[0].pixelFormat)) return;
    SEL en = NSSelectorFromString(@"setLogicOperationEnabled:"), op = NSSelectorFromString(@"setLogicOperation:");
    static int said;
    if ([desc respondsToSelector:en] && [desc respondsToSelector:op]) {
        ((void (*)(id, SEL, BOOL))objc_msgSend)(desc, en, YES);
        ((void (*)(id, SEL, NSUInteger))objc_msgSend)(desc, op, (NSUInteger)t_logic_op);
        if (said++ < 3) fprintf(stderr, "  [mtl] logic operation %d set on a render pipeline (private Metal API)\n", t_logic_op);
    } else if (!said++) {
        fprintf(stderr, "  [mtl] this Metal has no logic operations — the pipeline draws without one\n");
    }
}

static id klc_rps(id self, SEL _cmd, id desc, void *err) {
    klc_logic(desc);
    double t0 = klc_now();
    t_compiling++;
    id r = ((id (*)(id, SEL, id, void *))g_o_rps)(self, _cmd, desc, err);
    if (!--t_compiling) klc_note(t0);
    return r;
}
static id klc_rps_refl(id self, SEL _cmd, id desc, NSUInteger options, void *reflection, void *err) {
    klc_logic(desc);
    double t0 = klc_now();
    t_compiling++;
    id r = ((id (*)(id, SEL, id, NSUInteger, void *, void *))g_o_rps_refl)(self, _cmd, desc, options, reflection, err);
    if (!--t_compiling) klc_note(t0);
    return r;
}
static void klc_rps_cb(id self, SEL _cmd, id desc, id handler) {
    klc_logic(desc);
    if (t_compiling) { ((void (*)(id, SEL, id, id))g_o_rps_cb)(self, _cmd, desc, handler); return; }
    double t0 = klc_now();
    void (^h)(id, NSError *) = handler;
    ((void (*)(id, SEL, id, id))g_o_rps_cb)(self, _cmd, desc, ^(id state, NSError *e) {
        klc_note(t0);
        if (h) h(state, e);
    });
}
static void klc_rps_opt_cb(id self, SEL _cmd, id desc, NSUInteger options, id handler) {
    klc_logic(desc);
    if (t_compiling) {
        ((void (*)(id, SEL, id, NSUInteger, id))g_o_rps_opt_cb)(self, _cmd, desc, options, handler);
        return;
    }
    double t0 = klc_now();
    void (^h)(id, id, NSError *) = handler;
    ((void (*)(id, SEL, id, NSUInteger, id))g_o_rps_opt_cb)(self, _cmd, desc, options, ^(id state, id refl, NSError *e) {
        klc_note(t0);
        if (h) h(state, refl, e);
    });
}

static void klc_swap(Class c, SEL s, IMP mine, IMP *orig) {
    Method m = class_getInstanceMethod(c, s);
    if (!m) return;
    *orig = method_getImplementation(m);
    class_replaceMethod(c, s, mine, method_getTypeEncoding(m));
}

void kl_mtl_compiles_take(unsigned long *compiled, unsigned long *cached) {
    unsigned long n = atomic_exchange(&g_compiled, 0), c = atomic_exchange(&g_cached, 0);
    if (compiled) *compiled = n;
    if (cached) *cached = c;
}

int kl_mtl_busy_install(void) {
    pthread_mutex_lock(&g_mu);
    if (!g_state) {
        g_state = -1;
        @autoreleasepool {
            id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
            id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer];
            Class cls = cb ? object_getClass(cb) : Nil;
            Method m = cls ? class_getInstanceMethod(cls, @selector(commit)) : NULL;
            if (m) {
                g_orig_commit = method_getImplementation(m);
                class_replaceMethod(cls, @selector(commit), (IMP)klb_commit, method_getTypeEncoding(m));
                g_state = 1;
            }
            Class dc = dev ? object_getClass(dev) : Nil;
            if (dc) {
                klc_swap(dc, @selector(newLibraryWithSource:options:error:), (IMP)klc_lib, &g_o_lib);
                klc_swap(dc, @selector(newLibraryWithSource:options:completionHandler:), (IMP)klc_lib_cb, &g_o_lib_cb);
                klc_swap(dc, @selector(newRenderPipelineStateWithDescriptor:error:), (IMP)klc_rps, &g_o_rps);
                klc_swap(dc, @selector(newRenderPipelineStateWithDescriptor:options:reflection:error:),
                         (IMP)klc_rps_refl, &g_o_rps_refl);
                klc_swap(dc, @selector(newRenderPipelineStateWithDescriptor:completionHandler:), (IMP)klc_rps_cb,
                         &g_o_rps_cb);
                klc_swap(dc, @selector(newRenderPipelineStateWithDescriptor:options:completionHandler:),
                         (IMP)klc_rps_opt_cb, &g_o_rps_opt_cb);
            }
        }
    }
    int on = g_state > 0;
    pthread_mutex_unlock(&g_mu);
    return on;
}

double kl_mtl_busy_take(unsigned long *buffers) {
    pthread_mutex_lock(&g_mu);
    double s = g_state > 0 ? g_busy_s : -1.0;
    if (buffers) *buffers = g_busy_n;
    g_busy_s = 0; g_busy_n = 0;
    pthread_mutex_unlock(&g_mu);
    return s;
}
