// libklepton_ovrp — the OVRPlugin replacement. See kl_ovrp.h for why it is a
// replacement rather than a translation.
//
// This is the measurement stage. Unity's C# side reaches OVRPlugin through
// [DllImport("OVRPlugin")], which IL2CPP resolves with dlsym at runtime, so
// serving the soname and naming every lookup gives the real surface without
// guessing at any of the 466 exported entry points.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include "klepton.h"
#include "kl_ovrp.h"

#define KL_OVRP_MAX 512
static struct { const char *name; unsigned calls; } g_ovrp[KL_OVRP_MAX];
static unsigned g_novrp;
// Resolution and per-frame calls happen on different guest threads (main vs
// render); without this, two concurrent inserts can split a slot and leave a
// NULL name for a later strcmp — observed as SIGSEGV inside ovrp_hit.
static pthread_mutex_t g_ovrp_mu = PTHREAD_MUTEX_INITIALIZER;

// name -> the pointer we handed the guest, so a guest function table can be
// read back by name (see klovrp_dump_vrdevice).
static struct { const char *name; void *ptr; } g_sym[KL_OVRP_MAX];
static unsigned g_nsym;
static void *kl_ovrp_sym_inner(const char *name);

static int ovrp_slot(const char *name) {
    pthread_mutex_lock(&g_ovrp_mu);
    int rc = -1;
    for (unsigned i = 0; i < g_novrp; i++)
        if (strcmp(g_ovrp[i].name, name) == 0) { rc = (int)i; goto out; }
    if (g_novrp >= KL_OVRP_MAX) goto out;
    g_ovrp[g_novrp].name = strdup(name);
    g_ovrp[g_novrp].calls = 0;
    rc = (int)g_novrp++;
out:
    pthread_mutex_unlock(&g_ovrp_mu);
    return rc;
}

static int g_permissive = -1;
static int permissive(void) {
    if (g_permissive < 0) g_permissive = getenv("KL_PERMISSIVE") != NULL;
    return g_permissive;
}

// Reached through a per-name trampoline, so x0 is the entry point's own name.
// The stub tail-calls here, so this return value is the ovrp_ call's return
// value. ovrpSuccess is 0, which makes a permissive zero mean "it worked" —
// wrong in the usual way, but it is what collects the whole surface in one run.
static uint64_t klovrp_called(const char *name) {
    int s = ovrp_slot(name);
    if (s >= 0) g_ovrp[s].calls++;
    if (permissive()) {
        if (s >= 0 && g_ovrp[s].calls == 1)
            fprintf(stderr, "  [ovrp] call (permissive, returning 0): %s\n", name);
        return 0;
    }
    fprintf(stderr, "\n[klepton] fatal: guest called unimplemented OVRPlugin entry "
                    "point '%s'\n", name);
    kl_ovrp_report(stderr);
    kl_fatal_prepare();
    abort();
}

// ---------------------------------------------------------------------------
// The two return conventions, and why a blanket zero is not safe here
//
// OVRPlugin entry points return one of two things, and 0 means the opposite in
// each. `ovrpResult` is 0 for success and negative for failure, so 0 is "it
// worked". `ovrpBool` is a plain boolean, so 0 is "no" — and for a getter like
// ovrp_GetInitialized that reads as "XR never came up", which is the answer we
// are specifically trying not to give.
//
// So there is no single permissive value. Entry points are placed in one of two
// named lists by which type they return, and anything in neither still aborts by
// name. Getting this wrong is the trap-6d failure again: a silent zero that reads
// as a legitimate negative answer several layers from where it was invented.
#define OVRP_SUCCESS 0
#define OVRP_TRUE    1

// Record a call. The hand-written implementations below call this themselves so
// that they stay in the report — the report is the M6 work list, and an entry
// point silently dropping off it once implemented is how the list stops matching
// what the guest actually does.
static void ovrp_hit(const char *name) {
    int s = ovrp_slot(name);
    if (s >= 0) g_ovrp[s].calls++;
}

// M7 discovery: log each distinct argument value an input-family entry point
// is called with (node id, controller mask), once each, with the caller's
// image — names whose node/mask values the guest actually uses, and whether
// the poller is libunity or the game itself.
// The caller is part of the key, not just the argument: libunity's node loop
// polls every node every frame, so keying on (name, arg) alone suppresses the
// *game's* first call for a node libunity already asked about — which is
// exactly the call worth seeing (it tells apart "OVRInput reads hand poses"
// from "only the engine does"). Keyed on (name, arg) this read as "libil2cpp
// never asks for node 3", which was an artefact of the instrument.
static void ovrp_log_arg(const char *name, int arg, void *ra) {
    static struct { const char *name; int arg; void *ra; } seen[128];
    static int nseen; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nseen; i++)
        if (seen[i].name == name && seen[i].arg == arg && seen[i].ra == ra) {
            pthread_mutex_unlock(&mu); return;
        }
    if (nseen < 128) {
        size_t off = 0;
        const char *img = kl_addr_image(ra, &off);
        fprintf(stderr, "  [ovrp] %s(arg=%d/0x%x) — first seen, called from %s+0x%zx\n",
                name, arg, arg, img ? img : "?", off);
        seen[nseen].name = name; seen[nseen].arg = arg;
        seen[nseen].ra = ra; nseen++;
    }
    pthread_mutex_unlock(&mu);
}

// Returns ovrpResult: 0 is success. Also covers the void entry points, where the
// return value is simply ignored by the caller.
static uint64_t klovrp_ok(const char *name) {
    ovrp_hit(name);
    return OVRP_SUCCESS;
}

// Returns ovrpBool: 1 is "yes". Used for the capability and state predicates that
// have to agree with the fact that we answered ovrp_Initialize5 with success.
static uint64_t klovrp_yes(const char *name) {
    ovrp_hit(name);
    return OVRP_TRUE;
}

// Returns ovrpBool: 0 is "no". For capabilities we genuinely do not have — the
// honest answer, and the one that keeps Unity off a path with nothing behind it.
static uint64_t klovrp_no(const char *name) {
    ovrp_hit(name);
    return 0;
}

// ---------------------------------------------------------------------------
// The synthetic headset
//
// One coherent device description, for the reason the display group in kl_jni.c
// and the GLES capability set in kl_egl.c are each answered as a whole: Unity
// asks these in several places and cross-checks the answers, so they have to
// describe the same hardware. What is described is the Quest 2 we already claim
// to be through Build.MODEL — not the Vision Pro underneath — because every
// Oculus branch in the guest is written against that.
#define KL_OVRP_EYE_W    1832        // Quest 2 per-eye recommended render target
#define KL_OVRP_EYE_H    1920
#define KL_OVRP_REFRESH  72.0f       // Quest 2 default display frequency

// The version string Unity parses to gate optional features behind "is the
// plugin at least 1.x". Taken from the OVRPlugin actually shipped in this APK
// (strings(1) reports 1.60.0; the package is ovrplugin-android-universal:28.0.0)
// rather than picked: the guest's managed side was compiled against that plugin,
// so any other number invites it down a path its own C# does not match.
static const char *klovrp_GetVersion(void) {
    ovrp_hit("ovrp_GetVersion");
    return "1.60.0";
}

// ovrpSystemHeadset. The value is read out of the guest's own IL2CPP metadata
// rather than from an OVRPlugin header we do not have: global-metadata.dat lists
// the enum as Oculus_Quest, Oculus_Quest_2, Placeholder_10 .. Placeholder_14,
// Rift_DK1 ... — and the placeholders name their own values, which pins
// Oculus_Quest_2 at 9 (and, via PC_Placeholder_4103, Rift_DK1 at 0x1000).
//
// Quest 2 for the same reason Build.MODEL says Quest 2: it is the device this
// title is written for, and the answer has to agree with the one JNI already gave.
#define OVRP_HEADSET_OCULUS_QUEST_2 9
static uint64_t klovrp_GetSystemHeadsetType(void) {
    ovrp_hit("ovrp_GetSystemHeadsetType");
    return OVRP_HEADSET_OCULUS_QUEST_2;
}

// libunity's OculusVRDevice::Initialize calls this for the product name and,
// if the return is non-NULL, strlens and hashes it (guest code at 0x9bb538 —
// NULL is explicitly tolerated). A static string is therefore both safe and
// required if we want the name answered at all; "Oculus Quest 2" is what the
// real plugin returns on the device we are presenting as everywhere else.
static const char *klovrp_GetSystemProductName(void) {
    ovrp_hit("ovrp_GetSystemProductName");
    return "Oculus Quest 2";
}

