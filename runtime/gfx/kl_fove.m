// kl_fove — fixed foveated rendering for a Vulkan guest through a Metal
// rasterization rate map. The measured facts this rests on (Metal's rate-map
// semantics, how a guest's eye passes and shaders use screen position) and the
// full design, measurements and known gaps are in docs/FOVEATION.md; the knobs
// and log lines in DEBUG_ENV_VARS.md, "Fixed foveation".
//
// Klepton imposes the map; the guest's own FDM path stays off. Five pieces:
//
//  1. WHICH passes. kl_vulkan recognises the eye images and hands their
//     MTLTextures here. Three shapes (docs/FOVEATION.md §5): a
//     double-wide eye (colour + depth, both exactly the extent of the texture
//     the guest hands its runtime, 1 sample — no runtime in this tree reports
//     one yet, see kf_double_eye_geometry); the runtime's own eye images (kl_ovrp / kl_openxr swapchains),
//     drawn directly or resolved into from an eye-size MSAA target; and the
//     guest's own eye-size intermediate that later reaches an eye image. Any
//     Metal render encoder MoltenVK later opens whose colour attachment 0 (or
//     depth attachment) is one of them is a "marked" pass — for the
//     double-wide eye: the eye clear, the main eye pass and the second encoder
//     MoltenVK opens inside it after the local-read barrier, and the
//     hidden-area mask passes when those are on.
//
//  2. THE MAP. One MTLRasterizationRateMap per target shape, screen size = the
//     eye target, separable: per eye the envelope of two peaks, at that eye's
//     straight-ahead point (from the frustum the runtime hands the guest) and
//     at the other eye's placed in the same rect, so both eyes get the same
//     map relative to their pictures (KL_FOVE_PER_EYE=1: its own peak only);
//     the double-wide eye as one such pair per half, a layered eye pair as
//     one map layer per eye. Per axis the profile is Half-Life: Alyx's square
//     density profile: full density inside a box, a linear ramp, a floor.
//     The marked encoders get it on a copy of their descriptor and render IN
//     PLACE into the guest's screen-size images (Metal writes the top-left
//     physical region; LOAD/STORE passes sharing a map join).
//
//  3. gl_FragCoord. Under a map Metal's [[position]] is PHYSICAL; 99.6% of
//     Half-Life: Alyx's main-pass draws read it as a screen-space texture
//     coordinate. So every SPIRV-Cross fragment entry point (MoltenVK
//     hands Metal MSL SOURCE) that takes [[position]] gets one more argument,
//     `constant rasterization_rate_map_data& [[buffer(N)]]`, and its first
//     statement maps the position back to screen space. Marked encoders bind
//     the real map's parameter buffer at N; every other render encoder binds an
//     all-1.0 map's (an exact identity). N must be free in every
//     patched shader (a shader that has it taken is REFUSED, and any refusal
//     turns foveation off) and nobody else may bind N (setFragmentBuffer* at N
//     by anyone but us is counted; inside a marked encoder it turns
//     foveation off).
//
//     A marked pass that LOADs the eye is only mapped while an expand of it
//     is pending on the same command buffer (the eye still in physical
//     layout); otherwise it draws unfoveated (counted, logged).
//
//  4. THE EXPAND. After the last marked encoder and before the next render or
//     compute encoder (or the command buffer's commit), the eye colour's
//     physical region is copied to a scratch texture and drawn back over the
//     whole eye image with map_screen_to_physical_coordinates (bilinear, each
//     eye clamped to its own physical region so nothing bleeds across the
//     nasal seam; a layered eye per slice). Blit encoders do not trigger it
//     for an eye image (kl_vulkan's single-sample depth-resolve copy sits
//     between the eye clear and the main pass). Depth is not expanded:
//     nothing reads the eye depth after the eye pass.
//
//  5. THE EXPAND FENCE. MoltenVK orders its passes with its own MTLFences, not
//     Metal's hazard tracking; the expand's encoders are ours and took no part
//     in that, so the guest's next reader of the eye could run CONCURRENTLY
//     with the expand and see the raw physical layout. Every expand path
//     (single-sample one-slice, single-sample layered, MSAA) has its last
//     encoder update g_xfence, and every encoder opened afterwards on the same
//     queue waits on it. KL_FOVE_FENCE=0 = unordered.
//
// Off (the default) installs nothing. Debug knobs: KL_FOVE_FRAGCOORD=0
// leaves shaders unpatched (shows what the patch fixes), KL_FOVE_EXPAND=0
// leaves the eye in physical layout, KL_FOVE_MSL_DUMP=<dir> writes every
// MSL source MoltenVK compiles (as patched).
#include "kl_fove.h"
#include "kl_env.h"
#include "kl_ovrp.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <ctype.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#if __has_feature(objc_arc)
#define KF_AUTORELEASE(x) (x)
#define KF_RELEASE(x)     ((void)0)
#else
#define KF_AUTORELEASE(x) [(x) autorelease]
#define KF_RELEASE(x)     [(x) release]
#endif

#define KFI(...) fprintf(stderr, "  [fove] " __VA_ARGS__)

// ---------------------------------------------------------------- profile maths
void kl_fove_centre(const float t[4], float *fx, float *fy) {
    // t = tan(left) < 0, tan(right) > 0, tan(down) < 0, tan(up) > 0.
    float w = t[1] - t[0], h = t[3] - t[2];
    if (fx) *fx = w > 0 ? -t[0] / w : 0.5f;
    if (fy) *fy = h > 0 ? t[3] / h : 0.5f;
}

int kl_fove_inter_extent_ok(uint32_t ew, uint32_t eh, uint32_t w, uint32_t h) {
    return ew && eh && w >= ew && h >= eh && w - ew <= KL_FOVE_INTER_SLACK && h - eh <= KL_FOVE_INTER_SLACK;
}

int kl_fove_eye_px(float fx, float fy, uint32_t W, uint32_t H, const int vp[4], const int of[2],
                   float *cx, float *cy) {
    // The rect is a FRACTION of the texture it was measured against (kl_ovrp.h
    // viewport_of), applied to the image in hand — as the record can be a
    // generation behind it (Into the Radius: 0,0 5088x4081 of 5088x4081 while
    // the eyes were already 6784x4528; reading it as pixels parked the fovea
    // 848 px left / 204 px up). A stale record of the same fraction is then
    // harmless, so a map built from it needs no rebuild. No generation, or a
    // rect that does not fit it: the full image.
    float vx = 0, vy = 0, vw = (float)W, vh = (float)H;
    int used = 0;
    if (vp && of && of[0] > 0 && of[1] > 0 && vp[0] >= 0 && vp[1] >= 0 && vp[2] > 0 && vp[3] > 0 &&
        vp[0] + vp[2] <= of[0] && vp[1] + vp[3] <= of[1]) {
        float sx = (float)W / (float)of[0], sy = (float)H / (float)of[1];
        vx = vp[0] * sx; vy = vp[1] * sy; vw = vp[2] * sx; vh = vp[3] * sy;
        used = 1;
    }
    *cx = vx + fx * vw;
    *cy = vy + fy * vh;
    return used;
}

static float kf_profile(float d, float box, float ramp, float fl) {
    d = fabsf(d) - box * 0.5f;
    if (d <= 0) return 1.0f;
    if (ramp <= 0 || d >= ramp) return fl;
    return 1.0f - (1.0f - fl) * d / ramp;
}

// The envelope of two peaks: the larger of the two profiles.
static float kf_profile2(float x, const float c[2], float box, float ramp, float fl) {
    return fmaxf(kf_profile(x - c[0], box, ramp, fl), kf_profile(x - c[1], box, ramp, fl));
}

int kl_fove_curves_env2(uint32_t W, uint32_t H, const float cx0[2], const float cx1[2], float cy,
                        float box, float ramp, float fl, int nh, float *h, int nv, float *v) {
    if (!W || !H || nh < 2 || nv < 2 || !h || !v || !(fl > 0) || fl > 1 || box < 0 || ramp < 0)
        return 0;
    float half = W * 0.5f;
    for (int k = 0; k < nh; k++) {
        float x = (float)k * (float)W / (float)(nh - 1);
        // Each eye's half of the target follows its own peaks; the sample ON
        // the seam takes the larger of the two (curves are minimums).
        float a = kf_profile2(x, cx0, box, ramp, fl), b = kf_profile2(x, cx1, box, ramp, fl);
        h[k] = x < half ? a : x > half ? b : fmaxf(a, b);
    }
    for (int k = 0; k < nv; k++) {
        float y = (float)k * (float)H / (float)(nv - 1);
        v[k] = kf_profile(y - cy, box, ramp, fl);
    }
    return 1;
}

int kl_fove_curves(uint32_t W, uint32_t H, const float cx[2], float cy,
                   float box, float ramp, float fl, int nh, float *h, int nv, float *v) {
    const float a[2] = { cx[0], cx[0] }, b[2] = { cx[1], cx[1] };
    return kl_fove_curves_env2(W, H, a, b, cy, box, ramp, fl, nh, h, nv, v);
}

int kl_fove_curves_env(uint32_t W, uint32_t H, const float cx[2], const float cy[2],
                       float box, float ramp, float fl, int nh, float *h, int nv, float *v) {
    if (!W || !H || nh < 2 || nv < 2 || !h || !v || !(fl > 0) || fl > 1 || box < 0 || ramp < 0)
        return 0;
    for (int k = 0; k < nh; k++) h[k] = kf_profile2((float)k * (float)W / (float)(nh - 1), cx, box, ramp, fl);
    for (int k = 0; k < nv; k++) v[k] = kf_profile2((float)k * (float)H / (float)(nv - 1), cy, box, ramp, fl);
    return 1;
}

int kl_fove_curves1(uint32_t W, uint32_t H, float cx, float cy,
                    float box, float ramp, float fl, int nh, float *h, int nv, float *v) {
    const float a[2] = { cx, cx }, b[2] = { cy, cy };
    return kl_fove_curves_env(W, H, a, b, box, ramp, fl, nh, h, nv, v);
}

// ---------------------------------------------------------------- state
static int g_on = -1;                 // -1 unread, 0 off, 1 on
static int g_level;                   // 1 low, 2 medium, 3 high
static const char *g_level_name = "off";
static float g_box, g_ramp, g_floor;  // fractions of the target height (floor: rate)
static int g_patch_fc = 1, g_expand = 1, g_idx = 20;
// MSAA eyes: foveated unless KL_FOVE_MSAA=0 (then MSAA eyes stay
// unfoveated, as the double-wide single-sample design first left them). A single-sample eye never reaches the MSAA code.
static int g_msaa, g_expand_fovea = 1;
static int g_fence_on = 1;           // KL_FOVE_FENCE: order the guest's passes after the expand
static int g_inter = 1;              // KL_FOVE_INTERMEDIATE: foveate a confirmed eye-size intermediate
static int g_subp_helpers = 1;       // KL_FOVE_SUBPASS_HELPERS: a helper's subpass read gets the physical position
// KL_FOVE_PER_EYE: 1 = each eye's map peaks at its own straight-ahead point
// only (the shape until toolchain 565). 0, the default = the envelope of both
// eyes' points, the same map for both (kf_map_for has the reasons).
static int g_per_eye;
static int g_slow;                    // KL_FOVE_SLOW=<n> (debug stress): n busy iterations per MSAA expand fragment
static const char *g_dump;
// The double-wide eye's geometry: its extent and both eyes' frustum tangents
// (left, right, bottom, top as signed tangents, kl_fove_centre's order). The
// double-wide shape (one 2D texture, eye 0 left half, eye 1 right half) needs a
// runtime that hands the guest such a texture and reports it here; none does
// in this tree yet, so it answers 0 and that shape is never recognised.
static int kf_double_eye_geometry(uint32_t *w, uint32_t *h, float t[2][4]) {
    (void)w; (void)h; (void)t;
    return 0;
}

// The guest's frame record as one call: the most recently completed stage and
// what it was rendered with (its per-eye render viewports). 0 before the first.
static int kf_last_record(kl_ovrp_render_pose *out) {
    int st = kl_ovrp_last_complete_stage();
    return st >= 0 && kl_ovrp_stage_render_pose(st, out);
}

static id<MTLDevice> g_dev;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_in;             // we are creating/binding our own

// An eye target. kind DOUBLE: a double-wide pair (one 2D image, eye 0
// left half, eye 1 right half; Half-Life: Alyx). kind ARRAY: an eye image the
// runtime itself handed the guest (kl_ovrp / kl_openxr swapchain), or the
// eye-size target the guest resolves into one — `layers` 2 = both eyes as the
// two slices of one 2D-array image (multiview), 1 = one eye's own image (`eye`
// says which).
enum { KF_DOUBLE = 0, KF_ARRAY = 1 };
#define KF_EYE_N 8
// via: 0 = an eye image (or what an MSAA eye pass resolves into one).
// Otherwise `col` is the guest's OWN eye-size scene target, confirmed by
// kl_vulkan to reach an eye image: KL_FOVE_VIA_SUBPASS — a later subpass of the
// same render pass writes (or resolves into) one of `out` (the eye images,
// one per swapchain stage): the scene is never expanded (it is transient), the
// out is, with the same map; KL_FOVE_VIA_XFER / _SAMPLED — the scene itself is
// expanded, before any encoder that could read it (blit encoders included).
#define KF_OUT_N 4
// ms_tr: an MSAA intermediate the guest never stores and a
// later subpass resolves (kl_fove_note_ms_transient) — its layered no-resolve
// encoders are mapped and held; the resolve target is expanded.
static struct { void *col, *dep; uint32_t w, h, samples, layers; int kind, eye, via, ms_tr; void *out[KF_OUT_N]; } g_eye[KF_EYE_N];

// A rate map for one target shape, keyed by (W, H, kind, layers, eye): DOUBLE
// is (W, H, DOUBLE, 1, -1); a layered 2-eye pass is (W, H, ARRAY, 2, -1) with
// map layer l = eye l; a one-eye image is (W, H, ARRAY, 1, e).
typedef struct {
    uint32_t W, H;                    // screen size the map was built for
    int kind, layers, eye;
    id<MTLRasterizationRateMap> map;
    id<MTLBuffer> buf;
    uint32_t pw, ph, bcol;            // physical size (layer 0); DOUBLE: first physical column of eye 1
    uint32_t lpw[2], lph[2];          // physical size per layer
    // The full-rate screen rectangle of each eye (for the MSAA expand): screen
    // px [fx0, fx1) x [fy0, fy1) where physical = screen - a whole-pixel offset.
    uint32_t fx0[2], fx1[2], fy0, fy1;
    int offx[2], offy;                // screen - physical inside those rectangles
    int failed;
    float cx[2], cy[2];               // ARRAY: the centre each layer was built around (for the re-centre)
    float ocx[2], ocy[2];             // ARRAY: the other eye's point in that layer (the envelope's second peak)
    int env;                          // built as the envelope of both eyes' points (not KL_FOVE_PER_EYE)
    int superseded;                   // re-centred into another slot while expands still referred to this one:
                                      // lookups skip it, its pending expands still read it, recycled once none does
} kf_map_t;
#define KF_MAP_N 8
static kf_map_t g_maps[KF_MAP_N];
static kf_map_t *g_lastmap;           // the newest map, for the status field
static id<MTLBuffer> g_ident;         // all-1.0 map, 2 layers, 16384^2: identity for any target

// Scratch textures are cached per use in KF_SC_WAYS entries, one per (pixel
// format, sample count) — see kf_scratch_into.
#define KF_SC_WAYS 4
static id<MTLTexture> g_scratch[KF_SC_WAYS];
static uint64_t g_sc_allocs;          // scratch textures made (the selftest's counter)
static unsigned g_sc_rr;              // round-robin victim when every way is another format
static unsigned g_sc_leaked;          // replaced with g_old full (MRC: not released, may be in flight)
static id<MTLTexture> kf_scratch_into(id<MTLTexture> __strong *ways, const char *what, MTLPixelFormat fmt,
                                      NSUInteger samples, uint32_t w, uint32_t h, MTLTextureUsage usage);
static id<MTLRenderPipelineState> g_pso[4];
static MTLPixelFormat g_pso_fmt[4];
static id g_old[16];                  // replaced scratch textures, never freed (in-flight cbs)
static int g_nold;

static struct {
    uint64_t libs, patched, subpass, refused, nopos, late, marked_enc, ident_enc, expands, clobber, clobber_marked;
    uint64_t dropped, full, msread, ms_expands;
    // layered (2-slice) marked encoders, single-slice passes over a
    // 2-layer eye left unfoveated, MSAA passes resolved into a single-sample
    // eye (expanded there), MSAA-array passes with no resolve left unfoveated,
    // layer-index arguments added / reused in patched shaders
    uint64_t layered_enc, split, res_expands, ms_layer_refused, layer_arg_added, layer_arg_reused;
    uint64_t exp_frame_mark;
    // marked encoders of a guest intermediate (the scene target, or the
    // eye image a later subpass writes from it); out passes left UNMAPPED while
    // their scene was pending in physical layout (a broken picture: must be 0);
    // expands run before a blit encoder (the intermediate is copied into the eye)
    uint64_t inter_enc, inter_out_enc, inter_lost, blit_flush;
    // layered MSAA encoders of a transient (DONT_CARE, resolved
    // in-pass) intermediate mapped and held in physical layout; the resolving
    // encoders that then owed the expand
    uint64_t ms_tr_held, ms_tr_resolved;
    // a render encoder that LOADs an eye/intermediate
    // whose pending expand a COMPUTE encoder on the same command buffer just
    // flushed — MoltenVK resuming a pass it split for an indirect draw (its
    // multiview instance-count adjust); intermediates the Vulkan side refused
    uint64_t compute_split, inter_refused;
    // intermediates refused HERE, at the first split
    // pass the Metal side sees (the Vulkan-side record-time refusal missed them)
    uint64_t split_refused;
    int used_hist[32];
} g_st;

// Colour textures of intermediates that
// a SPLIT PASS was seen on. Sticky until kl_fove_forget (image destroyed):
// kl_fove_note_eye_scene will not register them again, whatever kl_vulkan's
// candidate/confirm table says. g_mu.
#define KF_SPLIT_N 16
static void *g_split_col[KF_SPLIT_N];
static int kf_split_find_locked(void *col) {
    for (int i = 0; i < KF_SPLIT_N; i++) if (col && g_split_col[i] == col) return i;
    return -1;
}

#define KF_CB_N 64
// depth_only: an MSAA eye whose depth-only clear is mapped but whose colour has not
// been drawn yet: pending for the LOAD rule, nothing to expand.
// slot: the eye slot (g_eye) whose content is in physical layout on this
// command buffer; map: the g_maps index it was drawn with. tex is
// what the expand rewrites (the eye colour, or the single-sample image an MSAA
// pass resolved into); NULL when there is nothing to rewrite.
// hold: an in-pass intermediate (KL_FOVE_VIA_SUBPASS) is mapped but
// its eye image has not been drawn yet — an encoder MoltenVK opens between the
// subpasses must not end the entry (only -commit does).
static struct { void *cb; void *tex; uint64_t gen; int depth_only, slot, map, hold; } g_pend[KF_CB_N];

static void kf_turn_off(const char *why) {
    if (__atomic_exchange_n(&g_on, 0, __ATOMIC_ACQ_REL) == 1)
        KFI("FOVEATION TURNED OFF: %s (the rest of the run renders unfoveated; patched "
            "shaders keep running with the identity map)\n", why);
}

static void kf_read_env(void) {
    const char *s = kl_env_str("KL_FOVEATION", "off");
    // This fork has a second rate-map module, kl_mtl_foveate.m (KL_VK_FOVEATE,
    // what Half-Life: Alyx is drawn through). Both replace the same two Metal
    // methods, so only one may be in force: where the older one is asked for,
    // this one stays off and says so.
    if (strcmp(s, "off") && strcmp(s, "0") && *s && kl_env_float("KL_VK_FOVEATE", 0.0f) > 0.0f) {
        fprintf(stderr, "  [fove] KL_FOVEATION=%s ignored: KL_VK_FOVEATE is set, and the two "
                        "foveation modules cannot both be in force\n", s);
        g_on = 0;
        return;
    }
    // Half-Life: Alyx's square density profile per level (docs/FOVEATION.md
    // §3.1), re-centred per eye: box and
    // ramp measured at a 2016-high target (box 1216/1024/768 px, ramp ~384),
    // floor = Valve's minimum density at that level.
    if (!strcmp(s, "low"))         { g_level = 1; g_box = 1216.f / 2016; g_floor = 1.f / 3; }
    else if (!strcmp(s, "medium")) { g_level = 2; g_box = 1024.f / 2016; g_floor = 0.22f; }
    else if (!strcmp(s, "high"))   { g_level = 3; g_box = 768.f / 2016;  g_floor = 0.125f; }
    else {
        if (strcmp(s, "off") && strcmp(s, "0") && *s)
            fprintf(stderr, "  [fove] KL_FOVEATION=%s not understood (off|low|medium|high) — off\n", s);
        g_on = 0;
        return;
    }
    g_level_name = s;
    g_ramp = 384.f / 2016;
    g_box = kl_env_float("KL_FOVE_BOX", g_box);
    g_ramp = kl_env_float("KL_FOVE_RAMP", g_ramp);
    g_floor = kl_env_float("KL_FOVE_FLOOR", g_floor);
    if (!(g_floor > 0.02f)) g_floor = 0.02f;
    if (g_floor > 1) g_floor = 1;
    if (g_box < 0) g_box = 0;
    if (g_ramp < 0) g_ramp = 0;
    {   // 1 patch, 0 leave unpatched; grid / gridraw: debug pictures (kl_fove.m, kf_patch_msl)
        const char *fc = kl_env_str("KL_FOVE_FRAGCOORD", "1");
        g_patch_fc = !strcmp(fc, "grid") ? 2 : !strcmp(fc, "gridraw") ? 3 : kl_env_on("KL_FOVE_FRAGCOORD", 1);
    }
    g_expand = kl_env_on("KL_FOVE_EXPAND", 1);
    g_idx = kl_env_int("KL_FOVE_BUFFER", g_idx);
    if (g_idx < 1 || g_idx > 30) g_idx = 20;
    g_dump = kl_env_str("KL_FOVE_MSL_DUMP", NULL);
    g_msaa = kl_env_on("KL_FOVE_MSAA", 1);
    g_expand_fovea = kl_env_on("KL_FOVE_MSAA_FOVEA", 1);   // debug: 0 = periphery draw only
    g_slow = kl_env_int("KL_FOVE_SLOW", 0);
    if (g_slow < 0) g_slow = 0;
    g_fence_on = kl_env_on("KL_FOVE_FENCE", 1);
    g_inter = kl_env_on("KL_FOVE_INTERMEDIATE", 1);
    g_subp_helpers = kl_env_on("KL_FOVE_SUBPASS_HELPERS", 1);
    g_per_eye = kl_env_on("KL_FOVE_PER_EYE", 0);
    g_on = 1;
}

int kl_fove_on(void) {
    if (g_on < 0) {
        pthread_mutex_lock(&g_mu);
        if (g_on < 0) kf_read_env();
        pthread_mutex_unlock(&g_mu);
    }
    return __atomic_load_n(&g_on, __ATOMIC_ACQUIRE) == 1;
}

// ---------------------------------------------------------------- swizzle plumbing
#define KF_SW_N 64
static struct { Class cls; SEL sel; IMP orig; } g_sw[KF_SW_N];
static int g_nsw;

static IMP kf_orig(id self, SEL sel) {
    for (Class c = object_getClass(self); c; c = class_getSuperclass(c))
        for (int i = 0; i < __atomic_load_n(&g_nsw, __ATOMIC_ACQUIRE); i++)
            if (g_sw[i].cls == c && g_sw[i].sel == sel) return g_sw[i].orig;
    return NULL;
}

static int kf_swap(Class c, const char *name, IMP mine) {
    SEL s = sel_registerName(name);
    Method m = class_getInstanceMethod(c, s);
    if (!m) { KFI("%s has no %s\n", class_getName(c), name); return 0; }
    IMP cur = method_getImplementation(m);
    if (cur == mine) return 1;
    for (int i = 0; i < g_nsw; i++) if (g_sw[i].cls == c && g_sw[i].sel == s) return 1;
    if (g_nsw >= KF_SW_N) return 0;
    g_sw[g_nsw].cls = c; g_sw[g_nsw].sel = s; g_sw[g_nsw].orig = cur;
    __atomic_store_n(&g_nsw, g_nsw + 1, __ATOMIC_RELEASE);
    // class_replaceMethod on the class itself: an inherited method gets an
    // override here, an own method its IMP replaced.
    class_replaceMethod(c, s, mine, method_getTypeEncoding(m));
    return 1;
}

// ---------------------------------------------------------------- MSL patch
static uint64_t kf_fnv(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 1099511628211ull; }
    return h;
}

// Every `[[buffer(n)]]` in the source, as a bit mask.
static uint32_t kf_buffers_used(const char *s) {
    uint32_t m = 0;
    for (const char *p = s; (p = strstr(p, "[[buffer(")); p += 9) {
        int n = atoi(p + 9);
        if (n >= 0 && n < 32) m |= 1u << n;
    }
    return m;
}

static void kf_dump(const char *src, size_t n, uint64_t h, const char *tag) {
    if (!g_dump) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%016llx-%s.metal", g_dump, (unsigned long long)h, tag);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fwrite(src, 1, n, f);
    fclose(f);
}

