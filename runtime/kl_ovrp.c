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
#include <time.h>
#include "klepton.h"
#include "kl_ovrp.h"
// kl_glfb_note_eye_texture (the capture's eye-FBO seam) and
// kl_glfb_last_render_stage (which stage the guest actually drew into — see
// klovrp_EndFrame). Up here rather than beside SetupEyeTexture2 now that the
// timewarp bookkeeping needs it too.
#include "kl_glfb.h"

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

// Reached through a per-name trampoline, so x0 is the entry point's own name.
// The stub tail-calls here, so this return value is the ovrp_ call's return
// value. ovrpSuccess is 0, which makes a permissive zero mean "it worked" —
// wrong in the usual way, but it is what collects the whole surface in one run.
static uint64_t klovrp_called(const char *name) {
    int s = ovrp_slot(name);
    if (s >= 0) g_ovrp[s].calls++;
    if (kl_permissive()) {
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
#define KL_OVRP_EYE_W    (1832*1.25)        // Quest 2 per-eye recommended render target
#define KL_OVRP_EYE_H    (1920*1.25)
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

int kl_ovrp_stage_count(void);

static uint64_t klovrp_GetEyeTextureStageCount(void) {
    ovrp_hit("ovrp_GetEyeTextureStageCount");
    return (uint64_t)kl_ovrp_stage_count();
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

// The eye's own ROTATION — the cant — which this seam used to drop on the floor.
//
// Vision Pro's displays are angled outward, so device_from_view is not a pure
// translation: each eye is turned, oppositely, and its frustum tangents are
// expressed in that turned frame (measured: l=1.7321 r=1.0000 for the left eye,
// mirrored for the right — tan 60 out, tan 45 in). We were handing the guest
// the turned frame's *tangents* while telling it the eye pointed straight
// ahead, so it rendered the right shape of cone in the wrong direction.
//
// The composite then placed that picture and viewed it through the real canted
// eye (kl_reproject.c's view_rot), which is the correct thing to do with the
// picture it was given — and the result on screen is each eye's image rotated
// by the cant, in opposite directions. Opposite per-eye rotation is the one
// error the visual system cannot merge: it is seen as **two images**, not as
// blur, which is what "doubling during head turns" was.
//
// Identity by default, so a host run and every headless test are unchanged.
// KL_OVRP_EYE_CANT=0 restores the dropped-cant behaviour as the A/B.
static float g_eye_rot[2][4] = { { 0, 0, 0, 1 }, { 0, 0, 0, 1 } };

void kl_ovrp_set_eye_rotation(int eye, float qx, float qy, float qz, float qw) {
    if ((unsigned)eye > 1) return;
    // A zero quaternion is not a rotation; treat it as "none" rather than
    // collapsing the eye's basis to nothing.
    if (!(qx * qx + qy * qy + qz * qz + qw * qw > 1e-6f)) return;
    g_eye_rot[eye][0] = qx; g_eye_rot[eye][1] = qy;
    g_eye_rot[eye][2] = qz; g_eye_rot[eye][3] = qw;
}

static int klovrp_eye_cant(void) {
    static int on = -1;
    if (on < 0) {
        on = kl_env_on("KL_OVRP_EYE_CANT", 0);
    }
    return on;
}


// KL_OVRP_IPD=<metres>: force a symmetric separation, ignoring whatever the
// frontend pushed. The A/B for "is the compositor's number the wrong one" —
// and, with no frontend at all, the only way to get stereo out of a host run.
static void klovrp_eye_offset(int eye, float *ox, float *oy, float *oz) {
    static float forced = -1.0f;
    if (forced < 0.0f) {
        float v = kl_env_float("KL_OVRP_IPD", 0.0f);
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
// Motion rides in the same struct as the pose, and that is the point: every
// latch in this file (the frontend seqlock, the per-frame pin, the per-step
// sample) copies a klovrp_pose by value, so velocity that lives here is
// automatically coherent with the orientation it belongs to. A parallel array
// would have to repeat all three latches and would eventually disagree with
// one of them. Zero for the head and the eyes, which have no velocity source.
typedef struct {
    float px, py, pz, qx, qy, qz, qw;
    float vx, vy, vz;        // linear velocity, m/s, tracking space
    float avx, avy, avz;     // angular velocity, rad/s, tracking-space axes
} klovrp_pose;
static klovrp_pose g_head_pose = {
    0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
};
static int g_head_set;              // has a frontend ever written a head pose?
// The two hands, published by the same frontend in the same breath. Declared
// here beside the head because the per-frame latch below promotes all three
// together — they are one sample of one instant and must stay so.
static klovrp_pose g_hand_pose[2];

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
        h = kl_env_float("KL_OVRP_EYE_HEIGHT", 1.6f);
    }
    return g_tracking_origin == 0 ? 0.0f : h;
}

// --- One pose per guest frame, and why the live one is wrong ---------------
//
// **This is what was left of the judder after the swapchain was fixed.**
//
// The frontend publishes a new pose every *display* frame, on its own thread
// (PLANNING §12.12). The guest reads poses through ovrp_GetNodePoseState
// whenever it likes during its own frame, and its frame is longer than a
// display frame whenever performance is short. So within one guest frame the
// answer to "where is the head" could change several times — and, worse, the
// pose recorded for timewarp (klovrp_BeginFrame, which latched the live value)
// was not necessarily any of the answers the guest was given.
//
// Reprojection subtracts the recorded pose from the display pose. If the guest
// rendered from P(T1) and we recorded P(T2), the correction is wrong by exactly
// P(T2) - P(T1): one guest frame of head rotation, applied backwards. A frame
// that is over-corrected followed by one that is under-corrected is not blur,
// it is **two images in two places** — the doubling seen on device during head
// rotation, growing as the frame rate falls, which is precisely when T2 - T1
// grows.
//
// So the guest gets ONE pose for the whole of its frame, promoted at its frame
// boundary, and that is the pose recorded. The record is then truthful by
// construction rather than by the two threads happening to be in step. This is
// what a real runtime does — a frame's poses come from one predicted instant —
// and it is the property kl_reproject.h's whole argument assumes.
//
// The hands come along for the ride: they are published in the same breath from
// the same ARKit query, and a frame that draws the head from one instant and
// the hands from another is incoherent in the same way, just less visibly.
// Controller *buttons* stay live — an edge is not a pose, and freshness there
// costs nothing.
//
// KL_OVRP_LATCH=0 restores the live read, which is every measurement taken
// before this and the A/B if the pinning is ever suspected of costing latency.
static klovrp_pose g_frame_head, g_frame_hand[2];
static uint32_t    g_frame_pose_seq;
static int         g_frame_latched;

static int klovrp_latch_enabled(void) {
    static int on = -1;
    if (on < 0) {
        on = kl_env_on("KL_OVRP_LATCH", 0);
    }
    return on;
}

// A seqlock of its own, because the writer is the *guest's* frame driver while
// g_pose_seq's writer is the frontend. Sharing one counter between two writer
// threads is exactly the bug that counter's comment warns about.
static klovrp_pose klovrp_seq_read(const klovrp_pose *src, const uint32_t *seq) {
    for (int try = 0; try < 8; try++) {
        uint32_t s = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
        if (s & 1u) continue;
        klovrp_pose v = *src;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(seq, __ATOMIC_RELAXED) == s) return v;
    }
    return *src;
}

// The head pose as PUBLISHED — where the frontend says the head is now. This is
// the display side's question, not the guest's: the compositor asks it to
// reproject towards, and the viewer asks it to drive its own composite.
static klovrp_pose klovrp_head_published(void) {
    klovrp_pose h = klovrp_pose_read(&g_head_pose);
    if (!__atomic_load_n(&g_head_set, __ATOMIC_ACQUIRE)) h.py = klovrp_eye_height();
    return h;
}

// The head pose as the GUEST sees it: pinned for the whole of its frame.
static klovrp_pose klovrp_head(void) {
    if (klovrp_latch_enabled() && __atomic_load_n(&g_frame_latched, __ATOMIC_ACQUIRE)) {
        klovrp_pose h = klovrp_seq_read(&g_frame_head, &g_frame_pose_seq);
        if (!__atomic_load_n(&g_head_set, __ATOMIC_ACQUIRE)) h.py = klovrp_eye_height();
        return h;
    }
    return klovrp_head_published();
}

// The shortest angle between two orientations, in degrees. Used only to report
// how much motion the latch is absorbing.
static float klovrp_quat_degrees(const klovrp_pose *a, const klovrp_pose *b) {
    float d = a->qx * b->qx + a->qy * b->qy + a->qz * b->qz + a->qw * b->qw;
    if (d < 0) d = -d;
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d) * (180.0f / 3.14159265358979f);
}

// Promote the published poses to the ones this guest frame will see. Called at
// the top of the guest's frame, before anything in it can ask.
//
// The number it reports is the measurement that justifies the whole mechanism:
// how far the head moved during the *previous* guest frame, i.e. how wrong the
// recorded pose used to be. At a comfortable frame rate it is a fraction of a
// degree; when the guest is struggling it is whole degrees, and a whole degree
// of mis-correction is plainly visible.
void kl_ovrp_frame_latch(void) {
    if (!klovrp_latch_enabled()) return;
    klovrp_pose h = klovrp_pose_read(&g_head_pose);
    klovrp_pose l = klovrp_pose_read(&g_hand_pose[0]);
    klovrp_pose r = klovrp_pose_read(&g_hand_pose[1]);

    static float worst;
    static unsigned n;
    if (__atomic_load_n(&g_frame_latched, __ATOMIC_ACQUIRE)) {
        float moved = klovrp_quat_degrees(&g_frame_head, &h);
        if (moved > worst) worst = moved;
    }
    if (++n % 300 == 0) {
        fprintf(stderr, "  [ovrp] pose latch: worst %.2f deg of head motion "
                        "inside one guest frame over the last 300\n", (double)worst);
        worst = 0;
    }

    uint32_t s = __atomic_load_n(&g_frame_pose_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_frame_pose_seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_frame_head = h; g_frame_hand[0] = l; g_frame_hand[1] = r;
    __atomic_store_n(&g_frame_pose_seq, s + 2, __ATOMIC_RELEASE);
    __atomic_store_n(&g_frame_latched, 1, __ATOMIC_RELEASE);
}

// The hand pose as the guest sees it — pinned with the head, for the same
// reason and at the same instant.
static klovrp_pose klovrp_hand(int hand) {
    if (klovrp_latch_enabled() && __atomic_load_n(&g_frame_latched, __ATOMIC_ACQUIRE))
        return klovrp_seq_read(&g_frame_hand[hand], &g_frame_pose_seq);
    return klovrp_pose_read(&g_hand_pose[hand]);
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

// The Hamilton product, a then b applied as "b first, then a" — so
// world_from_eye = world_from_device ⊗ device_from_view.
static klovrp_pose klovrp_qmul(const klovrp_pose *a, const float b[4]) {
    klovrp_pose r = *a;
    float ax = a->qx, ay = a->qy, az = a->qz, aw = a->qw;
    float bx = b[0],  by = b[1],  bz = b[2],  bw = b[3];
    r.qw = aw * bw - ax * bx - ay * by - az * bz;
    r.qx = aw * bx + ax * bw + ay * bz - az * by;
    r.qy = aw * by - ax * bz + ay * bw + az * bx;
    r.qz = aw * bz + ax * by - ay * bx + az * bw;
    return r;
}

void kl_ovrp_set_head_pose(float px, float py, float pz,
                           float qx, float qy, float qz, float qw) {
    // No velocity: DeviceAnchor does not report one, and the head's motion is
    // not a field the guest reads for node 9 anyway.
    klovrp_pose v = { px, py, pz, qx, qy, qz, qw, 0, 0, 0, 0, 0, 0 };
    klovrp_pose_write(&g_head_pose, &v);
    __atomic_store_n(&g_head_set, 1, __ATOMIC_RELEASE);
}

// The frontend's question — "where is the head NOW" — so it reads the published
// pose, not the one pinned for the guest's frame. The viewer's composite uses
// this as the pose to reproject *towards*, and reprojecting towards the pose
// the picture was already drawn with would be a no-op.
void kl_ovrp_get_head_pose(float *px, float *py, float *pz,
                           float *qx, float *qy, float *qz, float *qw) {
    klovrp_pose h = klovrp_head_published();
    if (px) *px = h.px; if (py) *py = h.py; if (pz) *pz = h.pz;
    if (qx) *qx = h.qx; if (qy) *qy = h.qy;
    if (qz) *qz = h.qz; if (qw) *qw = h.qw;
}

// --- ovrp_Update2: the guest's own latch point ---------------------------
//
// **This is the concrete signal we had been guessing around.** OVRPlugin's
// contract is that tracking is sampled once per frame per *step* — the guest
// calls ovrp_Update2(step, frameIndex, predictionSeconds), and every
// ovrp_GetNodePoseState(step, node) afterwards returns that sample. Measured on
// this title: 85 Update2 calls across 38 frames (two steps a frame) and 760
// GetNodePoseState calls, i.e. twenty reads per frame off two samples.
//
// We were answering Update2 from the shared ignore-the-arguments handler and
// serving every read from a live global that the frontend rewrites at display
// rate. So the twenty reads inside one guest frame could return twenty
// different poses, the Render and Physics steps collapsed into one drifting
// value, and the pose recorded for timewarp was not necessarily any of the
// answers the guest was actually given. Reprojection subtracts that record —
// so the correction was against a pose that never rendered anything.
//
// Now the guest's own call is the boundary. Nothing here is inferred: the step,
// the frame index and the moment all come from the guest.
//
//   ovrpStep_Render = -1, ovrpStep_Physics = 0   (OVRPlugin.Step)
#define KLOVRP_STEP_RENDER (-1)
#define KLOVRP_NSTEPS 2
static inline int klovrp_step_ix(int step) { return step == KLOVRP_STEP_RENDER ? 0 : 1; }

static klovrp_pose g_step_head[KLOVRP_NSTEPS];
static klovrp_pose g_step_hand[KLOVRP_NSTEPS][2];
static uint32_t    g_step_seq;
static int         g_step_valid[KLOVRP_NSTEPS];
static int         g_saw_update2;

// The render step's sample, kept apart so BeginFrame can record exactly what
// the guest was told to render with rather than re-reading anything.
static klovrp_pose g_render_sample;

static uint64_t klovrp_Update2(int step, int frame_index, double prediction) {
    ovrp_hit("ovrp_Update2");
    (void)frame_index; (void)prediction;
    int ix = klovrp_step_ix(step);

    klovrp_pose h = klovrp_pose_read(&g_head_pose);
    if (!__atomic_load_n(&g_head_set, __ATOMIC_ACQUIRE)) h.py = klovrp_eye_height();
    klovrp_pose l = klovrp_pose_read(&g_hand_pose[0]);
    klovrp_pose r = klovrp_pose_read(&g_hand_pose[1]);

    // How much the head moved between this frame's sample and the last one for
    // the same step. This is the quantity the old code was silently absorbing
    // into the timewarp delta, so it is worth being able to read.
    static float worst;
    static unsigned n;
    if (ix == 0 && g_step_valid[0]) {
        float moved = klovrp_quat_degrees(&g_step_head[0], &h);
        if (moved > worst) worst = moved;
        if (++n % 300 == 0) {
            fprintf(stderr, "  [ovrp] Update2(Render): worst %.2f deg between "
                            "consecutive samples over the last 300\n", (double)worst);
            worst = 0;
        }
    }

    uint32_t s = __atomic_load_n(&g_step_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_step_seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_step_head[ix] = h;
    g_step_hand[ix][0] = l; g_step_hand[ix][1] = r;
    if (ix == 0) g_render_sample = h;
    __atomic_store_n(&g_step_seq, s + 2, __ATOMIC_RELEASE);
    __atomic_store_n(&g_step_valid[ix], 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_saw_update2, 1, __ATOMIC_RELEASE);

    // Return value deliberately UNCHANGED from the shared handler this replaces.
    // OVRPlugin declares ovrp_Update2 returning Bool, where 0 is false, and both
    // libunity and libil2cpp reference the name — which is trap 10's exact
    // shape. It may well want OVRP_TRUE. But it has answered 0 for every
    // measurement taken so far, and changing the latch and the return in one
    // step would make the next device run unreadable. One variable at a time.
    return OVRP_SUCCESS;
}

// The pose for a given step, as the guest was told it. Falls back to the live
// value only where Update2 has never been seen — a headless run, or a guest
// that does not use this part of the API.
static klovrp_pose klovrp_step_head(int step) {
    int ix = klovrp_step_ix(step);
    if (__atomic_load_n(&g_step_valid[ix], __ATOMIC_ACQUIRE))
        return klovrp_seq_read(&g_step_head[ix], &g_step_seq);
    return klovrp_head();
}

static klovrp_pose klovrp_step_hand(int step, int hand) {
    int ix = klovrp_step_ix(step);
    if (__atomic_load_n(&g_step_valid[ix], __ATOMIC_ACQUIRE))
        return klovrp_seq_read(&g_step_hand[ix][hand], &g_step_seq);
    return klovrp_hand(hand);
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
    // The frame between BeginFrame and EndFrame, whose stage is not known yet.
    kl_ovrp_render_pose pending;
    int                 pending_index;   // the GUEST's frame index
    uint64_t            stage_disagree;  // index%%N vs the observed stage
    // How the observation window closed, counted per frame. These are the
    // numbers that say whether the pose↔picture association is *known* or
    // merely available — see klovrp_EndFrame.
    uint64_t            unobserved;      // the frame drew into no eye stage
    uint64_t            multi;           // ...into more than one
    uint64_t            cross_thread;    // drawn by a thread that is not this one
    // Frames filed per stage, and how many took the counter fallback. The one
    // number that says whether the swapchain is really cycling: a second stage
    // that never receives a frame is memory spent to fix nothing, and the
    // read-while-writing race it exists to remove is still there.
    uint64_t            filed[KLOVRP_MAX_STAGES];
    uint64_t            guessed;
    uint64_t            serial;         // frames begun
    int                 last_complete;  // stage of the last completed frame, -1 = none
    pthread_mutex_t     mu;
} g_frames = { .last_complete = -1, .mu = PTHREAD_MUTEX_INITIALIZER };

// How many swapchain stages we tell the guest it has.
//
// **This was 1, and one stage is a bug, not a simplification.** With a single
// stage the guest renders every frame into the *same* eye texture — the one the
// compositor is sampling for the frame before it. There is a fence in one
// direction only (kl_glfb signals at eglSwapBuffers, the composite waits on it),
// so "the guest has finished frame N" is ordered but "the composite has finished
// reading frame N" is not: the guest's frame N+1 overwrites the texture while
// the composite still has it bound, across two Metal queues that order nothing
// between them. Standing still the two pictures are nearly identical and nothing
// shows. **Turning your head, they differ — and the composite samples a mixture,
// which is exactly the judder that should not be there.** It gets worse with
// resolution, because a longer guest frame overlaps the composite for longer.
//
// Two stages is enough here and not by luck: the guest is driven one frame per
// published pose (PLANNING §12.12), so it cannot run more than one frame ahead
// of the compositor, and one spare buffer covers exactly that. KL_OVRP_STAGES
// is the A/B — `=1` restores the single-buffered behaviour every measurement
// before this was taken against, `=3` buys slack at the cost of another
// full-size eye texture (RGBA16F, two slices — over 100 MB at the resolutions
// this now runs at, which is why 3 is not the default).
//
// The old comment here warned that raising this makes the stage a *guess*:
// "the stage a frame draws into is derived from our own frame counter, which is
// only known to agree with libunity's own choice while there is exactly one
// stage to choose." That warning was right, and the answer is not to guess but
// to measure — see klovrp_EndFrame, which files each frame's record under the
// stage kl_glfb watched the guest actually draw into.
//
// **The association is now proven rather than inferred** (PLANNING §12.19). The
// observation is windowed — BeginFrame opens it, EndFrame closes it — so a
// frame that drew into no eye stage is reported as such instead of silently
// receiving the previous frame's answer, which is the exact off-by-one that
// pairs a fresh pose with a stale picture. Measured on the host at two and three
// stages, 300 frames each: every frame drew into exactly one stage, on the same
// thread as EndFrame, and the guest's own frame index `% stages` agreed with the
// observation on all 298 of them. Nothing here is a guess any more.
//
// **The default is 3, and the doubling that forced it down to 1 is gone.**
// One stage single-buffers the eye textures: the guest's frame N+1 overwrites
// the texture while the composite still has frame N bound, across two Metal
// queues that order nothing between them, and turning your head that reads as
// tearing. Two stages removed it and revealed the doubling; three made the
// doubling worse, which is what identified the association as the cause. With
// the association fixed and proven (above), depth is free to be what it should
// be — and 3 rather than 2 because the guest is decoupled from the compositor
// (PLANNING §12.12) and one spare buffer only just covers that, leaving nothing
// for a frame that runs long.
//
// The cost is memory, and it is not small: an eye texture is RGBA16F with two
// array slices, so at map resolution each stage is on the order of 160 MB and a
// swapchain re-creation transiently holds two generations. KL_OVRP_STAGES is
// the A/B in both directions.
#define KLOVRP_STAGES_DEFAULT 3

int kl_ovrp_stage_count(void) {
    static int n;
    if (!n) {
        n = kl_env_int("KL_OVRP_STAGES", KLOVRP_STAGES_DEFAULT);
        if (n < 1) n = 1;
        if (n > KLOVRP_MAX_STAGES) n = KLOVRP_MAX_STAGES;
    }
    return n;
}

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
    // The RENDER step's sample — the pose the guest was handed for this frame
    // and rendered every eye from. Not a fresh read: a fresh read at this
    // moment is a pose the picture was never drawn with, and reprojection
    // subtracts whatever is recorded here.
    klovrp_pose h = __atomic_load_n(&g_saw_update2, __ATOMIC_ACQUIRE)
                    ? klovrp_step_head(KLOVRP_STEP_RENDER) : klovrp_head();
    pthread_mutex_lock(&g_frames.mu);
    uint64_t s = ++g_frames.serial;
    // Into the PENDING record, not into a stage. Which stage this frame goes to
    // is the guest's choice and is not knowable yet — it is read off the draw
    // target at EndFrame. Filing it here under a guessed stage is what the old
    // one-stage code got away with and what more than one stage would break.
    kl_ovrp_render_pose *r = &g_frames.pending;
    r->px = h.px; r->py = h.py; r->pz = h.pz;
    r->qx = h.qx; r->qy = h.qy; r->qz = h.qz; r->qw = h.qw;
    // The frustum is recorded per frame rather than read live by the
    // compositor, because a frontend may push a new one at any time: a picture
    // rendered with the old field of view must keep being placed with the old
    // field of view, or it is resized by a change that happened after it.
    memcpy(r->tangents, g_eye_tan, sizeof r->tangents);
    r->serial = s;
    r->stage = -1;
    r->complete = 0;
    // Unity's own frame counter — the number it picks its stage from.
    g_frames.pending_index = guest_frame_index;
    pthread_mutex_unlock(&g_frames.mu);
    // Open the observation window. Everything the guest binds as a draw target
    // from here until EndFrame is what THIS frame committed to; a sticky
    // "last stage" without a window answers with the previous frame's when this
    // one drew nothing, and that answer is indistinguishable from a right one.
    kl_glfb_begin_render_window();
    return OVRP_SUCCESS;
}

// The guest has finished submitting this frame's eyes. Only now is the stage
// safe for a compositor to sample; before it, the record describes a picture
// that is still being drawn — and only now is the stage even *known*.
//
// The stage comes from kl_glfb, which watched which eye texture the guest bound
// as its draw target. It is not derived from our own frame counter any more:
// the counter agrees with the guest's cycle only by luck, and disagreeing means
// pairing one frame's picture with another frame's pose, which reprojection
// then "corrects" by a delta that was never real. Where the observation is
// unavailable — the null GL driver, `make check`, any run without KL_GLFB —
// the counter is still the fallback, and with a single stage it is exact.
static uint64_t klovrp_EndFrame(int guest_frame_index) {
    ovrp_hit("ovrp_EndFrame");
    // Close the observation window opened at BeginFrame. `observed` is still
    // the sticky answer; `mask`/`binds` are what say whether it belongs to this
    // frame, and they are the difference between an association that is known
    // and one that is merely plausible.
    uint32_t mask = 0, binds = 0;
    uint64_t draw_tid = 0;
    int observed = kl_glfb_render_stages(&mask, &binds, &draw_tid);
    int stages = kl_ovrp_stage_count();
    int nstages = __builtin_popcount(mask);

    // The three ways the window can close badly, each named the first time it
    // happens. All three are silent under a sticky global — that is the point.
    if (kl_glfb_has_mtl_provider()) {
        uint64_t self = 0;
        pthread_threadid_np(NULL, &self);
        static int said_none, said_multi, said_thread;
        if (binds == 0) {
            g_frames.unobserved++;
            // Not held under the lock: these are diagnostics on the guest's own
            // frame thread and a miscount is cheaper than a lock on this path.
            if (!said_none && observed >= 0) {
                said_none = 1;
                fprintf(stderr, "  [ovrp] frame %d drew into NO eye stage between "
                                "BeginFrame and EndFrame — it produced no new "
                                "picture, so its pose is DROPPED rather than filed "
                                "over a stage whose picture is older than it\n",
                        guest_frame_index);
            }
        } else if (nstages > 1) {
            g_frames.multi++;
            if (!said_multi) {
                said_multi = 1;
                fprintf(stderr, "  [ovrp] frame %d drew into %d stages (mask 0x%x) "
                                "— one pose per frame cannot describe that\n",
                        guest_frame_index, nstages, mask);
            }
        }
        if (binds && draw_tid && draw_tid != self) {
            g_frames.cross_thread++;
            if (!said_thread) {
                said_thread = 1;
                fprintf(stderr, "  [ovrp] eye draws are on thread %llu, EndFrame on "
                                "%llu — the window bounds nothing and the stage is "
                                "whatever that thread had reached\n",
                        (unsigned long long)draw_tid, (unsigned long long)self);
            }
        }
    }

    // Only worth saying when something is actually sampling by stage. Under the
    // null driver nothing renders and nothing composites, so an unobserved
    // stage is the expected state rather than a warning — and a warning that
    // fires on every diagnostic run is one nobody reads on the run that matters.
    if (observed < 0 && stages > 1 && kl_glfb_has_mtl_provider()) {
        // Loud, once. With one stage the counter is exact and this cannot
        // matter; with more than one it is a guess, and a wrong guess files
        // this frame's pose against the previous frame's picture — the same
        // mismatch multiple stages exist to remove. If this line appears, the
        // guest is attaching its eye textures through some entry point the
        // framebuffer thunks do not watch, and KL_OVRP_STAGES=1 is the way
        // back to a configuration that cannot be wrong.
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [ovrp] eye stage NOT observed — falling back to "
                            "the frame counter with %d stages; if the picture "
                            "judders, try KL_OVRP_STAGES=1\n", stages);
        }
    }
    // **A frame that drew into no eye stage must not file anything.**
    //
    // Measured on device (PLANNING §12.19): it happens, and it never happens on
    // the host, which is why every host measurement came back clean. Such a
    // frame produced no new picture — so every stage still holds an image from
    // an *earlier* frame, and writing this frame's pose over any of them makes
    // the compositor reproject an old picture by a new pose. That is a delta
    // that was never real, applied to one stage out of N, i.e. one frame in N
    // displaced and the rest correct: temporal doubling whose period is the
    // stage count. Dropping the record instead leaves every stage describing
    // the picture it actually holds, and the compositor shows the previous
    // frame again — which is what a frame that drew nothing *should* look like.
    //
    // Only when the observation has never worked at all (`observed < 0` — the
    // null GL driver, `make check`, any run without KL_GLFB) does the counter
    // remain the fallback. There, nothing composites and nothing reads these
    // records, so the old behaviour is preserved rather than reasoned about.
    int drop = binds == 0 && observed >= 0;
    pthread_mutex_lock(&g_frames.mu);
    if (g_frames.serial && !drop) {
        // In order of how much the answer is *known*:
        //   the window saw exactly one stage      — measured, this frame's
        //   the window saw several                — measured but ambiguous, take
        //                                           the last and count it
        //   the observation has never worked      — the counter, as before
        int stage;
        if (binds && nstages == 1)
            stage = __builtin_ctz(mask);
        else if (binds && observed >= 0 && observed < stages)
            stage = observed;
        else
            stage = (int)((g_frames.serial - 1) % (unsigned)stages);
        g_frames.pending.stage = stage;
        g_frames.pending.complete = 1;
        g_frames.r[stage] = g_frames.pending;
        g_frames.last_complete = stage;
        g_frames.filed[stage]++;
        if (!binds) g_frames.guessed++;

        // **The measurement that decides how the stage should be derived.**
        // Unity picks the stage it renders into from its own frame counter, and
        // hands us that counter here and at BeginFrame. If `index % stages`
        // agrees with the FBO we watched it draw into, then the index is the
        // concrete answer and the sticky observation can go — and if they
        // disagree, the difference IS the off-by-one that makes two stages
        // double. Either way this stops being inferred.
        int from_index = ((guest_frame_index % stages) + stages) % stages;
        if (stages > 1 && observed >= 0) {
            static unsigned n;
            if (from_index != stage) g_frames.stage_disagree++;
            if (n++ < 8 || (from_index != stage && g_frames.stage_disagree < 4))
                fprintf(stderr, "  [ovrp] stage: guest frame %d %% %d = %d, "
                                "observed %d%s\n", guest_frame_index, stages,
                        from_index, observed,
                        from_index == stage ? "" : "   <-- DISAGREE");
        }
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

// The association's health, live rather than at the end of the run.
//
// It belongs in the report too, but a device run normally ends by the immersive
// space being invalidated rather than by the guest finishing, so the report is
// the one thing that often does not get written. These go in the compositor's
// 2-second line, which always does.
void kl_ovrp_association_stats(uint64_t *dropped, uint64_t *multi,
                               uint64_t *cross, uint64_t *disagree) {
    pthread_mutex_lock(&g_frames.mu);
    if (dropped)  *dropped  = g_frames.unobserved;
    if (multi)    *multi    = g_frames.multi;
    if (cross)    *cross    = g_frames.cross_thread;
    if (disagree) *disagree = g_frames.stage_disagree;
    pthread_mutex_unlock(&g_frames.mu);
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
static int g_hand_set[2];

// Buttons/touches are ovrpButton/ovrpTouch bit values straight from the
// metadata (Button: One=1 Two=2 Start=0x100 PrimaryIndexTrigger=0x2000
// PrimaryHandTrigger=0x4000 PrimaryThumbstick=0x8000 ..., and the Secondary*
// block at <<20-ish for the other hand — the frontend hands us final bits).
static struct {
    uint32_t buttons, touches, neartouches;
    float    index_trigger, hand_trigger, stick_x, stick_y;
} g_input[2];

void kl_ovrp_set_hand_motion(int hand, float px, float py, float pz,
                             float qx, float qy, float qz, float qw,
                             float vx, float vy, float vz,
                             float avx, float avy, float avz) {
    if ((unsigned)hand > 1) return;
    klovrp_pose v = { px, py, pz, qx, qy, qz, qw, vx, vy, vz, avx, avy, avz };
    klovrp_pose_write(&g_hand_pose[hand], &v);
    __atomic_store_n(&g_hand_set[hand], 1, __ATOMIC_RELEASE);
}

void kl_ovrp_set_hand_pose(int hand, float px, float py, float pz,
                           float qx, float qy, float qz, float qw) {
    kl_ovrp_set_hand_motion(hand, px, py, pz, qx, qy, qz, qw, 0, 0, 0, 0, 0, 0);
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
    if (!kl_env_on("KL_OVRP_DUMP_VRDEVICE", 0)) { done = 1; return; }
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
    if (on < 0) on = kl_env_on("KL_OVRP_HANDS_SWEEP", 0);
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
    // The pose for the STEP the guest asked about, which is the sample it took
    // at ovrp_Update2 — not a live global re-read twenty times a frame. The
    // argument was being discarded; see klovrp_Update2.
    klovrp_pose head = klovrp_step_head(step);
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
        // ...and this eye's own orientation, not the head's. The eye is TURNED
        // on this display (see kl_ovrp_set_eye_rotation), and the frustum we
        // report for it through ovrp_GetNodeFrustum2 is expressed in that
        // turned frame — so a node pose carrying the head's orientation
        // describes an eye that does not exist, and the guest renders the right
        // cone of directions pointing the wrong way.
        eye = klovrp_eye_cant() ? klovrp_qmul(&head, g_eye_rot[node]) : head;
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
        if (inview < 0) inview = kl_env_on("KL_OVRP_HANDS_IN_VIEW", 0);
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
            hand = klovrp_step_hand(step, h);
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
    // Velocity at +0x1c and angular velocity at +0x34, both of which were left
    // zero until now. libunity copies all four vectors straight into its XR
    // node state, so a zero here is not "unknown", it is "not moving" — and
    // Unity's own code differentiates nothing, it trusts what the plugin says.
    // Acceleration (+0x28) and angular acceleration (+0x40) stay zero: no
    // tracker on this platform reports them, and a difference-of-differences
    // estimate off a 90 Hz pose stream is noise wearing a physical name.
    f[7]  = p->vx;  f[8]  = p->vy;  f[9]  = p->vz;
    f[13] = p->avx; f[14] = p->avy; f[15] = p->avz;
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
        mask = kl_env_uint("KL_OVRP_FAKE_BUTTONS", 0);
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
        v = kl_env_on("KL_OVRP_FAKE_TRIGGER", 0) ? 1.0f : 0.0f;
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

// ---------------------------------------------------------------------------
// M8 — haptics: the seam running the OTHER way
//
// Every other entry point in this file answers a question the guest asked.
// These three are the guest telling us to do something to the hardware, so the
// seam is inverted: the guest fills a queue here, and the frontend drains it
// once a frame (kl_ovrp_haptics_pull) and plays it on whatever it has.
//
// **This title drives the BUFFERED path, not the vibration one.** Its
// global-metadata.dat carries OVRHaptics / OVRHapticsClip / OVRHapticsOutput
// and the whole OVRHaptics.Config property set — SampleRateHz,
// SampleSizeInBytes, MinimumSafeSamplesQueued, MinimumBufferSamplesCount,
// OptimalBufferSamplesCount, MaximumBufferSamplesCount — which is Oculus's
// sample-stream API rather than the one-shot OVRInput.SetControllerVibration:
//
//   ovrp_GetControllerHapticsDesc  -- how fast, how wide, how deep. Read ONCE,
//                                     at OVRHaptics's static init.
//   ovrp_GetControllerHapticsState -- how much is still queued and how much
//                                     room is left. Every frame, per hand.
//   ovrp_SetControllerHaptics      -- here are N amplitude samples.
//
// **The descriptor is load-bearing, and zeroing it is not a neutral answer.**
// OVRHapticsOutput sizes its native sample buffer at MaximumBufferSamplesCount,
// paces itself to keep OptimalBufferSamplesCount queued, and clamps what it
// sends to the SamplesAvailable we report. A zeroed descriptor plus a zeroed
// state is therefore a coherent "this controller cannot vibrate": the managed
// side keeps queueing clips into a zero-length buffer and never calls
// SetControllerHaptics at all. That is what was happening, and it is why there
// was nothing downstream to implement until these two answered honestly.
//
// The numbers below are Touch's, which is the controller we claim to be
// everywhere else (Build.MODEL, ovrp_GetSystemHeadsetType) — 320 Hz, one
// unsigned byte per sample, buffers of 1..256 samples with 20 the pace target
// and 5 the don't-let-it-run-dry mark. Reporting a Sense controller's real
// capabilities instead would be more literally true and would put Unity's
// pacing maths somewhere this title has never been.
//
// **How to tell we read the two struct layouts the right way round**, which is
// the one thing here that is an ABI claim rather than a measurement: with the
// descriptor answered, SetControllerHaptics calls start arriving, and their
// SamplesCount lands at or below OptimalBufferSamplesCount (20). A transposed
// descriptor shows up immediately as counts of 1 or 256. For the state struct
// the failure is silent instead — swap SamplesAvailable and SamplesQueued and
// the guest computes "0 samples of room" forever and sends nothing — so that
// one has KL_HAPTICS_SWAP_STATE=1 as its A/B, and KL_HAPTICS_TRACE=1 prints
// both halves of the conversation.
#define KLOVRP_HAP_RATE     320        // samples per second
#define KLOVRP_HAP_MAX      256        // MaximumBufferSamplesCount, and our ring
#define KLOVRP_HAP_OPTIMAL  20         // OptimalBufferSamplesCount
#define KLOVRP_HAP_SAFE     5          // MinimumSafeSamplesQueued
#define KLOVRP_HAP_MINBUF   1          // MinimumBufferSamplesCount

// The shortest span worth waking the frontend's haptics engine for, and the
// longest one pulse may cover. The floor is ALVR's (0.032 s) and exists
// because the guest tops its queue up EVERY frame: at 90 Hz that is three or
// four new samples a time, and handing a CoreHaptics engine a stream of 11 ms
// events is a series of restarts rather than a buzz. We can afford to wait —
// the queue runs ~60 ms ahead of the sound, which is the whole point of a
// buffered API. The ceiling is ALVR's too, and is a safety rail: a single
// event that runs for half a second is already longer than any note cut.
#define KLOVRP_HAP_MIN_S    0.032f
#define KLOVRP_HAP_MAX_S    0.5f

// Deltas only, never compared against anything outside this file, so which
// Darwin monotonic clock this is does not matter — CACurrentMediaTime() on the
// frontend side is a different epoch and is deliberately not mixed in.
static double klovrp_mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// One ring per hand. The guest pushes from its own thread, the frontend pulls
// from the render thread, and GetControllerHapticsState is read from a third,
// so the mutex is not defensive.
static struct klovrp_haptics {
    pthread_mutex_t mu;
    uint8_t  s[KLOVRP_HAP_MAX];
    int      head;          // index of the next sample due to play
    int      count;         // samples queued, playing forward from head
    int      published;     // how many of those the frontend already has
    double   t_head;        // monotonic instant at which s[head] plays
    int      fed;           // did the guest queue anything since the last pull?
    // The legacy vibration path, kept SEPARATE from the ring above rather than
    // folded into it. It has to be: this title calls
    // ovrp_SetControllerVibration(mask, 0, 0) on both hands EVERY FRAME — an
    // idle "nothing should be buzzing" — while OVRHaptics feeds the ring from
    // the same managed frame. Serving the stop by clearing the queue would let
    // one path silently erase the other's clip sixty times a second, and the
    // symptom would be haptics that are merely intermittent rather than absent.
    float    vib_amp;       // 0 = not vibrating
    double   vib_until;     // when this vibration lapses if not refreshed
    double   vib_pub;       // vibration handed to the frontend up to this instant
    uint64_t pushes, samples, pulses;
    float    peak;
} g_hap[2] = {
    { .mu = PTHREAD_MUTEX_INITIALIZER }, { .mu = PTHREAD_MUTEX_INITIALIZER },
};

static int klovrp_hap_trace(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_HAPTICS_TRACE", 0);
    return on;
}

// Retire the samples whose moment has passed. This is what makes the state we
// report a real answer rather than a guess: the queue drains on the wall clock
// at exactly the rate we told the guest it would, so its own prediction of
// SamplesQueued (OVRHapticsOutput checks ours against it every frame) agrees.
// Call with the lock held.
static void klovrp_hap_drain(struct klovrp_haptics *h, double now) {
    if (h->count <= 0) {
        h->count = 0; h->published = 0; h->t_head = now;
        return;
    }
    double due = (now - h->t_head) * KLOVRP_HAP_RATE;
    if (due < 1.0) return;
    int adv = (int)due;
    if (adv > h->count) adv = h->count;
    h->head = (h->head + adv) % KLOVRP_HAP_MAX;
    h->count -= adv;
    h->published -= adv;
    if (h->published < 0) h->published = 0;
    h->t_head += (double)adv / KLOVRP_HAP_RATE;
    if (h->count == 0) h->t_head = now;
}

// Which hands a controller mask names. Active means "whatever is connected",
// and we report both Touch controllers connected, so it means both.
static int klovrp_hap_hands(int mask) {
    uint32_t m = (uint32_t)mask;
    if (m & OVRP_CTRL_ACTIVE) m |= OVRP_CTRL_LTOUCH | OVRP_CTRL_RTOUCH;
    // LHand/RHand (0x20/0x40) name the hand-tracking "controllers" the same
    // enum carries; they are the same two hands to us.
    int hands = 0;
    if (m & (OVRP_CTRL_LTOUCH | 0x20u)) hands |= 1;
    if (m & (OVRP_CTRL_RTOUCH | 0x40u)) hands |= 2;
    return hands;
}

static void klovrp_hap_enqueue(int hand, const uint8_t *s, int n) {
    struct klovrp_haptics *h = &g_hap[hand];
    double now = klovrp_mono();
    pthread_mutex_lock(&h->mu);
    klovrp_hap_drain(h, now);
    int dropped = 0;
    for (int i = 0; i < n; i++) {
        if (h->count >= KLOVRP_HAP_MAX) { dropped = n - i; break; }
        h->s[(h->head + h->count) % KLOVRP_HAP_MAX] = s[i];
        h->count++;
        if (s[i] / 255.0f > h->peak) h->peak = s[i] / 255.0f;
    }
    h->fed = 1;
    h->pushes++;
    h->samples += (uint64_t)(n - dropped);
    pthread_mutex_unlock(&h->mu);
    if (dropped && klovrp_hap_trace())
        fprintf(stderr, "  [ovrp] haptics: hand %d queue full, dropped %d "
                        "sample(s) — the guest ignored SamplesAvailable\n",
                hand, dropped);
}

// (mask = w0, HapticsBuffer by value = x1/x2). The buffer is
// { void *Samples; int SamplesCount; } — 16 bytes, so AAPCS64 passes it in two
// registers rather than by reference, which is why the count arrives as a
// second scalar and not through a pointer. Returns ovrpBool, not ovrpResult
// (managed OVRPlugin tests it against Bool.True).
static uint64_t klovrp_SetControllerHaptics(int mask, const void *samples,
                                            uint64_t count) {
    ovrp_hit("ovrp_SetControllerHaptics");
    int n = (int)(uint32_t)count;
    if (!samples || n <= 0) return 1;
    if (n > KLOVRP_HAP_MAX) n = KLOVRP_HAP_MAX;   // the guest was told the ceiling
    int hands = klovrp_hap_hands(mask);
    if (klovrp_hap_trace())
        fprintf(stderr, "  [ovrp] haptics: SetControllerHaptics(mask=0x%x) "
                        "%d sample(s), first=%u\n",
                (unsigned)mask, n, ((const uint8_t *)samples)[0]);
    if (hands & 1) klovrp_hap_enqueue(0, samples, n);
    if (hands & 2) klovrp_hap_enqueue(1, samples, n);
    return 1;
}

// The legacy level API: (mask = w0, frequency = s0, amplitude = s1), ovrpBool.
// A vibration set here runs until it is changed — OVRInput's own contract is
// that a caller which means to sustain one keeps calling — so what is recorded
// is a level and a lapse time, not a finite buffer.
//
// **Measured on this title: it arrives with amplitude 0 on both hands every
// single frame** (3138 calls across 3000 frames), i.e. as a per-frame "stop"
// rather than as the way anything is actually played. That is what forced the
// two sources apart; see the `vib_amp` comment on the struct.
//
// KL_HAPTICS_VIB_LAPSE=<seconds> is the ceiling on an un-refreshed vibration,
// default 2 s. The real API has one of about that; ours matters more, because a
// frontend that has already been handed a pulse cannot be told to stop.
//
// The frequency argument is dropped, deliberately. OVRPlugin's 0..1 is a
// selector between two fixed Touch motor rates; a Sense controller's second
// axis is CoreHaptics *sharpness*, which is not the same quantity, and there is
// no measurement here that would justify a mapping between them.
static uint64_t klovrp_SetControllerVibration(int mask, float frequency,
                                              float amplitude) {
    ovrp_hit("ovrp_SetControllerVibration");
    int hands = klovrp_hap_hands(mask);
    static float lapse = -1.0f;
    if (lapse < 0.0f) lapse = kl_env_float("KL_HAPTICS_VIB_LAPSE", 2.0f);
    float a = amplitude < 0.0f ? 0.0f : amplitude > 1.0f ? 1.0f : amplitude;
    double now = klovrp_mono();
    for (int hand = 0; hand < 2; hand++) {
        if (!(hands & (1 << hand))) continue;
        struct klovrp_haptics *h = &g_hap[hand];
        pthread_mutex_lock(&h->mu);
        float was = h->vib_amp;
        h->vib_amp = a;
        h->vib_until = a > 0.0f ? now + lapse : 0.0;
        // Starting from rest owes the frontend a pulse immediately; refreshing
        // an already-running vibration must NOT reset the accumulator or a
        // caller that re-asserts it every frame would re-trigger every frame.
        if (a > 0.0f && was <= 0.0f) h->vib_pub = now;
        pthread_mutex_unlock(&h->mu);
        // Traced on the EDGE only. The per-frame idle stop above would
        // otherwise bury every other line in this subsystem.
        if (klovrp_hap_trace() && (was > 0.0f) != (a > 0.0f))
            fprintf(stderr, "  [ovrp] haptics: hand %d vibration %s "
                            "(freq=%.2f, amp=%.2f)\n",
                    hand, a > 0.0f ? "on" : "off",
                    (double)frequency, (double)a);
    }
    return 1;
}

// ovrpHapticsState is { int SamplesAvailable; int SamplesQueued; } — 8 bytes,
// so it comes home in x0 with SamplesAvailable in the low word. This used to
// sit in the answer-zero list, which reads as "no room, nothing queued": the
// managed side clamps what it sends to SamplesAvailable, so zero room meant it
// never sent anything at all.
static uint64_t klovrp_GetControllerHapticsState(int mask) {
    ovrp_hit("ovrp_GetControllerHapticsState");
    int hands = klovrp_hap_hands(mask);
    int hand = (hands & 1) ? 0 : 1;          // one hand per call, left first
    struct klovrp_haptics *h = &g_hap[hand];
    double now = klovrp_mono();
    pthread_mutex_lock(&h->mu);
    klovrp_hap_drain(h, now);
    int queued = h->count;
    pthread_mutex_unlock(&h->mu);
    int available = KLOVRP_HAP_MAX - queued;

    static int swap = -1;
    if (swap < 0) swap = kl_env_on("KL_HAPTICS_SWAP_STATE", 0);
    uint32_t lo = swap ? (uint32_t)queued : (uint32_t)available;
    uint32_t hi = swap ? (uint32_t)available : (uint32_t)queued;
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

// 24-byte ovrpHapticsDesc by value via x8. See the block comment above for why
// every field of this matters and why zeroing it silenced the whole path.
uint64_t klovrp_GetControllerHapticsDesc_impl(int mask, void *out) {
    ovrp_hit("ovrp_GetControllerHapticsDesc");
    (void)mask;                                  // one controller model, both hands
    int32_t *d = out;
    d[0] = KLOVRP_HAP_RATE;                      // SampleRateHz
    d[1] = 1;                                    // SampleSizeInBytes
    d[2] = KLOVRP_HAP_SAFE;                      // MinimumSafeSamplesQueued
    d[3] = KLOVRP_HAP_MINBUF;                    // MinimumBufferSamplesCount
    d[4] = KLOVRP_HAP_OPTIMAL;                   // OptimalBufferSamplesCount
    d[5] = KLOVRP_HAP_MAX;                       // MaximumBufferSamplesCount
    return OVRP_SUCCESS;
}

// The buffered source. Returns the span it is handing out, 0 if it has
// nothing to say yet. Call with the lock held.
static float klovrp_hap_take_buffered(struct klovrp_haptics *h, float *amp) {
    int fed = h->fed;
    h->fed = 0;
    int pending = h->count - h->published;
    if (pending <= 0) return 0.0f;
    float span = (float)pending / KLOVRP_HAP_RATE;
    // Hold a short span back while the guest is still feeding — the tail of a
    // clip arrives on the first pull that sees no new samples, so nothing is
    // ever stranded in the queue.
    if (span < KLOVRP_HAP_MIN_S && fed) return 0.0f;
    if (span > KLOVRP_HAP_MAX_S) {
        span = KLOVRP_HAP_MAX_S;
        pending = (int)(KLOVRP_HAP_MAX_S * KLOVRP_HAP_RATE);
    }
    // The PEAK, not the mean: what a hand feels across a 30 ms window is the
    // attack, and averaging one over a window that includes the silence before
    // it turns every sharp cut into a soft push.
    int peak = 0;
    for (int i = 0; i < pending; i++) {
        int v = h->s[(h->head + h->published + i) % KLOVRP_HAP_MAX];
        if (v > peak) peak = v;
    }
    h->published += pending;
    *amp = peak / 255.0f;
    return span;
}

// ...and the level source, in chunks, because a level has no natural end: a
// running vibration owes the frontend one pulse per elapsed chunk, and the
// accumulator (`vib_pub`) is what stops a caller that re-asserts it every frame
// from re-triggering every frame. Chunks are short so that a stop takes effect
// promptly — nothing can cancel a pulse already handed over. Lock held.
#define KLOVRP_HAP_VIB_CHUNK 0.1f
static float klovrp_hap_take_level(struct klovrp_haptics *h, double now, float *amp) {
    if (h->vib_amp <= 0.0f) return 0.0f;
    if (now >= h->vib_until) { h->vib_amp = 0.0f; return 0.0f; }
    double owed = now - h->vib_pub;
    if (owed < KLOVRP_HAP_VIB_CHUNK) return 0.0f;
    float span = (float)owed;
    if (span > KLOVRP_HAP_MAX_S) span = KLOVRP_HAP_MAX_S;
    h->vib_pub += span;
    *amp = h->vib_amp;
    return span;
}

int kl_ovrp_haptics_pull(int hand, float *amplitude, float *seconds) {
    if ((unsigned)hand > 1) return 0;
    struct klovrp_haptics *h = &g_hap[hand];
    double now = klovrp_mono();
    pthread_mutex_lock(&h->mu);
    klovrp_hap_drain(h, now);
    float amp = 0, span = klovrp_hap_take_buffered(h, &amp);
    if (span <= 0.0f) {
        // Only when the buffered path has nothing. The two are alternatives,
        // not a mix: they are different APIs describing the same actuator, and
        // in any frame where both had something to say the sample stream is the
        // one that knows the shape of the effect. Nothing observed so far uses
        // both at once — the vibration calls this title makes are all stops.
        span = klovrp_hap_take_level(h, now, &amp);
    }
    if (span > 0.0f && amp > 0.0f) h->pulses++;
    pthread_mutex_unlock(&h->mu);
    if (span <= 0.0f || amp <= 0.0f) return 0;
    if (amplitude) *amplitude = amp;
    if (seconds) *seconds = span;
    if (klovrp_hap_trace())
        fprintf(stderr, "  [ovrp] haptics: pull hand %d amp %.2f for %.0f ms\n",
                hand, (double)amp, (double)span * 1000.0);
    return 1;
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
    kl_glfb_note_eye_texture(eye, stage, (uint32_t)handle);
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
    {"ovrp_Update2", (void *)klovrp_Update2},
    {"ovrp_GetControllerHapticsDesc", (void *)klovrp_GetControllerHapticsDesc_entry},
    // M8 — haptics out. All three must be real together: the descriptor sizes
    // the guest's buffer, the state paces it, and only then does it ever call
    // the setter. See the block comment above klovrp_SetControllerHaptics.
    {"ovrp_GetControllerHapticsState", (void *)klovrp_GetControllerHapticsState},
    {"ovrp_SetControllerHaptics", (void *)klovrp_SetControllerHaptics},
    {"ovrp_SetControllerVibration", (void *)klovrp_SetControllerVibration},
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
    "ovrp_EndEye2",
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
    // Haptics, both halves in one line per hand: what the guest queued and what
    // a frontend took. Pushes with no pulses is a headless or hand-tracked run
    // and is fine; ZERO pushes on a run that cut notes means the guest never
    // got past the descriptor, which is the failure this line exists to name.
    for (int hand = 0; hand < 2; hand++) {
        pthread_mutex_lock(&g_hap[hand].mu);
        uint64_t pushes = g_hap[hand].pushes, samples = g_hap[hand].samples;
        uint64_t pulses = g_hap[hand].pulses;
        float hpeak = g_hap[hand].peak;
        pthread_mutex_unlock(&g_hap[hand].mu);
        if (!pushes && !pulses) continue;
        fprintf(f, "  haptics %s: %llu buffer(s) / %llu samples queued "
                   "(%.1f s at %d Hz), peak %.2f, %llu pulse(s) played\n",
                hand ? "right" : "left",
                (unsigned long long)pushes, (unsigned long long)samples,
                (double)samples / KLOVRP_HAP_RATE, KLOVRP_HAP_RATE,
                (double)hpeak, (unsigned long long)pulses);
    }
    // The eye swapchain. Frames should be spread across the stages: all of them
    // on one stage means the guest is not cycling and the compositor is reading
    // the texture the guest is writing (see KLOVRP_STAGES_DEFAULT).
    pthread_mutex_lock(&g_frames.mu);
    uint64_t serial = g_frames.serial, guessed = g_frames.guessed;
    uint64_t unobserved = g_frames.unobserved, multi = g_frames.multi;
    uint64_t cross = g_frames.cross_thread, disagree = g_frames.stage_disagree;
    uint64_t filed[KLOVRP_MAX_STAGES];
    memcpy(filed, g_frames.filed, sizeof filed);
    pthread_mutex_unlock(&g_frames.mu);
    if (serial) {
        fprintf(f, "  eye swapchain: %d stage(s), frames per stage",
                kl_ovrp_stage_count());
        for (int i = 0; i < kl_ovrp_stage_count(); i++)
            fprintf(f, " %llu", (unsigned long long)filed[i]);
        fprintf(f, " (%llu begun, %llu filed on a guessed stage, "
                   "%llu where the guest's frame index disagreed)\n",
                (unsigned long long)serial, (unsigned long long)guessed,
                (unsigned long long)disagree);
        // What the GL side saw, independently of any of the bookkeeping above.
        // A stage whose draw count stops climbing is a frozen picture, and that
        // is a different bug from a mis-filed pose even though both look like
        // doubling in the headset.
        fprintf(f, "  eye draw targets observed per stage:");
        for (int i = 0; i < kl_ovrp_stage_count(); i++)
            fprintf(f, " %llu", (unsigned long long)kl_glfb_stage_draw_count(i));
        fprintf(f, "\n");
        // The association's own health. All three should be 0; each non-zero
        // one names a different reason the pose filed against a stage may not
        // be the pose its picture was drawn with.
        fprintf(f, "  pose<->picture association: %llu frame(s) drew into no eye "
                   "stage, %llu into several, %llu drawn off-thread%s\n",
                (unsigned long long)unobserved, (unsigned long long)multi,
                (unsigned long long)cross,
                (unobserved || multi || cross) ? "" : "  (clean)");
    }
    fprintf(f, "  --- called ---\n");
    for (unsigned i = 0; i < g_novrp; i++)
        if (g_ovrp[i].calls) fprintf(f, "    %-44s x%u\n", g_ovrp[i].name, g_ovrp[i].calls);
    fprintf(f, "  --- resolved but never called ---\n");
    for (unsigned i = 0; i < g_novrp; i++)
        if (!g_ovrp[i].calls) fprintf(f, "    %s\n", g_ovrp[i].name);
}