// ---------------------------------------------------------------------------
// The libunity steady-state contract
//
// Everything below exists because guest libunity.so (2019.4.28f1, legacy VR)
// calls it through the dlsym'd Oculus table, with the argument shapes and
// return checks recovered from its own machine code — not invented from an
// OVRPlugin header. Offsets cited in the comments are guest libunity
// addresses; the recovery story is in PLANNING.md §M6.

// float return: s0, not x0 — this is why it cannot share klovrp_yes.
// Stored into Unity's VR timing config (0x9bce28). No division by it in the
// Oculus path, but 0.0 would poison Unity-side pacing math; 72 is the Quest 2
// default and agrees with the device we describe.
// The display frequency we report. Quest 2's 72 Hz by default — the device we
// claim to be everywhere else — until a frontend measures the real one and
// pushes it through kl_ovrp_set_display_frequency. Unity reads this once,
// early, and stores it into its VR timing config, so the push has to happen
// before the guest boots; on visionOS that is what the compositor's priming
// pass is for.
static float g_display_hz = KL_OVRP_REFRESH;

void kl_ovrp_set_display_frequency(float hz) {
    // A display frequency is a divisor in Unity's pacing math and a period
    // everywhere else. Anything outside the range a headset can actually run at
    // is a measurement that went wrong, and passing it on turns one bad number
    // into a frame loop that never settles.
    if (!(hz >= 30.0f) || !(hz <= 240.0f)) return;
    g_display_hz = hz;
}

float kl_ovrp_display_frequency(void) { return g_display_hz; }

static float klovrp_GetSystemDisplayFrequency(void) {
    ovrp_hit("ovrp_GetSystemDisplayFrequency");
    return g_display_hz;
}

// Packed return: width in the low 32 bits, height in the high (0x9bce58).
static uint64_t klovrp_GetEyeTextureSize(void) {
    ovrp_hit("ovrp_GetEyeTextureSize");
    return (uint64_t)KL_OVRP_EYE_W | ((uint64_t)KL_OVRP_EYE_H << 32);
}

static uint64_t klovrp_GetEyeTextureStageCount(void) {
    ovrp_hit("ovrp_GetEyeTextureStageCount");
    return 1;
}

// libunity maps 3->22, 2->2, anything else->4 (0x9bcf40), so any value is
// survivable; 2 keeps it on the mapping the real plugin produces.
static uint64_t klovrp_GetDesiredEyeTextureFormat(void) {
    ovrp_hit("ovrp_GetDesiredEyeTextureFormat");
    return 2;
}

// The frustum we tell the guest to render with, per eye: left, right, top,
// bottom tangents, all positive (cp_view_get_tangents order — see kl_ovrp.h).
// 1.0 everywhere is the coherent symmetric 90-degree frustum every host run has
// used; a frontend that knows the display's real field of view overwrites it
// through kl_ovrp_set_eye_frustum.
static float g_eye_tan[2][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}};

void kl_ovrp_set_eye_frustum(int eye, float left, float right, float top, float bottom) {
    if ((unsigned)eye > 1) return;
    // A zero or negative tangent is not a narrow frustum, it is a degenerate
    // one — libunity divides by their max for the aspect. Refuse rather than
    // pass it on; the default stays, which is at worst the wrong field of view
    // instead of a division by zero four layers down.
    if (!(left > 0) || !(right > 0) || !(top > 0) || !(bottom > 0)) return;
    g_eye_tan[eye][0] = left;  g_eye_tan[eye][1] = right;
    g_eye_tan[eye][2] = top;   g_eye_tan[eye][3] = bottom;
}

// The head->eye offsets, head-local metres, and the guest's whole source of
// stereo separation — see kl_ovrp.h. Zero is the historical host behaviour and
// stays the default; the visionOS compositor pushes the display's own numbers.
static float g_eye_off[2][3];

void kl_ovrp_set_eye_offset(int eye, float x, float y, float z) {
    if ((unsigned)eye > 1) return;
    g_eye_off[eye][0] = x; g_eye_off[eye][1] = y; g_eye_off[eye][2] = z;
}

// KL_OVRP_IPD=<metres>: force a symmetric separation, ignoring whatever the
// frontend pushed. The A/B for "is the compositor's number the wrong one" —
// and, with no frontend at all, the only way to get stereo out of a host run.
static void klovrp_eye_offset(int eye, float *ox, float *oy, float *oz) {
    static float forced = -1.0f;
    if (forced < 0.0f) {
        const char *e = getenv("KL_OVRP_IPD");
        float v = e ? (float)atof(e) : 0.0f;
        forced = (v > 0.0f && v < 0.2f) ? v : 0.0f;
    }
    if (forced > 0.0f) {
        *ox = (eye == 1 ? 0.5f : -0.5f) * forced; *oy = 0.0f; *oz = 0.0f;
        return;
    }
    *ox = g_eye_off[eye][0]; *oy = g_eye_off[eye][1]; *oz = g_eye_off[eye][2];
}

// Fills four f32 fov tangents at out+0x08..+0x14 (0x9bcbd4). libunity divides
// by their max for the aspect, so 0 is not survivable.
//
// The order here is ovrpFovf — UpTan, DownTan, LeftTan, RightTan — which is not
// the order the seam speaks, and this is the one place that transposition
// belongs: everything above the ABI uses Compositor Services' (left, right,
// top, bottom) so the compositor never has to remember two conventions. With
// the default symmetric frustum the two are indistinguishable, which is exactly
// why getting it wrong here would stay invisible until the day a real
// asymmetric field of view is pushed in.
//
// So the order is read, not assumed — out of the guest's own metadata, the same
// way the node ids and controller masks were. global-metadata.dat's string
// table has `zNear, zFar` adjacent (ovrpFrustum2f) and `UpTan, DownTan,
// LeftTan, RightTan` adjacent (ovrpFovf), in declaration order, which pins both
// the struct layout and this transposition.
static uint64_t klovrp_GetNodeFrustum2(int node, void *out) {
    ovrp_hit("ovrp_GetNodeFrustum2");
    // Nodes 0/1 are EyeLeft/EyeRight; anything else asking about a frustum
    // (EyeCenter, Head) gets the left eye's, which is what a symmetric or
    // near-symmetric display makes true enough to be honest.
    const float *t = g_eye_tan[node == 1 ? 1 : 0];
    float *f = (float *)out;
    f[2] = t[2];    // UpTan    <- top
    f[3] = t[3];    // DownTan  <- bottom
    f[4] = t[0];    // LeftTan  <- left
    f[5] = t[1];    // RightTan <- right
    return OVRP_SUCCESS;
}

// ovrpTrackingOrigin: EyeLevel=0, FloorLevel=1, Stage=2. This decides what
// space every pose we report is *in*, so it cannot stay a discarded argument
// on the generic yes-stub: under FloorLevel/Stage the guest expects the head
// to sit at standing eye height above y=0, and an identity (y=0) head puts the
// camera on the floor — the menu then renders above the view and the hands are
// below the floor, out of frustum. Recorded here and read by the pose defaults.
static int g_tracking_origin;          // 0 = eye level, until the guest says otherwise
int kl_ovrp_tracking_origin(void) { return g_tracking_origin; }
static uint64_t klovrp_SetTrackingOriginType(int origin) {
    ovrp_hit("ovrp_SetTrackingOriginType");
    ovrp_log_arg("ovrp_SetTrackingOriginType", origin, __builtin_return_address(0));
    g_tracking_origin = origin;
    return OVRP_TRUE;
}
static uint64_t klovrp_GetTrackingOriginType(int *out) {
    ovrp_hit("ovrp_GetTrackingOriginType");
    if (out) *out = g_tracking_origin;
    return OVRP_TRUE;
}

// The per-frame node loop's tracked/valid probes (0x9bbe78..0x9bbeb4): u32
// out-param, which is why these cannot be shared handlers. Tracked and valid
// is the honest answer for the head we pose ourselves.
//
// These return ovrpResult, NOT ovrpBool — the answer is the out-param and
// SUCCESS IS 0. Returning OVRP_TRUE (1) here read as a failure code, and the
// two callers disagree about how much that matters: libunity's node loop only
// reads the out-param, so its nodes tracked fine and this stayed invisible,
// while managed OVRPlugin checks `result == Result.Success` first and answers
// false without ever looking at the out-param. That is what made
// OVRInput.GetControllerPositionValid false with the controllers connected and
// tracked — and with it Beat Saber's controller poses, its laser and every UI
// hit, since VRController takes a pose only from a valid node.
static uint64_t node_tracked(const char *name, int n, uint32_t *out) {
    ovrp_hit(name);
    ovrp_log_arg(name, n, __builtin_return_address(0));
    *out = 1;
    return OVRP_SUCCESS;
}
static uint64_t klovrp_GetNodePositionTracked2(int n, uint32_t *o) {
    return node_tracked("ovrp_GetNodePositionTracked2", n, o);
}
static uint64_t klovrp_GetNodePositionValid(int n, uint32_t *o) {
    return node_tracked("ovrp_GetNodePositionValid", n, o);
}
static uint64_t klovrp_GetNodeOrientationValid(int n, uint32_t *o) {
    return node_tracked("ovrp_GetNodeOrientationValid", n, o);
}