// Whether `id` (length n) names a parameter in the list [pb, pe) whose type is a
// texture (`texture…` / `depth…`): the receiver of a subpass read in a helper.
// A template argument list may hold commas, so the type's start is found with
// '<' / '>' balanced.
static int kf_param_is_texture(const char *pb, const char *pe, const char *id, size_t n) {
    for (const char *p = pb; p + n <= pe; p++) {
        if (strncmp(p, id, n)) continue;
        if (p > pb && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue;
        char after = p + n < pe ? p[n] : ')';
        if (isalnum((unsigned char)after) || after == '_') continue;
        if (after != ',' && after != ')' && after != ' ' && after != '[') continue;
        const char *t = p;
        int ang = 0;
        while (t > pb) {
            if (t[-1] == '>') ang++;
            else if (t[-1] == '<') ang--;
            else if ((t[-1] == ',' || t[-1] == '(') && ang == 0) break;
            t--;
        }
        while (t < p && (*t == ' ' || *t == '\n')) t++;
        if (!strncmp(t, "texture", 7) || !strncmp(t, "depth", 5)) return 1;
    }
    return 0;
}

// Returns a patched copy (malloc'd) or NULL to leave the source alone.
static char *kf_patch_msl(const char *s, size_t n) {
    __atomic_add_fetch(&g_st.libs, 1, __ATOMIC_RELAXED);
    uint64_t h = kf_fnv(s, n);
    // A SPIRV-Cross fragment entry point: `fragment <ret> main0(` at a line start
    // — or, for Unreal Engine 5, `fragment <ret> main_XXXXXXXX_XXXXXXXX(`: UE5's
    // Vulkan RHI names every SPIR-V entry point "main_%0.8x_%0.8x" (code size,
    // CRC), and SPIRV-Cross renames only a bare "main" to main0. Without this
    // Pinball FX (UE 5.3.2) showed libraries 1363, patched 0, without
    // [[position]] 0 — every fragment library fell through as "other".
    const char *f = NULL;
    for (const char *p = s; (p = strstr(p, "fragment ")); p += 9)
        if (p == s || p[-1] == '\n') {
            const char *paren = strchr(p, '(');
            const char *nl = strchr(p, '\n');
            if (!paren || (nl && paren > nl) || paren - p < 6) continue;
            const char *id = paren;
            while (id > p && (isalnum((unsigned char)id[-1]) || id[-1] == '_')) id--;
            size_t il = (size_t)(paren - id);
            if ((il == 5 && !strncmp(id, "main0", 5)) ||
                (il == 22 && !strncmp(id, "main_", 5) && id[13] == '_')) { f = p; break; }
        }
    if (!f) { kf_dump(s, n, h, "other"); return NULL; }
    // The parameter list: from '(' to its matching ')'.
    const char *open = strchr(f, '('), *close = NULL;
    int depth = 0;
    for (const char *p = open; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')' && --depth == 0) { close = p; break; }
    }
    if (!close) { kf_dump(s, n, h, "unparsed"); return NULL; }
    // The [[position]] argument and its name.
    const char *pos = NULL;
    for (const char *p = open; p && p < close; p++) {
        p = strstr(p, "[[position");
        if (!p || p >= close) break;
        pos = p;
        break;
    }
    if (!pos) { __atomic_add_fetch(&g_st.nopos, 1, __ATOMIC_RELAXED); kf_dump(s, n, h, "frag-nopos"); return NULL; }
    const char *e = pos;
    while (e > open && e[-1] == ' ') e--;
    const char *b = e;
    while (b > open && (isalnum((unsigned char)b[-1]) || b[-1] == '_')) b--;
    if (b == e) { kf_dump(s, n, h, "unparsed"); return NULL; }
    char name[128];
    size_t nl = (size_t)(e - b) < sizeof name - 1 ? (size_t)(e - b) : sizeof name - 1;
    memcpy(name, b, nl); name[nl] = 0;
    uint32_t used = kf_buffers_used(s);
    for (int i = 0; i < 32; i++) if (used & (1u << i)) __atomic_add_fetch(&g_st.used_hist[i], 1, __ATOMIC_RELAXED);
    if (used & (1u << g_idx)) {
        uint64_t r = __atomic_add_fetch(&g_st.refused, 1, __ATOMIC_RELAXED);
        if (r <= 8) {
            char sig[200];
            size_t sl = (size_t)(close - f) < sizeof sig - 1 ? (size_t)(close - f) : sizeof sig - 1;
            memcpy(sig, f, sl); sig[sl] = 0;
            KFI("REFUSED shader %016llx: it binds buffer(%d) itself (%s...)\n", (unsigned long long)h, g_idx, sig);
        }
        kf_dump(s, n, h, "refused");
        kf_turn_off("a fragment shader that reads [[position]] already uses the rate-map buffer slot "
                    "(KL_FOVE_BUFFER picks another)");
        return NULL;
    }
    if (!g_patch_fc) { kf_dump(s, n, h, "frag-unpatched"); return NULL; }
    const char *brace = strchr(close, '{');
    if (!brace) { kf_dump(s, n, h, "unparsed"); return NULL; }
    // MoltenVK emulates a subpass input (Alyx's one local-read draw) as
    // `tex.read(uint2(gl_FragCoord.xy))` on the attachment itself — a read of
    // the SAME pixel of an image that is in physical layout while the pass
    // runs, so it must keep the PHYSICAL position. SPIRV-Cross spells the
    // guest's own texelFetch(ivec2(gl_FragCoord.xy)) `uint2(int2(...))`, so the
    // exact spelling below is the emulation and nothing else.
    char subp[160], subp_to[160];
    snprintf(subp, sizeof subp, "uint2(%s.xy)", name);
    int subp_tl = snprintf(subp_to, sizeof subp_to, "uint2(kl_fove_phys.xy)");
    size_t subp_l = strlen(subp);
    int nsub = 0;
    for (const char *p = brace; (p = strstr(p, subp)); p += subp_l) nsub++;
    // The same emulation inside a HELPER function. Unreal 4's mobile depth
    // fetch (`VulkanSubpassDepthFetch`, DepthFade / soft-edge translucency in
    // Moss) is a static inline function above the entry point that takes the
    // position as `thread float4& <name>` and reads `.read(uint2(<name>.xy), ...)`.
    // The scan above starts at the entry's brace and never sees it, so the
    // helper got the REMAPPED (screen) position and read the physical-layout
    // depth at the wrong texel. A helper whose only uses of <name> are that
    // spelling, read from one of its own texture parameters, is fed the saved
    // physical position at its call sites in the entry body instead.
    enum { KF_HC_N = 64 };
    const char *hc[KF_HC_N];
    int nhc = 0;
    size_t name_l = strlen(name);
    if (g_subp_helpers) {                    // KL_FOVE_SUBPASS_HELPERS=0: the old behaviour (A/B)
        char par[160];
        snprintf(par, sizeof par, "thread float4& %s", name);
        size_t par_l = strlen(par);
        for (const char *p = s; (p = strstr(p, par)) && p < f; p += par_l) {
            const char *pe = p + par_l;
            if (isalnum((unsigned char)*pe) || *pe == '_') continue;
            // The helper's name: the identifier before the '(' that opens this list.
            const char *lp = p;
            int d = 0;
            while (lp > s && !(lp[-1] == '(' && d == 0)) {
                if (lp[-1] == ')') d++;
                else if (lp[-1] == '(') d--;
                if (lp[-1] == '\n' || lp[-1] == ';' || lp[-1] == '}') break;
                lp--;
            }
            if (lp <= s || lp[-1] != '(') continue;
            const char *pc = lp;                 // the parameter list's ')'
            for (int pd = 1; *pc && pc < f; pc++) {
                if (*pc == '(') pd++;
                else if (*pc == ')' && --pd == 0) break;
            }
            if (*pc != ')') continue;
            const char *ne = lp - 1, *nb = ne;
            while (nb > s && (isalnum((unsigned char)nb[-1]) || nb[-1] == '_')) nb--;
            if (nb == ne || (size_t)(ne - nb) > 120) continue;
            // Its body: the first '{' after the parameter list, to its match.
            const char *bo = strchr(pe, '{'), *bc = NULL;
            if (!bo || bo > f) continue;
            int bd = 0;
            for (const char *q = bo; *q && q < f; q++) {
                if (*q == '{') bd++;
                else if (*q == '}' && --bd == 0) { bc = q; break; }
            }
            if (!bc) continue;
            int uses = 0, reads = 0;
            for (const char *q = bo; (q = strstr(q, name)) && q < bc; q += name_l) {
                if ((q > s && (isalnum((unsigned char)q[-1]) || q[-1] == '_')) ||
                    isalnum((unsigned char)q[name_l]) || q[name_l] == '_') continue;
                uses++;
                // Only `<tex>.read(uint2(<name>.xy)…` with <tex> a texture parameter of
                // this helper: the subpass emulation, not some other uint2 of the position.
                if (q - bo >= 12 && !strncmp(q - 6, "uint2(", 6) && !strncmp(q + name_l, ".xy)", 4) &&
                    !strncmp(q - 12, ".read(", 6)) {
                    const char *te = q - 12, *tb = te;
                    while (tb > bo && (isalnum((unsigned char)tb[-1]) || tb[-1] == '_')) tb--;
                    if (tb < te && kf_param_is_texture(lp, pc, tb, (size_t)(te - tb))) reads++;
                }
            }
            if (!reads || reads != uses) continue;
            // Every call of it in the entry body: the bare <name> argument.
            char hn[128];
            size_t hl = (size_t)(ne - nb);
            memcpy(hn, nb, hl); hn[hl] = 0;
            for (const char *q = brace; (q = strstr(q, hn)) && nhc < KF_HC_N; q += hl) {
                if (q[hl] != '(' || isalnum((unsigned char)q[-1]) || q[-1] == '_') continue;
                int cd = 0;
                for (const char *a = q + hl; *a; a++) {
                    if (*a == '(') cd++;
                    else if (*a == ')' && --cd == 0) break;
                    else if (cd == 1 && !strncmp(a, name, name_l) && (a[-1] == '(' || a[-1] == ' ' || a[-1] == ',') &&
                             (a[name_l] == ',' || a[name_l] == ')' || a[name_l] == ' ') && nhc < KF_HC_N)
                        hc[nhc++] = a;
                }
            }
        }
        // Copied out in source order below.
        for (int i = 1; i < nhc; i++)
            for (int j = i; j > 0 && hc[j - 1] > hc[j]; j--) { const char *t = hc[j]; hc[j] = hc[j - 1]; hc[j - 1] = t; }
    }
    nsub += nhc;
    // MSAA eyes (seen first in Half-Life: Alyx): with tiler mode off Valve's
    // translucent shaders read the eye's own multisampled DEPTH (soft
    // particles) and one draw its multisampled COLOUR as textures,
    // `<ms texture>.read(uint2(int2(gl_FragCoord.xy)), sample)` — the same pixel
    // of an eye image, which is in physical layout while a mapped pass runs
    // (and physical == screen in every unmapped one). Those reads keep the
    // PHYSICAL position; every other FragCoord use is remapped as before.
    enum { KF_MSR_N = 64 };
    const char *msr[KF_MSR_N];
    int nmsr = 0;
    char msp[160];
    snprintf(msp, sizeof msp, "uint2(int2(%s.xy))", name);
    size_t msp_l = strlen(msp);
    if (g_msaa && (g_patch_fc == 1 || g_patch_fc == 2)) {
        for (const char *p = brace; (p = strstr(p, msp)) && nmsr < KF_MSR_N; p += msp_l) {
            if (p - s < 6 || strncmp(p - 6, ".read(", 6)) continue;
            const char *ie = p - 6, *ib = ie;
            while (ib > s && (isalnum((unsigned char)ib[-1]) || ib[-1] == '_')) ib--;
            if (ib == ie || (size_t)(ie - ib) > 100) continue;
            // Is that member declared as a multisampled texture?
            int ms = 0;
            for (const char *d = s; !ms && (d = strstr(d, "2d_ms<")); d += 6) {
                const char *gt = strchr(d, '>');
                if (!gt) break;
                const char *nb = gt + 1;
                while (*nb == ' ') nb++;
                if (!strncmp(nb, ib, (size_t)(ie - ib)) && !(isalnum((unsigned char)nb[ie - ib]) || nb[ie - ib] == '_'))
                    ms = 1;
            }
            if (ms) msr[nmsr++] = p;
        }
    }
    // The map LAYER. A layered pass (a multiview eye pair rendered
    // as the two slices of one array, renderTargetArrayLength 2) maps each slice
    // through its own layer of the rate map, so the decoder needs the slice the
    // fragment is in: [[render_target_array_index]]. A shader that already takes
    // it as an entry-point argument (SPIRV-Cross spells gl_Layer / gl_ViewIndex
    // that way) is reused — a second argument with the same attribute does not
    // compile; otherwise one is added. Metal answers 0 when the vertex stage
    // does not write it (measured on this GPU, tests/t_fove), which is the
    // right layer for every non-layered pass and the identity map.
    char layer[136] = "kl_fove_layer";
    int add_layer = 1;
    for (const char *p = open; p && p < close; p++) {
        p = strstr(p, "[[render_target_array_index]]");
        if (!p || p >= close) break;
        const char *le = p;
        while (le > open && le[-1] == ' ') le--;
        const char *lb = le;
        while (lb > open && (isalnum((unsigned char)lb[-1]) || lb[-1] == '_')) lb--;
        if (lb < le && (size_t)(le - lb) < sizeof layer - 1) {
            memcpy(layer, lb, (size_t)(le - lb)); layer[le - lb] = 0;
            add_layer = 0;
        }
        break;
    }
    if (add_layer) {
        // Declared as a member of the stage_in struct instead: reach it through
        // the [[stage_in]] argument (`in.gl_Layer`). Not reachable at all = the
        // layer is taken as 0 (right for every non-layered pass), counted.
        const char *m = strstr(s, "[[render_target_array_index]]");
        if (m) {
            const char *me = m, *mb;
            while (me > s && me[-1] == ' ') me--;
            for (mb = me; mb > s && (isalnum((unsigned char)mb[-1]) || mb[-1] == '_'); mb--) {}
            const char *si = NULL;
            for (const char *p = open; p && p < close; p++) {
                p = strstr(p, "[[stage_in]]");
                if (!p || p >= close) break;
                si = p; break;
            }
            const char *se = si, *sb;
            if (se) { while (se > open && se[-1] == ' ') se--; }
            for (sb = se; se && sb > open && (isalnum((unsigned char)sb[-1]) || sb[-1] == '_'); sb--) {}
            if (si && sb < se && mb < me && (size_t)(se - sb) + (size_t)(me - mb) + 2 < sizeof layer)
                snprintf(layer, sizeof layer, "%.*s.%.*s", (int)(se - sb), sb, (int)(me - mb), mb);
            else
                snprintf(layer, sizeof layer, "0u");
            add_layer = 0;
        }
    }
    __atomic_add_fetch(add_layer ? &g_st.layer_arg_added : &g_st.layer_arg_reused, 1, __ATOMIC_RELAXED);
    char arg[240], stmt[460];
    int al = snprintf(arg, sizeof arg, ", constant rasterization_rate_map_data& kl_fove_rrm [[buffer(%d)]]%s", g_idx,
                      add_layer ? ", uint kl_fove_layer [[render_target_array_index]]" : "");
    char keep[160] = "";
    if (nsub || nmsr) snprintf(keep, sizeof keep, "\n    float4 kl_fove_phys = %s;", name);
    int sl = snprintf(stmt, sizeof stmt,
                      "%s\n    %s%s.xy = rasterization_rate_map_decoder(kl_fove_rrm).map_physical_to_screen_coordinates(%s.xy, %s);",
                      keep, g_patch_fc == 3 ? "if (false) " : "", name, name, layer);
    // Debug pictures (KL_FOVE_FRAGCOORD=grid|gridraw): every patched shader
    // writes a 64-px grid of the position it sees into colour 0 — regular in
    // screen space when the position is remapped (grid), squeezed toward the
    // periphery when it is left physical (gridraw).
    char grid[400] = "";
    const char *ret = NULL;
    if (g_patch_fc >= 2) {
        const char *st = strstr(s, "struct main0_out"), *c0 = st ? strstr(st, "[[color(0)]]") : NULL;
        const char *se = st ? strstr(st, "};") : NULL;
        ret = strstr(brace, "\n    return out;");
        if (c0 && se && c0 < se && ret) {
            const char *ne = c0;
            while (ne > st && ne[-1] == ' ') ne--;
            const char *nb = ne;
            while (nb > st && (isalnum((unsigned char)nb[-1]) || nb[-1] == '_')) nb--;
            const char *tb = nb - 1;
            while (tb > st && tb[-1] != ' ' && tb[-1] != '\n') tb--;
            char ty[16] = "", mem[64] = "";
            if (nb - 1 - tb < (long)sizeof ty) memcpy(ty, tb, (size_t)(nb - 1 - tb));
            if (ne - nb < (long)sizeof mem) memcpy(mem, nb, (size_t)(ne - nb));
            if ((!strcmp(ty, "float4") || !strcmp(ty, "half4")) && mem[0])
                snprintf(grid, sizeof grid,
                         "\n    { float2 kt = rasterization_rate_map_decoder(kl_fove_rrm).map_physical_to_screen_coordinates(float2(700.5)); "
                         "if (any(abs(kt - 700.5) > 0.01)) { float2 kg = step(fract(%s.xy / 64.0), float2(0.09)); "
                         "out.%s = %s(max(kg.x, kg.y) * 4.0, fract(%s.x / 1296.0), fract(%s.y / 504.0), 1.0); } }",
                         name, mem, ty, name, name);
        }
        if (!grid[0]) ret = NULL;
    }
    size_t gl = strlen(grid);
    size_t out_n = n + (size_t)al + (size_t)sl + 1 + (size_t)nsub * 32 + (size_t)nmsr * 32 + gl;
    char *o = malloc(out_n);
    if (!o) return NULL;
    size_t a = (size_t)(close - s), c = (size_t)(brace + 1 - s);
    size_t k = 0;
    memcpy(o + k, s, a); k += a;
    memcpy(o + k, arg, (size_t)al); k += (size_t)al;
    memcpy(o + k, s + a, c - a); k += c - a;
    memcpy(o + k, stmt, (size_t)sl); k += (size_t)sl;
    int mi = 0, hi = 0;
    for (const char *p = s + c; *p; ) {      // the body, with the subpass (and MSAA eye) reads kept physical
        const char *q = nsub ? strstr(p, subp) : NULL;
        while (mi < nmsr && msr[mi] < p) mi++;
        const char *m = mi < nmsr ? msr[mi] : NULL;
        while (hi < nhc && hc[hi] < p) hi++;
        const char *hp = hi < nhc ? hc[hi] : NULL;
        if (hp && (!q || hp < q) && (!m || hp < m) && (!ret || ret < p || hp < ret)) {
            memcpy(o + k, p, (size_t)(hp - p)); k += (size_t)(hp - p);
            k += (size_t)sprintf(o + k, "kl_fove_phys");
            p = hp + name_l; hi++;
            continue;
        }
        if (ret && (!q || ret < q) && (!m || ret < m) && ret >= p) {   // debug grid before `return out;`
            memcpy(o + k, p, (size_t)(ret - p)); k += (size_t)(ret - p);
            memcpy(o + k, grid, gl); k += gl;
            p = ret; ret = NULL;
            continue;
        }
        if (m && (!q || m < q)) {
            memcpy(o + k, p, (size_t)(m - p)); k += (size_t)(m - p);
            k += (size_t)sprintf(o + k, "uint2(int2(kl_fove_phys.xy))");
            p = m + msp_l; mi++;
            continue;
        }
        if (!q) { size_t r = n - (size_t)(p - s); memcpy(o + k, p, r); k += r; break; }
        memcpy(o + k, p, (size_t)(q - p)); k += (size_t)(q - p);
        memcpy(o + k, subp_to, (size_t)subp_tl); k += (size_t)subp_tl;
        p = q + subp_l;
    }
    o[k] = 0;
    __atomic_add_fetch(&g_st.patched, 1, __ATOMIC_RELAXED);
    if (nsub) {
        uint64_t m = __atomic_add_fetch(&g_st.subpass, 1, __ATOMIC_RELAXED);
        if (m <= 4) KFI("shader %016llx: %d subpass-input read(s) kept at the PHYSICAL position (%d through a helper's call)\n",
                        (unsigned long long)h, nsub, nhc);
    }
    if (nmsr) {
        uint64_t m = __atomic_add_fetch(&g_st.msread, 1, __ATOMIC_RELAXED);
        if (m <= 4) KFI("shader %016llx: %d multisampled eye-image read(s) kept at the PHYSICAL position (MSAA)\n",
                        (unsigned long long)h, nmsr);
    }
    kf_dump(o, k, h, nsub ? "frag-patched-subpass" : nmsr ? "frag-patched-msread" : "frag-patched");
    return o;
}

// t_fove: the patch on a source string, with the defaults (slot 20, patch on).
char *kl_fove_patch_for_test(const char *src) { return src ? kf_patch_msl(src, strlen(src)) : NULL; }
void kl_fove_set_subpass_helpers_for_test(int on) { g_subp_helpers = on != 0; }

static NSString *kf_patched_source(NSString *src) {
    if (!src) return nil;
    const char *s = src.UTF8String;
    if (!s) return nil;
    char *o = kf_patch_msl(s, strlen(s));
    if (!o) return nil;
    NSString *r = [[NSString alloc] initWithBytesNoCopy:o length:strlen(o)
                                               encoding:NSUTF8StringEncoding freeWhenDone:YES];
    return KF_AUTORELEASE(r);
}

// A patched source that does not compile must not cost the guest its
// pipeline: the failure is logged, foveation turns off, and the ORIGINAL
// source is compiled instead.
static void kf_compile_failed(NSError *e) {
    static int said;
    if (said++ < 2) KFI("a PATCHED shader failed to compile: %s\n", e ? e.description.UTF8String : "?");
    kf_turn_off("a patched shader failed to compile (the original was compiled instead)");
}

static id kf_lib_err(id self, SEL cmd, NSString *src, id opts, NSError **err) {
    IMP o = kf_orig(self, cmd);
    NSString *p = t_in ? nil : kf_patched_source(src);
    if (!p) return ((id (*)(id, SEL, id, id, NSError **))o)(self, cmd, src, opts, err);
    NSError *e = nil;
    id lib = ((id (*)(id, SEL, id, id, NSError **))o)(self, cmd, p, opts, &e);
    if (lib) { if (err) *err = e; return lib; }
    kf_compile_failed(e);
    return ((id (*)(id, SEL, id, id, NSError **))o)(self, cmd, src, opts, err);
}

static void kf_lib_cb(id self, SEL cmd, NSString *src, id opts, id handler) {
    IMP o = kf_orig(self, cmd);
    NSString *p = t_in ? nil : kf_patched_source(src);
    if (!p || !handler) { ((void (*)(id, SEL, id, id, id))o)(self, cmd, p ? p : src, opts, handler); return; }
    void (^h)(id, NSError *) = handler;
    void (^mine)(id, NSError *) = ^(id lib, NSError *e) {
        if (lib) { h(lib, e); return; }
        kf_compile_failed(e);
        ((void (*)(id, SEL, id, id, id))o)(self, cmd, src, opts, h);
    };
    ((void (*)(id, SEL, id, id, id))o)(self, cmd, p, opts, mine);
}

static id kf_lib_data(id self, SEL cmd, id data, NSError **err) {
    static int said;
    if (!said++) KFI("WARNING: newLibraryWithData: called — a precompiled library bypasses the "
                     "[[position]] patch\n");
    return ((id (*)(id, SEL, id, NSError **))kf_orig(self, cmd))(self, cmd, data, err);
}

// ---------------------------------------------------------------- the map
static id<MTLBuffer> kf_params(id<MTLRasterizationRateMap> m) {
    id<MTLBuffer> b = [g_dev newBufferWithLength:m.parameterBufferSizeAndAlign.size
                                          options:MTLResourceStorageModeShared];
    [m copyParameterDataToBuffer:b offset:0];
    return b;
}

// One layer descriptor per curve pair; layers 2 = one map whose layer l is
// eye l's (a layered multiview pass picks it by [[render_target_array_index]]).
static id<MTLRasterizationRateMap> kf_make_map_n(uint32_t W, uint32_t H, int layers, int nh,
                                                 const float *const *hc, int nv, const float *const *vc) {
    Class LD = objc_getClass("MTLRasterizationRateLayerDescriptor");
    Class MD = objc_getClass("MTLRasterizationRateMapDescriptor");
    if (!LD || !MD || layers < 1 || layers > 2) return nil;
    MTLRasterizationRateLayerDescriptor *L[2] = { nil, nil };
    for (int l = 0; l < layers; l++)
        L[l] = [[LD alloc] initWithSampleCount:MTLSizeMake((NSUInteger)nh, (NSUInteger)nv, 0)
                                    horizontal:hc[l] vertical:vc[l]];
    MTLRasterizationRateMapDescriptor *D = layers == 1
        ? [MD rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(W, H, 1) layer:L[0]]
        : [MD rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(W, H, 1) layerCount:(NSUInteger)layers
                                                    layers:(MTLRasterizationRateLayerDescriptor *const *)L];
    id<MTLRasterizationRateMap> m = [g_dev newRasterizationRateMapWithDescriptor:D];
    for (int l = 0; l < layers; l++) KF_RELEASE(L[l]);
    return m;
}

// t_fove: a stand-in for the frustum and the frame record (no OVRPlugin on the host).
static int g_trec_on, g_trec_vp[2][4], g_trec_of[2];
static float g_trec_fx[2], g_trec_fy[2];
void kl_fove_test_eye_record(int on, const float fx[2], const float fy[2], const int vp[2][4], const int of[2]) {
    pthread_mutex_lock(&g_mu);
    g_trec_on = on;
    for (int e = 0; on && e < 2; e++) {
        g_trec_fx[e] = fx[e]; g_trec_fy[e] = fy[e];
        for (int k = 0; k < 4; k++) g_trec_vp[e][k] = vp ? vp[e][k] : 0;
    }
    g_trec_of[0] = on && of ? of[0] : 0; g_trec_of[1] = on && of ? of[1] : 0;
    pthread_mutex_unlock(&g_mu);
}

// Maps replaced by a re-centre: kept alive for KF_RETIRE_S seconds, as
// encoders already in flight hold the old one, then released. Until toolchain
// 565 they were never freed and a run got 8 re-centres; a guest on dynamic
// resolution (Alien: Rogue Incursion, KL_OVRP_ADAPTIVE_RES) moves its render
// viewport every few seconds, and from the ninth move on its fovea stayed where
// the eighth had put it. A re-centre that finds every slot younger than that
// is put off to a later lookup.
#define KF_RETIRED_N 32
#define KF_RETIRE_S 2.0
static struct { id map, buf; double t; } g_retired[KF_RETIRED_N];
static int g_recentres;
static double kf_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}
// g_mu held. A free retirement slot (releasing the ones old enough), or -1.
static int kf_retire_slot(void) {
    double now = kf_now();
    int fr = -1;
    for (int i = 0; i < KF_RETIRED_N; i++) {
        if (g_retired[i].map && now - g_retired[i].t >= KF_RETIRE_S) {
            KF_RELEASE(g_retired[i].map); KF_RELEASE(g_retired[i].buf);
            g_retired[i].map = nil; g_retired[i].buf = nil;
        }
        if (!g_retired[i].map && fr < 0) fr = i;
    }
    return fr;
}

// The straight-ahead point of eye e inside a W x H one-eye image,
// in screen px. The frustum: the guest's own eye-layer Fov when it set one up
// (kl_ovrp_eye_layer_fov — under Oculus symmetric projection Unity renders
// each eye with the UNION cone into a widened texture, so the point is not
// where the told cone puts it), else the one kl_ovrp tells it (kl_openxr's
// xrLocateViews answers from the same call). Tangents l, r, t, b, positive. A
// sub-rect the guest rendered into (renderViewportScale) moves the point.
// The straight-ahead fraction of eye e (kl_fove_centre): 2 = from the guest's
// eye-layer Fov, 1 = the cone kl_ovrp tells it, 0 = none (centred).
static int kf_eye_frac(int e, float *fx, float *fy) {
    float t4[4] = {0, 0, 0, 0};
    // The cone the guest says it rendered with (its eye layer's Fov — the union
    // under symmetric projection) beats the one it was told.
    int from_layer = kl_ovrp_eye_layer_fov(e, t4);
    if (!from_layer) kl_ovrp_eye_tangents(e, t4);
    int have = t4[0] > 0 && t4[1] > 0 && t4[2] > 0 && t4[3] > 0;
    float t[4] = { -t4[0], t4[1], -t4[3], t4[2] };
    if (!have) { t[0] = -1; t[1] = 1; t[2] = -1; t[3] = 1; }
    kl_fove_centre(t, fx, fy);
    return have ? (from_layer ? 2 : 1) : 0;
}

// ...and, when ocx is not NULL, the OTHER eye's straight-ahead point placed in
// eye e's own rect (its fraction, this eye's viewport): the second peak of the
// envelope (kf_map_for).
static int kf_eye_point(int e, uint32_t W, uint32_t H, float *cx, float *cy, float *fx, float *fy,
                        float *ocx, float *ocy, char *why, size_t nwhy) {
    if (g_trec_on) {
        *fx = g_trec_fx[e & 1]; *fy = g_trec_fy[e & 1];
        int got = g_trec_of[0] > 0;
        int used = kl_fove_eye_px(*fx, *fy, W, H, got ? g_trec_vp[e & 1] : NULL, got ? g_trec_of : NULL, cx, cy);
        if (ocx) kl_fove_eye_px(g_trec_fx[(e & 1) ^ 1], g_trec_fy[(e & 1) ^ 1], W, H, got ? g_trec_vp[e & 1] : NULL,
                                got ? g_trec_of : NULL, ocx, ocy);
        snprintf(why, nwhy, !got ? "no frame record: full image" : used ? "viewport %d,%d %dx%d of %dx%d (test)"
                                                                        : "unusable (test)",
                 g_trec_vp[e & 1][0], g_trec_vp[e & 1][1], g_trec_vp[e & 1][2], g_trec_vp[e & 1][3], g_trec_of[0], g_trec_of[1]);
        return 2;
    }
    int have = kf_eye_frac(e, fx, fy);
    kl_ovrp_render_pose rec;
    int got = kf_last_record(&rec);
    int used = kl_fove_eye_px(*fx, *fy, W, H, got ? rec.viewport[e] : NULL, got ? rec.viewport_of : NULL, cx, cy);
    {   // A guest whose records alternate between a rect and none would flip the map.
        static int last = -1, said;
        if (got && used != last) {
            if (last >= 0 && said < 6) {
                said++;
                KFI("frame record (stage %d) %s a render viewport for eye %d (was %s)\n", rec.stage,
                    used ? "carries" : "carries no usable", e, last ? "a rect" : "none");
            }
            last = used;
        }
    }
    if (ocx) {
        float ofx, ofy;
        kf_eye_frac(e ^ 1, &ofx, &ofy);
        kl_fove_eye_px(ofx, ofy, W, H, got ? rec.viewport[e] : NULL, got ? rec.viewport_of : NULL, ocx, ocy);
    }
    // For the map line: which generation the centre came from.
    if (!got) snprintf(why, nwhy, "no frame record: full image");
    else if (!used) snprintf(why, nwhy, "viewport %dx%d of %dx%d unusable: full image", rec.viewport[e][2],
                             rec.viewport[e][3], rec.viewport_of[0], rec.viewport_of[1]);
    else snprintf(why, nwhy, "viewport %d,%d %dx%d of %dx%d%s", rec.viewport[e][0], rec.viewport[e][1],
                  rec.viewport[e][2], rec.viewport[e][3], rec.viewport_of[0], rec.viewport_of[1],
                  (uint32_t)rec.viewport_of[0] != W || (uint32_t)rec.viewport_of[1] != H
                      ? ", another generation: scaled as a fraction" : "");
    return have;
}

// g_mu held. Does a pending expand still refer to map slot mi?
static int kf_map_referenced(int mi) {
    for (int i = 0; i < KF_CB_N; i++) if (g_pend[i].cb && g_pend[i].map == mi) return 1;
    return 0;
}

// g_mu held. The cached map for a target shape, built on first use; NULL when
// it cannot be built (logged once per shape). keep_mi: the map slot the
// calling command buffer has already drawn with (its pending entry's), or -1;
// that buffer keeps it for the rest of its passes, re-centred or not (a scene
// subpass and the eye subpass that reads it must share one map).
static kf_map_t *kf_map_for_cb(uint32_t W, uint32_t H, int kind, int layers, int eye, int keep_mi) {
    kf_map_t *mp = NULL, *fr = NULL;
    for (int i = 0; i < KF_MAP_N; i++) {
        kf_map_t *c = &g_maps[i];
        if (i == keep_mi && c->W == W && c->H == H && c->kind == kind && c->layers == layers && c->eye == eye && c->map)
            return c;
    }
    for (int i = 0; i < KF_MAP_N; i++) {
        kf_map_t *c = &g_maps[i];
        if (c->superseded) continue;
        if (c->W == W && c->H == H && c->kind == kind && c->layers == layers && c->eye == eye) { mp = c; break; }
    }
    // A free slot: never used, or superseded with no expand left on it (its map
    // is retired like a re-centred one, kept a while for frames in flight).
    for (int i = 0; i < KF_MAP_N && !fr; i++) {
        kf_map_t *c = &g_maps[i];
        if (!c->W) fr = c;
        else if (c->superseded && !kf_map_referenced(i)) {
            int rs = kf_retire_slot();
            if (rs < 0) continue;
            g_retired[rs].map = c->map; g_retired[rs].buf = c->buf; g_retired[rs].t = kf_now();
            c->map = nil; c->buf = nil;
            *c = (kf_map_t){0};
            if (g_lastmap == c) g_lastmap = NULL;
            fr = c;
        }
    }
    int recentre = 0;
    if (mp && !mp->failed && kind == KF_ARRAY) {
        // The centre follows the frame record. A
        // map built before the first record (or before the guest's render
        // viewport settled) used the full image — Deadpool renders each eye
        // into a 4012-wide sub-rect of a 5088 texture (eye0 at 0, eye1 at
        // 1075), so its fovea sat 682 px off the straight-ahead point and the
        // ramp crossed the middle of the view: a hard vertical clear/blurry
        // line in both eyes. Rebuilt when a layer's centre moves more than
        // 4 px: in place when no pending expand refers to the map, else into
        // another slot, this one superseded until its expands have run. Until
        // toolchain 565 a referenced map was simply kept, and Alien: Rogue
        // Incursion always has an expand in flight: its map stayed where the
        // fifth re-centre put it while dynamic resolution took the viewport
        // from 79% to 55% of the image (the fovea ~800 px off on the Mac).
        char w[128];
        float ncx, ncy, nfx, nfy;
        int moved = 0, mi = (int)(mp - g_maps);
        for (int l = 0; l < layers && l < 2; l++) {
            int e = layers == 2 ? l : (eye == 1 ? 1 : 0);
            kf_eye_point(e, W, H, &ncx, &ncy, &nfx, &nfy, NULL, NULL, w, sizeof w);
            if (fabsf(ncx - mp->cx[l]) > 4.0f || fabsf(ncy - mp->cy[l]) > 4.0f) moved = 1;
        }
        if (!moved) return mp;
        if (kf_map_referenced(mi)) {
            if (!fr) return mp;           // no slot to build into: next lookup
            g_recentres++;
            if (g_recentres <= 8 || g_recentres % 50 == 0)
                KFI("map %ux%u RE-CENTRED (#%d): it was built at %.0f,%.0f / %.0f,%.0f — the frame record moved the "
                    "straight-ahead point (built into slot %d; slot %d kept for the expands still pending on it)\n",
                    W, H, g_recentres, mp->cx[0], mp->cy[0], mp->cx[1], mp->cy[1], (int)(fr - g_maps), mi);
            mp->superseded = 1;
            mp = fr;
            recentre = 1;
            goto build;
        }
        int rs = kf_retire_slot();
        if (rs < 0) {
            static int said;
            if (!said++) KFI("map %ux%u: the centre moved again but all %d retired maps are younger than %.0f s — "
                             "kept for now\n", W, H, KF_RETIRED_N, KF_RETIRE_S);
            return mp;
        }
        g_recentres++;
        recentre = 1;
        if (g_recentres <= 8 || g_recentres % 50 == 0)
            KFI("map %ux%u RE-CENTRED (#%d): it was built at %.0f,%.0f / %.0f,%.0f — the frame record moved the "
                "straight-ahead point (rebuilt in place, the old map kept %.0f s for frames in flight)\n", W, H,
                g_recentres, mp->cx[0], mp->cy[0], mp->cx[1], mp->cy[1], KF_RETIRE_S);
        g_retired[rs].map = mp->map; g_retired[rs].buf = mp->buf; g_retired[rs].t = kf_now();
        mp->map = nil; mp->buf = nil;
    } else if (mp) return mp->failed ? NULL : mp;
    if (!fr && !recentre) fr = &g_maps[KF_MAP_N - 1];   // evict the last (never reached: one guest, one or two shapes)
    if (!recentre) mp = fr;
build:
    if (mp->map && g_nold < 15) { g_old[g_nold++] = mp->map; g_old[g_nold++] = mp->buf; }
    *mp = (kf_map_t){0};
    mp->W = W; mp->H = H; mp->kind = kind; mp->layers = layers; mp->eye = eye; mp->failed = 1;
    float box = g_box * (float)H, ramp = g_ramp * (float)H;
    int nh = (int)(W / 16) + 1, nv = (int)(H / 16) + 1;
    if (nh > 4096) nh = 4096;
    if (nv > 4096) nv = 4096;
    float *hc[2] = { malloc(sizeof(float) * (size_t)nh), malloc(sizeof(float) * (size_t)nh) };
    float *vc[2] = { malloc(sizeof(float) * (size_t)nv), malloc(sizeof(float) * (size_t)nv) };
    int ok = hc[0] && hc[1] && vc[0] && vc[1];
    float cx[2] = {0, 0}, cy[2] = {0, 0}, fx[2] = {0.5f, 0.5f}, fy[2] = {0.5f, 0.5f};
    float ocx[2] = {0, 0}, ocy[2] = {0, 0};
    int have = 0, env = !g_per_eye;
    char vwhy[2][128] = { "", "" };
    uint32_t ew = 0, eh = 0;
    if (ok && kind == KF_DOUBLE) {
        float t[2][4] = {{-1, 1, -1, 1}, {-1, 1, -1, 1}};
        have = kf_double_eye_geometry(&ew, &eh, t);
        kl_fove_centre(t[0], &fx[0], &fy[0]);
        kl_fove_centre(t[1], &fx[1], &fy[1]);
        float half = W * 0.5f;
        cx[0] = fx[0] * half; cx[1] = half + fx[1] * half;
        cy[0] = cy[1] = 0.5f * (fy[0] + fy[1]) * (float)H;
        // The envelope (below): each half also peaks where the other eye's
        // point would be in it.
        float e0[2] = { cx[0], env ? fx[1] * half : cx[0] }, e1[2] = { cx[1], env ? half + fx[0] * half : cx[1] };
        ok = kl_fove_curves_env2(W, H, e0, e1, cy[0], box, ramp, g_floor, nh, hc[0], nv, vc[0]);
    } else if (ok) {
        // Each eye's straight-ahead point is off the middle of its picture,
        // toward the nose (the Vision Pro's cone is 60 degrees out and 45 in:
        // 63% across the left eye, 37% across the right). A map peaked there
        // alone leaves the outer side of each eye at the floor — the left eye
        // blurred over its left half, the right eye over its right half, as
        // seen in Alien: Rogue Incursion (toolchain 565) and, through the
        // display's own curves, in Doom 3 (f9f514f). So each layer is the
        // ENVELOPE of both eyes' points placed in that eye's rect: the same
        // curve relative to each eye's picture, symmetric about its middle,
        // both straight-ahead points at full rate. A symmetric cone (or the
        // union cone under symmetric projection) makes the two points one
        // and changes nothing. KL_FOVE_PER_EYE=1 is the single peak.
        for (int l = 0; l < layers && ok; l++) {
            int e = layers == 2 ? l : (eye == 1 ? 1 : 0);
            have = kf_eye_point(e, W, H, &cx[l], &cy[l], &fx[l], &fy[l], &ocx[l], &ocy[l], vwhy[l], sizeof vwhy[l]);
            if (!env) { ocx[l] = cx[l]; ocy[l] = cy[l]; }
            const float ex[2] = { cx[l], ocx[l] }, ey[2] = { cy[l], ocy[l] };
            ok = kl_fove_curves_env(W, H, ex, ey, box, ramp, g_floor, nh, hc[l], nv, vc[l]);
        }
    }
    id<MTLRasterizationRateMap> m = nil;
    if (ok) m = kf_make_map_n(W, H, kind == KF_DOUBLE ? 1 : layers, nh, (const float *const *)hc, nv,
                              (const float *const *)vc);
    for (int l = 0; l < 2; l++) { free(hc[l]); free(vc[l]); }
    if (!ok) { KFI("map for %ux%u: bad profile — this target stays unfoveated\n", W, H); return NULL; }
    if (!m) { KFI("newRasterizationRateMapWithDescriptor returned nil for %ux%u (%d layer(s))\n", W, H, layers); return NULL; }
    MTLSize P = [m physicalSizeForLayer:0];
    for (int l = 0; l < (kind == KF_DOUBLE ? 1 : layers); l++) {
        MTLSize Q = [m physicalSizeForLayer:(NSUInteger)l];
        mp->lpw[l] = (uint32_t)Q.width; mp->lph[l] = (uint32_t)Q.height;
    }
    float half = W * 0.5f;
    uint32_t bcol = (uint32_t)P.width;   // ARRAY: no seam, nothing to clamp
    if (kind == KF_DOUBLE) {
        bcol = 0;
        for (uint32_t i = 0; i < P.width; i++)
            if ([m mapPhysicalToScreenCoordinates:(MTLCoordinate2D){(float)i + 0.5f, 0} forLayer:0].x >= half) { bcol = i; break; }
    }
    // Full-rate runs (MSAA expand, DOUBLE and one-eye targets): the longest run
    // of screen pixels whose physical centre is the screen centre minus one
    // whole-pixel offset.
    uint32_t frr[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    int fo[3] = {0, 0, 0};
    int parts = kind == KF_DOUBLE ? 2 : 1;
    for (int axis = 0; g_msaa && layers == 1 && axis < 2; axis++) {
        uint32_t n = axis ? H : W;
        for (int part = 0; part < (axis ? 1 : parts); part++) {
            uint32_t lo = axis ? 0 : part ? (uint32_t)half : 0;
            uint32_t hi = axis ? n : (parts == 1 || part) ? n : (uint32_t)half;
            uint32_t best0 = 0, best1 = 0, run0 = lo;
            float prev_off = NAN, best_off = 0;
            for (uint32_t i = lo; i <= hi; i++) {
                float off = NAN;
                if (i < hi) {
                    MTLCoordinate2D sc = axis ? (MTLCoordinate2D){0, (float)i + 0.5f} : (MTLCoordinate2D){(float)i + 0.5f, 0};
                    MTLCoordinate2D pc = [m mapScreenToPhysicalCoordinates:sc forLayer:0];
                    float o = (axis ? sc.y - pc.y : sc.x - pc.x);
                    if (fabsf(o - roundf(o)) < 1e-3f) off = roundf(o);
                }
                if (!(off == prev_off)) {   // run ends (NaN never equals)
                    if (!isnan(prev_off) && i - run0 > best1 - best0) { best0 = run0; best1 = i; best_off = prev_off; }
                    run0 = i;
                }
                prev_off = off;
            }
            // A run shorter than 8 px is no fovea (the profile has no full-rate box).
            if (best1 - best0 < 8) best0 = best1 = 0;
            frr[axis ? 2 : part][0] = best0; frr[axis ? 2 : part][1] = best1;
            fo[axis ? 2 : part] = (int)best_off;
        }
    }
    mp->map = m;
    mp->buf = kf_params(m);
    mp->pw = (uint32_t)P.width; mp->ph = (uint32_t)P.height; mp->bcol = bcol;
    mp->fx0[0] = frr[0][0]; mp->fx1[0] = frr[0][1]; mp->fx0[1] = frr[1][0]; mp->fx1[1] = frr[1][1];
    mp->fy0 = frr[2][0]; mp->fy1 = frr[2][1];
    mp->offx[0] = fo[0]; mp->offx[1] = fo[1]; mp->offy = fo[2];
    mp->cx[0] = cx[0]; mp->cx[1] = cx[1]; mp->cy[0] = cy[0]; mp->cy[1] = cy[1];
    mp->ocx[0] = ocx[0]; mp->ocx[1] = ocx[1]; mp->ocy[0] = ocy[0]; mp->ocy[1] = ocy[1];
    mp->env = env;
    mp->failed = 0;
    g_lastmap = mp;
    double frac = 0;
    int nl = kind == KF_DOUBLE ? 1 : layers;
    for (int l = 0; l < nl; l++) frac += (double)mp->lpw[l] * mp->lph[l] / ((double)W * H) / nl;
    if (kind == KF_DOUBLE) {
        KFI("map %ux%u (%s: box %.0f px, ramp %.0f px, floor %.3f; %d x %d samples): eye centres x %.0f / %.0f "
            "(%.1f%% / %.1f%% of each eye from its left edge), y %.0f (%.1f%% from the top)%s; physical %ux%u = "
            "%.1f%% of the fragments (x %.3f, y %.3f); eye 1 starts at physical column %u; granularity %lux%lu\n",
            W, H, g_level_name, box, ramp, g_floor, nh, nv, cx[0], cx[1], 100 * fx[0], 100 * fx[1], cy[0],
            50 * (fy[0] + fy[1]), have ? "" : " (NO frustum from the runtime: centred)", mp->pw, mp->ph,
            100.0 * frac, (double)mp->pw / W, (double)mp->ph / H, bcol,
            (unsigned long)m.physicalGranularity.width, (unsigned long)m.physicalGranularity.height);
        if (ew && (W != ew || H != eh))
            KFI("note: target %ux%u is not the submitted texture's %ux%u\n", W, H, ew, eh);
    } else {
        KFI("map %ux%u, %d layer(s)%s (%s: box %.0f px, ramp %.0f px, floor %.3f; %d x %d samples): "
            "layer 0 = eye %d centre %.0f,%.0f (%.1f%% / %.1f%%; %s) physical %ux%u%s%s; "
            "%.1f%% of the fragments; granularity %lux%lu\n",
            W, H, layers, layers == 2 ? " (one per eye)" : "", g_level_name, box, ramp, g_floor, nh, nv,
            layers == 2 ? 0 : (eye == 1 ? 1 : 0), cx[0], cy[0], 100 * fx[0], 100 * fy[0], vwhy[0], mp->lpw[0], mp->lph[0],
            have == 2 ? " (frustum: the guest's eye-layer Fov)" : have ? " (frustum: the cone kl_ovrp tells the guest)"
                      : " (NO frustum from kl_ovrp: centred)", "", 100.0 * frac,
            (unsigned long)m.physicalGranularity.width, (unsigned long)m.physicalGranularity.height);
        if (layers == 2)
            KFI("map %ux%u layer 1 = eye 1 centre %.0f,%.0f (%.1f%% / %.1f%%; %s) physical %ux%u\n",
                W, H, cx[1], cy[1], 100 * fx[1], 100 * fy[1], vwhy[1], mp->lpw[1], mp->lph[1]);
    }
    if (env && kind == KF_DOUBLE)
        KFI("map %ux%u: the envelope of both eyes' points (KL_FOVE_PER_EYE=0): each half also at full rate at "
            "x %.0f / %.0f\n", W, H, fx[1] * half, half + fx[0] * half);
    else if (env)
        for (int l = 0; l < layers; l++)
            KFI("map %ux%u layer %d: the envelope of both eyes' points (KL_FOVE_PER_EYE=0) — also at full rate at "
                "%.0f,%.0f, the other eye's point in this eye's rect; full-rate middle x %.0f\n", W, H, l, ocx[l],
                ocy[l], 0.5f * (cx[l] + ocx[l]));
    else
        KFI("map %ux%u: one peak per eye (KL_FOVE_PER_EYE=1)\n", W, H);
    if (g_msaa && layers == 1)
        KFI("full-rate rectangles (MSAA eyes copy these per sample): eye 0 x %u..%u, eye 1 x %u..%u, y %u..%u "
            "(screen - physical: x %d / %d, y %d)\n", mp->fx0[0], mp->fx1[0], mp->fx0[1], mp->fx1[1],
            mp->fy0, mp->fy1, mp->offx[0], mp->offx[1], mp->offy);
    return mp;
}

static kf_map_t *kf_map_for(uint32_t W, uint32_t H, int kind, int layers, int eye) {
    return kf_map_for_cb(W, H, kind, layers, eye, -1);
}

// ---------------------------------------------------------------- the expand
static NSString *kExpandMSL = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct KfV { float4 pos [[position]]; };\n"
    "struct KfC { float bcol; float half_w; float pad0; float pad1; };\n"
    "vertex KfV kf_vs(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  KfV o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o;\n"
    "}\n"
    "fragment float4 kf_fs(KfV in [[stage_in]], constant rasterization_rate_map_data& d [[buffer(0)]],\n"
    "                      constant KfC& c [[buffer(1)]], texture2d<float> phys [[texture(0)]]) {\n"
    "  constexpr sampler ls(coord::pixel, filter::linear, address::clamp_to_edge);\n"
    "  rasterization_rate_map_decoder dec(d);\n"
    "  float2 p = dec.map_screen_to_physical_coordinates(in.pos.xy);\n"
    "  if (in.pos.x < c.half_w) p.x = min(p.x, c.bcol - 0.5); else p.x = max(p.x, c.bcol + 0.5);\n"
    "  return phys.sample(ls, p);\n"
    "}\n"
    // one slice of a layered eye — its own map layer, clamped to that
    // layer's physical region (a per-layer scratch is that layer's size).
    "struct KfA { uint layer; float pw; float ph; float pad; };\n"
    "fragment float4 kf_fs_a(KfV in [[stage_in]], constant rasterization_rate_map_data& d [[buffer(0)]],\n"
    "                        constant KfA& c [[buffer(1)]], texture2d<float> phys [[texture(0)]]) {\n"
    "  constexpr sampler ls(coord::pixel, filter::linear, address::clamp_to_edge);\n"
    "  rasterization_rate_map_decoder dec(d);\n"
    "  float2 p = dec.map_screen_to_physical_coordinates(in.pos.xy, c.layer);\n"
    "  p = clamp(p, float2(0.5), float2(c.pw, c.ph) - 0.5);\n"
    "  return phys.sample(ls, p);\n"
    "}\n";

static id<MTLRenderPipelineState> g_pso_a[4];
static MTLPixelFormat g_pso_a_fmt[4];
static id<MTLTexture> g_scratch_l[2][KF_SC_WAYS];  // per-layer physical copies, per format

// g_mu held. layered: the per-slice function (kf_fs_a), else the one-slice kf_fs.
static id<MTLRenderPipelineState> kf_pso_l(MTLPixelFormat fmt, int layered) {
    id<MTLRenderPipelineState> __strong *pso = layered ? g_pso_a : g_pso;
    MTLPixelFormat *pf = layered ? g_pso_a_fmt : g_pso_fmt;
    for (int i = 0; i < 4; i++) if (pso[i] && pf[i] == fmt) return pso[i];
    static id<MTLLibrary> lib;
    if (!lib) {
        NSError *err = nil;
        t_in = 1;
        lib = [g_dev newLibraryWithSource:kExpandMSL options:nil error:&err];
        t_in = 0;
        if (!lib) { KFI("expand shader: %s\n", err ? err.description.UTF8String : "?"); return nil; }
    }
    MTLRenderPipelineDescriptor *d = [objc_getClass("MTLRenderPipelineDescriptor") new];
    id<MTLFunction> vf = [lib newFunctionWithName:@"kf_vs"], ff = [lib newFunctionWithName:layered ? @"kf_fs_a" : @"kf_fs"];
    d.vertexFunction = vf; d.fragmentFunction = ff;
    d.colorAttachments[0].pixelFormat = fmt;
    NSError *err = nil;
    id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithDescriptor:d error:&err];
    KF_RELEASE(d); KF_RELEASE(vf); KF_RELEASE(ff);
    if (!p) { KFI("expand pipeline (format %lu): %s\n", (unsigned long)fmt, err ? err.description.UTF8String : "?"); return nil; }
    for (int i = 0; i < 4; i++) if (!pso[i]) { pso[i] = p; pf[i] = fmt; return p; }
    pso[3] = p; pf[3] = fmt;
    return p;
}
static id<MTLRenderPipelineState> kf_pso(MTLPixelFormat fmt) { return kf_pso_l(fmt, 0); }

// g_mu held.
static id<MTLTexture> kf_scratch(MTLPixelFormat fmt, uint32_t w, uint32_t h) {
    return kf_scratch_into(g_scratch, "kl-fove-scratch", fmt, 1, w, h, MTLTextureUsageShaderRead);
}

// The MSAA expand. The eye colour is multisampled and the guest's
// own resolve (a compute pass after the eye passes) reads every sample, so the
// expand must leave a MULTISAMPLED screen-layout image. Three steps, all on the
// eye's command buffer:
//  1. resolve: the physical region of the eye -> a single-sample physical-size
//     scratch (one pixel-rate draw averaging each pixel's samples);
//  2. copy each eye's full-rate rectangle (physical position) with its samples
//     into a small multisampled scratch (blit);
//  3. one encoder on the eye: a pixel-rate draw over the whole eye writes the
//     bilinear-filtered resolved value to every sample (the periphery — exactly
//     the single-sample expand, from resolved values), then one per-sample draw per eye,
//     scissored to its full-rate rectangle, copies the fovea's samples back
//     (there physical = screen - a whole-pixel offset, so the fovea keeps its
//     MSAA edges, sample for sample).
static NSString *kExpandMSMSL = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct KfV { float4 pos [[position]]; };\n"
    "struct KfC { float bcol; float half_w; float pad0; float pad1; };\n"
    "struct KfF { uint2 src0; uint2 dst0; };\n"
    "vertex KfV kf_vs(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  KfV o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o;\n"
    "}\n"
    "fragment float4 kf_fs_resolve(KfV in [[stage_in]], texture2d_ms<float> ms [[texture(0)]]) {\n"
    "  uint2 q = uint2(in.pos.xy); uint n = ms.get_num_samples(); float4 a = 0;\n"
    "  for (uint i = 0; i < n; i++) a += ms.read(q, i);\n"
    "  return a / float(n);\n"
    "}\n"
    "fragment float4 kf_fs_ms(KfV in [[stage_in]], constant rasterization_rate_map_data& d [[buffer(0)]],\n"
    "                         constant KfC& c [[buffer(1)]], texture2d<float> phys [[texture(0)]]) {\n"
    "  constexpr sampler ls(coord::pixel, filter::linear, address::clamp_to_edge);\n"
    "  rasterization_rate_map_decoder dec(d);\n"
    "  float2 p = dec.map_screen_to_physical_coordinates(in.pos.xy);\n"
    "  if (in.pos.x < c.half_w) p.x = min(p.x, c.bcol - 0.5); else p.x = max(p.x, c.bcol + 0.5);\n"
    "  float4 r = phys.sample(ls, p);\n"
    "  float a = p.x * 1e-4;\n"                     // KL_FOVE_SLOW (debug stress): busy work, result unused
    "  for (uint i = 0; i < uint(c.pad0); i++) a = sin(a * 1.0001 + 0.37);\n"
    "  if (a == 1234.5) r.x += 1.0;\n"
    "  return r;\n"
    "}\n"
    "fragment float4 kf_fs_ms_exact(KfV in [[stage_in]], uint sid [[sample_id]], constant KfF& f [[buffer(0)]],\n"
    "                               texture2d_ms<float> fov [[texture(0)]]) {\n"
    "  return fov.read(uint2(in.pos.xy) - f.src0 + f.dst0, sid);\n"
    "}\n";

static id<MTLRenderPipelineState> g_pso_ms[8];
static uint64_t g_pso_ms_key[8];
static id<MTLTexture> g_scratch_fov[KF_SC_WAYS]; // the fovea rectangles, multisampled
static id<MTLTexture> g_scratch_res[KF_SC_WAYS]; // the resolved physical region

// g_mu held. kind 0: periphery (pixel rate, into the MSAA eye), 1: fovea
// (per sample, into the MSAA eye), 2: resolve (into the single-sample scratch).
static id<MTLRenderPipelineState> kf_pso_ms(MTLPixelFormat fmt, NSUInteger samples, int kind) {
    uint64_t key = (uint64_t)fmt << 16 | (uint64_t)samples << 2 | (uint64_t)kind;
    for (int i = 0; i < 8; i++) if (g_pso_ms[i] && g_pso_ms_key[i] == key) return g_pso_ms[i];
    static id<MTLLibrary> lib;
    if (!lib) {
        NSError *err = nil;
        t_in = 1;
        lib = [g_dev newLibraryWithSource:kExpandMSMSL options:nil error:&err];
        t_in = 0;
        if (!lib) { KFI("MSAA expand shader: %s\n", err ? err.description.UTF8String : "?"); return nil; }
    }
    static NSString *const names[3] = { @"kf_fs_ms", @"kf_fs_ms_exact", @"kf_fs_resolve" };
    MTLRenderPipelineDescriptor *d = [objc_getClass("MTLRenderPipelineDescriptor") new];
    id<MTLFunction> vf = [lib newFunctionWithName:@"kf_vs"];
    id<MTLFunction> ff = [lib newFunctionWithName:names[kind]];
    d.vertexFunction = vf; d.fragmentFunction = ff;
    d.colorAttachments[0].pixelFormat = fmt;
    d.rasterSampleCount = kind == 2 ? 1 : samples;
    NSError *err = nil;
    id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithDescriptor:d error:&err];
    KF_RELEASE(d); KF_RELEASE(vf); KF_RELEASE(ff);
    if (!p) {
        KFI("MSAA expand pipeline (format %lu x%lu kind %d): %s\n", (unsigned long)fmt, (unsigned long)samples,
            kind, err ? err.description.UTF8String : "?");
        return nil;
    }
    for (int i = 0; i < 8; i++) if (!g_pso_ms[i]) { g_pso_ms[i] = p; g_pso_ms_key[i] = key; return p; }
    g_pso_ms[7] = p; g_pso_ms_key[7] = key;
    return p;
}