// The head pose the frontend last gave us (kl_ovrp_set_head_pose — the seam
// declared in kl_ovrp.h). Identity by default, so a headless run sees exactly
// what it always saw.
typedef struct { float px, py, pz, qx, qy, qz, qw; } klovrp_pose;
static klovrp_pose g_head_pose = {
    0, 0, 0, 0, 0, 0, 1,
};
static int g_head_set;              // has a frontend ever written a head pose?

// --- The frontend/guest pose handoff, and why it is a seqlock now -----------
//
// These used to be plain unsynchronised stores, on the argument that a torn
// read costs one frame of staleness and the frontend rewrites it next frame
// anyway. That argument was wrong in a way that did not matter yet, and
// PLANNING §12.12 is what makes it matter:
//
//  - A torn read is not a *stale* pose, it is an *incoherent* one — half of one
//    frame's rotation with half of another's, a pose that never existed.
//  - Until the guest was decoupled from the compositor thread, that could
//    barely happen: on device the writer and the reader were the same thread,
//    so the reader could only see a fully-written value because it *was* the
//    writer. That is no longer true of either frontend.
//  - And the consequence grew. Reprojection *subtracts* the pose a frame was
//    rendered with from the pose it is displayed at (kl_reproject.c), so an
//    incoherent latch is a wrong delta, and a wrong delta is a visible jump
//    rather than a shrug.
//
// A seqlock rather than a mutex, because the readers are on the guest's hot
// path — every ovrp_GetNodePoseState — while the writer is one thread at
// display rate. The retry count is **bounded**: past it the read is taken
// unsynchronised, which is exactly what this code did before, so a descheduled
// writer degrades to the old behaviour instead of spinning inside a frame.
static uint32_t g_pose_seq;         // even = stable, odd = a write in flight

static klovrp_pose klovrp_pose_read(const klovrp_pose *src) {
    for (int try = 0; try < 8; try++) {
        uint32_t s = __atomic_load_n(&g_pose_seq, __ATOMIC_ACQUIRE);
        if (s & 1u) continue;
        klovrp_pose v = *src;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_pose_seq, __ATOMIC_RELAXED) == s) return v;
    }
    return *src;
}