// g_mu held. A private scratch texture from a per-use cache of KF_SC_WAYS
// entries: ONE entry per (pixel format, sample count), (re)made only when that
// entry's size changes. Why: with a single entry per use, a guest with two
// confirmed intermediates of different formats (Paint the Town Red: RG11B10
// HDR, Metal fmt 92, and RGBA8 sRGB LDR, 71, both expanded every frame)
// replaced each layer's scratch twice a frame (4 x 14 MB new textures per
// frame); Metal did not hand the replaced ones back in time and the footprint
// climbed 5.2 -> 7.8 GB in ~2 s, a silent jetsam kill 2 s into the level,
// every time.
static id<MTLTexture> kf_scratch_into(id<MTLTexture> __strong *ways, const char *what, MTLPixelFormat fmt,
                                      NSUInteger samples, uint32_t w, uint32_t h, MTLTextureUsage usage) {
    int same = -1, empty = -1;
    for (int i = 0; i < KF_SC_WAYS; i++) {
        id<MTLTexture> t = ways[i];
        if (!t) { if (empty < 0) empty = i; continue; }
        if (t.pixelFormat == fmt && t.sampleCount == samples) {
            if (t.width == w && t.height == h) return t;
            same = i;
        }
    }
    int v = same >= 0 ? same : empty >= 0 ? empty : (int)(g_sc_rr++ % KF_SC_WAYS);
    MTLTextureDescriptor *d = [objc_getClass("MTLTextureDescriptor") texture2DDescriptorWithPixelFormat:fmt
                                                                                                    width:w height:h mipmapped:NO];
    if (samples > 1) { d.textureType = MTLTextureType2DMultisample; d.sampleCount = samples; }
    d.usage = usage;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
    if (!t) return nil;
    t.label = [NSString stringWithUTF8String:what];
    // The runtime ships WITHOUT ARC (Makefile XROS_CFLAGS): the slot assignment
    // below does not release the texture it replaces. A replaced entry may
    // still be read by an in-flight command buffer, so it is parked in g_old
    // (never freed) or, once that is full, deliberately leaked — which before
    // the per-format cache was EVERY replacement after the 16th (14 MB each, 4 per frame). With
    // one entry per format a replacement now needs a size change: rare.
    if (ways[v]) { if (g_nold < 16) g_old[g_nold++] = ways[v]; else g_sc_leaked++; }
    ways[v] = t;
    g_sc_allocs++;
    KFI("%s %ux%u x%lu fmt %lu (%.1f MB, way %d, %s)\n", what, w, h, (unsigned long)samples, (unsigned long)fmt,
        (double)w * h * samples * 4 / 1e6, v, same >= 0 ? "resized" : empty >= 0 ? "new format" : "evicted another format");
    if (g_sc_leaked) KFI("  (%u replaced scratch texture(s) leaked so far: g_old full)\n", g_sc_leaked);
    return t;
}

static SEL s_render;

// ---------------------------------------------------------------- expand ordering (the expand fence)
// MoltenVK (useMTLHeap 1, argument buffers) does not rely on Metal's hazard
// tracking to order its passes: every encoder it opens WAITS on its own barrier
// fences and UPDATES them at its end (KL_FOVE_PROBE shows ~8 waits and 2-3
// updates per encoder in Half-Life: Alyx). The guest's later readers of the eye are
// ordered only through those fences. The expand's encoders are ours: they
// neither wait nor update any of MoltenVK's fences, so nothing the guest
// encodes after them waits for them. When the reader is the very next encoder
// (Half-Life: Alyx: Valve's compute MSAA resolve right after the eye passes) the GPU runs
// it CONCURRENTLY with the expand and it reads the eye while it still holds
// (all or part of) the physical layout — shrunken copies of the scene and
// blocks of the wrong picture in the eye.
//
// Fix: the expand's last encoder updates g_xfence, and every encoder opened
// afterwards waits on it: in the same command buffer at once; in a later
// command buffer of the same queue once the expand's buffer is committed (so a
// wait can never precede its update in submission order). A wait on a fence
// whose update has completed costs nothing. The layered single-sample expand
// is two encoders (one per slice): slice 0 updates g_xfence_l, slice 1 waits on
// it and updates g_xfence, so the one fence the guest waits on covers both.
// KL_FOVE_FENCE=0 = the old, unordered behaviour (control).
//
// Not covered: an encoder on ANOTHER queue (counted as "other queue"): no wait
// is recorded there — Metal fences only order work within one queue, and a
// cross-queue reader (the runtime's compositor) is ordered by the frame's
// completion instead.
static id<MTLFence> g_xfence, g_xfence_l;
static struct { void *cb; uint64_t gen; void *queue; int committed; uint64_t n; } g_xf;   // g_mu
static uint64_t g_xf_waits;
// Why an encoder opened after an expand did or did not wait (the 10 s stats line):
// same = on the expand's own buffer, later = a later buffer once the expand's was committed,
// open = a different buffer while the expand's was still uncommitted (NOT ordered), queue = another queue.
static uint64_t g_xf_same, g_xf_later, g_xf_open, g_xf_queue, g_xf_at_commit, g_xf_at_enc;
static uint64_t kf_cb_gen(id cb, int make);

static void kf_xf_updated(id cb) {      // an expand's last encoder updated g_xfence (on cb)
    uint64_t gen = kf_cb_gen(cb, 1);
    void *q = (__bridge void *)[(id<MTLCommandBuffer>)cb commandQueue];
    pthread_mutex_lock(&g_mu);
    g_xf.cb = (__bridge void *)cb; g_xf.gen = gen; g_xf.queue = q; g_xf.committed = 0; g_xf.n++;
    pthread_mutex_unlock(&g_mu);
}
static void kf_xf_committed(id cb) {
    if (!g_xfence) return;
    uint64_t gen = kf_cb_gen(cb, 0);
    pthread_mutex_lock(&g_mu);
    if (g_xf.cb == (__bridge void *)cb && g_xf.gen == gen && gen) g_xf.committed = 1;
    pthread_mutex_unlock(&g_mu);
}
// kind 0 render, 1 compute, 2 blit. A new encoder `en` on command buffer `cb`.
static void kf_xf_wait(id cb, id en, int kind) {
    if (!g_xfence || !en || t_in) return;
    int need;
    pthread_mutex_lock(&g_mu);
    need = g_xf.n && g_xf.cb;
    void *xcb = g_xf.cb, *xq = g_xf.queue; uint64_t xgen = g_xf.gen; int xc = g_xf.committed;
    pthread_mutex_unlock(&g_mu);
    if (!need) return;
    int same = xcb == (__bridge void *)cb && kf_cb_gen(cb, 0) == xgen;
    int sameq = xq == (__bridge void *)[(id<MTLCommandBuffer>)cb commandQueue];
    if (same) need = 1;
    else need = xc && sameq;
    __atomic_add_fetch(same ? &g_xf_same : need ? &g_xf_later : !sameq ? &g_xf_queue : &g_xf_open, 1, __ATOMIC_RELAXED);
    if (!need) return;
    t_in = 1;
    if (kind == 0) [(id<MTLRenderCommandEncoder>)en waitForFence:g_xfence beforeStages:MTLRenderStageVertex];
    else if (kind == 1) [(id<MTLComputeCommandEncoder>)en waitForFence:g_xfence];
    else [(id<MTLBlitCommandEncoder>)en waitForFence:g_xfence];
    t_in = 0;
    uint64_t n = __atomic_add_fetch(&g_xf_waits, 1, __ATOMIC_RELAXED);
    if (n == 1)
        KFI("expand fence: the guest's encoders after an expand wait for it (first wait: a %s encoder; "
            "KL_FOVE_FENCE=0 turns this off)\n", kind == 0 ? "render" : kind == 1 ? "compute" : "blit");
}
static void kf_hazard_line(id<MTLTexture> eye) {
    KFI("eye colour hazard tracking %s (mode %lu), %s\n",
        eye.hazardTrackingMode == MTLHazardTrackingModeUntracked ? "UNTRACKED" : "tracked",
        (unsigned long)eye.hazardTrackingMode, eye.heap ? "in a heap" : "not in a heap");
}

static void kf_expand_ms(id cb, id<MTLTexture> eye, int mi) {
    NSUInteger S = eye.sampleCount;
    MTLPixelFormat fmt = eye.pixelFormat;
    pthread_mutex_lock(&g_mu);
    kf_map_t *mp = mi >= 0 && mi < KF_MAP_N && g_maps[mi].map ? &g_maps[mi] : NULL;
    if (!mp) { pthread_mutex_unlock(&g_mu); return; }
    uint32_t pw = mp->pw, ph = mp->ph, W = mp->W, H = mp->H;
    uint32_t fx0[2] = { mp->fx0[0], mp->fx0[1] }, fx1[2] = { mp->fx1[0], mp->fx1[1] };
    uint32_t fy0 = mp->fy0, fy1 = mp->fy1;
    int ox[2] = { mp->offx[0], mp->offx[1] }, oy = mp->offy;
    // The fovea rectangles as they sit in PHYSICAL layout, and where each goes
    // in the fovea scratch (side by side).
    int fov[2] = { 0, 0 };
    uint32_t fw[2], fh = fy1 > fy0 ? fy1 - fy0 : 0, base[2] = { 0, 0 };
    for (int e = 0; e < 2; e++) {
        fw[e] = fx1[e] > fx0[e] ? fx1[e] - fx0[e] : 0;
        long sx = (long)fx0[e] - ox[e], sy = (long)fy0 - oy;
        fov[e] = g_expand_fovea && fw[e] && fh && fx1[e] <= W && fy1 <= H && sx >= 0 && sy >= 0 &&
                 sx + fw[e] <= pw && sy + fh <= ph;
    }
    base[1] = fov[0] ? fw[0] : 0;
    uint32_t sw = (fov[0] ? fw[0] : 0) + (fov[1] ? fw[1] : 0);
    id<MTLTexture> res = mp->map ? kf_scratch_into(g_scratch_res, "kl-fove-msaa-resolved", fmt, 1, pw, ph,
                                                     MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget) : nil;
    id<MTLTexture> fsc = res && sw ? kf_scratch_into(g_scratch_fov, "kl-fove-msaa-fovea", fmt, S, sw, fh,
                                                     MTLTextureUsageShaderRead) : nil;
    id<MTLRenderPipelineState> pr = res ? kf_pso_ms(fmt, S, 2) : nil;
    id<MTLRenderPipelineState> pb = pr ? kf_pso_ms(fmt, S, 0) : nil;
    id<MTLRenderPipelineState> px = pb && fsc ? kf_pso_ms(fmt, S, 1) : nil;
    id<MTLBuffer> mb = mp->buf;
    // DOUBLE: clamp each half to its own eye's physical columns. One-eye
    // image: no seam — every pixel takes the first branch and is clamped to
    // the physical width (bcol = pw).
    float c[4] = { (float)mp->bcol, mp->kind == KF_DOUBLE ? mp->W * 0.5f : 1e30f, (float)g_slow, 0 };
    pthread_mutex_unlock(&g_mu);
    if (!res || !pr || !pb || !mb) return;
    @autoreleasepool {
        t_in = 1;
        IMP o = kf_orig(cb, s_render);
        // 1. resolve the physical region
        MTLRenderPassDescriptor *rp = [objc_getClass("MTLRenderPassDescriptor") renderPassDescriptor];
        rp.colorAttachments[0].texture = res;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> en = ((id (*)(id, SEL, id))o)(cb, s_render, rp);
        if (en) {
            [en setRenderPipelineState:pr];
            [en setFragmentTexture:eye atIndex:0];
            [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [en endEncoding];
        }
        // 2. the fovea rectangles, every sample
        if (px) {
            id<MTLBlitCommandEncoder> bl = [(id<MTLCommandBuffer>)cb blitCommandEncoder];
            for (int e = 0; e < 2; e++)
                if (fov[e])
                    [bl copyFromTexture:eye sourceSlice:0 sourceLevel:0
                           sourceOrigin:MTLOriginMake((NSUInteger)((long)fx0[e] - ox[e]), (NSUInteger)((long)fy0 - oy), 0)
                             sourceSize:MTLSizeMake(fw[e], fh, 1) toTexture:fsc destinationSlice:0 destinationLevel:0
                      destinationOrigin:MTLOriginMake(base[e], 0, 0)];
            [bl endEncoding];
        }
        // 3. the eye, screen layout
        rp = [objc_getClass("MTLRenderPassDescriptor") renderPassDescriptor];
        rp.colorAttachments[0].texture = eye;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        en = ((id (*)(id, SEL, id))o)(cb, s_render, rp);
        if (en) {
            [en setRenderPipelineState:pb];
            [en setFragmentBuffer:mb offset:0 atIndex:0];
            [en setFragmentBytes:c length:sizeof c atIndex:1];
            [en setFragmentTexture:res atIndex:0];
            [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            if (px) {
                [en setRenderPipelineState:px];
                [en setFragmentTexture:fsc atIndex:0];
                for (int e = 0; e < 2; e++) {
                    if (!fov[e]) continue;
                    uint32_t f[4] = { fx0[e], fy0, base[e], 0 };
                    [en setFragmentBytes:f length:sizeof f atIndex:0];
                    [en setScissorRect:(MTLScissorRect){ fx0[e], fy0, fw[e], fh }];
                    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                }
            }
            if (g_xfence) [en updateFence:g_xfence afterStages:MTLRenderStageFragment];
            [en endEncoding];
        }
        t_in = 0;
        if (en && g_xfence) kf_xf_updated(cb);
    }
    uint64_t n = __atomic_add_fetch(&g_st.ms_expands, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_st.expands, 1, __ATOMIC_RELAXED);
    if (n == 1)
        KFI("first MSAA expand: eye colour %lux%lu x%lu fmt %lu, physical %ux%u -> screen; fovea copied per sample: "
            "eye 0 %s (%ux%u from physical %d,%d), eye 1 %s (%ux%u from physical %d,%d)%s\n",
            (unsigned long)eye.width, (unsigned long)eye.height, (unsigned long)S, (unsigned long)fmt, pw, ph,
            fov[0] ? "yes" : "NO", fw[0], fh, (int)fx0[0] - ox[0], (int)fy0 - oy,
            fov[1] ? "yes" : "NO", fw[1], fh, (int)fx0[1] - ox[1], (int)fy0 - oy,
            g_expand_fovea ? "" : " (KL_FOVE_MSAA_FOVEA=0)");
    if (n == 1) kf_hazard_line(eye);
}

// The single-sample expand. A one-slice eye (DOUBLE, or a one-eye image): copy
// the physical region to a scratch, draw it back over the whole eye. A layered
// eye (a 2D-array eye pair drawn as two slices):
// the same per slice, with that slice's own map layer and physical size.
static void kf_expand(id cb, id<MTLTexture> eye, int mi) {
    if (!g_expand) return;
    if (eye.sampleCount > 1) { kf_expand_ms(cb, eye, mi); return; }
    pthread_mutex_lock(&g_mu);
    kf_map_t *mp = mi >= 0 && mi < KF_MAP_N && g_maps[mi].map ? &g_maps[mi] : NULL;
    int layered = mp && mp->kind == KF_ARRAY && mp->layers == 2;
    int nl = layered ? 2 : 1;
    id<MTLTexture> sc[2] = { nil, nil };
    id<MTLRenderPipelineState> pso = nil;
    uint32_t pw[2] = {0, 0}, ph[2] = {0, 0};
    id<MTLBuffer> mb = mp ? mp->buf : nil;
    float c[4] = { 0, 0, 0, 0 };
    if (mp) {
        if (layered) {
            for (int l = 0; l < 2; l++) {
                pw[l] = mp->lpw[l]; ph[l] = mp->lph[l];
                sc[l] = kf_scratch_into(g_scratch_l[l], l ? "kl-fove-scratch-layer1" : "kl-fove-scratch-layer0",
                                        eye.pixelFormat, 1, pw[l], ph[l], MTLTextureUsageShaderRead);
            }
            pso = sc[0] && sc[1] ? kf_pso_l(eye.pixelFormat, 1) : nil;
        } else {
            pw[0] = mp->pw; ph[0] = mp->ph;
            sc[0] = kf_scratch(eye.pixelFormat, pw[0], ph[0]);
            pso = sc[0] ? kf_pso(eye.pixelFormat) : nil;
            c[0] = (float)mp->bcol; c[1] = mp->kind == KF_DOUBLE ? mp->W * 0.5f : 1e30f;
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (!sc[0] || !pso || !mb) return;
    int array_tex = eye.textureType == MTLTextureType2DArray;
    if (layered && (!array_tex || eye.arrayLength < 2)) {
        static int said;
        if (!said++) KFI("expand: a layered map but the eye texture is not a 2-slice array (type %lu, %lu slice(s)) "
                         "— left in physical layout\n", (unsigned long)eye.textureType, (unsigned long)eye.arrayLength);
        return;
    }
    @autoreleasepool {
        t_in = 1;
        id<MTLBlitCommandEncoder> bl = [(id<MTLCommandBuffer>)cb blitCommandEncoder];
        for (int l = 0; l < nl; l++)
            [bl copyFromTexture:eye sourceSlice:(NSUInteger)l sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(pw[l], ph[l], 1) toTexture:sc[l] destinationSlice:0 destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        [bl endEncoding];
        IMP o = kf_orig(cb, s_render);
        int fenced = 0, chained = 0;      // g_xfence updated (last encoder) / g_xfence_l updated (slice 0)
        for (int l = 0; l < nl; l++) {
            MTLRenderPassDescriptor *rp = [objc_getClass("MTLRenderPassDescriptor") renderPassDescriptor];
            rp.colorAttachments[0].texture = eye;
            rp.colorAttachments[0].slice = (NSUInteger)l;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> en = ((id (*)(id, SEL, id))o)(cb, s_render, rp);
            if (!en) continue;
            [en setRenderPipelineState:pso];
            [en setFragmentBuffer:mb offset:0 atIndex:0];
            if (layered) {
                struct { uint32_t layer; float pw, ph, pad; } ca = { (uint32_t)l, (float)pw[l], (float)ph[l], 0 };
                [en setFragmentBytes:&ca length:sizeof ca atIndex:1];
            } else
                [en setFragmentBytes:c length:sizeof c atIndex:1];
            [en setFragmentTexture:sc[l] atIndex:0];
            [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            if (g_xfence) {
                // Layered: slice 0 -> g_xfence_l -> slice 1 -> g_xfence (the
                // guest waits on g_xfence only, so it must cover both slices).
                if (chained) [en waitForFence:g_xfence_l beforeStages:MTLRenderStageVertex];
                if (l + 1 < nl && g_xfence_l) { [en updateFence:g_xfence_l afterStages:MTLRenderStageFragment]; chained = 1; }
                else { [en updateFence:g_xfence afterStages:MTLRenderStageFragment]; fenced = 1; }
            }
            [en endEncoding];
        }
        t_in = 0;
        if (fenced) kf_xf_updated(cb);
    }
    uint64_t n = __atomic_add_fetch(&g_st.expands, 1, __ATOMIC_RELAXED);
    if (n == 1) KFI("first expand: eye colour %lux%lu%s fmt %lu, physical %ux%u%s -> screen\n",
                    (unsigned long)eye.width, (unsigned long)eye.height, layered ? " x2 slices" : "",
                    (unsigned long)eye.pixelFormat, pw[0], ph[0], layered ? " / layer 1 below" : "");
    if (n == 1 && layered) KFI("first expand: layer 1 physical %ux%u\n", pw[1], ph[1]);
    if (n == 1) kf_hazard_line(eye);
}

// The pending expand of a command buffer. The table is keyed by the (unretained)
// command-buffer pointer PLUS a generation number that lives on the command
// buffer itself, as an associated "tag" object: a lookup only matches when the
// buffer it is asked about carries the entry's generation, and the tag's
// dealloc (the command buffer going away — committed or not) drops whatever the
// buffer still had pending. So a buffer released WITHOUT -commit can neither
// leave a stale entry that a later buffer at the same address would inherit,
// nor leak a slot. A full table is checked BEFORE a pass is mapped (a mapped
// pass whose expand cannot be recorded would leave the eye in physical layout).
static uint64_t g_pend_gen;           // g_mu held
static int g_pend_quiet;              // t_fove self-test

// Pure table operations (g_mu held by the callers here; public for t_fove).
int kl_fove_pend_find(void *cb, uint64_t gen) {
    if (!cb || !gen) return -1;
    for (int i = 0; i < KF_CB_N; i++) if (g_pend[i].cb == cb && g_pend[i].gen == gen) return i;
    return -1;
}
int kl_fove_pend_put(void *cb, uint64_t gen, void *tex) {
    if (!cb || !gen) return 0;
    int i = kl_fove_pend_find(cb, gen), f = -1;
    if (i >= 0) { g_pend[i].tex = tex; g_pend[i].depth_only = 0; return 1; }   // slot/map: the caller
    for (int j = 0; j < KF_CB_N; j++) {
        // A slot holding the same pointer under an older generation is stale by
        // definition (that buffer is gone): reuse it.
        if (g_pend[j].cb == cb) { f = j; break; }
        if (!g_pend[j].cb && f < 0) f = j;
    }
    if (f < 0) return 0;
    g_pend[f].cb = cb; g_pend[f].gen = gen; g_pend[f].tex = tex; g_pend[f].depth_only = 0;
    g_pend[f].slot = -1; g_pend[f].map = -1; g_pend[f].hold = 0;
    return 1;
}
void *kl_fove_pend_take(void *cb, uint64_t gen) {
    int i = kl_fove_pend_find(cb, gen);
    if (i < 0) return NULL;
    void *t = g_pend[i].depth_only ? NULL : g_pend[i].tex;
    memset(&g_pend[i], 0, sizeof g_pend[i]);
    return t;
}
int kl_fove_pend_free_slots(void) {
    int n = 0;
    for (int i = 0; i < KF_CB_N; i++) n += !g_pend[i].cb;
    return n;
}

@interface KlFovePendTag : NSObject { @public void *cb; uint64_t gen; }
@end
@implementation KlFovePendTag
- (void)dealloc {
    pthread_mutex_lock(&g_mu);
    void *t = kl_fove_pend_take(cb, gen);
    pthread_mutex_unlock(&g_mu);
    if (t) {
        uint64_t n = __atomic_add_fetch(&g_st.dropped, 1, __ATOMIC_RELAXED);
        if (n <= 4 && !g_pend_quiet) KFI("a command buffer went away with a foveated eye pass but no -commit: its pending "
                        "expand dropped (#%llu)\n", (unsigned long long)n);
    }
#if !__has_feature(objc_arc)
    [super dealloc];
#endif
}
@end
static char g_tag_key;

// The command buffer's generation; 0 when it has none and `make` is 0.
static uint64_t kf_cb_gen(id cb, int make) {
    KlFovePendTag *t = objc_getAssociatedObject(cb, &g_tag_key);
    if (t) return t->gen;
    if (!make) return 0;
    t = [KlFovePendTag new];
    t->cb = (__bridge void *)cb;
    pthread_mutex_lock(&g_mu);
    t->gen = ++g_pend_gen;
    pthread_mutex_unlock(&g_mu);
    objc_setAssociatedObject(cb, &g_tag_key, t, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    uint64_t g = t->gen;
    KF_RELEASE(t);
    return g;
}

static void *kf_pend_take(id cb, int *map) {
    uint64_t gen = kf_cb_gen(cb, 0);
    *map = -1;
    if (!gen) return NULL;
    pthread_mutex_lock(&g_mu);
    int i = kl_fove_pend_find((__bridge void *)cb, gen);
    if (i >= 0) *map = g_pend[i].map;
    void *t = kl_fove_pend_take((__bridge void *)cb, gen);
    pthread_mutex_unlock(&g_mu);
    return t;
}
static int kf_pend_set(id cb, uint64_t gen, void *tex) {
    pthread_mutex_lock(&g_mu);
    int done = kl_fove_pend_put((__bridge void *)cb, gen, tex);
    pthread_mutex_unlock(&g_mu);
    if (!done) { static int said; if (!said++) KFI("pending-expand table full\n"); }
    return done;
}
// t_fove: the tag's dealloc path with plain NSObjects standing in for command
// buffers (no GPU). Returns the number of failed checks.
int kl_fove_pend_selftest(void) {
    int bad = 0;
    g_pend_quiet = 1;
    memset(g_pend, 0, sizeof g_pend);
    @autoreleasepool {
        void *tex = (void *)0x1234;
        for (int round = 0; round < 3 * KF_CB_N; round++) {   // more buffers than slots
            NSObject *cb = [NSObject new];
            uint64_t g = kf_cb_gen(cb, 1);
            if (!g || kf_cb_gen(cb, 1) != g) bad++;
            if (!kf_pend_set(cb, g, tex)) bad++;
            if (kf_cb_gen(cb, 0) != g) bad++;
            KF_RELEASE(cb);                                    // never "committed"
#if __has_feature(objc_arc)
            cb = nil;
#endif
        }
    }
    if (kl_fove_pend_free_slots() != KF_CB_N) bad++;          // every entry dropped with its buffer
    // A same-address buffer under a newer generation never sees the old entry.
    if (!kl_fove_pend_put((void *)0x10, 7, (void *)0x1)) bad++;
    if (kl_fove_pend_find((void *)0x10, 8) >= 0) bad++;
    if (!kl_fove_pend_put((void *)0x10, 8, (void *)0x2)) bad++;
    if (kl_fove_pend_free_slots() != KF_CB_N - 1) bad++;      // the stale slot was reused
    if (kl_fove_pend_take((void *)0x10, 7) != NULL) bad++;
    if (kl_fove_pend_take((void *)0x10, 8) != (void *)0x2) bad++;
    // Full table: put refuses, nothing is overwritten.
    for (int i = 0; i < KF_CB_N; i++) if (!kl_fove_pend_put((void *)(uintptr_t)(0x100 + i), 1, (void *)0x3)) bad++;
    if (kl_fove_pend_put((void *)0x99999, 1, (void *)0x4)) bad++;
    if (kl_fove_pend_free_slots() != 0) bad++;
    memset(g_pend, 0, sizeof g_pend);
    g_pend_quiet = 0;
    return bad;
}

// ---------------------------------------------------------------- sync probe (debug, for the expand fence)
// KL_FOVE_PROBE=1: counts the synchronisation calls MoltenVK makes on its
// render and compute encoders (fences, useResource, memory barriers) and, for the
// first expands, lists the calls made on the encoders that follow the expand —
// i.e. how (whether) the guest's next pass is ordered after our expand.
static int g_probe;
static __thread void *t_probe_eye;     // the eye just expanded on this thread (until the next encoder ends)
static __thread int t_probe_left;
static uint64_t g_pr_wait, g_pr_upd, g_pr_use, g_pr_mb;
static void kf_probe_note(id self, const char *what, id obj, unsigned long a) {
    if (!t_probe_eye || t_probe_left <= 0) return;
    void *o = (__bridge void *)obj;
    int is_eye = o && (o == t_probe_eye || ([obj respondsToSelector:@selector(parentTexture)] &&
                                             (__bridge void *)[(id<MTLTexture>)obj parentTexture] == t_probe_eye));
    KFI("probe: after expand, %s %s %p 0x%lx%s\n", class_getName(object_getClass(self)), what, o, a,
        is_eye ? " (THE EYE)" : "");
}
static void kf_pr_wait2(id self, SEL cmd, id f, NSUInteger st) {
    __atomic_add_fetch(&g_pr_wait, 1, __ATOMIC_RELAXED); kf_probe_note(self, "waitForFence:beforeStages:", f, st);
    ((void (*)(id, SEL, id, NSUInteger))kf_orig(self, cmd))(self, cmd, f, st);
}
static void kf_pr_upd2(id self, SEL cmd, id f, NSUInteger st) {
    __atomic_add_fetch(&g_pr_upd, 1, __ATOMIC_RELAXED); kf_probe_note(self, "updateFence:afterStages:", f, st);
    ((void (*)(id, SEL, id, NSUInteger))kf_orig(self, cmd))(self, cmd, f, st);
}
static void kf_pr_wait1(id self, SEL cmd, id f) {
    __atomic_add_fetch(&g_pr_wait, 1, __ATOMIC_RELAXED); kf_probe_note(self, "waitForFence:", f, 0);
    ((void (*)(id, SEL, id))kf_orig(self, cmd))(self, cmd, f);
}
static void kf_pr_upd1(id self, SEL cmd, id f) {
    __atomic_add_fetch(&g_pr_upd, 1, __ATOMIC_RELAXED); kf_probe_note(self, "updateFence:", f, 0);
    ((void (*)(id, SEL, id))kf_orig(self, cmd))(self, cmd, f);
}
static void kf_pr_use2(id self, SEL cmd, id r, NSUInteger u) {
    __atomic_add_fetch(&g_pr_use, 1, __ATOMIC_RELAXED); kf_probe_note(self, "useResource:usage:", r, u);
    ((void (*)(id, SEL, id, NSUInteger))kf_orig(self, cmd))(self, cmd, r, u);
}
static void kf_pr_use3(id self, SEL cmd, id r, NSUInteger u, NSUInteger st) {
    __atomic_add_fetch(&g_pr_use, 1, __ATOMIC_RELAXED); kf_probe_note(self, "useResource:usage:stages:", r, u | st << 8);
    ((void (*)(id, SEL, id, NSUInteger, NSUInteger))kf_orig(self, cmd))(self, cmd, r, u, st);
}
static void kf_pr_tex(id self, SEL cmd, id t, NSUInteger i) {
    kf_probe_note(self, "setTexture:atIndex:", t, i);
    ((void (*)(id, SEL, id, NSUInteger))kf_orig(self, cmd))(self, cmd, t, i);
}
static void kf_pr_mb3(id self, SEL cmd, NSUInteger sc, NSUInteger a, NSUInteger b) {
    __atomic_add_fetch(&g_pr_mb, 1, __ATOMIC_RELAXED); kf_probe_note(self, "memoryBarrierWithScope:after:before:", nil, sc << 16 | a << 8 | b);
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger))kf_orig(self, cmd))(self, cmd, sc, a, b);
}
static void kf_pr_mb1(id self, SEL cmd, NSUInteger sc) {
    __atomic_add_fetch(&g_pr_mb, 1, __ATOMIC_RELAXED); kf_probe_note(self, "memoryBarrierWithScope:", nil, sc);
    ((void (*)(id, SEL, NSUInteger))kf_orig(self, cmd))(self, cmd, sc);
}
static void kf_pr_disp(id self, SEL cmd, MTLSize a, MTLSize b) {
    kf_probe_note(self, "dispatchThreadgroups", nil, (unsigned long)a.width << 16 | a.height);
    ((void (*)(id, SEL, MTLSize, MTLSize))kf_orig(self, cmd))(self, cmd, a, b);
}
static void kf_pr_end(id self, SEL cmd) {
    if (t_probe_eye && t_probe_left > 0) {
        kf_probe_note(self, "endEncoding", nil, 0);
        t_probe_left--;
        if (!t_probe_left) t_probe_eye = NULL;
    }
    ((void (*)(id, SEL))kf_orig(self, cmd))(self, cmd);
}
static void kf_probe_class(Class c, int compute) {
    if (!g_probe || !c) return;
    kf_swap(c, "waitForFence:beforeStages:", (IMP)kf_pr_wait2);
    kf_swap(c, "updateFence:afterStages:", (IMP)kf_pr_upd2);
    kf_swap(c, "useResource:usage:", (IMP)kf_pr_use2);
    kf_swap(c, "useResource:usage:stages:", (IMP)kf_pr_use3);
    kf_swap(c, "endEncoding", (IMP)kf_pr_end);
    if (compute) {
        kf_swap(c, "waitForFence:", (IMP)kf_pr_wait1);
        kf_swap(c, "updateFence:", (IMP)kf_pr_upd1);
        kf_swap(c, "memoryBarrierWithScope:", (IMP)kf_pr_mb1);
        kf_swap(c, "setTexture:atIndex:", (IMP)kf_pr_tex);
        kf_swap(c, "dispatchThreadgroups:threadsPerThreadgroup:", (IMP)kf_pr_disp);
    } else {
        kf_swap(c, "memoryBarrierWithScope:afterStages:beforeStages:", (IMP)kf_pr_mb3);
        kf_swap(c, "setFragmentTexture:atIndex:", (IMP)kf_pr_tex);
    }
    KFI("probe: %s encoder class %s hooked\n", compute ? "compute" : "render", class_getName(c));
}

// g_mu held: does cb's pending entry ask for something before an encoder of
// this kind? what 0: a render/compute encoder — every entry but a HELD one
// (an in-pass intermediate whose eye subpass has not run yet); 1: a blit
// encoder — only an intermediate the guest copies (or may copy) into the eye.
static int kf_pend_due_locked(void *cb, uint64_t gen, int what) {
    int i = kl_fove_pend_find(cb, gen);
    if (i < 0) return 0;
    if (what == 0) return !(g_pend[i].hold && !g_pend[i].tex);
    int s = g_pend[i].slot;
    return g_pend[i].tex && s >= 0 && s < KF_EYE_N &&
           (g_eye[s].via == KL_FOVE_VIA_XFER || g_eye[s].via == KL_FOVE_VIA_SAMPLED);
}

static void kf_flush_at(id cb, int at_commit) {
    if (!at_commit) {
        uint64_t gen = kf_cb_gen(cb, 0);
        if (!gen) return;
        pthread_mutex_lock(&g_mu);
        int due = kf_pend_due_locked((__bridge void *)cb, gen, 0);
        pthread_mutex_unlock(&g_mu);
        if (!due) return;
    }
    int mi;
    void *t = kf_pend_take(cb, &mi);
    if (t) {
        __atomic_add_fetch(at_commit ? &g_xf_at_commit : &g_xf_at_enc, 1, __ATOMIC_RELAXED);
        kf_expand(cb, (__bridge id<MTLTexture>)t, mi);
        static int shown;
        if (g_probe && shown < 4) { shown++; t_probe_eye = t; t_probe_left = 2; }
    }
}
static void kf_flush(id cb) { kf_flush_at(cb, 0); }

// The slot a COMPUTE encoder's open
// just expanded on this thread's command buffer. MoltenVK splits a multiview
// render pass around every indirect draw (a compute encoder adjusts the
// instance count, then the pass resumes with LOAD); the compute open flushes
// the expand mid-pass, so the resumed encoder LOADs an expanded colour over a
// depth still in physical layout (Paint the Town Red's combat: the world went black / see-
// through). kl_vulkan refuses such passes at record time; this only counts it.
static __thread void *t_cf_cb;
static __thread uint64_t t_cf_gen;
static __thread int t_cf_slot = -1;
static void kf_compute_flush(id cb) {
    if (t_in) return;
    uint64_t gen = kf_cb_gen(cb, 0);
    int slot = -1;
    if (gen) {
        pthread_mutex_lock(&g_mu);
        int i = kl_fove_pend_find((__bridge void *)cb, gen);
        if (i >= 0 && kf_pend_due_locked((__bridge void *)cb, gen, 0)) slot = g_pend[i].slot;
        pthread_mutex_unlock(&g_mu);
    }
    kf_flush(cb);
    if (slot >= 0) { t_cf_cb = (__bridge void *)cb; t_cf_gen = gen; t_cf_slot = slot; }
}

// ---------------------------------------------------------------- encoder hooks
static void *g_enc_cls;               // the render-encoder class whose fragment-buffer setters are watched
static void kf_hook_encoder(id en);

// Which eye a render pass draws into: the index in g_eye or -1. g_mu held.
// *is_out: the colour is the eye image a later subpass of an in-pass
// intermediate writes (checked FIRST: the same eye image may also be a plain
// eye target of another pass, and inside the intermediate's render pass it
// must join the intermediate's pending entry, not flush it).
static int kf_match_locked(MTLRenderPassDescriptor *d, id<MTLTexture> *col, int *is_out) {
    id<MTLTexture> c = d.colorAttachments[0].texture, dt = d.depthAttachment.texture;
    void *ck = (__bridge void *)c, *cp = c ? (__bridge void *)c.parentTexture : NULL;
    void *dk = (__bridge void *)dt, *dp = dt ? (__bridge void *)dt.parentTexture : NULL;
    *is_out = 0;
    if (ck)
        for (int i = 0; i < KF_EYE_N; i++) {
            if (!g_eye[i].col || g_eye[i].via != KL_FOVE_VIA_SUBPASS) continue;
            for (int o = 0; o < KF_OUT_N; o++)
                if (g_eye[i].out[o] && (ck == g_eye[i].out[o] || cp == g_eye[i].out[o])) {
                    *col = c; *is_out = 1;
                    return i;
                }
        }
    for (int i = 0; i < KF_EYE_N; i++) {
        if (!g_eye[i].col) continue;
        // The colour attachment decides; the depth alone only for a pass with
        // no colour at all (a pass drawing elsewhere with the eye depth bound
        // — Valve's composite does that — must stay unmapped).
        if ((ck && (ck == g_eye[i].col || cp == g_eye[i].col)) ||
            (!ck && dk && g_eye[i].dep && (dk == g_eye[i].dep || dp == g_eye[i].dep))) {
            *col = c && (ck == g_eye[i].col || cp == g_eye[i].col) ? c : nil;
            return i;
        }
    }
    return -1;
}

// g_mu held: the pending entry of (cb, gen) records eye slot `slot` drawn with
// map `mi`. tex NULL never replaces a texture already pending (a pass that
// leaves nothing to rewrite must not cancel an earlier pass's expand).
static int kf_pend_mark_locked(void *cb, uint64_t gen, void *tex, int slot, int mi, int depth_only) {
    int i = kl_fove_pend_find(cb, gen);
    void *keep = i >= 0 ? g_pend[i].tex : NULL;
    int keep_do = i >= 0 ? g_pend[i].depth_only : 1;
    if (!kl_fove_pend_put(cb, gen, tex ? tex : keep)) return 0;
    i = kl_fove_pend_find(cb, gen);
    if (i < 0) return 0;
    g_pend[i].slot = slot;
    g_pend[i].map = mi;
    // depth_only only while nothing but depth-only passes have been mapped.
    g_pend[i].depth_only = depth_only && keep_do && !keep;
    return 1;
}

// KL_FOVE_TRACE_ENC=<n> (debug): the first n render-pass descriptors MoltenVK
// opens after the first eye target is registered, attachment by attachment.
static void kf_trace_enc(MTLRenderPassDescriptor *d) {
    static int left = -1;
    if (left < 0) left = kl_env_int("KL_FOVE_TRACE_ENC", 0);
    int any = 0;
    for (int i = 0; i < KF_EYE_N; i++) any |= g_eye[i].col != NULL;
    if (!left || !any) return;
    left--;
    id<MTLTexture> c = d.colorAttachments[0].texture, r = d.colorAttachments[0].resolveTexture, z = d.depthAttachment.texture;
    KFI("enc: colour %p (parent %p) %lux%lu type %lu x%lu slice %lu load %lu store %lu | resolve %p (parent %p) type %lu "
        "slice %lu | depth %p type %lu x%lu load %lu store %lu resolve %p | rtal %lu\n",
        (__bridge void *)c, c ? (__bridge void *)c.parentTexture : NULL, (unsigned long)c.width, (unsigned long)c.height,
        (unsigned long)c.textureType, (unsigned long)c.sampleCount, (unsigned long)d.colorAttachments[0].slice,
        (unsigned long)d.colorAttachments[0].loadAction, (unsigned long)d.colorAttachments[0].storeAction,
        (__bridge void *)r, r ? (__bridge void *)r.parentTexture : NULL, (unsigned long)r.textureType,
        (unsigned long)d.colorAttachments[0].resolveSlice, (__bridge void *)z, (unsigned long)z.textureType,
        (unsigned long)z.sampleCount, (unsigned long)d.depthAttachment.loadAction, (unsigned long)d.depthAttachment.storeAction,
        (__bridge void *)d.depthAttachment.resolveTexture, (unsigned long)d.renderTargetArrayLength);
}

static id kf_render(id self, SEL cmd, MTLRenderPassDescriptor *d) {
    IMP o = kf_orig(self, cmd);
    if (t_in || !d) return ((id (*)(id, SEL, id))o)(self, cmd, d);
    kf_trace_enc(d);
    int on = __atomic_load_n(&g_on, __ATOMIC_ACQUIRE) == 1;
    id<MTLTexture> col = nil;
    int eye = -1, mi = -1;
    void *exp_tex = NULL;                 // what this pass leaves to expand: the colour, or what it resolves into
    int depth_only = 0;                   // mapped, nothing of the colour to expand (yet)
    id<MTLBuffer> mb = nil;
    id<MTLRasterizationRateMap> map = nil;
    int late = 0, full = 0, split = 0, ms_layered = 0, kept_ms = 0;
    int is_out = 0, via = 0, lost = 0;   // the eye image an in-pass intermediate writes / the slot's via
    int ms_tr = 0;                        // 1 a transient MSAA scene encoder held, 2 its resolving encoder
    int cf_resume = 0;                    // a pass resumed after a compute encoder flushed its expand
    void *cf_col = NULL;                  // that intermediate's colour, refused below
    // This command buffer's generation (made now, outside g_mu: the tag's
    // creation takes the lock) — only when the pass might be marked.
    uint64_t gen = on && !d.rasterizationRateMap ? kf_cb_gen(self, 1) : 0;
    if (on && !d.rasterizationRateMap) {
        pthread_mutex_lock(&g_mu);
        eye = kf_match_locked(d, &col, &is_out);
        via = eye >= 0 ? g_eye[eye].via : 0;
        // An in-pass intermediate's eye subpass with the scene pending in
        // physical layout on this buffer MUST be mapped; if any rule below
        // leaves it unmapped the picture is broken (counted, logged).
        int out_owed = 0;
        if (is_out) {
            int pi = kl_fove_pend_find((__bridge void *)self, gen);
            out_owed = pi >= 0 && g_pend[pi].slot == eye;
        }
        // Likewise a later encoder of a transient MSAA intermediate
        // whose samples are HELD in physical layout on this buffer (its LOAD
        // must be mapped, or it draws screen layout over physical).
        if (eye >= 0 && !is_out && g_eye[eye].ms_tr) {
            int pi = kl_fove_pend_find((__bridge void *)self, gen);
            if (pi >= 0 && g_pend[pi].slot == eye && g_pend[pi].hold && !g_pend[pi].tex) out_owed = 1;
        }
        // A pass that LOADs the eye is mapped only while the eye is still in
        // physical layout, i.e. a foveated pass of the SAME eye target is
        // pending on THIS command buffer; one that LOADs it after the expand
        // (or in another command buffer) would read screen-layout pixels as
        // physical — it draws unfoveated instead (counted).
        if (eye >= 0) {
            id<MTLTexture> dt = d.depthAttachment.texture;
            void *dk = (__bridge void *)dt, *dp = dt ? (__bridge void *)dt.parentTexture : NULL;
            int eye_depth = dk && g_eye[eye].dep && (dk == g_eye[eye].dep || dp == g_eye[eye].dep);
            int cl = col && d.colorAttachments[0].loadAction == MTLLoadActionLoad;
            int dl = eye_depth && d.depthAttachment.loadAction == MTLLoadActionLoad;
            if (cl || dl) {
                int pi = kl_fove_pend_find((__bridge void *)self, gen);
                int same = pi >= 0 && g_pend[pi].slot == eye;
                // A colour LOAD needs foveated colour pending, not just a mapped
                // depth-only pass (MSAA tiler-0 clear); a depth LOAD any pass of
                // this eye. Single-sample eyes only ever leave colour entries.
                int pending = same && (!cl || !g_pend[pi].depth_only);
                if (!pending) {
                    if (t_cf_slot == eye && t_cf_cb == (__bridge void *)self && t_cf_gen == gen) {
                        cf_resume = 1;
                        if (g_eye[eye].via) cf_col = g_eye[eye].col;   // an intermediate, not a runtime eye
                    }
                    eye = -1;
                    late = 1;
                }
            }
        }
        // Which map: DOUBLE one layer; a 2-layer eye pair only as a LAYERED pass
        // (renderTargetArrayLength 2: both slices at once, map layer = slice);
        // a pass over one slice of the pair is left unfoveated (counted: the
        // per-slice pending bookkeeping is not built); a one-eye image its eye.
        int kind = KF_DOUBLE, layers = 1, ekey = -1;
        if (eye >= 0 && g_eye[eye].kind == KF_ARRAY) {
            kind = KF_ARRAY;
            if (g_eye[eye].layers >= 2) {
                if (d.renderTargetArrayLength >= 2) layers = 2;
                else { split = 1; eye = -1; }
            } else ekey = g_eye[eye].eye == 1 ? 1 : 0;
        }
        // What the pass leaves in physical layout. A single-sample colour: the
        // colour. MSAA colour resolved in the pass: the single-sample image it
        // resolves into (rewritten like any single-sample eye); MSAA stored with
        // no resolve: the MSAA colour (the MSAA expand, one slice only); MSAA
        // neither stored nor resolved: nothing.
        if (eye >= 0 && col && is_out) {
            exp_tex = (__bridge void *)col;          // the eye image itself (single-sample)
        } else if (eye >= 0 && col && via == KL_FOVE_VIA_SUBPASS) {
            // The scene target of an in-pass intermediate: transient (the
            // guest's storeOp is DONT_CARE, kl_vulkan checks), nothing to
            // expand — unless this subpass resolves into the eye image.
            id<MTLTexture> rt = d.colorAttachments[0].resolveTexture;
            void *rk = (__bridge void *)rt, *rp = rt ? (__bridge void *)rt.parentTexture : NULL;
            for (int o = 0; rt && o < KF_OUT_N; o++)
                if (g_eye[eye].out[o] && (rk == g_eye[eye].out[o] || rp == g_eye[eye].out[o])) exp_tex = rk;
        } else if (eye >= 0 && col) {
            if (g_eye[eye].samples > 1) {
                MTLStoreAction sa = d.colorAttachments[0].storeAction;
                id<MTLTexture> rt = d.colorAttachments[0].resolveTexture;
                // MoltenVK opens a resolving pass with storeAction Unknown and
                // the resolve texture set, and picks the real action at
                // endEncoding (seen on GORN 2's eye pass): a resolve texture
                // with Unknown resolves.
                int resolves = rt && (sa == MTLStoreActionMultisampleResolve ||
                                      sa == MTLStoreActionStoreAndMultisampleResolve ||
                                      sa == MTLStoreActionUnknown);
                int stores = sa == MTLStoreActionStore || sa == MTLStoreActionStoreAndMultisampleResolve ||
                             (sa == MTLStoreActionUnknown && !rt);
                if (resolves) {
                    exp_tex = (__bridge void *)rt; kept_ms = stores;
                    if (g_eye[eye].ms_tr) ms_tr = 2;
                } else if (stores) {
                    // A transient intermediate's samples die with its
                    // render pass (the guest's storeOp is DONT_CARE; MoltenVK
                    // stores only to carry them into the next subpass encoder,
                    // which resolves them): mapped, held, nothing to expand yet.
                    if (layers == 2 && g_eye[eye].ms_tr) ms_tr = 1;
                    else if (layers == 2) { ms_layered = 1; eye = -1; }
                    else exp_tex = (__bridge void *)col;
                } else depth_only = 0;   // nothing kept: pending for the LOAD rule only
            } else exp_tex = (__bridge void *)col;
        }
        // A mapped colour pass needs a slot to record its expand in.
        if (eye >= 0 && col && kl_fove_pend_find((__bridge void *)self, gen) < 0 && !kl_fove_pend_free_slots()) {
            eye = -1;
            full = 1;
        }
        int pe = eye >= 0 ? kl_fove_pend_find((__bridge void *)self, gen) : -1;
        kf_map_t *mp = eye >= 0 ? kf_map_for_cb(g_eye[eye].w, g_eye[eye].h, kind, layers, ekey,
                                                pe >= 0 ? g_pend[pe].map : -1) : NULL;
        if (mp) {
            map = mp->map; mb = mp->buf; mi = (int)(mp - g_maps);
            // MSAA (tiler mode off): the frame opens with a DEPTH-ONLY clear of
            // the eye, then the main pass LOADs that depth. The depth is in
            // physical layout from here on: a depth_only pending entry lets the
            // main pass's depth LOAD be mapped; nothing is expanded until a
            // mapped pass has drawn the colour.
            if (!col && g_eye[eye].samples > 1) depth_only = 1;
        } else eye = -1;
        if (out_owed && eye < 0) lost = 1;
        pthread_mutex_unlock(&g_mu);
    }
    t_cf_slot = -1; t_cf_cb = NULL;       // only the encoder right after the compute one counts
    if (cf_resume) {
        uint64_t n = __atomic_add_fetch(&g_st.compute_split, 1, __ATOMIC_RELAXED);
        if (n <= 4)
            KFI("SPLIT PASS: a render encoder LOADs an eye/intermediate (colour load %lu, depth load %lu) whose expand "
                "a COMPUTE encoder on the same command buffer just ran — MoltenVK resuming a multiview pass it split "
                "for an indirect draw; its depth is in PHYSICAL layout, so this half draws wrong (#%llu). kl_vulkan "
                "refuses an intermediate with an indirect draw in its pass; KL_FOVE_INTERMEDIATE=0 is the fallback\n",
                (unsigned long)d.colorAttachments[0].loadAction, (unsigned long)d.depthAttachment.loadAction,
                (unsigned long long)n);
        // The Vulkan-side refusal can miss the
        // indirect draw (Unity records it in a SECONDARY command buffer); the
        // split itself is the proof, so refuse the intermediate here. This
        // frame is already drawn wrong; from the next one it is unfoveated.
        if (cf_col) kl_fove_split_refuse(cf_col);
    }
    if (lost) {
        uint64_t n = __atomic_add_fetch(&g_st.inter_lost, 1, __ATOMIC_RELAXED);
        if (n <= 4)
            KFI("BROKEN FRAME: the eye subpass (or the LOAD of held MSAA samples) of a foveated intermediate was left UNMAPPED (late %d, one slice %d, "
                "table full %d; array length %lu) while its scene is in physical layout (#%llu) — "
                "KL_FOVE_INTERMEDIATE=0 turns the intermediate path off\n", late, split, full,
                (unsigned long)d.renderTargetArrayLength, (unsigned long long)n);
    }
    if (full) {
        uint64_t n = __atomic_add_fetch(&g_st.full, 1, __ATOMIC_RELAXED);
        if (n <= 4) KFI("pending-expand table full (%d command buffers with a foveated eye pending): this "
                        "pass drawn UNFOVEATED (#%llu)\n", KF_CB_N, (unsigned long long)n);
    }
    if (late) {
        uint64_t n = __atomic_add_fetch(&g_st.late, 1, __ATOMIC_RELAXED);
        if (n <= 4)
            KFI("a pass LOADs the eye with no foveated content pending on its command buffer (colour load %lu, "
                "depth load %lu): drawn UNFOVEATED (#%llu)\n", (unsigned long)d.colorAttachments[0].loadAction,
                (unsigned long)d.depthAttachment.loadAction, (unsigned long long)n);
    }
    if (split) {
        uint64_t n = __atomic_add_fetch(&g_st.split, 1, __ATOMIC_RELAXED);
        if (n <= 4)
            KFI("a pass draws ONE slice (%lu) of a 2-layer eye pair (renderTargetArrayLength %lu): drawn UNFOVEATED "
                "(#%llu) — only layered passes are foveated\n", (unsigned long)d.colorAttachments[0].slice,
                (unsigned long)d.renderTargetArrayLength, (unsigned long long)n);
    }
    if (ms_layered) {
        uint64_t n = __atomic_add_fetch(&g_st.ms_layer_refused, 1, __ATOMIC_RELAXED);
        if (n <= 4)
            KFI("a layered MSAA eye pass keeps its samples with no resolve: drawn UNFOVEATED (#%llu) — the "
                "per-sample expand is one-slice only\n", (unsigned long long)n);
    }
    if (eye < 0) {
        kf_flush(self);
        id en = ((id (*)(id, SEL, id))o)(self, cmd, d);
        if (en) {
            kf_hook_encoder(en);
            t_in = 1;
            [(id<MTLRenderCommandEncoder>)en setFragmentBuffer:g_ident offset:0 atIndex:(NSUInteger)g_idx];
            t_in = 0;
            __atomic_add_fetch(&g_st.ident_enc, 1, __ATOMIC_RELAXED);
            kf_xf_wait(self, en, 0);
        }
        return en;
    }
    // A pending expand for ANOTHER eye target is due before this one starts.
    {
        int other = 0;
        pthread_mutex_lock(&g_mu);
        int pi = kl_fove_pend_find((__bridge void *)self, gen);
        if (pi >= 0 && g_pend[pi].slot != eye) other = 1;
        pthread_mutex_unlock(&g_mu);
        if (other) kf_flush(self);
    }
    MTLRenderPassDescriptor *c = [d copy];
    c.rasterizationRateMap = map;
    id en = ((id (*)(id, SEL, id))o)(self, cmd, c);
    uint64_t n = __atomic_add_fetch(&g_st.marked_enc, 1, __ATOMIC_RELAXED);
    if (d.renderTargetArrayLength >= 2) __atomic_add_fetch(&g_st.layered_enc, 1, __ATOMIC_RELAXED);
    if (via) {
        uint64_t ni = __atomic_add_fetch(is_out ? &g_st.inter_out_enc : &g_st.inter_enc, 1, __ATOMIC_RELAXED);
        if (ni == 1)
            KFI("intermediate: first marked %s encoder (%s): colour %p %lux%lu x%lu, array length %lu, map %d\n",
                is_out ? "EYE-SUBPASS" : "scene", via == KL_FOVE_VIA_SUBPASS ? "via subpass" :
                via == KL_FOVE_VIA_XFER ? "via copy/blit" : "via sampled", (__bridge void *)col,
                (unsigned long)col.width, (unsigned long)col.height, (unsigned long)col.sampleCount,
                (unsigned long)d.renderTargetArrayLength, mi);
    }
    if (exp_tex && col && exp_tex != (__bridge void *)col) __atomic_add_fetch(&g_st.res_expands, 1, __ATOMIC_RELAXED);
    if (ms_tr) {
        uint64_t nt = __atomic_add_fetch(ms_tr == 1 ? &g_st.ms_tr_held : &g_st.ms_tr_resolved, 1, __ATOMIC_RELAXED);
        if (nt == 1)
            KFI("transient MSAA intermediate: first %s encoder (colour %p %lux%lu x%lu, array length %lu, load %lu, "
                "map %d)%s\n", ms_tr == 1 ? "HELD (samples stored between subpasses, left in physical layout)"
                : "RESOLVING (its x1 resolve target is what gets expanded)", (__bridge void *)col,
                (unsigned long)col.width, (unsigned long)col.height, (unsigned long)col.sampleCount,
                (unsigned long)d.renderTargetArrayLength, (unsigned long)d.colorAttachments[0].loadAction, mi,
                ms_tr == 2 ? "" : "");
    }
    if (n <= 6)
        KFI("marked encoder #%llu: colour %s %lux%lu x%lu load %lu store %lu%s, depth %s load %lu store %lu, "
            "array length %lu slice %lu -> rate map set (map %d)%s\n",
            (unsigned long long)n, col ? "eye" : "-", (unsigned long)d.colorAttachments[0].texture.width,
            (unsigned long)d.colorAttachments[0].texture.height, (unsigned long)d.colorAttachments[0].texture.sampleCount,
            (unsigned long)d.colorAttachments[0].loadAction, (unsigned long)d.colorAttachments[0].storeAction,
            d.colorAttachments[0].resolveTexture ? " + resolve" : "", d.depthAttachment.texture ? "eye" : "-",
            (unsigned long)d.depthAttachment.loadAction, (unsigned long)d.depthAttachment.storeAction,
            (unsigned long)d.renderTargetArrayLength, (unsigned long)d.colorAttachments[0].slice, mi,
            kept_ms ? " (MSAA samples also stored: left in physical layout)" : "");
    KF_RELEASE(c);
    if (en) {
        kf_hook_encoder(en);
        kf_xf_wait(self, en, 0);
        t_in = 1;
        [(id<MTLRenderCommandEncoder>)en setFragmentBuffer:mb offset:0 atIndex:(NSUInteger)g_idx];
        t_in = 0;
        // Remember the encoder as marked, for the clobber check.
        objc_setAssociatedObject(en, &g_enc_cls, (id)kCFBooleanTrue, OBJC_ASSOCIATION_ASSIGN);
        if (col || depth_only) {
            pthread_mutex_lock(&g_mu);
            int ok = kf_pend_mark_locked((__bridge void *)self, gen, exp_tex, eye, mi, !col);
            if (ok) {
                int pi = kl_fove_pend_find((__bridge void *)self, gen);
                if (pi >= 0)
                    g_pend[pi].hold = (via == KL_FOVE_VIA_SUBPASS || g_eye[eye].ms_tr) && !g_pend[pi].tex;
            }
            pthread_mutex_unlock(&g_mu);
            if (!ok && col) { static int said; if (!said++) KFI("pending-expand table full\n"); }
        }
    }
    return en;
}

static id kf_c0(id self, SEL cmd) {
    if (!t_in) kf_compute_flush(self);
    id en = ((id (*)(id, SEL))kf_orig(self, cmd))(self, cmd);
    kf_xf_wait(self, en, 1);
    return en;
}
static id kf_cdt(id self, SEL cmd, NSUInteger t) {
    if (!t_in) kf_compute_flush(self);
    id en = ((id (*)(id, SEL, NSUInteger))kf_orig(self, cmd))(self, cmd, t);
    kf_xf_wait(self, en, 1);
    return en;
}
static id kf_cdesc(id self, SEL cmd, id d) {
    if (!t_in) kf_compute_flush(self);
    id en = ((id (*)(id, SEL, id))kf_orig(self, cmd))(self, cmd, d);
    kf_xf_wait(self, en, 1);
    return en;
}
// Blit encoders never trigger the expand of an eye image (kl_vulkan's single-sample depth-resolve copy
// sits between the eye clear and the main pass), but one opened after an
// expand waits for it. A foveated guest INTERMEDIATE (via copy/blit
// or sampled) is expanded first: the blit may be its copy into the eye.
static void kf_blit_flush(id cb) {
    if (t_in || !g_inter) return;
    uint64_t gen = kf_cb_gen(cb, 0);
    if (!gen) return;
    pthread_mutex_lock(&g_mu);
    int due = kf_pend_due_locked((__bridge void *)cb, gen, 1);
    pthread_mutex_unlock(&g_mu);
    if (!due) return;
    uint64_t n = __atomic_add_fetch(&g_st.blit_flush, 1, __ATOMIC_RELAXED);
    if (n == 1) KFI("intermediate: expanded before a blit encoder (its copy into the eye) — first time\n");
    kf_flush(cb);
}
static id kf_b0(id self, SEL cmd) {
    kf_blit_flush(self);
    id en = ((id (*)(id, SEL))kf_orig(self, cmd))(self, cmd);
    kf_xf_wait(self, en, 2);
    return en;
}
static id kf_bdesc(id self, SEL cmd, id d) {
    kf_blit_flush(self);
    id en = ((id (*)(id, SEL, id))kf_orig(self, cmd))(self, cmd, d);
    kf_xf_wait(self, en, 2);
    return en;
}
// The [fove] stats line is written by kl_fove_perf_field (every 10th call).
// With nothing else asking for the field, the commit path drives it once a
// second, so a run's log carries the counters on its own. One thread at a
// time: the due time is claimed with a compare-and-swap.
static void kf_tick(void) {
    static uint64_t due;
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC), d = __atomic_load_n(&due, __ATOMIC_RELAXED);
    if (now < d || !__atomic_compare_exchange_n(&due, &d, now + 1000000000ull, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;
    char field[200];
    kl_fove_perf_field(field, sizeof field);
}
static void kf_commit(id self, SEL cmd) {
    kf_flush_at(self, 1);
    ((void (*)(id, SEL))kf_orig(self, cmd))(self, cmd);
    kf_xf_committed(self);
    kf_tick();
}

// The clobber check: anyone but us binding fragment buffer slot N.
static void kf_clobbered(id en, NSUInteger idx) {
    if (t_in || idx != (NSUInteger)g_idx) return;
    uint64_t n = __atomic_add_fetch(&g_st.clobber, 1, __ATOMIC_RELAXED);
    int marked = objc_getAssociatedObject(en, &g_enc_cls) != nil;
    if (marked) __atomic_add_fetch(&g_st.clobber_marked, 1, __ATOMIC_RELAXED);
    if (n <= 4) KFI("fragment buffer slot %d bound by someone else (%s encoder)\n", g_idx, marked ? "a MARKED" : "an unmarked");
    if (marked) kf_turn_off("the rate-map buffer slot was rebound inside a marked pass");
}
static void kf_sfb(id self, SEL cmd, id b, NSUInteger off, NSUInteger idx) {
    kf_clobbered(self, idx);
    ((void (*)(id, SEL, id, NSUInteger, NSUInteger))kf_orig(self, cmd))(self, cmd, b, off, idx);
}
static void kf_sfbytes(id self, SEL cmd, const void *p, NSUInteger len, NSUInteger idx) {
    kf_clobbered(self, idx);
    ((void (*)(id, SEL, const void *, NSUInteger, NSUInteger))kf_orig(self, cmd))(self, cmd, p, len, idx);
}
static void kf_sfbo(id self, SEL cmd, NSUInteger off, NSUInteger idx) {
    kf_clobbered(self, idx);
    ((void (*)(id, SEL, NSUInteger, NSUInteger))kf_orig(self, cmd))(self, cmd, off, idx);
}
static void kf_sfbs(id self, SEL cmd, const id *b, const NSUInteger *offs, NSRange r) {
    if ((NSUInteger)g_idx >= r.location && (NSUInteger)g_idx < r.location + r.length) kf_clobbered(self, (NSUInteger)g_idx);
    ((void (*)(id, SEL, const id *, const NSUInteger *, NSRange))kf_orig(self, cmd))(self, cmd, b, offs, r);
}

static void kf_hook_encoder(id en) {
    Class c = object_getClass(en);
    void *ck = (__bridge void *)c;
    if (__atomic_load_n(&g_enc_cls, __ATOMIC_ACQUIRE) == ck) return;
    pthread_mutex_lock(&g_mu);
    if (g_enc_cls != ck) {
        kf_swap(c, "setFragmentBuffer:offset:atIndex:", (IMP)kf_sfb);
        kf_swap(c, "setFragmentBytes:length:atIndex:", (IMP)kf_sfbytes);
        kf_swap(c, "setFragmentBufferOffset:atIndex:", (IMP)kf_sfbo);
        kf_swap(c, "setFragmentBuffers:offsets:withRange:", (IMP)kf_sfbs);
        if (g_enc_cls) KFI("another render-encoder class %s: its fragment-buffer slot %d is watched too\n",
                           class_getName(c), g_idx);
        else KFI("render encoder class %s: fragment-buffer slot %d watched\n", class_getName(c), g_idx);
        kf_probe_class(c, 0);
        __atomic_store_n(&g_enc_cls, ck, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_mu);
}

// ---------------------------------------------------------------- install
static void kf_wrap_cb_class(Class c) {
    if (!c) return;
    kf_swap(c, "renderCommandEncoderWithDescriptor:", (IMP)kf_render);
    kf_swap(c, "computeCommandEncoder", (IMP)kf_c0);
    kf_swap(c, "computeCommandEncoderWithDispatchType:", (IMP)kf_cdt);
    kf_swap(c, "computeCommandEncoderWithDescriptor:", (IMP)kf_cdesc);
    kf_swap(c, "commit", (IMP)kf_commit);
    if (g_fence_on || g_inter) {
        kf_swap(c, "blitCommandEncoder", (IMP)kf_b0);
        kf_swap(c, "blitCommandEncoderWithDescriptor:", (IMP)kf_bdesc);
    }
}

void kl_fove_install(void *mtl_device) {
    static int done;
    if (done || !mtl_device || !kl_fove_on()) return;
    done = 1;
    g_dev = (__bridge id<MTLDevice>)mtl_device;
    s_render = sel_registerName("renderCommandEncoderWithDescriptor:");
    if (![g_dev supportsRasterizationRateMapWithLayerCount:1]) {
        kf_turn_off("this GPU has no rasterization rate maps (supportsRasterizationRateMapWithLayerCount:1 = NO)");
        return;
    }
    if (![g_dev supportsRasterizationRateMapWithLayerCount:2])
        KFI("this GPU has no 2-layer rasterization rate maps: 2-layer eye pairs stay unfoveated\n");
    // The identity has two layers (every layer index a patched shader can
    // pass is in range; one layer where two are not supported).
    float one[2] = {1, 1};
    const float *o2[2] = { one, one };
    int il = [g_dev supportsRasterizationRateMapWithLayerCount:2] ? 2 : 1;
    id<MTLRasterizationRateMap> im = kf_make_map_n(16384, 16384, il, 2, o2, 2, o2);
    g_ident = im ? kf_params(im) : nil;
    if (!g_ident) { kf_turn_off("could not build the identity rate map"); return; }
    @autoreleasepool {
        Class dc = object_getClass((id)g_dev);
        kf_swap(dc, "newLibraryWithSource:options:error:", (IMP)kf_lib_err);
        kf_swap(dc, "newLibraryWithSource:options:completionHandler:", (IMP)kf_lib_cb);
        kf_swap(dc, "newLibraryWithData:error:", (IMP)kf_lib_data);
        id<MTLCommandQueue> q = [g_dev newCommandQueue];
        id a = [q commandBuffer], b = [q commandBufferWithUnretainedReferences];
        id c = nil;
        Class dsc = objc_getClass("MTLCommandBufferDescriptor");
        if (dsc) { id d = [dsc new]; c = [q commandBufferWithDescriptor:d]; KF_RELEASE(d); }
        if (g_fence_on) {
            g_xfence = [g_dev newFence];
            g_xfence.label = @"kl-fove-expand";
            g_xfence_l = [g_dev newFence];
            g_xfence_l.label = @"kl-fove-expand-slice0";
        }
        kf_wrap_cb_class(object_getClass(a));
        g_probe = kl_env_on("KL_FOVE_PROBE", 0);
        if (g_probe) {
            id<MTLCommandBuffer> pc = [q commandBuffer];
            t_in = 1;
            id<MTLComputeCommandEncoder> ce = [pc computeCommandEncoder];
            t_in = 0;
            kf_probe_class(object_getClass(ce), 1);
            [ce endEncoding];
        }
        if (object_getClass(b) != object_getClass(a)) kf_wrap_cb_class(object_getClass(b));
        if (c && object_getClass(c) != object_getClass(a) && object_getClass(c) != object_getClass(b))
            kf_wrap_cb_class(object_getClass(c));
        KFI("ON — level %s (box %.3f H, ramp %.3f H, floor %.3f); device %s, command buffer %s; "
            "[[position]] patch %s at fragment buffer(%d); expand %s%s; MSAA eyes %s%s%s\n",
            g_level_name, g_box, g_ramp, g_floor, class_getName(dc), class_getName(object_getClass(a)),
            g_patch_fc == 1 ? "ON" : g_patch_fc == 2 ? "ON + debug grid" : g_patch_fc == 3 ? "OFF + debug grid (gridraw)"
                : "OFF (KL_FOVE_FRAGCOORD=0, debug)", g_idx,
            g_expand ? "ON" : "OFF (KL_FOVE_EXPAND=0, debug)",
            !g_expand ? "" : g_xfence ? " (fenced: the guest's later passes wait for it)" : " (UNFENCED, KL_FOVE_FENCE=0)",
            g_msaa ? "FOVEATED" : "UNFOVEATED (KL_FOVE_MSAA=0)",
            g_dump ? "; MSL dumped to " : "",
            g_dump ? g_dump : "");
        KF_RELEASE(q);
    }
}

int kl_fove_eye_extent(uint32_t w, uint32_t h) {
    if (!kl_fove_on()) return 0;
    uint32_t ew = 0, eh = 0;
    if (!kf_double_eye_geometry(&ew, &eh, NULL)) return 0;
    return w == ew && h == eh;
}

int kl_fove_msaa(void) { return kl_fove_on() && g_msaa; }

void kl_fove_note_eye(void *col, void *dep, uint32_t w, uint32_t h, uint32_t samples) {
    if (!col || !kl_fove_on()) return;
    pthread_mutex_lock(&g_mu);
    int slot = -1;
    for (int i = 0; i < KF_EYE_N; i++) if (g_eye[i].col == col) { slot = i; break; }
    int fresh = slot < 0;
    if (slot < 0) for (int i = 0; i < KF_EYE_N; i++) if (!g_eye[i].col) { slot = i; break; }
    if (slot < 0) slot = 0;
    g_eye[slot].col = col; g_eye[slot].dep = dep; g_eye[slot].w = w; g_eye[slot].h = h;
    g_eye[slot].samples = samples > 1 ? samples : 1;
    g_eye[slot].kind = KF_DOUBLE; g_eye[slot].layers = 1; g_eye[slot].eye = -1;
    g_eye[slot].via = 0; memset(g_eye[slot].out, 0, sizeof g_eye[slot].out);
    pthread_mutex_unlock(&g_mu);
    if (fresh) {
        if (samples > 1)
            KFI("eye target registered: colour %p depth %p %ux%u x%u MSAA (slot %d)\n", col, dep, w, h, samples, slot);
        else
            KFI("eye target registered: colour %p depth %p %ux%u (slot %d)\n", col, dep, w, h, slot);
    }
}

void kl_fove_note_eye_array(void *col, void *dep, uint32_t w, uint32_t h, uint32_t samples,
                            uint32_t layers, int eye) {
    if (!col || !kl_fove_on()) return;
    if (layers >= 2 && ![g_dev supportsRasterizationRateMapWithLayerCount:2]) return;
    pthread_mutex_lock(&g_mu);
    int slot = -1;
    for (int i = 0; i < KF_EYE_N; i++) if (g_eye[i].col == col) { slot = i; break; }
    int fresh = slot < 0 || g_eye[slot].dep != dep || g_eye[slot].layers != (layers >= 2 ? 2u : 1u);
    if (slot < 0) for (int i = 0; i < KF_EYE_N; i++) if (!g_eye[i].col) { slot = i; break; }
    if (slot < 0) slot = 0;
    g_eye[slot].col = col; g_eye[slot].dep = dep; g_eye[slot].w = w; g_eye[slot].h = h;
    g_eye[slot].samples = samples > 1 ? samples : 1;
    g_eye[slot].kind = KF_ARRAY; g_eye[slot].layers = layers >= 2 ? 2 : 1; g_eye[slot].eye = layers >= 2 ? -1 : eye;
    g_eye[slot].via = 0; memset(g_eye[slot].out, 0, sizeof g_eye[slot].out);
    pthread_mutex_unlock(&g_mu);
    if (fresh)
        KFI("eye target registered: colour %p depth %p %ux%u x%u sample(s), %s (slot %d)\n", col, dep, w, h,
            samples > 1 ? samples : 1, layers >= 2 ? "2-layer eye pair" : eye == 1 ? "eye 1 alone" : "eye 0 alone", slot);
}

void kl_fove_note_eye_scene(void *col, void *dep, void *out, uint32_t w, uint32_t h, uint32_t samples,
                            uint32_t layers, int eye, int via) {
    if (!col || !kl_fove_on() || !g_inter || via < KL_FOVE_VIA_SUBPASS || via > KL_FOVE_VIA_SAMPLED) return;
    if (via == KL_FOVE_VIA_SUBPASS && !out) return;
    if (layers >= 2 && g_dev && ![g_dev supportsRasterizationRateMapWithLayerCount:2]) return;
    uint32_t L = layers >= 2 ? 2u : 1u;
    pthread_mutex_lock(&g_mu);
    if (kf_split_find_locked(col) >= 0) { pthread_mutex_unlock(&g_mu); return; }   // split-refused
    int slot = -1;
    for (int i = 0; i < KF_EYE_N; i++) if (g_eye[i].col == col) { slot = i; break; }
    int fresh = slot < 0 || g_eye[slot].dep != dep || g_eye[slot].layers != L || g_eye[slot].via != via;
    if (slot < 0) for (int i = 0; i < KF_EYE_N; i++) if (!g_eye[i].col) { slot = i; break; }
    if (slot < 0) slot = 0;
    if (fresh) memset(&g_eye[slot], 0, sizeof g_eye[slot]);
    g_eye[slot].col = col; g_eye[slot].dep = dep; g_eye[slot].w = w; g_eye[slot].h = h;
    g_eye[slot].samples = samples > 1 ? samples : 1;
    g_eye[slot].kind = KF_ARRAY; g_eye[slot].layers = L; g_eye[slot].eye = L == 2 ? -1 : eye;
    g_eye[slot].via = via;
    // The eye images it writes: one per swapchain stage. Several command
    // buffers may be recorded ahead of their encoding, so every stage seen
    // stays matched (the oldest is replaced past KF_OUT_N).
    int new_out = 0;
    if (out) {
        int have = 0;
        for (int o = 0; o < KF_OUT_N; o++) have |= g_eye[slot].out[o] == out;
        if (!have) {
            int o = 0;
            while (o < KF_OUT_N && g_eye[slot].out[o]) o++;
            if (o == KF_OUT_N) { memmove(g_eye[slot].out, g_eye[slot].out + 1, sizeof(void *) * (KF_OUT_N - 1)); o = KF_OUT_N - 1; }
            g_eye[slot].out[o] = out;
            new_out = 1;
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (fresh)
        KFI("intermediate target registered: scene colour %p depth %p %ux%u x%u sample(s), %s, via %s (slot %d)%s\n",
            col, dep, w, h, samples > 1 ? samples : 1, L == 2 ? "2-layer eye pair" : eye == 1 ? "eye 1 alone" : "eye 0 alone",
            via == KL_FOVE_VIA_SUBPASS ? "subpass (a later subpass writes the eye image: that is what is expanded)"
            : via == KL_FOVE_VIA_XFER ? "copy/blit (the scene is expanded, before the copy)"
            : "sampled (the scene is expanded, before the pass that reads it)", slot,
            via == KL_FOVE_VIA_SUBPASS ? "" : "");
    static int nsaid;
    if (new_out && nsaid < 2 * KF_OUT_N) {
        nsaid++;
        KFI("intermediate slot %d: eye image %p is written in-pass from it\n", slot, out);
    }
}

int kl_fove_intermediate(void) { return kl_fove_on() && g_inter; }

void kl_fove_note_ms_transient(void *col, int transient) {
    if (!col || !kl_fove_on()) return;
    int slot = -1, was = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < KF_EYE_N; i++)
        if (g_eye[i].col == col && g_eye[i].via) {
            slot = i; was = g_eye[i].ms_tr;
            g_eye[i].ms_tr = transient && g_eye[i].samples > 1 && g_eye[i].layers == 2;
            transient = g_eye[i].ms_tr;
            break;
        }
    pthread_mutex_unlock(&g_mu);
    if (slot >= 0 && was != transient)
        KFI("intermediate slot %d: MSAA scene %s\n", slot, transient
            ? "is TRANSIENT (never stored, resolved by a later subpass): its layered encoders are mapped and held, "
              "the resolve target is expanded"
            : "is no longer transient: a layered MSAA store with no resolve draws unfoveated");
}

void kl_fove_forget(void *tex) {
    if (!tex || g_on < 0) return;
    pthread_mutex_lock(&g_mu);
    { int k = kf_split_find_locked(tex); if (k >= 0) g_split_col[k] = NULL; }   // no longer split-refused
    for (int i = 0; i < KF_EYE_N; i++) {
        if (g_eye[i].col == tex || g_eye[i].dep == tex) {
            KFI("eye target slot %d forgotten (image destroyed)\n", i);
            memset(&g_eye[i], 0, sizeof g_eye[i]);
        }
        for (int o = 0; o < KF_OUT_N; o++) if (g_eye[i].out[o] == tex) g_eye[i].out[o] = NULL;
    }
    for (int i = 0; i < KF_CB_N; i++) if (g_pend[i].tex == tex) memset(&g_pend[i], 0, sizeof g_pend[i]);
    pthread_mutex_unlock(&g_mu);
}

// ---------------------------------------------------------------- intermediate state
// kl_vulkan's candidate -> confirmed bookkeeping for the guest's own eye-size
// scene targets, keyed by VkImage handle. A candidate is an eye-size layered
// pass's colour that is not an eye image; it is CONFIRMED (sticky until the
// image is destroyed) when it is seen reaching an eye image: written into one
// by a later subpass, copied/blitted/resolved into one, or followed by a pass
// that draws into one (sampled). Pure table: no Metal, no foveation state.
#define KF_INTER_N 16
static struct { uint64_t img, seq; int state, via; } g_it[KF_INTER_N];
static uint64_t g_it_seq;
static pthread_mutex_t g_it_mu = PTHREAD_MUTEX_INITIALIZER;

static int kf_it_find_locked(uint64_t img) {
    for (int i = 0; i < KF_INTER_N; i++) if (g_it[i].img == img) return i;
    return -1;
}
int kl_fove_inter_note(uint64_t img, int *via) {
    if (via) *via = 0;
    if (!img) return 0;
    pthread_mutex_lock(&g_it_mu);
    int i = kf_it_find_locked(img);
    if (i < 0) {
        // A free row, else the oldest candidate, else the oldest row.
        int f = -1, oc = -1, oa = 0;
        for (int j = 0; j < KF_INTER_N; j++) {
            if (!g_it[j].img) { f = j; break; }
            if (g_it[j].state == 1 && (oc < 0 || g_it[j].seq < g_it[oc].seq)) oc = j;
            if (g_it[j].seq < g_it[oa].seq) oa = j;
        }
        i = f >= 0 ? f : oc >= 0 ? oc : oa;
        g_it[i].img = img; g_it[i].state = 1; g_it[i].via = 0;
    }
    g_it[i].seq = ++g_it_seq;
    int st = g_it[i].state;
    if (via) *via = g_it[i].via;
    pthread_mutex_unlock(&g_it_mu);
    return st;
}
int kl_fove_inter_confirm(uint64_t img, int via) {
    if (!img || via < KL_FOVE_VIA_SUBPASS || via > KL_FOVE_VIA_SAMPLED) return -1;
    pthread_mutex_lock(&g_it_mu);
    int i = kf_it_find_locked(img), r = -1;
    if (i >= 0 && g_it[i].state == 1) { g_it[i].state = 2; g_it[i].via = via; r = 1; }
    else if (i >= 0) r = 0;
    pthread_mutex_unlock(&g_it_mu);
    return r;
}
int kl_fove_inter_state(uint64_t img, int *via) {
    if (via) *via = 0;
    if (!img) return 0;
    pthread_mutex_lock(&g_it_mu);
    int i = kf_it_find_locked(img), st = i >= 0 ? g_it[i].state : 0;
    if (i >= 0 && via) *via = g_it[i].via;
    pthread_mutex_unlock(&g_it_mu);
    return st;
}
// kl_vulkan saw an indirect draw inside
// this image's (multiview) pass. MoltenVK splits such a pass around a compute
// encoder and resumes it with LOAD, and the compute open flushes the expand
// mid-pass: the second half draws in screen layout over a depth still in
// physical layout (Paint the Town Red, combat: the bar went black outside ~30% of the view,
// or see-through to bright textures). State 3 = REFUSED: never a candidate,
// never confirmed, sticky until vkDestroyImage. Returns the previous state.
int kl_fove_inter_refuse(uint64_t img) {
    if (!img) return -1;
    pthread_mutex_lock(&g_it_mu);
    int i = kf_it_find_locked(img), was = 0;
    if (i < 0) {
        int f = -1, oc = -1, oa = 0;
        for (int j = 0; j < KF_INTER_N; j++) {
            if (!g_it[j].img) { f = j; break; }
            if (g_it[j].state == 1 && (oc < 0 || g_it[j].seq < g_it[oc].seq)) oc = j;
            if (g_it[j].seq < g_it[oa].seq) oa = j;
        }
        i = f >= 0 ? f : oc >= 0 ? oc : oa;
        g_it[i].img = img; g_it[i].seq = ++g_it_seq;
    } else was = g_it[i].state;
    g_it[i].state = 3; g_it[i].via = 0;
    pthread_mutex_unlock(&g_it_mu);
    if (was != 3) __atomic_add_fetch(&g_st.inter_refused, 1, __ATOMIC_RELAXED);
    return was;
}
// The Metal half of a refusal: the intermediate's colour texture stops being a
// marked target (its slot is cleared; a pending expand already recorded for it
// still runs — it is in physical layout on that command buffer).
void kl_fove_demote(void *col) {
    if (!col || g_on < 0) return;
    int said = -1;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < KF_EYE_N; i++)
        if (g_eye[i].col == col && g_eye[i].via) { memset(&g_eye[i], 0, sizeof g_eye[i]); said = i; }
    pthread_mutex_unlock(&g_mu);
    if (said >= 0) KFI("intermediate slot %d (colour %p) DEMOTED: no longer foveated\n", said, col);
}
// A SPLIT PASS was seen on this intermediate's colour. Paint the Town Red
// showed 340 split passes and ZERO record-time refusals: Unity draws the scene through SECONDARY command buffers, so the
// indirect draw's command buffer is not the one the pass was begun on. Refuse
// on the Metal side too: demote it now and never register it again until the
// image is destroyed. Returns 1 if newly refused.
int kl_fove_split_refuse(void *col) {
    if (!col || g_on < 0) return 0;
    int fresh = 0, slot = -1;
    pthread_mutex_lock(&g_mu);
    if (kf_split_find_locked(col) < 0) {
        int k = -1;
        for (int i = 0; i < KF_SPLIT_N && k < 0; i++) if (!g_split_col[i]) k = i;
        if (k < 0) { memmove(g_split_col, g_split_col + 1, sizeof(void *) * (KF_SPLIT_N - 1)); k = KF_SPLIT_N - 1; }
        g_split_col[k] = col;
        fresh = 1;
    }
    for (int i = 0; i < KF_EYE_N; i++)
        if (g_eye[i].col == col && g_eye[i].via) { memset(&g_eye[i], 0, sizeof g_eye[i]); slot = i; }
    pthread_mutex_unlock(&g_mu);
    if (fresh) {
        uint64_t n = __atomic_add_fetch(&g_st.split_refused, 1, __ATOMIC_RELAXED);
        KFI("intermediate SPLIT-REFUSED (G11): colour %p (slot %d) — MoltenVK split its pass (an indirect draw the "
            "Vulkan side did not see in it); left UNFOVEATED until it is destroyed (#%llu)\n", col, slot,
            (unsigned long long)n);
    }
    return fresh;
}

// A pass drawing into a runtime eye image
// closes a "window"; with direct = 1 (the eye image is the pass's own colour)
// every candidate noted since the previous window close is CONFIRMED via
// sampled — whether or not that eye pass carries a depth attachment (Unity
// binds the scene depth, DONT_CARE, to its final blit into the eye), and every
// hop of a post chain (HDR scene -> LDR post target -> eye) at once. direct = 0
// (an MSAA pass resolving into the eye, or an in-pass intermediate) only closes
// the window: its candidates stay candidates.
// Returns how many were newly confirmed; the first `cap` go to conf[].
static uint64_t g_it_win;
int kl_fove_inter_eye_reached(int direct, uint64_t *conf, int cap) {
    int n = 0;
    pthread_mutex_lock(&g_it_mu);
    if (direct)
        for (int i = 0; i < KF_INTER_N; i++)
            if (g_it[i].img && g_it[i].state == 1 && g_it[i].seq > g_it_win) {
                g_it[i].state = 2; g_it[i].via = KL_FOVE_VIA_SAMPLED;
                if (conf && n < cap) conf[n] = g_it[i].img;
                n++;
            }
    g_it_win = g_it_seq;
    pthread_mutex_unlock(&g_it_mu);
    return n;
}
uint64_t kl_fove_inter_last(void) {
    uint64_t img = 0, best = 0;
    pthread_mutex_lock(&g_it_mu);
    for (int i = 0; i < KF_INTER_N; i++)
        if (g_it[i].img && g_it[i].state == 1 && g_it[i].seq > best) { best = g_it[i].seq; img = g_it[i].img; }
    pthread_mutex_unlock(&g_it_mu);
    return img;
}
void kl_fove_inter_forget(uint64_t img) {
    if (!img) return;
    pthread_mutex_lock(&g_it_mu);
    int i = kf_it_find_locked(img);
    if (i >= 0) memset(&g_it[i], 0, sizeof g_it[i]);
    pthread_mutex_unlock(&g_it_mu);
}
void kl_fove_inter_reset_for_test(void) {
    pthread_mutex_lock(&g_it_mu);
    memset(g_it, 0, sizeof g_it);
    g_it_win = g_it_seq;
    pthread_mutex_unlock(&g_it_mu);
}

// t_fove: drive the swizzled command-buffer class with real encoders over
// real textures, as MoltenVK would for Batman: Arkham Shadow's pass (2-sample
// 2-layer scene + depth, a compute encoder between subpasses, the eye
// subpass, commit), then the copy-into-the-eye shape and the unmapped-eye-
// subpass alarm. Counters are read back from g_st.
static id<MTLTexture> kf_t_tex(id<MTLDevice> dev, MTLPixelFormat f, uint32_t W, uint32_t H, NSUInteger samples) {
    MTLTextureDescriptor *d = [objc_getClass("MTLTextureDescriptor") new];
    d.textureType = samples > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray;
    d.pixelFormat = f; d.width = W; d.height = H; d.arrayLength = 2; d.sampleCount = samples;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d];
    KF_RELEASE(d);
    return KF_AUTORELEASE(t);
}
static void kf_t_pass(id<MTLCommandBuffer> cb, id<MTLTexture> c, id<MTLTexture> z, MTLLoadAction la, NSUInteger rtal) {
    MTLRenderPassDescriptor *d = [objc_getClass("MTLRenderPassDescriptor") renderPassDescriptor];
    d.colorAttachments[0].texture = c;
    d.colorAttachments[0].loadAction = la;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (z) {
        d.depthAttachment.texture = z;
        d.depthAttachment.loadAction = la == MTLLoadActionLoad ? MTLLoadActionLoad : MTLLoadActionClear;
        d.depthAttachment.storeAction = MTLStoreActionStore;
    }
    d.renderTargetArrayLength = rtal;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:d];
    [en endEncoding];
}
int kl_fove_inter_gpu_selftest(char *why, unsigned cap) {
    if (!kl_fove_on() || !g_inter) { snprintf(why, cap, "KL_FOVEATION unset or KL_FOVE_INTERMEDIATE=0"); return -1; }
    int bad = 0;
    @autoreleasepool {
        // Metal by name: the runtime does not link it (MoltenVK / the app do).
        void *mh = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_LAZY);
        id (*mk)(void) = mh ? (id (*)(void))dlsym(mh, "MTLCreateSystemDefaultDevice") : NULL;
        id<MTLDevice> dev = mk ? mk() : nil;
        if (!dev || ![dev supportsRasterizationRateMapWithLayerCount:2]) {
            snprintf(why, cap, "no Metal device with 2-layer rate maps");
            KF_RELEASE(dev);
            return -1;
        }
        kl_fove_install((__bridge void *)dev);
        if (!kl_fove_on()) { snprintf(why, cap, "kl_fove_install turned foveation off"); KF_RELEASE(dev); return -1; }
        const uint32_t W = 1024, H = 1024;
        id<MTLTexture> scene = kf_t_tex(dev, MTLPixelFormatRGBA8Unorm_sRGB, W, H, 2);
        id<MTLTexture> depth = kf_t_tex(dev, MTLPixelFormatDepth32Float, W, H, 2);
        id<MTLTexture> eye0 = kf_t_tex(dev, MTLPixelFormatRGBA8Unorm_sRGB, W, H, 1);
        id<MTLTexture> eye1 = kf_t_tex(dev, MTLPixelFormatRGBA8Unorm_sRGB, W, H, 1);
        id<MTLTexture> mid = kf_t_tex(dev, MTLPixelFormatRGBA8Unorm_sRGB, W, H, 1);
        id<MTLCommandQueue> q = [dev newCommandQueue];
        if (!scene || !depth || !eye0 || !eye1 || !mid || !q) { snprintf(why, cap, "texture/queue creation failed"); return -1; }
        // Two swapchain stages: both stay matched (command buffers are encoded
        // after later frames were recorded).
        kl_fove_note_eye_scene((__bridge void *)scene, (__bridge void *)depth, (__bridge void *)eye0, W, H, 2, 2, -1,
                               KL_FOVE_VIA_SUBPASS);
        kl_fove_note_eye_scene((__bridge void *)scene, (__bridge void *)depth, (__bridge void *)eye1, W, H, 2, 2, -1,
                               KL_FOVE_VIA_SUBPASS);
        // 1. Batman: Arkham Shadow's shape, eye stage 0: scene subpass, an internal compute
        //    encoder, a LOADing scene subpass, the eye subpass, commit.
        uint64_t m0 = g_st.marked_enc, e0 = g_st.expands, i0 = g_st.inter_enc, o0 = g_st.inter_out_enc;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        kf_t_pass(cb, scene, depth, MTLLoadActionDontCare, 2);
        uint64_t gen = kf_cb_gen(cb, 0);
        pthread_mutex_lock(&g_mu);
        int pi = kl_fove_pend_find((__bridge void *)cb, gen);
        int held = pi >= 0 && g_pend[pi].hold && !g_pend[pi].tex;
        pthread_mutex_unlock(&g_mu);
        if (g_st.marked_enc != m0 + 1 || g_st.inter_enc != i0 + 1) { bad++; KFI("selftest: scene subpass not marked\n"); }
        if (!held) { bad++; KFI("selftest: scene subpass left no HELD pending entry\n"); }
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce endEncoding];
        if (g_st.expands != e0) { bad++; KFI("selftest: an encoder between subpasses expanded\n"); }
        kf_t_pass(cb, scene, depth, MTLLoadActionLoad, 2);
        if (g_st.marked_enc != m0 + 2) { bad++; KFI("selftest: the LOADing scene subpass after a compute encoder not marked\n"); }
        kf_t_pass(cb, eye0, nil, MTLLoadActionDontCare, 2);
        if (g_st.marked_enc != m0 + 3 || g_st.inter_out_enc != o0 + 1) { bad++; KFI("selftest: eye subpass not marked\n"); }
        pthread_mutex_lock(&g_mu);
        pi = kl_fove_pend_find((__bridge void *)cb, gen);
        int owes = pi >= 0 && g_pend[pi].tex == (__bridge void *)eye0 && !g_pend[pi].hold;
        pthread_mutex_unlock(&g_mu);
        if (!owes) { bad++; KFI("selftest: the eye image is not what is pending\n"); }
        [cb commit];
        [cb waitUntilCompleted];
        if (g_st.expands != e0 + 1) { bad++; KFI("selftest: the eye was not expanded at commit\n"); }
        if (cb.error) { bad++; KFI("selftest: command buffer error %s\n", cb.error.description.UTF8String); }
        // 2. Eye stage 1 (registered second) is matched too.
        cb = [q commandBuffer];
        kf_t_pass(cb, scene, depth, MTLLoadActionDontCare, 2);
        kf_t_pass(cb, eye1, nil, MTLLoadActionDontCare, 2);
        if (g_st.inter_out_enc != o0 + 2) { bad++; KFI("selftest: eye stage 1 subpass not marked\n"); }
        [cb commit];
        [cb waitUntilCompleted];
        if (g_st.expands != e0 + 2) { bad++; KFI("selftest: eye stage 1 not expanded\n"); }
        // 3. The alarm: an eye subpass that cannot be mapped (one slice) while
        //    the scene is pending is counted.
        uint64_t l0 = g_st.inter_lost;
        cb = [q commandBuffer];
        kf_t_pass(cb, scene, depth, MTLLoadActionDontCare, 2);
        kf_t_pass(cb, eye0, nil, MTLLoadActionDontCare, 1);
        if (g_st.inter_lost != l0 + 1) { bad++; KFI("selftest: an unmapped eye subpass was not counted\n"); }
        [cb commit];
        [cb waitUntilCompleted];
        // 4. Via copy: a single-sample intermediate is expanded BEFORE the
        //    blit encoder that copies it into the eye.
        kl_fove_note_eye_scene((__bridge void *)mid, NULL, NULL, W, H, 1, 2, -1, KL_FOVE_VIA_XFER);
        uint64_t b0 = g_st.blit_flush, e1 = g_st.expands;
        cb = [q commandBuffer];
        kf_t_pass(cb, mid, nil, MTLLoadActionDontCare, 2);
        if (g_st.expands != e1) { bad++; KFI("selftest: intermediate expanded too early\n"); }
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        if (g_st.expands != e1 + 1 || g_st.blit_flush != b0 + 1) { bad++; KFI("selftest: no expand before the copy's blit encoder\n"); }
        [be copyFromTexture:mid toTexture:eye0];
        [be endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.error) { bad++; KFI("selftest: command buffer error %s\n", cb.error.description.UTF8String); }
        if (g_st.expands != e1 + 1) { bad++; KFI("selftest: expanded twice\n"); }
        // 6. A confirmed SAMPLED intermediate whose
        //    pass MoltenVK split around a compute encoder (an indirect draw in
        //    a multiview pass): the compute open expands it mid-pass and the
        //    resumed LOAD encoder is unfoveated — counted as a SPLIT PASS (the
        //    detector the Vulkan-side refusal is proven against).
        {
            id<MTLTexture> s6 = kf_t_tex(dev, MTLPixelFormatRG11B10Float, W, H, 2);
            id<MTLTexture> d6 = kf_t_tex(dev, MTLPixelFormatDepth32Float, W, H, 2);
            if (!s6 || !d6) { bad++; KFI("selftest: G10 textures\n"); }
            kl_fove_note_eye_scene((__bridge void *)s6, (__bridge void *)d6, NULL, W, H, 1, 2, -1, KL_FOVE_VIA_SAMPLED);
            uint64_t c0 = g_st.compute_split, e6 = g_st.expands, l6 = g_st.late, m6 = g_st.marked_enc;
            uint64_t r6 = g_st.split_refused;
            cb = [q commandBuffer];
            kf_t_pass(cb, s6, d6, MTLLoadActionClear, 2);
            if (g_st.marked_enc != m6 + 1) { bad++; KFI("selftest: G10 scene pass not marked\n"); }
            id<MTLComputeCommandEncoder> ce6 = [cb computeCommandEncoder];
            [ce6 endEncoding];
            if (g_st.expands != e6 + 1) { bad++; KFI("selftest: G10 the compute encoder did not flush the expand\n"); }
            kf_t_pass(cb, s6, d6, MTLLoadActionLoad, 2);                   // MoltenVK's resume
            if (g_st.compute_split != c0 + 1 || g_st.late != l6 + 1) { bad++; KFI("selftest: G10 split pass not counted\n"); }
            kf_t_pass(cb, s6, d6, MTLLoadActionLoad, 2);                   // a later LOAD: not the resume
            if (g_st.compute_split != c0 + 1) { bad++; KFI("selftest: G10 counted a non-resume LOAD\n"); }
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.error) { bad++; KFI("selftest: G10 command buffer error %s\n", cb.error.description.UTF8String); }
            // The split itself refused it (no
            // kl_vulkan refusal here) — re-registering it is ignored, so the
            // next frame's scene pass is NOT marked.
            if (g_st.split_refused != r6 + 1) { bad++; KFI("selftest: G11 the split did not refuse the intermediate\n"); }
            kl_fove_note_eye_scene((__bridge void *)s6, (__bridge void *)d6, NULL, W, H, 1, 2, -1, KL_FOVE_VIA_SAMPLED);
            m6 = g_st.marked_enc;
            cb = [q commandBuffer];
            kf_t_pass(cb, s6, d6, MTLLoadActionClear, 2);
            if (g_st.marked_enc != m6) { bad++; KFI("selftest: G10/G11 a split-refused intermediate was still marked\n"); }
            [cb commit];
            [cb waitUntilCompleted];
            kl_fove_forget((__bridge void *)s6);
            // forgotten (destroyed): the refusal is gone with it
            kl_fove_note_eye_scene((__bridge void *)s6, (__bridge void *)d6, NULL, W, H, 1, 2, -1, KL_FOVE_VIA_SAMPLED);
            m6 = g_st.marked_enc;
            cb = [q commandBuffer];
            kf_t_pass(cb, s6, d6, MTLLoadActionClear, 2);
            if (g_st.marked_enc != m6 + 1) { bad++; KFI("selftest: G11 a forgotten texture stayed split-refused\n"); }
            [cb commit];
            [cb waitUntilCompleted];
            kl_fove_forget((__bridge void *)s6);
        }
        // 5. Deadpool's UE4 shape (transient MSAA): a 2-sample 2-layer scene the guest
        //    never stores (DONT_CARE), confirmed "sampled"; MoltenVK opens
        //    subpass 0 storing its samples with no resolve, then subpass 1
        //    LOADs them and resolves into a single-sample 2-layer target, then
        //    the post pass reads that. Both scene encoders are marked with one
        //    map, nothing is expanded between them, the RESOLVE target is
        //    expanded before the post pass, nothing is counted unfoveated.
        {
            id<MTLTexture> sc2 = kf_t_tex(dev, MTLPixelFormatRGBA8Unorm_sRGB, W + 16, H, 2);
            id<MTLTexture> dp2 = kf_t_tex(dev, MTLPixelFormatDepth32Float, W + 16, H, 2);
            id<MTLTexture> rs2 = kf_t_tex(dev, MTLPixelFormatRGBA8Unorm_sRGB, W + 16, H, 1);
            id<MTLTexture> post = kf_t_tex(dev, MTLPixelFormatRGBA8Unorm_sRGB, W, H, 1);
            if (!sc2 || !dp2 || !rs2 || !post) { bad++; KFI("selftest: G7 textures\n"); }
            kl_fove_note_eye_scene((__bridge void *)sc2, (__bridge void *)dp2, NULL, W + 16, H, 2, 2, -1,
                                   KL_FOVE_VIA_SAMPLED);
            // Not (yet) transient: the layered store with no resolve is refused, as before.
            uint64_t r0 = g_st.ms_layer_refused, m1 = g_st.marked_enc;
            cb = [q commandBuffer];
            kf_t_pass(cb, sc2, dp2, MTLLoadActionClear, 2);
            if (g_st.ms_layer_refused != r0 + 1 || g_st.marked_enc != m1) { bad++; KFI("selftest: G7 control not refused\n"); }
            [cb commit];
            [cb waitUntilCompleted];
            kl_fove_note_ms_transient((__bridge void *)sc2, 1);
            uint64_t h0 = g_st.ms_tr_held, v0 = g_st.ms_tr_resolved, l1 = g_st.late, e2 = g_st.expands;
            uint64_t x0 = g_st.res_expands, lost0 = g_st.inter_lost;
            r0 = g_st.ms_layer_refused; m1 = g_st.marked_enc;
            cb = [q commandBuffer];
            kf_t_pass(cb, sc2, dp2, MTLLoadActionClear, 2);                 // subpass 0: stores, no resolve
            uint64_t gen5 = kf_cb_gen(cb, 0);
            pthread_mutex_lock(&g_mu);
            int p5 = kl_fove_pend_find((__bridge void *)cb, gen5);
            int held5 = p5 >= 0 && g_pend[p5].hold && !g_pend[p5].tex && !g_pend[p5].depth_only;
            pthread_mutex_unlock(&g_mu);
            if (g_st.marked_enc != m1 + 1 || g_st.ms_tr_held != h0 + 1 || g_st.ms_layer_refused != r0) {
                bad++; KFI("selftest: G7 subpass 0 not marked/held\n");
            }
            if (!held5) { bad++; KFI("selftest: G7 subpass 0 left no HELD pending entry\n"); }
            id<MTLComputeCommandEncoder> ce5 = [cb computeCommandEncoder];
            [ce5 endEncoding];
            if (g_st.expands != e2) { bad++; KFI("selftest: G7 an encoder between the subpasses expanded\n"); }
            {                                                               // subpass 1: LOAD + resolve
                MTLRenderPassDescriptor *d = [objc_getClass("MTLRenderPassDescriptor") renderPassDescriptor];
                d.colorAttachments[0].texture = sc2;
                d.colorAttachments[0].loadAction = MTLLoadActionLoad;
                d.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
                d.colorAttachments[0].resolveTexture = rs2;
                d.depthAttachment.texture = dp2;
                d.depthAttachment.loadAction = MTLLoadActionLoad;
                d.depthAttachment.storeAction = MTLStoreActionDontCare;
                d.renderTargetArrayLength = 2;
                id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:d];
                [en endEncoding];
            }
            pthread_mutex_lock(&g_mu);
            p5 = kl_fove_pend_find((__bridge void *)cb, gen5);
            int owes5 = p5 >= 0 && g_pend[p5].tex == (__bridge void *)rs2 && !g_pend[p5].hold;
            pthread_mutex_unlock(&g_mu);
            if (g_st.marked_enc != m1 + 2 || g_st.ms_tr_resolved != v0 + 1 || g_st.late != l1) {
                bad++; KFI("selftest: G7 the LOADing resolve subpass not marked\n");
            }
            if (!owes5) { bad++; KFI("selftest: G7 the resolve target is not what is pending\n"); }
            kf_t_pass(cb, post, nil, MTLLoadActionDontCare, 2);              // the post pass reads it
            if (g_st.expands != e2 + 1 || g_st.res_expands != x0 + 1) { bad++; KFI("selftest: G7 resolve target not expanded before the post pass\n"); }
            if (g_st.inter_lost != lost0) { bad++; KFI("selftest: G7 counted a broken frame\n"); }
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.error) { bad++; KFI("selftest: G7 command buffer error %s\n", cb.error.description.UTF8String); }
            if (g_st.expands != e2 + 1) { bad++; KFI("selftest: G7 expanded twice\n"); }
            // The alarm: the resolving subpass cannot be mapped (one slice) while
            // the samples are held physical — counted as a broken frame.
            lost0 = g_st.inter_lost;
            cb = [q commandBuffer];
            kf_t_pass(cb, sc2, dp2, MTLLoadActionClear, 2);
            kf_t_pass(cb, sc2, dp2, MTLLoadActionLoad, 1);
            if (g_st.inter_lost != lost0 + 1) { bad++; KFI("selftest: G7 an unmapped LOAD of held samples was not counted\n"); }
            [cb commit];
            [cb waitUntilCompleted];
            kl_fove_forget((__bridge void *)sc2);
        }
        // Forget everything this test registered.
        kl_fove_forget((__bridge void *)scene);
        kl_fove_forget((__bridge void *)mid);
        kl_fove_forget((__bridge void *)eye0);
        kl_fove_forget((__bridge void *)eye1);
        KF_RELEASE(q);
        KF_RELEASE(dev);
    }
    return bad;
}