// One sequence counter for the whole handoff, which is sound because there is
// exactly one writer: the frontend samples every pose for a frame from a single
// thread (the compositor's render loop, the viewer's UI thread) and pushes them
// through here. A second writer thread would need a real lock, so if one ever
// appears, this is the comment it invalidates.
static void klovrp_pose_write(klovrp_pose *dst, const klovrp_pose *v) {
    uint32_t s = __atomic_load_n(&g_pose_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pose_seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *dst = *v;
    __atomic_store_n(&g_pose_seq, s + 2, __ATOMIC_RELEASE);
}

// Standing eye height, and why the default head cannot be the origin.
// The guest sets ovrp_SetTrackingOriginType(FloorLevel) — measured, see that
// function — so y=0 is the *floor*, not the eye. An identity head under
// FloorLevel is a camera lying on the ground: Beat Saber's menu is authored at
// eye height, so it renders above the view, and anything parked near y=0 (the
// hands) is a metre and a half below the frustum. Under EyeLevel the eye *is*
// the origin, so the offset is zero and nothing moves.
static float klovrp_eye_height(void) {
    static int init;
    static float h = 1.6f;
    if (!init) {
        init = 1;
        const char *e = getenv("KL_OVRP_EYE_HEIGHT");
        if (e) h = strtof(e, NULL);
    }
    return g_tracking_origin == 0 ? 0.0f : h;
}

// The head pose as reported: the frontend's, or a synthesized one standing at
// eye height when no frontend has spoken.
static klovrp_pose klovrp_head(void) {
    klovrp_pose h = klovrp_pose_read(&g_head_pose);
    if (!__atomic_load_n(&g_head_set, __ATOMIC_ACQUIRE)) h.py = klovrp_eye_height();
    return h;
}

// v' = q ⊗ v ⊗ q⁻¹ for a unit quaternion, expanded — used to carry a
// head-relative offset into tracking space.
static void klovrp_qrot(const klovrp_pose *q, float vx, float vy, float vz,
                        float *ox, float *oy, float *oz) {
    float tx = 2.0f * (q->qy * vz - q->qz * vy);
    float ty = 2.0f * (q->qz * vx - q->qx * vz);
    float tz = 2.0f * (q->qx * vy - q->qy * vx);
    *ox = vx + q->qw * tx + (q->qy * tz - q->qz * ty);
    *oy = vy + q->qw * ty + (q->qz * tx - q->qx * tz);
    *oz = vz + q->qw * tz + (q->qx * ty - q->qy * tx);
}

void kl_ovrp_set_head_pose(float px, float py, float pz,
                           float qx, float qy, float qz, float qw) {
    klovrp_pose v = { px, py, pz, qx, qy, qz, qw };
    klovrp_pose_write(&g_head_pose, &v);
    __atomic_store_n(&g_head_set, 1, __ATOMIC_RELEASE);
}

void kl_ovrp_get_head_pose(float *px, float *py, float *pz,
                           float *qx, float *qy, float *qz, float *qw) {
    klovrp_pose h = klovrp_head();
    if (px) *px = h.px; if (py) *py = h.py; if (pz) *pz = h.pz;
    if (qx) *qx = h.qx; if (qy) *qy = h.qy;
    if (qz) *qz = h.qz; if (qw) *qw = h.qw;
}

// --- Timewarp bookkeeping ---------------------------------------------------
// See kl_ovrp.h for what this is for and why it is keyed to the stage. The
// consumer is kl_reproject.c / the two compositors.
//
// Locked, unlike the pose above. The pose is written and read every frame by
// two threads and a torn read costs one frame of staleness; a *record* is
// different — half of one frame's rotation with half of another's is a pose
// that never existed, and reprojecting against it would produce a visible jump
// rather than a shrug. The lock is taken twice per frame per side and is
// uncontended in practice.
#define KLOVRP_MAX_STAGES 4
static struct {
    kl_ovrp_render_pose r[KLOVRP_MAX_STAGES];
    uint64_t            serial;         // frames begun
    int                 last_complete;  // stage of the last completed frame, -1 = none
    pthread_mutex_t     mu;
} g_frames = { .last_complete = -1, .mu = PTHREAD_MUTEX_INITIALIZER };

// How many swapchain stages we tell the guest it has. Raising this is a
// deliberate act with a consequence: the stage a frame draws into is derived
// below from our own frame counter, which is only *known* to agree with
// libunity's own choice while there is exactly one stage to choose. Measure
// which stage the guest actually renders into before raising it.
#define KLOVRP_STAGES 1

// libunity's frame dispatcher (0x9bb808) calls BeginFrame, EndEye2 twice, then
// EndFrame. This is where the pose the frame will be rendered with is fixed:
// the frontend wrote it before driving this frame, and every
// ovrp_GetNodePoseState the guest is about to make will answer the same thing.
//
// The argument is the guest's own frame index, and it is deliberately ignored:
// our own counter is what the ring is indexed by, so a guest that numbers
// frames differently — or restarts them — cannot desynchronise the ring from
// the records in it.
static uint64_t klovrp_BeginFrame(int guest_frame_index) {
    ovrp_hit("ovrp_BeginFrame");
    (void)guest_frame_index;
    klovrp_pose h = klovrp_head();
    pthread_mutex_lock(&g_frames.mu);
    uint64_t s = ++g_frames.serial;
    int stage = (int)((s - 1) % KLOVRP_STAGES);
    kl_ovrp_render_pose *r = &g_frames.r[stage];
    r->px = h.px; r->py = h.py; r->pz = h.pz;
    r->qx = h.qx; r->qy = h.qy; r->qz = h.qz; r->qw = h.qw;
    // The frustum is recorded per frame rather than read live by the
    // compositor, because a frontend may push a new one at any time: a picture
    // rendered with the old field of view must keep being placed with the old
    // field of view, or it is resized by a change that happened after it.
    memcpy(r->tangents, g_eye_tan, sizeof r->tangents);
    r->serial = s;
    r->stage = stage;
    r->complete = 0;
    pthread_mutex_unlock(&g_frames.mu);
    return OVRP_SUCCESS;
}

// The guest has finished submitting this frame's eyes. Only now is the stage
// safe for a compositor to sample; before it, the record describes a picture
// that is still being drawn.
static uint64_t klovrp_EndFrame(int guest_frame_index) {
    ovrp_hit("ovrp_EndFrame");
    (void)guest_frame_index;
    pthread_mutex_lock(&g_frames.mu);
    if (g_frames.serial) {
        int stage = (int)((g_frames.serial - 1) % KLOVRP_STAGES);
        g_frames.r[stage].complete = 1;
        g_frames.last_complete = stage;
    }
    pthread_mutex_unlock(&g_frames.mu);
    return OVRP_SUCCESS;
}

int kl_ovrp_stage_render_pose(int stage, kl_ovrp_render_pose *out) {
    if ((unsigned)stage >= KLOVRP_MAX_STAGES || !out) return 0;
    pthread_mutex_lock(&g_frames.mu);
    int have = g_frames.r[stage].serial != 0;
    if (have) *out = g_frames.r[stage];
    pthread_mutex_unlock(&g_frames.mu);
    return have;
}

int kl_ovrp_last_complete_stage(void) {
    pthread_mutex_lock(&g_frames.mu);
    int s = g_frames.last_complete;
    pthread_mutex_unlock(&g_frames.mu);
    return s;
}

// --- M7: the two hands ------------------------------------------------------
// Node ids and enum values are not invented: they are read out of the guest's
// own global-metadata.dat (the OVRPlugin C# it was compiled against):
//   Node:      EyeLeft=0 EyeRight=1 EyeCenter=2 HandLeft=3 HandRight=4
//              TrackerZero=5..TrackerThree=8 Head=9
//   Controller: LTouch=1 RTouch=2 Touch=3 Remote=4 Gamepad=0x10
//               LHand=0x20 RHand=0x40 Hands=0x60 Active=0x80000000
// Default hand poses ride a head-relative offset (resolved in
// klovrp_GetNodePoseState), so controllers render somewhere sensible before any
// frontend starts driving them — and stay in the frustum wherever the head is.
// These were absolute tracking-space coordinates until the FloorLevel origin
// was measured, which put them below the floor and out of view.
static klovrp_pose g_hand_pose[2];
static int g_hand_set[2];

// Buttons/touches are ovrpButton/ovrpTouch bit values straight from the
// metadata (Button: One=1 Two=2 Start=0x100 PrimaryIndexTrigger=0x2000
// PrimaryHandTrigger=0x4000 PrimaryThumbstick=0x8000 ..., and the Secondary*
// block at <<20-ish for the other hand — the frontend hands us final bits).
static struct {
    uint32_t buttons, touches, neartouches;
    float    index_trigger, hand_trigger, stick_x, stick_y;
} g_input[2];

void kl_ovrp_set_hand_pose(int hand, float px, float py, float pz,
                           float qx, float qy, float qz, float qw) {
    if ((unsigned)hand > 1) return;
    klovrp_pose v = { px, py, pz, qx, qy, qz, qw };
    klovrp_pose_write(&g_hand_pose[hand], &v);
    __atomic_store_n(&g_hand_set[hand], 1, __ATOMIC_RELEASE);
}

void kl_ovrp_set_controller_input(int hand, uint32_t buttons, uint32_t touches,
                                  float index_trigger, float hand_trigger,
                                  float stick_x, float stick_y) {
    if ((unsigned)hand > 1) return;
    g_input[hand].buttons = buttons;
    g_input[hand].touches = touches;
    g_input[hand].neartouches = touches;   // capacitive proximity ~ touch
    g_input[hand].index_trigger = index_trigger;
    g_input[hand].hand_trigger = hand_trigger;
    g_input[hand].stick_x = stick_x;
    g_input[hand].stick_y = stick_y;
}

// KL_OVRP_HANDS_SWEEP=1: collapse both hands onto the head and sweep their
// pitch from -70 to +70 degrees in 5-degree steps, holding each step long
// enough for the KL_OVRP_FAKE_TRIGGER duty cycle to complete two presses.
//
// It exists because the ray a controller casts is NOT the pose we report:
// Beat Saber's IVRPlatformHelper.AdjustControllerTransform rotates the
// controller transform by a device-specific offset (a Touch controller does
// not point along its tracked forward axis), and that offset is game data we
// cannot read. A sweep does not need to know it — if any pitch produces a UI
// hit, the offset is the only unknown left; if none does over 140 degrees, the
// ray is not the problem and the controller never reaches the raycaster.
// Pair it with KL_OVRP_FAKE_TRIGGER=1 and watch for the menu advancing.
// KL_OVRP_DUMP_VRDEVICE=1: dump libunity's own Oculus VRDevice object once.
// The pointer lives at a fixed vaddr in this build (libunity+0x127a6c0 — the
// `ldr x8, [x?, #1728]` every VRDevice method starts with), and its first three
// words are the unique device ids libunity stamps into both the joystick
// descriptors (0x9bd3fc/0x9bd60c) and the XR node states (0x9bbf60..0x9bbf98):
// [+0] left controller, [+4] right controller, [+8] HMD. Zero ids mean Unity
// never allocated the controller devices, which is the difference between "the
// game ignores our controllers" and "there are no controllers to ignore".
// Read-only, and only when asked for — this is a build-specific address.
static void klovrp_dump_vrdevice(void) {
    static int done;
    if (done) return;
    if (!getenv("KL_OVRP_DUMP_VRDEVICE")) { done = 1; return; }
    kl_image *img = kl_find_image("libunity.so");
    if (!img) { fprintf(stderr, "  [ovrp] VRDevice: no libunity image\n"); done = 1; return; }
    unsigned char *base = kl_base(img);
    if (!base || kl_span(img) < 0x127a6c8) {
        fprintf(stderr, "  [ovrp] VRDevice: base=%p span=0x%zx\n", (void *)base, kl_span(img));
        done = 1; return;
    }
    void *obj = *(void **)(base + 0x127a000 + 1728);
    if (!obj) return;                    // not built yet — try again next frame
    done = 1;
    const uint32_t *w = obj;
    fprintf(stderr, "  [ovrp] VRDevice %p: idLeft=%u idRight=%u idHmd=%u\n",
            obj, w[0], w[1], w[2]);
    // Name every function-pointer slot by matching it against what we handed
    // back from kl_ovrp_sym. This is the VRDevice's whole contract with the
    // plugin in one place: which entry point sits behind each `ldr x8, [x?,
    // #N] / blr x8` in the disassembly, so a status predicate we answer wrong
    // can be found by name instead of by chasing offsets.
    void *const *slot = (void *const *)obj;
    for (size_t off = 0; off + 8 <= 768; off += 8) {
        void *fn = slot[off / 8];
        if (!fn) continue;
        for (unsigned i = 0; i < g_nsym; i++)
            if (g_sym[i].ptr == fn) {
                fprintf(stderr, "  [ovrp]   +%-4zu %s\n", off, g_sym[i].name);
                break;
            }
    }
}

static void klovrp_hand_sweep(const klovrp_pose *head, klovrp_pose *hand) {
    static int on = -1;
    if (on < 0) on = getenv("KL_OVRP_HANDS_SWEEP") != NULL;
    if (!on) return;
    static unsigned polls;
    // ~2 hand-pose polls a frame from libunity's node loop, so 512 polls is
    // ~256 frames a step — two full presses of the trigger's 64-frame period.
    // 29 steps is ~7400 frames, so a long run sweeps more than once and at
    // least one full sweep happens after the menu is up.
    unsigned step = (polls++ / 512u) % 29u;
    float deg = -70.0f + 5.0f * (float)step;
    static unsigned last_step = ~0u;
    if (step != last_step) {
        last_step = step;
        fprintf(stderr, "  [ovrp] hand sweep: pitch %+.0f deg\n", (double)deg);
    }
    float half = deg * 0.5f * 3.14159265f / 180.0f;
    float sx = sinf(half), cw = cosf(half);
    // q = q_head ⊗ q_pitch, with q_pitch a rotation about the head's local X.
    hand->qx = head->qw * sx + head->qx * cw;
    hand->qy = head->qy * cw + head->qz * sx;
    hand->qz = head->qz * cw - head->qy * sx;
    hand->qw = head->qw * cw - head->qx * sx;
    hand->px = head->px; hand->py = head->py; hand->pz = head->pz;
}

// out-struct via the sret register x8, NOT x2: the real function returns the
// 88-byte ovrpPoseStatef by value (its own prologue does `mov x19, x8` and an
// 0x58-byte memcpy back), so callers place the destination in x8 and the
// declared args are just (w0=step, w1=nodeId). x8 must be captured before any
// call — see kl_ovrp_sret.S for why the entry point is an assembly thunk and
// `out` arrives here as an ordinary parameter. Fields libunity consumes:
// +0x00 quat xyzw (w at +0x0c), +0x10 position, +0x1c velocity,
// +0x28 acceleration, +0x34 angular velocity, +0x40 angular acceleration.
// Node 9 (Head) and the eyes/trackers report the frontend head pose; nodes
// 3/4 (HandLeft/HandRight) report their own poses.
uint64_t klovrp_GetNodePoseState_impl(int step, int node, void *out) {
    ovrp_hit("ovrp_GetNodePoseState");
    ovrp_log_arg("ovrp_GetNodePoseState", node, __builtin_return_address(0));
    memset(out, 0, 0x58);
    klovrp_pose head = klovrp_head();
    const klovrp_pose *p = &head;
    klovrp_pose hand, eye;
    if (node == 0 || node == 1) {
        // EyeLeft/EyeRight. The head pose displaced by this eye's own offset,
        // rotated into the world by the head's orientation — this pair IS the
        // guest's IPD (kl_ovrp.h). Node 2 (EyeCenter) and node 9 (Head) keep
        // the head pose itself, which is what they mean.
        float dx, dy, dz, ox, oy, oz;
        klovrp_eye_offset(node, &dx, &dy, &dz);
        klovrp_qrot(&head, dx, dy, dz, &ox, &oy, &oz);
        eye = head;
        eye.px = head.px + ox; eye.py = head.py + oy; eye.pz = head.pz + oz;
        p = &eye;
    } else if (node == 3 || node == 4) {
        int h = node - 3;
        // KL_OVRP_HANDS_IN_VIEW=1: park both hands at a fixed spot well inside
        // the *current head's* frustum, overriding whatever the frontend last
        // wrote. Answers one question and only one — does the guest draw
        // controllers at all? Read side, so it beats the viewer's per-frame
        // writes. Diagnostic: the hands do not move with your real hands.
        //
        // This offset is head-relative on purpose. It used to be absolute
        // tracking-space coordinates chosen for an identity head, and the guest
        // asks for a FloorLevel origin — so the "in view" park was 12 cm below
        // the floor and 1.7 m under the head, i.e. reliably out of frustum.
        // The knob meant to prove the controllers render was hiding them.
        static int inview = -1;
        if (inview < 0) inview = getenv("KL_OVRP_HANDS_IN_VIEW") != NULL;
        if (inview || !__atomic_load_n(&g_hand_set[h], __ATOMIC_ACQUIRE)) {
            float dx = inview ? (h ? 0.15f : -0.15f) : (h ? 0.20f : -0.20f);
            float dy = inview ? -0.12f : -0.30f;
            float dz = inview ? -0.55f : -0.35f;
            float ox, oy, oz;
            klovrp_qrot(&head, dx, dy, dz, &ox, &oy, &oz);
            hand = head;
            hand.px = head.px + ox;
            hand.py = head.py + oy;
            hand.pz = head.pz + oz;
        } else {
            hand = klovrp_pose_read(&g_hand_pose[h]);
        }
        klovrp_dump_vrdevice();
        klovrp_hand_sweep(&head, &hand);
        p = &hand;
    }
    float *f = out;
    f[0] = p->qx; f[1] = p->qy;      // quat xyz at +0x00
    f[2] = p->qz; f[3] = p->qw;      // quat w at +0x0c
    f[4] = p->px; f[5] = p->py;      // position at +0x10
    f[6] = p->pz;
    return OVRP_SUCCESS;
}

// ovrpVector3f by value — a 12-byte HFA, so the floats go home in s0..s2,
// which a uint64 return would never set. Returning a three-float struct from
// C makes Clang emit exactly the real plugin's `ldp s0, s1 / ldr s2 / ret`.
// Zeros are also what the real plugin returns when no boundary exists.
typedef struct { float x, y, z; } klovrp_vec3;
static klovrp_vec3 klovrp_GetBoundaryDimensions(void) {
    ovrp_hit("ovrp_GetBoundaryDimensions");
    return (klovrp_vec3){0, 0, 0};
}

// Controller masks (metadata OVRPlugin.Controller): LTouch=1, RTouch=2,
// Touch=3, Remote=4, Gamepad=0x10, LHand=0x20, RHand=0x40, Hands=0x60,
// Active=0x80000000. Only the Touch controllers exist here, so a mask asking
// for anything else (Remote/Gamepad/Hands) connects nothing. Active expands
// to the controllers that are present.
#define OVRP_CTRL_LTOUCH 0x1u
#define OVRP_CTRL_RTOUCH 0x2u
#define OVRP_CTRL_ACTIVE 0x80000000u

// ovrpControllerState prefix, shared by all three versions (field order from
// the guest's own metadata — OVRInput.ControllerState{,2,4}):
//   +0x00 u32 ConnectedControllers   +0x10 f32 LIndexTrigger
//   +0x04 u32 Buttons                +0x14 f32 RIndexTrigger
//   +0x08 u32 Touches                +0x18 f32 LHandTrigger
//   +0x0C u32 NearTouches            +0x1C f32 RHandTrigger
//   +0x20 vec2 LThumbstick           +0x28 vec2 RThumbstick
// v2 appends +0x30/+0x38 vec2 L/RTouchpad; v4 appends +0x40.. bytes
// L/RBatteryPercentRemaining, L/RRecenterCount, then 28 reserved.
// KL_OVRP_FAKE_BUTTONS=<hex>: OR this mask into both hands' Buttons and
// Touches, toggling on/off on a duty cycle so GetDown/GetUp transitions
// actually occur — a held-from-boot bit never reads as a press.
//
// Which bits reach the game, measured from libunity's joystick fill
// (0x9bd338 left / 0x9bd548 right) against this title's InputManager axes:
//   Buttons 0x1 (A)   -> joystick button 0  = MenuButtonRightHand
//   Buttons 0x2 (B)   -> joystick button 1
//   Buttons 0x100 (X) -> joystick button 2  = MenuButtonLeftHand
//   Buttons 0x200 (Y) -> joystick button 3
//   Buttons 0x400 (LThumbstick) -> button 8 = MenuButtonLeftHandOculusTouch
//   Buttons 0x4   (RThumbstick) -> button 9 = MenuButtonRightHandOculusTouch
//   Buttons 0x100000 (Start)    -> button 7
// Every other bit — including the raw trigger bits 0x04000000/0x08000000/
// 0x10000000/0x20000000 — is read by nobody: libunity carries the triggers as
// *float axes*, never as buttons. So this knob cannot produce a UI click, and
// a "nothing reacts with 0xffffffff" result says nothing about the trigger.
// That is what KL_OVRP_FAKE_TRIGGER below is for.
static unsigned klovrp_fake_phase(void) {
    // ~7 controller polls a frame, so 256 on / 256 off is roughly a 0.4 s
    // press at 90 Hz — slow enough for a UI to see both edges.
    static unsigned calls;
    return (calls++ >> 8) & 1;
}

static uint32_t klovrp_fake_buttons(void) {
    static int init;
    static uint32_t mask;
    if (!init) {
        init = 1;
        const char *e = getenv("KL_OVRP_FAKE_BUTTONS");
        if (e) mask = (uint32_t)strtoul(e, NULL, 0);
    }
    if (!mask) return 0;
    return klovrp_fake_phase() ? mask : 0;
}

// KL_OVRP_FAKE_TRIGGER=<0..1>: drive both index triggers to this value on the
// same duty cycle. This is the click, not a button: Beat Saber's
// VRControllersInputManager reads Input.GetAxis("TriggerLeftHand"/"...Right"),
// which the InputManager asset binds to joystick axes 8 and 9 — libunity fills
// those from LIndexTrigger/RIndexTrigger, the floats. It is the only way to
// exercise a menu click without the interactive viewer.
static float klovrp_fake_trigger(void) {
    static int init;
    static float v;
    if (!init) {
        init = 1;
        const char *e = getenv("KL_OVRP_FAKE_TRIGGER");
        if (e) v = *e ? strtof(e, NULL) : 1.0f;
    }
    if (v <= 0.0f) return 0.0f;
    return klovrp_fake_phase() ? v : 0.0f;
}

static void fill_controller_state(int mask, void *out, int version) {
    memset(out, 0, version == 4 ? 0x60 : version == 2 ? 0x40 : 0x30);
    uint32_t m = (uint32_t)mask;
    if (m & OVRP_CTRL_ACTIVE) m |= OVRP_CTRL_LTOUCH | OVRP_CTRL_RTOUCH;
    uint32_t conn = m & (OVRP_CTRL_LTOUCH | OVRP_CTRL_RTOUCH);
    uint32_t fake = conn ? klovrp_fake_buttons() : 0;
    float fake_trig = conn ? klovrp_fake_trigger() : 0.0f;
    uint8_t *b = out;
    uint32_t *w = (uint32_t *)out;
    float *f = (float *)out;
    w[0] = conn;                                        // ConnectedControllers
    if (conn & OVRP_CTRL_LTOUCH) {
        w[1] |= g_input[0].buttons;                     // Buttons
        w[2] |= g_input[0].touches;                     // Touches
        w[3] |= g_input[0].neartouches;                 // NearTouches
        f[4] = g_input[0].index_trigger;                // LIndexTrigger
        f[6] = g_input[0].hand_trigger;                 // LHandTrigger
        f[8] = g_input[0].stick_x; f[9] = g_input[0].stick_y;
        if (version >= 2) { f[12] = g_input[0].stick_x; f[13] = g_input[0].stick_y; }
        if (version >= 4) b[0x40] = 100;                // LBatteryPercentRemaining
    }
    if (conn & OVRP_CTRL_RTOUCH) {
        w[1] |= g_input[1].buttons;
        w[2] |= g_input[1].touches;
        w[3] |= g_input[1].neartouches;
        f[5] = g_input[1].index_trigger;                // RIndexTrigger
        f[7] = g_input[1].hand_trigger;                 // RHandTrigger
        f[10] = g_input[1].stick_x; f[11] = g_input[1].stick_y;
        if (version >= 2) { f[14] = g_input[1].stick_x; f[15] = g_input[1].stick_y; }
        if (version >= 4) b[0x41] = 100;                // RBatteryPercentRemaining
    }
    w[1] |= fake;                                       // Buttons
    w[2] |= fake;                                       // Touches
    if (fake_trig > 0.0f) {
        if (conn & OVRP_CTRL_LTOUCH) f[4] = fake_trig;  // LIndexTrigger
        if (conn & OVRP_CTRL_RTOUCH) f[5] = fake_trig;  // RIndexTrigger
    }
}

// 64-byte ovrpControllerState2 by value via x8 (real plugin: stp q0..q3 of
// zeros on the failure path).
uint64_t klovrp_GetControllerState2_impl(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerState2");
    ovrp_log_arg("ovrp_GetControllerState2", mask, __builtin_return_address(0));
    fill_controller_state(mask, out, 2);
    return OVRP_SUCCESS;
}

// Same state, newest shape: (mask=w0, out=x1), 0x60 bytes (real plugin's
// memcpy size), plain ovrpResult. This is the one the game itself polls
// (OVRP_1_16_0::ovrp_GetControllerState4 via P/Invoke).
static uint64_t klovrp_GetControllerState4(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerState4");
    ovrp_log_arg("ovrp_GetControllerState4", mask, __builtin_return_address(0));
    fill_controller_state(mask, out, 4);
    return OVRP_SUCCESS;
}

// Same shape, 48 bytes (real plugin: three q-stores). v1 of the above; this
// is the one libunity's legacy VRDevice polls once a frame with mask=Touch.
uint64_t klovrp_GetControllerState_impl(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerState");
    ovrp_log_arg("ovrp_GetControllerState", mask, __builtin_return_address(0));
    fill_controller_state(mask, out, 1);
    return OVRP_SUCCESS;
}

// 24-byte ovrpHapticsDesc by value via x8; the real plugin zeroes it exactly
// like this when haptics are unavailable, and we have no haptics backend.
uint64_t klovrp_GetControllerHapticsDesc_impl(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerHapticsDesc");
    memset(out, 0, 0x18);
    return OVRP_SUCCESS;
}

// ovrpResult with a bool OUT-PARAM (OVRP_1_18_0), NOT the bare ovrpBool its
// name-mates ovrp_GetAppHasVrFocus/ovrp_GetUserPresent use. It sat in the
// bool-yes list returning 1, and managed OVRPlugin reads that as
//
//     Result result = ovrp_GetAppHasInputFocus(out inputFocus);
//     if (Result.Success == result) return inputFocus == Bool.True;
//     return false;                       // <- 1 is not Success (0)
//
// so `OVRPlugin.hasInputFocus` was false while we believed we were answering
// yes. Beat Saber hides its menu controllers while input focus is away — it
// has inputFocusWasCaptured/Released events for exactly this — which switched
// off the MenuControllers object and with it both saber hilts, both lasers and
// every UI raycast. hasVrFocus stayed true throughout, which is what made the
// pair look healthy.
//
// Safe to give an out-param because ONLY libil2cpp references this name;
// libunity does not (checked against both binaries). ovrp_GetAppHasVrFocus is
// referenced by both and really is a bare bool, so it stays where it is.
static uint64_t klovrp_GetAppHasInputFocus(char *out) {
    ovrp_hit("ovrp_GetAppHasInputFocus");
    if (out) *out = 1;
    return OVRP_SUCCESS;
}

// ovrpResult with a bool OUT-PARAM (real plugin: ldrb/str w8 to [x0],
// -1001 on NULL). We accepted ovrp_SetAppAsymmetricFov at init, so the
// read-back says enabled — one device story, like the headset above.
static uint64_t klovrp_GetAppAsymmetricFov(char *out) {
    ovrp_hit("ovrp_GetAppAsymmetricFov");
    *out = 1;
    return OVRP_SUCCESS;
}

// (float *buf, int *count) -> int count (real plugin: second arg
// null-checked to -1001, return clamped to >= 0). One frequency, matching
// the GetSystemDisplayFrequency answer above.
static uint64_t klovrp_GetSystemDisplayAvailableFrequencies(float *buf, int *count) {
    ovrp_hit("ovrp_GetSystemDisplayAvailableFrequencies");
    // One rate, and it is the one we report as current. Offering a menu of
    // frequencies we cannot actually switch between would invite the guest to
    // ask for one — ovrp_SetSystemDisplayFrequency is not implemented, and a
    // list is a promise.
    if (buf) buf[0] = g_display_hz;
    *count = 1;
    return 1;
}

// ovrpResult with an enum OUT-PARAM. ovrpXrApiType: 0 Unknown, 1 Oculus
// (legacy VrApi), 2 OpenXR. This APK's plugin is the VrApi build — no OpenXR
// string exists anywhere in it — so 1 is the only coherent answer.
static uint64_t klovrp_GetNativeXrApiType(int *out) {
    ovrp_hit("ovrp_GetNativeXrApiType");
    *out = 1;
    return OVRP_SUCCESS;
}

// The two-attempt contract (libunity 0x9bbb48/0x9bbb9c): attempt 1 passes
// handle=0 and must FAIL; the retry passes Unity's own GL texture name
// (measured: 0x18/0x1a — the very names the eye FBOs then attach). Storage
// for that name arrives from nowhere else: Unity never calls
// glTexImage2D/glTexStorage2D for the eye color textures in the whole trace,
// and the real plugin cannot (it imports no storage calls — its VrApi does
// it internally). So the plugin's job on the retry is to allocate storage
// for the texture Unity hands down, on the context current right here
// (Unity's render thread — this call arrives inside
// IVRDeviceCallback_CreateEyeTextureResources). fmt=2 maps to sRGB, matching
// the eye-sized color textures Unity allocates for itself (0x8c43).
#include "kl_egl.h"
#include "kl_glfb.h"      // kl_glfb_note_eye_texture — the capture's eye-FBO seam
// GL_RGBA16F: Unity renders the scene into an RGBA16F MSAA renderbuffer
// (measured: fmt 0x881a, samples=4, via the blit probe), and ES 3.0 makes a
// float->unorm blit INVALID_OPERATION — the eye texture must be float to
// match. This is what the guest's fmt=2 means in practice.
#define KL_OVRP_TEXFMT_EYE  0x881A
static uint64_t klovrp_SetupEyeTexture2(int eye, int stage, uintptr_t handle,
                                        int w, int h, int depth, int fmt, void *ctx) {
    ovrp_hit("ovrp_SetupEyeTexture2");
    if (!handle) return 0;
    static void (*gl_BindTexture)(uint32_t, uint32_t);
    static void (*gl_TexStorage2D)(uint32_t, int32_t, uint32_t, int32_t, int32_t);
    if (!gl_BindTexture) {
        gl_BindTexture   = kl_egl_sym("glBindTexture");
        gl_TexStorage2D  = kl_egl_sym("glTexStorage2D");
    }
    fprintf(stderr, "  [ovrp] SetupEyeTexture2(eye=%d stage=%d) -> tex=%zu %dx%d\n",
            eye, stage, (size_t)handle, w, h);
    // The capture reads the frame back from the FBO this texture is attached
    // to — tell kl_glfb which names are eyes (it is a no-op consumer when the
    // null driver is doing the "rendering").
    kl_glfb_note_eye_texture(eye, (uint32_t)handle);
    // P5: when the host has MTLTextures for the compositor to sample, the eye
    // texture's storage IS one of them — glEGLImageTargetTexture2DOES in place of
    // glTexStorage2D, and nothing else about this function changes (PLANNING
    // §12.9). The h,w transposition below applies identically, so it is passed on
    // in the same order.
    //
    // With no provider registered — every host run, so `make check` too — this is
    // a single NULL test and the GL path below is unchanged.
    if (kl_glfb_has_mtl_provider() &&
        kl_glfb_bind_eye_mtl_texture(eye, stage, (uint32_t)handle, h, w,
                                     KL_OVRP_TEXFMT_EYE))
        return 1;
    if (gl_BindTexture && gl_TexStorage2D) {
        gl_BindTexture(0x0DE1 /* GL_TEXTURE_2D */, (uint32_t)handle);
        // Allocate h-by-w, not w-by-h: the guest's own eye-resolve blit writes
        // a (0,0)-(h,w) region (measured: rb 2198x2304, blit rect 2198x2304,
        // args w=2304 h=2198), so the texture must be h wide and w tall or the
        // blit clips — losing a strip of the picture and leaving an
        // unwritten column of stale garbage at the right edge (the "narrow
        // vertical line" in the viewer). Whether the real signature orders
        // these h,w or Unity pre-transposes, the blit rect is ground truth.
        gl_TexStorage2D(0x0DE1, 1, KL_OVRP_TEXFMT_EYE, h, w);
    }
    return 1;
}

// libunity reads the RETURNED pointer's +0x118/+0x11c first and early-outs
// when they are zero (0x9bcc58) — an out-param it is not: returning 0 here
// segfaulted at [x0+0xcc]. A zeroed static struct is the complete honest
// answer: no perf stats, because there is no compositor producing them.
static uint64_t klovrp_GetAppPerfStats(void) {
    static char stats[0x120];
    ovrp_hit("ovrp_GetAppPerfStats");
    return (uint64_t)(uintptr_t)stats;
}

// Optional depth-compositing probe (slot null-checked, 0x9bced4). We do not
// composite depth, so both the return and the out-int say so.
static uint64_t klovrp_GetDepthCompositingSupported(int *out) {
    ovrp_hit("ovrp_GetDepthCompositingSupported");
    *out = 0;
    return 0;
}

// Unity calls this on every native plugin it loads, handing over its
// IUnityInterfaces registry. The real OVRPlugin uses it to grab
// IUnityGraphicsVulkan/GLES; we record it and do nothing, which is correct until
// there is a renderer to bind to (PLANNING M5/M6).
static void klovrp_UnityPluginLoad(void *unity_interfaces) {
    fprintf(stderr, "  [ovrp] UnityPluginLoad(%p) — recorded; no graphics device "
                    "bound yet\n", unity_interfaces);
}
static void klovrp_UnityPluginUnload(void) {}

static const char g_ovrp_handle[] = "klepton-ovrplugin";

// Assembly entry thunks that capture the x8 sret pointer before any call can
// clobber it (kl_ovrp_sret.S); the _impl bodies are above.
void klovrp_GetNodePoseState_entry(void);
void klovrp_GetControllerState2_entry(void);
void klovrp_GetControllerHapticsDesc_entry(void);
void klovrp_GetControllerState_entry(void);

int kl_ovrp_claims(const char *soname) {
    if (!soname) return 0;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    // Both spellings occur: Unity asks the ClassLoader for "OVRPlugin" and then
    // dlopens whatever path came back.
    return strcmp(b, "libOVRPlugin.so") == 0 || strcmp(b, "OVRPlugin") == 0;
}

void *kl_ovrp_dlopen(const char *soname) {
    if (!kl_ovrp_claims(soname)) return NULL;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    fprintf(stderr, "  [ovrp] guest dlopen(\"%s\") -> synthetic OVRPlugin "
                    "(the real one NEEDs libvrapi.so; see PLANNING 3.1)\n", b);
    return (void *)g_ovrp_handle;
}

int kl_ovrp_is_handle(const void *h) { return h == (const void *)g_ovrp_handle; }

static const struct { const char *name; void *fn; } g_ovrp_impl[] = {
    {"UnityPluginLoad",   (void *)klovrp_UnityPluginLoad},
    {"UnityPluginUnload", (void *)klovrp_UnityPluginUnload},
    {"ovrp_GetVersion",   (void *)klovrp_GetVersion},
    {"ovrp_GetSystemHeadsetType", (void *)klovrp_GetSystemHeadsetType},
    {"ovrp_GetSystemProductName", (void *)klovrp_GetSystemProductName},
    {"ovrp_GetSystemDisplayFrequency", (void *)klovrp_GetSystemDisplayFrequency},
    {"ovrp_GetEyeTextureSize", (void *)klovrp_GetEyeTextureSize},
    {"ovrp_GetEyeTextureStageCount", (void *)klovrp_GetEyeTextureStageCount},
    {"ovrp_GetDesiredEyeTextureFormat", (void *)klovrp_GetDesiredEyeTextureFormat},
    {"ovrp_SetTrackingOriginType", (void *)klovrp_SetTrackingOriginType},
    {"ovrp_GetTrackingOriginType", (void *)klovrp_GetTrackingOriginType},
    {"ovrp_GetNodeFrustum2", (void *)klovrp_GetNodeFrustum2},
    // Real implementations only so that the frame boundary can be *observed* —
    // both still answer ovrpSuccess and neither has an out-param. This is the
    // timewarp bookkeeping (kl_ovrp.h): the pose the guest is about to render
    // with, latched against the stage that frame draws into.
    {"ovrp_BeginFrame", (void *)klovrp_BeginFrame},
    {"ovrp_EndFrame", (void *)klovrp_EndFrame},
    {"ovrp_GetNodePositionTracked2", (void *)klovrp_GetNodePositionTracked2},
    {"ovrp_GetNodePositionValid", (void *)klovrp_GetNodePositionValid},
    {"ovrp_GetNodeOrientationValid", (void *)klovrp_GetNodeOrientationValid},
    {"ovrp_GetNodePoseState", (void *)klovrp_GetNodePoseState_entry},
    {"ovrp_GetAppPerfStats", (void *)klovrp_GetAppPerfStats},
    {"ovrp_GetBoundaryDimensions", (void *)klovrp_GetBoundaryDimensions},
    {"ovrp_GetControllerState2", (void *)klovrp_GetControllerState2_entry},
    {"ovrp_GetControllerState", (void *)klovrp_GetControllerState_entry},
    {"ovrp_GetControllerState4", (void *)klovrp_GetControllerState4},
    {"ovrp_GetAppAsymmetricFov", (void *)klovrp_GetAppAsymmetricFov},
    {"ovrp_GetAppHasInputFocus", (void *)klovrp_GetAppHasInputFocus},
    {"ovrp_GetNativeXrApiType", (void *)klovrp_GetNativeXrApiType},
    {"ovrp_GetSystemDisplayAvailableFrequencies", (void *)klovrp_GetSystemDisplayAvailableFrequencies},
    {"ovrp_SetupEyeTexture2", (void *)klovrp_SetupEyeTexture2},
    {"ovrp_GetControllerHapticsDesc", (void *)klovrp_GetControllerHapticsDesc_entry},
    {"ovrp_GetDepthCompositingSupported", (void *)klovrp_GetDepthCompositingSupported},
};

// Entry points answered by one of the shared handlers above. Each is reached
// through the same per-name trampoline as the aborting handler, so x0 is the
// entry point's own name and kl_ovrp_report still counts them individually.
//
// Ignoring the arguments is ABI-safe whatever the real arity: under AAPCS64 the
// caller passes in registers and cleans up after itself, so a callee that reads
// none of them and returns a scalar cannot corrupt anything. That is what makes a
// shared handler viable without knowing all 466 signatures. It stops being true
// the moment an entry point has an *out-parameter* — those must know where the
// pointer is and what shape it points at, so they get real implementations.
static const char *const g_ovrp_result_ok[] = {
    // Unity's native plugin interface. All void.
    "UnitySetGraphicsDevice", "UnitySetEventQueue", "UnityShaderCompilerExtEvent",
    "UnityRenderingExtEvent",
    // Bring-up. This is the decision recorded in PLANNING M6: we answer success
    // and stand behind it, rather than reporting a failure Unity would be right
    // to believe.
    "ovrp_PreInitialize", "ovrp_Initialize5",
    // Configuration the guest sets and never reads back.
    "ovrp_SetAppAsymmetricFov",
    // Called with an out-pointer (void**) it may write; libunity pre-zeroes
    // the local and ignores the x0 return (0x9bb334-0x9bb414), and never
    // dereferences whatever lands in the slot — so leaving it untouched and
    // returning 0 stores NULL, which is the truthful "no native SDK here".
    "ovrp_GetNativeSDKPointer2",
    // The frame lifecycle (0x9bb808 dispatcher: BeginFrame, EndEye2 x2,
    // EndFrame) and the one-shot Update2 at the end of init (0x9bb52c) and
    // reconfigure (0x9bce3c). All have their return ignored by the guest and
    // take no out-params. BeginFrame and EndFrame have moved to real
    // implementations above — they answer the same ovrpSuccess, but they are
    // where the timewarp bookkeeping is latched.
    "ovrp_Update2", "ovrp_EndEye2",
    "ovrp_RecenterTrackingOrigin",
    // Thread-scheduling hints from PlayerSettings, set once at init; void
    // configuration like the setters above.
    "ovrp_AutoThreadScheduling", "ovrp_SetThreadPerformance",
    // Called even with texture-array support answered 0 — recorded state,
    // like the other setters.
    "ovrp_SetEyeTextureArrayEnabled",
    // Teardown; return ignored (0x9bbbf4).
    "ovrp_DestroyEyeTexture",
    // Pushed by the C# side despite GetDepthCompositingSupported=0; recorded
    // state, like the other setters.
    "ovrp_SetDepthProjInfo",
    // Performance-level hints from OVRManager; recorded, not applied — same
    // class as Process.setThreadPriority in kl_jni.
    "ovrp_SetSystemCpuLevel", "ovrp_SetSystemGpuLevel",
    // Color-space hints from the C# side; recorded state, like the setters.
    "ovrp_SetClientColorDesc",
    // Audio device ids — PC-legacy queries; NULL until the guest proves it
    // dereferences the answer.
    "ovrp_GetAudioOutId", "ovrp_GetAudioInId", "ovrp_GetDisplayAdapterId",
    // Managed-side Media facade init + MRC configuration; ovrpResult/void.
    "ovrp_Media_Initialize", "ovrp_Media_SetMrcAudioSampleRate",
    "ovrp_Media_SetMrcInputVideoBufferType", "ovrp_Media_GetMrcInputVideoBufferType",
    "ovrp_Media_SetMrcActivationMode",
    // 8-byte state struct home in x0; zero = no haptic samples queued or
    // playing, which is true.
    "ovrp_GetControllerHapticsState",
};

static const char *const g_ovrp_bool_yes[] = {
    // bool-returning setters. libunity's OculusVRDevice::Initialize (in the
    // guest, at 0x9bb1fc/0x9bb220/0x9bb2e8) requires each of these to return
    // 1 and deletes the VR device otherwise — answering 0 here is how the run
    // lost the device silently after ovrp_Initialize5. Success is the honest
    // answer: the scale/flip is recorded state in the real plugin, and
    // refusing it would only desync Unity from what we report elsewhere.
    "ovrp_SetEyeTextureScale", "ovrp_SetEyeViewportScale",
    "ovrp_SetEyeTextureFlippedY",
    // Per-frame predicates (0x9bbe58 gate; focus gates whether Unity renders
    // at all). The head is present and the app is focused: true.
    "ovrp_GetNodePresent", "ovrp_GetAppHasVrFocus", "ovrp_GetUserPresent",
    // The OVRP_0_1_2 shape of the tracked predicates: these return ovrpBool
    // DIRECTLY, where the ...Tracked2/...Valid forms above take a u32
    // out-param and return ovrpResult — same question, two ABIs, and only the
    // out-param pair was answered. Managed OVRPlugin reaches for these from
    // OVRInput.GetControllerPositionTracked, so a controller-tracking query
    // from the game's own C# (not libunity's node loop) landed on the
    // unimplemented trampoline and aborted the run.
    "ovrp_GetNodePositionTracked", "ovrp_GetNodeOrientationTracked",
    // We answered ovrp_Initialize5 with success and stand behind it.
    // NOT ovrp_GetAppHasInputFocus — that one is ovrpResult + out-param, see
    // klovrp_GetAppHasInputFocus below.
    "ovrp_GetInitialized",
    // Agrees with ovrp_Media_Initialize's success above.
    "ovrp_Media_GetInitialized",
    // Setup calls whose return the guest ignores (0x9bba0c, 0x9bcd5c); 1 for
    // consistency with the other "it worked" answers.
    "ovrp_SetupDistortionWindow3", "ovrp_SetupDisplayObjects",
    // Haptics commands we accept and drop — no haptics backend yet (M8).
    "ovrp_SetControllerHaptics", "ovrp_SetControllerVibration",
};

static const char *const g_ovrp_bool_no[] = {
    // Unity asks OVRPlugin which rendering-extension hooks it wants (before/after
    // rendering events, etc.). Our replacement has no render-thread bookkeeping,
    // so "no" is the truthful answer — Unity then never issues the events.
    "UnityRenderingExtQuery",
    // Events that must never fire on a healthy run (0x9bbdf8 acts on
    // recenter; quit/recreate tear things down).
    "ovrp_GetAppShouldRecenter", "ovrp_GetAppShouldQuit",
    "ovrp_GetAppShouldRecreateDistortionWindow",
    // Capabilities we do not have: no eye texture arrays, no multiview in
    // our GL gateway, no preview-rect override (return 0 = skip, 0x9bcf9c).
    "ovrp_GetEyeTextureArraySupported", "ovrp_GetSystemMultiViewSupported",
    "ovrp_GetEyePreviewRect",
    // No Guardian here — bool return (real plugin maps failure to false).
    "ovrp_GetBoundaryGeometry2",
    // No occlusion mesh data exists on this host (bool return).
    "ovrp_GetEyeOcclusionMesh",
    // No fixed-foveated/tiled multires rendering in our GL gateway.
    "ovrp_GetTiledMultiResSupported",
};

static void *klovrp_shared(const char *name) {
    for (size_t i = 0; i < sizeof g_ovrp_result_ok / sizeof g_ovrp_result_ok[0]; i++)
        if (strcmp(g_ovrp_result_ok[i], name) == 0)
            return kl_named_stub(name, (void *)klovrp_ok);
    for (size_t i = 0; i < sizeof g_ovrp_bool_yes / sizeof g_ovrp_bool_yes[0]; i++)
        if (g_ovrp_bool_yes[i] && strcmp(g_ovrp_bool_yes[i], name) == 0)
            return kl_named_stub(name, (void *)klovrp_yes);
    for (size_t i = 0; i < sizeof g_ovrp_bool_no / sizeof g_ovrp_bool_no[0]; i++)
        if (g_ovrp_bool_no[i] && strcmp(g_ovrp_bool_no[i], name) == 0)
            return kl_named_stub(name, (void *)klovrp_no);
    return NULL;
}

void *kl_ovrp_sym(const char *name) {
    if (!name) return NULL;
    ovrp_slot(name);          // before the impl check, so "resolved" counts everything
    void *fn = kl_ovrp_sym_inner(name);
    if (fn && g_nsym < KL_OVRP_MAX) {
        pthread_mutex_lock(&g_ovrp_mu);
        unsigned i = 0;
        for (; i < g_nsym; i++) if (g_sym[i].ptr == fn) break;
        if (i == g_nsym && g_nsym < KL_OVRP_MAX) {
            g_sym[g_nsym].name = strdup(name);
            g_sym[g_nsym].ptr = fn;
            g_nsym++;
        }
        pthread_mutex_unlock(&g_ovrp_mu);
    }
    return fn;
}

static void *kl_ovrp_sym_inner(const char *name) {
    for (size_t i = 0; i < sizeof g_ovrp_impl / sizeof g_ovrp_impl[0]; i++)
        if (strcmp(g_ovrp_impl[i].name, name) == 0) return g_ovrp_impl[i].fn;
    void *shared = klovrp_shared(name);
    if (shared) return shared;
    return kl_named_stub(name, (void *)klovrp_called);
}

void kl_ovrp_report(FILE *f) {
    static int done;
    if (done || !g_novrp) return;
    done = 1;
    unsigned called = 0;
    for (unsigned i = 0; i < g_novrp; i++) if (g_ovrp[i].calls) called++;
    fprintf(f, "\n=== OVRPlugin surface (M6 work list) ===\n");
    fprintf(f, "  resolved: %u, of which called: %u\n", g_novrp, called);
    fprintf(f, "  --- called ---\n");
    for (unsigned i = 0; i < g_novrp; i++)
        if (g_ovrp[i].calls) fprintf(f, "    %-44s x%u\n", g_ovrp[i].name, g_ovrp[i].calls);
    fprintf(f, "  --- resolved but never called ---\n");
    for (unsigned i = 0; i < g_novrp; i++)
        if (!g_ovrp[i].calls) fprintf(f, "    %s\n", g_ovrp[i].name);
}