void kl_fove_perf_field(char *out, unsigned cap) {
    if (!cap) return;
    out[0] = 0;
    if (g_on < 0 || !g_level) return;
    static uint64_t last_exp, last_marked;
    uint64_t e = g_st.expands, m = g_st.marked_enc;
    snprintf(out, cap, " | fove %s%s%s %.0f%% exp %llu marked %llu refused %llu clobber %llu",
             g_level_name, g_on == 1 ? "" : " (OFF)", g_st.ms_expands ? " msaa" : "",
             g_lastmap ? 100.0 * ((double)g_lastmap->lpw[0] * g_lastmap->lph[0] +
                                  (g_lastmap->layers == 2 ? (double)g_lastmap->lpw[1] * g_lastmap->lph[1] : 0)) /
                             ((double)g_lastmap->W * g_lastmap->H * (g_lastmap->layers == 2 ? 2 : 1)) : 0.0,
             (unsigned long long)(e - last_exp), (unsigned long long)(m - last_marked),
             (unsigned long long)g_st.refused, (unsigned long long)g_st.clobber);
    last_exp = e; last_marked = m;
    static int every;
    if (++every % 10 == 1) {
        char hist[256]; int o = 0;
        for (int i = 0; i < 32 && o < (int)sizeof hist - 16; i++)
            if (g_st.used_hist[i]) o += snprintf(hist + o, sizeof hist - (size_t)o, " %d:%d", i, g_st.used_hist[i]);
        KFI("libraries %llu, fragment entry points patched %llu (%llu with subpass reads, %llu with MSAA eye reads), refused %llu, without [[position]] %llu; "
            "encoders marked %llu (layered %llu) identity %llu, eye LOADs left unfoveated %llu, one-slice passes over an eye pair "
            "left unfoveated %llu, layered MSAA without resolve left unfoveated %llu; expands %llu (MSAA %llu, of a resolve "
            "target %llu marks; dropped uncommitted %llu, table full %llu); intermediate marked %llu (eye subpasses %llu, "
            "eye subpasses UNMAPPED %llu, expands before a blit %llu); transient MSAA held %llu / resolved %llu; slot clobbers %llu (in marked %llu); layer index "
            "added %llu / reused %llu; buffer indices used by [[position]] shaders:%s\n",
            (unsigned long long)g_st.libs, (unsigned long long)g_st.patched, (unsigned long long)g_st.subpass,
            (unsigned long long)g_st.msread, (unsigned long long)g_st.refused,
            (unsigned long long)g_st.nopos, (unsigned long long)g_st.marked_enc, (unsigned long long)g_st.layered_enc,
            (unsigned long long)g_st.ident_enc,
            (unsigned long long)g_st.late, (unsigned long long)g_st.split, (unsigned long long)g_st.ms_layer_refused,
            (unsigned long long)g_st.expands, (unsigned long long)g_st.ms_expands, (unsigned long long)g_st.res_expands,
            (unsigned long long)g_st.dropped,
            (unsigned long long)g_st.full, (unsigned long long)(g_st.inter_enc + g_st.inter_out_enc),
            (unsigned long long)g_st.inter_out_enc, (unsigned long long)g_st.inter_lost,
            (unsigned long long)g_st.blit_flush, (unsigned long long)g_st.ms_tr_held,
            (unsigned long long)g_st.ms_tr_resolved, (unsigned long long)g_st.clobber,
            (unsigned long long)g_st.clobber_marked, (unsigned long long)g_st.layer_arg_added,
            (unsigned long long)g_st.layer_arg_reused, hist);
        KFI("split passes (G10): resumed after a compute encoder flushed their expand %llu (must stay 0 once refusals "
            "are in), intermediates refused for an indirect draw in their pass %llu, split-refused on the Metal side "
            "(G11) %llu\n",
            (unsigned long long)g_st.compute_split, (unsigned long long)g_st.inter_refused,
            (unsigned long long)g_st.split_refused);
        if (g_xfence)
            KFI("expand fence: encoders after an expand: waited on its own buffer %llu, on a later buffer %llu; "
                "NOT waited: other buffer while the expand's was uncommitted %llu, other queue %llu; "
                "expands recorded at an encoder %llu, at commit %llu\n",
                (unsigned long long)g_xf_same, (unsigned long long)g_xf_later, (unsigned long long)g_xf_open,
                (unsigned long long)g_xf_queue, (unsigned long long)g_xf_at_enc, (unsigned long long)g_xf_at_commit);
        if (g_probe)
            KFI("probe totals: fence waits %llu, fence updates %llu, useResource %llu, memory barriers %llu\n",
                (unsigned long long)g_pr_wait, (unsigned long long)g_pr_upd, (unsigned long long)g_pr_use,
                (unsigned long long)g_pr_mb);
    }
}

// Local rate of map layer l at screen column x (row y): physical px per screen px.
static float kf_t_rate(id<MTLRasterizationRateMap> m, int l, float x, float y) {
    MTLCoordinate2D a = [m mapScreenToPhysicalCoordinates:(MTLCoordinate2D){x - 8, y} forLayer:(NSUInteger)l];
    MTLCoordinate2D b = [m mapScreenToPhysicalCoordinates:(MTLCoordinate2D){x + 8, y} forLayer:(NSUInteger)l];
    return (b.x - a.x) / 16.0f;
}
// The middle of layer l's full-rate box along x at row y (screen px), -1 when none.
static float kf_t_box_mid(id<MTLRasterizationRateMap> m, int l, uint32_t W, float y) {
    int lo = -1, hi = -1;
    for (uint32_t x = 16; x + 16 < W; x += 4)
        if (kf_t_rate(m, l, (float)x, y) > 0.97f) { if (lo < 0) lo = (int)x; hi = (int)x; }
    return lo < 0 ? -1.0f : 0.5f * (lo + hi);
}

int kl_fove_recentre_selftest(char *why, unsigned cap) {
    if (!kl_fove_on() || !g_dev) { snprintf(why, cap, "no foveation device (run after the intermediate GPU self-test)"); return -1; }
    int bad = 0;
    // Deadpool: 5088x3236 intermediate, 5088x3234 eye, the guest's
    // eye-layer Fov L60 R45 (eye 0) puts the point 63.4% / 36.6% across its
    // OWN viewport; viewports eye0 0,0 4012x3234, eye1 1075,0 4013x3234.
    const uint32_t W = 5088, H = 3236;
    const float fx[2] = { 0.634f, 0.366f }, fy[2] = { 0.456f, 0.456f };
    const int vp[2][4] = { { 0, 0, 4012, 3234 }, { 1075, 0, 4013, 3234 } }, of[2] = { 5088, 3234 };
    const int full[2][4] = { { 0, 0, 5088, 3234 }, { 0, 0, 5088, 3234 } };
    @autoreleasepool {
        pthread_mutex_lock(&g_mu);
        int r0 = g_recentres;
        g_trec_on = 0;
        pthread_mutex_unlock(&g_mu);
        // 1. Built before any record: the full image (3226 / 1862).
        kl_fove_test_eye_record(1, fx, fy, NULL, NULL);
        pthread_mutex_lock(&g_mu);
        kf_map_t *mp = kf_map_for(W, H, KF_ARRAY, 2, -1);
        id<MTLRasterizationRateMap> m1 = mp ? mp->map : nil;
        float c0 = mp ? mp->cx[0] : -1, c1 = mp ? mp->cx[1] : -1;
        pthread_mutex_unlock(&g_mu);
        if (!mp || fabsf(c0 - 3226) > 2 || fabsf(c1 - 1862) > 2) {
            bad++; KFI("recentre selftest: no-record centres %.0f / %.0f, want 3226 / 1862\n", c0, c1);
        }
        if (!mp) { kl_fove_test_eye_record(0, fx, fy, NULL, NULL); return bad; }
        float b0 = kf_t_box_mid(m1, 0, W, 1476), b1 = kf_t_box_mid(m1, 1, W, 1476);
        KFI("recentre selftest: before the record, full-rate box middles x %.0f / %.0f; rate at x 2544: %.2f / %.2f\n",
            b0, b1, kf_t_rate(m1, 0, 2544, 1476), kf_t_rate(m1, 1, 2544, 1476));
        // 2. The record arrives while another command buffer's pending expand
        //    refers to the map: the new map goes into another slot, the old
        //    one stays as it was (superseded) for that expand, and the buffer
        //    that already drew with it keeps getting it.
        int mi = (int)(mp - g_maps);
        kl_fove_test_eye_record(1, fx, fy, vp, of);
        pthread_mutex_lock(&g_mu);
        int ps = -1;
        for (int i = 0; i < KF_CB_N; i++) if (!g_pend[i].cb) { ps = i; break; }
        if (ps >= 0) { g_pend[ps].cb = (void *)0x5eed; g_pend[ps].gen = 1; g_pend[ps].map = mi; g_pend[ps].slot = -1; }
        kf_map_t *mq = kf_map_for(W, H, KF_ARRAY, 2, -1);
        if (ps < 0 || !mq || mq == mp || mp->map != m1 || !mp->superseded) {
            bad++; KFI("recentre selftest: under another buffer's pending expand: not built into a new slot, or the "
                       "old map not kept\n");
        }
        if (kf_map_for_cb(W, H, KF_ARRAY, 2, -1, mi) != mp) { bad++; KFI("recentre selftest: the drawing buffer lost its map\n"); }
        if (ps >= 0) memset(&g_pend[ps], 0, sizeof g_pend[ps]);
        // 3. The new map: 2544 / 2544, and looked up again without a rebuild.
        kf_map_t *mn = mq;
        mq = kf_map_for(W, H, KF_ARRAY, 2, -1);
        id<MTLRasterizationRateMap> m2 = mq ? mq->map : nil;
        c0 = mq ? mq->cx[0] : -1; c1 = mq ? mq->cx[1] : -1;
        int rc = g_recentres - r0;
        pthread_mutex_unlock(&g_mu);
        if (mq != mn || !m2 || m2 == m1 || rc != 1) { bad++; KFI("recentre selftest: not re-centred once (re-centres %d)\n", rc); }
        if (fabsf(c0 - 2544) > 2 || fabsf(c1 - 2544) > 2) {
            bad++; KFI("recentre selftest: record centres %.0f / %.0f, want 2544 / 2544\n", c0, c1);
        }
        if (m2) {
            // The full-rate box spans both eyes' points placed in this eye's
            // rect (the envelope; KL_FOVE_PER_EYE=1: its own point alone), so
            // its middle is the middle of the two; 2544 itself stays at full rate.
            for (int l = 0; l < 2; l++) {
                float want = mq->env ? 0.5f * (mq->cx[l] + mq->ocx[l]) : 2544;
                float bm = kf_t_box_mid(m2, l, W, 1476), r = kf_t_rate(m2, l, 2544, 1476);
                KFI("recentre selftest: layer %d full-rate box middle x %.0f (want %.0f), rate at 2544 %.2f\n", l, bm,
                    want, r);
                if (fabsf(bm - want) > 48 || r < 0.97f) { bad++; KFI("recentre selftest: layer %d fovea not at 2544\n", l); }
            }
        }
        // 4. The same record again: no rebuild. Into the Radius's full-image record with a
        //    centred frustum: the same centre as no record, no rebuild either.
        pthread_mutex_lock(&g_mu);
        mq = kf_map_for(W, H, KF_ARRAY, 2, -1);
        if (!mq || mq->map != m2 || g_recentres - r0 != 1) { bad++; KFI("recentre selftest: rebuilt for an unchanged record\n"); }
        pthread_mutex_unlock(&g_mu);
        const float cfx[2] = { 0.5f, 0.5f };
        kl_fove_test_eye_record(1, cfx, fy, NULL, NULL);
        pthread_mutex_lock(&g_mu);
        kf_map_t *ms = kf_map_for(W, H - 2, KF_ARRAY, 2, -1);   // a fresh shape: built with no record
        id<MTLRasterizationRateMap> s1 = ms ? ms->map : nil;
        pthread_mutex_unlock(&g_mu);
        kl_fove_test_eye_record(1, cfx, fy, full, of);
        pthread_mutex_lock(&g_mu);
        kf_map_t *ms2 = kf_map_for(W, H - 2, KF_ARRAY, 2, -1);
        if (!ms || ms2 != ms || ms2->map != s1 || g_recentres - r0 != 1) {
            bad++; KFI("recentre selftest: ITR's full-image record rebuilt the map\n");
        }
        pthread_mutex_unlock(&g_mu);
        kl_fove_test_eye_record(0, fx, fy, NULL, NULL);
    }
    return bad;
}

// t_fove: two intermediates of different formats
// expanded alternately every frame must NOT remake the per-layer scratch each
// time. Runs after kl_fove_inter_gpu_selftest (needs its device).
int kl_fove_scratch_selftest(char *why, unsigned cap) {
    if (!kl_fove_on() || !g_dev) { snprintf(why, cap, "no foveation device (run after the intermediate GPU self-test)"); return -1; }
    int bad = 0;
    static id<MTLTexture> ways[2][KF_SC_WAYS];
    @autoreleasepool {
        pthread_mutex_lock(&g_mu);
        int nold0 = g_nold;
        uint64_t a0 = g_sc_allocs;
        id<MTLTexture> first[2][2] = { { nil, nil }, { nil, nil } };
        // 30 frames of Paint the Town Red's level shape: HDR (92) then LDR (71), both layers.
        for (int f = 0; f < 30; f++)
            for (int k = 0; k < 2; k++)
                for (int l = 0; l < 2; l++) {
                    id<MTLTexture> t = kf_scratch_into(ways[l], "selftest-scratch", k ? MTLPixelFormatRGBA8Unorm_sRGB :
                                                       MTLPixelFormatRG11B10Float, 1, 1968, 1808, MTLTextureUsageShaderRead);
                    if (!t) { bad++; break; }
                    if (!f) first[l][k] = t;
                    else if (t != first[l][k]) { bad++; KFI("scratch selftest: frame %d layer %d fmt #%d remade\n", f, l, k); }
                }
        if (g_sc_allocs - a0 != 4) { bad++; KFI("scratch selftest: %llu textures made for 2 formats x 2 layers, want 4\n",
                                                 (unsigned long long)(g_sc_allocs - a0)); }
        // A size change replaces that format's entry only.
        id<MTLTexture> r = kf_scratch_into(ways[0], "selftest-scratch", MTLPixelFormatRGBA8Unorm_sRGB, 1, 1982, 1808,
                                           MTLTextureUsageShaderRead);
        id<MTLTexture> h = kf_scratch_into(ways[0], "selftest-scratch", MTLPixelFormatRG11B10Float, 1, 1968, 1808,
                                           MTLTextureUsageShaderRead);
        int live = 0;
        for (int i = 0; i < KF_SC_WAYS; i++) live += ways[0][i] != nil;
        if (!r || r.width != 1982 || h != first[0][0] || live != 2) {
            bad++; KFI("scratch selftest: resize kept %d entries (want 2), HDR entry %s\n", live, h == first[0][0] ? "kept" : "LOST");
        }
        // More formats than ways: bounded.
        const MTLPixelFormat more[4] = { MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA16Float, MTLPixelFormatBGRA8Unorm,
                                         MTLPixelFormatRGB10A2Unorm };
        for (int i = 0; i < 4; i++)
            kf_scratch_into(ways[0], "selftest-scratch", more[i], 1, 64, 64, MTLTextureUsageShaderRead);
        live = 0;
        for (int i = 0; i < KF_SC_WAYS; i++) live += ways[0][i] != nil;
        if (live > KF_SC_WAYS) { bad++; KFI("scratch selftest: %d entries > %d ways\n", live, KF_SC_WAYS); }
        // The test's textures are in no command buffer: free them (MRC or ARC).
        for (int l = 0; l < 2; l++) for (int i = 0; i < KF_SC_WAYS; i++) { KF_RELEASE(ways[l][i]); ways[l][i] = nil; }
        for (int i = nold0; i < g_nold; i++) { KF_RELEASE(g_old[i]); g_old[i] = nil; }
        g_nold = nold0;
        pthread_mutex_unlock(&g_mu);
    }
    return bad;
}

// t_fove: the layered eye pair through the GPU. Pass 1 draws both layers of a
// 2-layer target under the map (one instance per layer, as MoltenVK lowers
// multiview); each fragment writes the screen x of its own centre (R) and a
// 2-px checker of its screen position (G). Pass 2 brings each slice back to
// screen layout as the expand does (R nearest, G linear). A screen column is
// drawn at full rate when R is its own centre; the share of distinct R values
// over a stretch is that stretch's rate.
static NSString *kLayersMSL = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct V { float4 pos [[position]]; uint layer [[render_target_array_index]]; };\n"
    "vertex V lt_vs(uint vid [[vertex_id]], uint iid [[instance_id]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  V o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); o.layer = iid; return o;\n"
    "}\n"
    "fragment float2 lt_draw(V in [[stage_in]], constant rasterization_rate_map_data& d [[buffer(0)]]) {\n"
    "  rasterization_rate_map_decoder dec(d);\n"
    "  float2 s = dec.map_physical_to_screen_coordinates(in.pos.xy, in.layer);\n"
    "  float2 c = floor(s * 0.5);\n"
    "  return float2(s.x, fmod(c.x + c.y, 2.0));\n"
    "}\n"
    "struct W1 { float4 pos [[position]]; };\n"
    "vertex W1 lt_vs1(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  W1 o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o;\n"
    "}\n"
    "struct L { uint layer; float pw; float ph; float pad; };\n"
    "fragment float2 lt_expand(W1 in [[stage_in]], constant rasterization_rate_map_data& d [[buffer(0)]],\n"
    "                          constant L& c [[buffer(1)]], texture2d_array<float> ph [[texture(0)]]) {\n"
    "  constexpr sampler ns(coord::pixel, filter::nearest, address::clamp_to_edge);\n"
    "  constexpr sampler ls(coord::pixel, filter::linear, address::clamp_to_edge);\n"
    "  rasterization_rate_map_decoder dec(d);\n"
    "  float2 p = dec.map_screen_to_physical_coordinates(in.pos.xy, c.layer);\n"
    "  p = clamp(p, float2(0.5), float2(c.pw, c.ph) - 0.5);\n"
    "  return float2(ph.sample(ns, p, c.layer).r, ph.sample(ls, p, c.layer).g);\n"
    "}\n";

// g_mu held. Forget the cached map of a shape (test only: nothing in flight).
static void kf_t_drop(uint32_t W, uint32_t H, int kind, int layers, int eye) {
    for (int i = 0; i < KF_MAP_N; i++) {
        kf_map_t *c = &g_maps[i];
        if (c->W == W && c->H == H && c->kind == kind && c->layers == layers && c->eye == eye) {
            KF_RELEASE(c->map); KF_RELEASE(c->buf);
            c->map = nil; c->buf = nil;
            *c = (kf_map_t){0};
            if (g_lastmap == c) g_lastmap = NULL;
        }
    }
}

// A w x h crop (from 0,0) of an RG32Float slice as a 24-bit BMP of its
// horizontal rate: 1 / how many screen pixels of the row share a fragment
// (channel 0 is the fragment's screen x). Green = full rate, red = the floor;
// columns mx[0] (white) and mx[1] (blue) marked.
static void kf_t_bmp_heat(const char *path, const float *rg, uint32_t stride, uint32_t w, uint32_t h,
                          const float mx[2]) {
    float *q = malloc(sizeof(float) * (size_t)w * h);
    if (!q) return;
    for (uint32_t y = 0; y < h; y++) {       // along x: runs of equal fragment x
        uint32_t x = 0;
        while (x < w) {
            uint32_t e = x + 1;
            while (e < w && rg[((size_t)y * stride + e) * 2] == rg[((size_t)y * stride + x) * 2]) e++;
            for (uint32_t k = x; k < e; k++) q[(size_t)y * w + k] = 1.0f / (float)(e - x);
            x = e;
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) { free(q); return; }
    uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    uint8_t hd[54] = { 'B', 'M' };
    memcpy(hd + 2, &size, 4);
    uint32_t off = 54, dib = 40, ppm = 2835, sz = row * h;
    int32_t iw = (int32_t)w, ih = (int32_t)h;
    uint16_t planes = 1, bpp = 24;
    memcpy(hd + 10, &off, 4); memcpy(hd + 14, &dib, 4); memcpy(hd + 18, &iw, 4); memcpy(hd + 22, &ih, 4);
    memcpy(hd + 26, &planes, 2); memcpy(hd + 28, &bpp, 2); memcpy(hd + 34, &sz, 4);
    memcpy(hd + 38, &ppm, 4); memcpy(hd + 42, &ppm, 4);
    fwrite(hd, 1, 54, f);
    uint8_t *line = calloc(1, row);
    for (uint32_t y = 0; line && y < h; y++) {
        uint32_t sy = h - 1 - y;
        for (uint32_t x = 0; x < w; x++) {
            float v = q[(size_t)sy * w + x];
            uint8_t b = 0, g = (uint8_t)(v * 255.0f), r = (uint8_t)((1.0f - v) * 255.0f);
            if (mx && fabsf((float)x - mx[0]) < 2) b = g = r = 255;
            else if (mx && fabsf((float)x - mx[1]) < 2) { b = 255; g = 64; r = 0; }
            line[3 * x] = b; line[3 * x + 1] = g; line[3 * x + 2] = r;
        }
        fwrite(line, 1, row, f);
    }
    free(line); free(q);
    fclose(f);
}

// A W x H crop of channel 1 of an RG32Float slice as a grey 24-bit BMP.
static void kf_t_bmp(const char *path, const float *rg, uint32_t stride, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    uint8_t hd[54] = { 'B', 'M' };
    memcpy(hd + 2, &size, 4);
    uint32_t off = 54, dib = 40, ppm = 2835, sz = row * h;
    int32_t iw = (int32_t)w, ih = (int32_t)h;
    uint16_t planes = 1, bpp = 24;
    memcpy(hd + 10, &off, 4); memcpy(hd + 14, &dib, 4); memcpy(hd + 18, &iw, 4); memcpy(hd + 22, &ih, 4);
    memcpy(hd + 26, &planes, 2); memcpy(hd + 28, &bpp, 2); memcpy(hd + 34, &sz, 4);
    memcpy(hd + 38, &ppm, 4); memcpy(hd + 42, &ppm, 4);
    fwrite(hd, 1, 54, f);
    uint8_t *line = calloc(1, row);
    for (uint32_t y = 0; line && y < h; y++) {
        const float *src = rg + ((size_t)(y0 + h - 1 - y) * stride + x0) * 2;
        for (uint32_t x = 0; x < w; x++) {
            float g = src[2 * x + 1];
            uint8_t v = (uint8_t)(g <= 0 ? 0 : g >= 1 ? 255 : g * 255.0f + 0.5f);
            line[3 * x] = line[3 * x + 1] = line[3 * x + 2] = v;
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
}

// Draw both layers of mp through the GPU and read them back (RG32Float, W x H
// per slice, into out[l]). 0 on failure.
static int kf_t_draw_layers(kf_map_t *mp, float *out[2]) {
    static id<MTLRenderPipelineState> draw, expand;
    static id<MTLCommandQueue> q;
    const uint32_t W = mp->W, H = mp->H;
    if (!draw) {
        NSError *err = nil;
        t_in = 1;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:kLayersMSL options:nil error:&err];
        t_in = 0;
        if (!lib) { KFI("layers selftest: shader: %s\n", err ? err.description.UTF8String : "?"); return 0; }
        MTLRenderPipelineDescriptor *pd = [objc_getClass("MTLRenderPipelineDescriptor") new];
        id<MTLFunction> f1 = [lib newFunctionWithName:@"lt_vs"], f2 = [lib newFunctionWithName:@"lt_draw"];
        id<MTLFunction> f3 = [lib newFunctionWithName:@"lt_vs1"], f4 = [lib newFunctionWithName:@"lt_expand"];
        pd.vertexFunction = f1; pd.fragmentFunction = f2;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRG32Float;
        pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
        draw = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        pd.vertexFunction = f3; pd.fragmentFunction = f4;
        pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassUnspecified;
        expand = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        KF_RELEASE(pd); KF_RELEASE(f1); KF_RELEASE(f2); KF_RELEASE(f3); KF_RELEASE(f4); KF_RELEASE(lib);
        q = [g_dev newCommandQueue];
        if (!draw || !expand || !q) { KFI("layers selftest: pipeline: %s\n", err ? err.description.UTF8String : "?"); return 0; }
    }
    id<MTLTexture> phys = kf_t_tex(g_dev, MTLPixelFormatRG32Float, W, H, 1);
    id<MTLTexture> scr = kf_t_tex(g_dev, MTLPixelFormatRG32Float, W, H, 1);
    size_t slice = (size_t)W * H * 8;
    id<MTLBuffer> rb = KF_AUTORELEASE([g_dev newBufferWithLength:2 * slice options:MTLResourceStorageModeShared]);
    if (!phys || !scr || !rb) return 0;
    id<MTLCommandBuffer> cb = [q commandBuffer];
    t_in = 1;
    MTLRenderPassDescriptor *d = [objc_getClass("MTLRenderPassDescriptor") renderPassDescriptor];
    d.colorAttachments[0].texture = phys;
    d.colorAttachments[0].loadAction = MTLLoadActionClear;
    d.colorAttachments[0].clearColor = MTLClearColorMake(-1, 0, 0, 0);
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    d.renderTargetArrayLength = 2;
    d.rasterizationRateMap = mp->map;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d];
    [e setRenderPipelineState:draw];
    [e setFragmentBuffer:mp->buf offset:0 atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:2];
    [e endEncoding];
    for (int l = 0; l < 2; l++) {
        MTLRenderPassDescriptor *d2 = [objc_getClass("MTLRenderPassDescriptor") renderPassDescriptor];
        d2.colorAttachments[0].texture = scr;
        d2.colorAttachments[0].slice = (NSUInteger)l;
        d2.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        d2.colorAttachments[0].storeAction = MTLStoreActionStore;
        struct { uint32_t layer; float pw, ph, pad; } c = { (uint32_t)l, (float)mp->lpw[l], (float)mp->lph[l], 0 };
        id<MTLRenderCommandEncoder> e2 = [cb renderCommandEncoderWithDescriptor:d2];
        [e2 setRenderPipelineState:expand];
        [e2 setFragmentBuffer:mp->buf offset:0 atIndex:0];
        [e2 setFragmentBytes:&c length:sizeof c atIndex:1];
        [e2 setFragmentTexture:phys atIndex:0];
        [e2 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e2 endEncoding];
    }
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    for (int l = 0; l < 2; l++)
        [b copyFromTexture:scr sourceSlice:(NSUInteger)l sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(W, H, 1) toBuffer:rb destinationOffset:(NSUInteger)l * slice
                destinationBytesPerRow:(NSUInteger)W * 8 destinationBytesPerImage:slice];
    [b endEncoding];
    t_in = 0;
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) { KFI("layers selftest: command buffer failed\n"); return 0; }
    for (int l = 0; l < 2; l++) memcpy(out[l], (const char *)rb.contents + (size_t)l * slice, slice);
    return 1;
}

// Is screen column x of row y (an RG slice of width W) drawn at full rate?
static int kf_t_sharp(const float *rg, uint32_t W, uint32_t x, uint32_t y) {
    return fabsf(rg[((size_t)y * W + x) * 2] - ((float)x + 0.5f)) < 0.26f;
}
// The rate over columns [x0, x1) of row y: distinct fragments / columns.
static float kf_t_run_rate(const float *rg, uint32_t W, uint32_t x0, uint32_t x1, uint32_t y) {
    int n = 0;
    for (uint32_t x = x0; x < x1; x++)
        if (x == x0 || rg[((size_t)y * W + x) * 2] != rg[((size_t)y * W + x - 1) * 2]) n++;
    return x1 > x0 ? (float)n / (float)(x1 - x0) : 0;
}

int kl_fove_layers_selftest(char *why, unsigned cap, float *rates, const char *shots) {
    if (!kl_fove_on() || !g_dev) { snprintf(why, cap, "no foveation device (run after the intermediate GPU self-test)"); return -1; }
    int bad = 0;
    // Alien: Rogue Incursion's shape at half size: a 2-layer eye image both of
    // whose eyes render into 0,0 at 70% of it (dynamic resolution), with the
    // Vision Pro's cones (KL_OVRP_EYE_TAN=vision: 63.4% / 36.6% across, 45.6% down).
    const uint32_t W = 1520, H = 1232;
    const float fx[2] = { 0.634f, 0.366f }, fy[2] = { 0.456f, 0.456f };
    const int vw = 1064, vh = 862;
    const int vp[2][4] = { { 0, 0, vw, vh }, { 0, 0, vw, vh } }, of[2] = { (int)W, (int)H };
    const uint32_t y = (uint32_t)(fy[0] * vh);
    float *img[2][2] = { { malloc((size_t)W * H * 8), malloc((size_t)W * H * 8) },
                         { malloc((size_t)W * H * 8), malloc((size_t)W * H * 8) } };
    if (!img[0][0] || !img[0][1] || !img[1][0] || !img[1][1]) { snprintf(why, cap, "out of memory"); return -1; }
    int saved = g_per_eye;
    @autoreleasepool {
        kl_fove_test_eye_record(1, fx, fy, vp, of);
        for (int mode = 0; mode < 2; mode++) {   // 0: one peak per eye (until 565), 1: the envelope
            pthread_mutex_lock(&g_mu);
            g_per_eye = !mode;
            kf_t_drop(W, H, KF_ARRAY, 2, -1);
            kf_map_t *mp = kf_map_for(W, H, KF_ARRAY, 2, -1);
            float c[2] = { mp ? mp->cx[0] : 0, mp ? mp->cx[1] : 0 }, oc[2] = { mp ? mp->ocx[0] : 0, mp ? mp->ocx[1] : 0 };
            uint32_t pw[2] = { mp ? mp->lpw[0] : 0, mp ? mp->lpw[1] : 0 }, ph[2] = { mp ? mp->lph[0] : 0, mp ? mp->lph[1] : 0 };
            pthread_mutex_unlock(&g_mu);
            // Unlocked: the swizzled encoder and commit methods take g_mu.
            int ok = mp && mp->map && kf_t_draw_layers(mp, img[mode]);
            if (!ok) { bad++; KFI("layers selftest: %s map not drawn\n", mode ? "envelope" : "per-eye"); continue; }
            float r[2][8];
            for (int l = 0; l < 2; l++)
                for (int k = 0; k < 8; k++) {
                    r[l][k] = kf_t_run_rate(img[mode][l], W, (uint32_t)(k * vw / 8), (uint32_t)((k + 1) * vw / 8), y);
                    if (rates) rates[(mode * 2 + l) * 8 + k] = r[l][k];
                }
            for (int l = 0; l < 2; l++)
                KFI("layers selftest: %-8s eye %d (%s) rate by eighths of its rect: %.2f %.2f %.2f %.2f | %.2f %.2f "
                    "%.2f %.2f; physical %ux%u\n", mode ? "envelope" : "per-eye", l, l ? "right" : "left",
                    r[l][0], r[l][1], r[l][2], r[l][3], r[l][4], r[l][5], r[l][6], r[l][7], pw[l], ph[l]);
            // Each eye's own straight-ahead point is at full rate in both modes.
            for (int l = 0; l < 2; l++)
                for (int dx = -4; dx <= 4; dx++)
                    if (!kf_t_sharp(img[mode][l], W, (uint32_t)(c[l] + dx), y)) {
                        bad++; KFI("layers selftest: %s eye %d: its own point x %.0f not at full rate\n",
                                   mode ? "envelope" : "per-eye", l, c[l]); break;
                    }
            if (!mode) {
                // The bug, reproduced: the outer side of each eye is the coarse one.
                if (!(r[0][0] + 0.2f < r[0][7])) { bad++; KFI("layers selftest: per-eye: the left eye's left side is not coarser\n"); }
                if (!(r[1][7] + 0.2f < r[1][0])) { bad++; KFI("layers selftest: per-eye: the right eye's right side is not coarser\n"); }
                continue;
            }
            // The envelope: both eyes the same, each symmetric about its rect's middle.
            for (int k = 0; k < 8; k++) {
                if (fabsf(r[0][k] - r[1][k]) > 0.03f) {
                    bad++; KFI("layers selftest: eighth %d: left %.2f, right %.2f — not equally sharp\n", k, r[0][k], r[1][k]);
                }
                for (int l = 0; l < 2; l++)
                    if (fabsf(r[l][k] - r[l][7 - k]) > 0.08f) {
                        bad++; KFI("layers selftest: eye %d eighths %d / %d: %.2f / %.2f — not symmetric\n", l, k, 7 - k,
                                   r[l][k], r[l][7 - k]);
                    }
            }
            for (int l = 0; l < 2; l++) {
                float pts[3] = { oc[l], 0.5f * (float)vw, c[l] };
                for (int i = 0; i < 3; i++)
                    for (int dx = -4; dx <= 4; dx++)
                        if (!kf_t_sharp(img[1][l], W, (uint32_t)(pts[i] + dx), y)) {
                            bad++; KFI("layers selftest: envelope eye %d: x %.0f not at full rate\n", l, pts[i]); break;
                        }
            }
            if (pw[0] != pw[1] || ph[0] != ph[1]) { bad++; KFI("layers selftest: the two layers' physical sizes differ\n"); }
        }
        if (shots && *shots) {
            static const char *nm[2] = { "per-eye", "envelope" };
            char p[1024];
            for (int mode = 0; mode < 2; mode++)
                for (int l = 0; l < 2; l++) {
                    snprintf(p, sizeof p, "%s/t_fove-%s-%s.bmp", shots, nm[mode], l ? "right" : "left");
                    kf_t_bmp(p, img[mode][l], W, 0, 0, (uint32_t)vw, (uint32_t)vh);
                    // own point white, the other eye's point (in this rect) blue
                    const float mx[2] = { fx[l] * vw, fx[l ^ 1] * vw };
                    snprintf(p, sizeof p, "%s/t_fove-rate-%s-%s.bmp", shots, nm[mode], l ? "right" : "left");
                    kf_t_bmp_heat(p, img[mode][l], W, (uint32_t)vw, (uint32_t)vh, mx);
                }
            KFI("layers selftest: pictures in %s\n", shots);
        }
        // Dynamic resolution: the viewport grows from 70% to 100% in 20 steps,
        // and the map follows every one (the cap was 8 re-centres a run).
        pthread_mutex_lock(&g_mu);
        g_per_eye = 0;
        int r0 = g_recentres, followed = 0;
        pthread_mutex_unlock(&g_mu);
        for (int s = 1; s <= 20; s++) {
            int w = (int)W * (70 + 30 * s / 20) / 100, h = (int)H * (70 + 30 * s / 20) / 100;
            const int v2[2][4] = { { 0, 0, w, h }, { 0, 0, w, h } };
            pthread_mutex_lock(&g_mu);
            // Every other step an expand of another buffer is still pending on
            // the current map (Alien always has one): a new slot, recycled later.
            int ps = -1;
            kf_map_t *cur = kf_map_for(W, H, KF_ARRAY, 2, -1);   // the previous step's record: no rebuild
            if (s & 1 && cur)
                for (int i = 0; i < KF_CB_N; i++)
                    if (!g_pend[i].cb) {
                        ps = i; g_pend[i].cb = (void *)0x5eef; g_pend[i].gen = (uint64_t)s;
                        g_pend[i].map = (int)(cur - g_maps); g_pend[i].slot = -1; break;
                    }
            pthread_mutex_unlock(&g_mu);
            kl_fove_test_eye_record(1, fx, fy, v2, of);
            pthread_mutex_lock(&g_mu);
            kf_map_t *mp = kf_map_for(W, H, KF_ARRAY, 2, -1);
            if (s & 1 && (mp == cur || !cur || !cur->superseded)) {
                bad++; KFI("layers selftest: step %d: not built into another slot under a pending expand\n", s);
            }
            if (mp && fabsf(mp->cx[0] - fx[0] * w) < 2 && fabsf(mp->cx[1] - fx[1] * w) < 2 &&
                fabsf(mp->ocx[0] - fx[1] * w) < 2 && fabsf(mp->ocx[1] - fx[0] * w) < 2)
                followed++;
            if (ps >= 0) memset(&g_pend[ps], 0, sizeof g_pend[ps]);
            pthread_mutex_unlock(&g_mu);
        }
        pthread_mutex_lock(&g_mu);
        int rc = g_recentres - r0, live = 0;
        for (int i = 0; i < KF_MAP_N; i++) live += g_maps[i].W == W && g_maps[i].H == H;
        kf_t_drop(W, H, KF_ARRAY, 2, -1);
        g_per_eye = saved;
        pthread_mutex_unlock(&g_mu);
        KFI("layers selftest: dynamic resolution: %d of 20 viewport steps followed (10 under another buffer's pending "
            "expand), %d re-centres, %d slot(s) of this shape left\n", followed, rc, live);
        if (followed != 20 || rc != 20) { bad++; KFI("layers selftest: the map stopped following the viewport\n"); }
        if (live > 2) { bad++; KFI("layers selftest: superseded slots are not recycled (%d live)\n", live); }
        kl_fove_test_eye_record(0, fx, fy, NULL, NULL);
    }
    for (int m = 0; m < 2; m++) for (int l = 0; l < 2; l++) free(img[m][l]);
    return bad;
}
