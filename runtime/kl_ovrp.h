// M6 front door — libklepton_ovrp, the replacement for Oculus's OVRPlugin.
//
// PLANNING §3.1 settled this before any code existed: libOVRPlugin.so links
// against libossdk.so / libossdk.oculus.so / libOVRMrcLib.so and NEEDs
// libvrapi.so, none of which ship in the APK. It cannot be translated, so the XR
// cut line is *above* VrApi and OVRPlugin is replaced wholesale. That also
// terminates the chain: libvrapi.so is never loaded at all.
//
// The trace confirmed it the hard way. Unity resolved
// ClassLoader.findLibrary("OVRPlugin") to the real library, we loaded it,
// UnityPluginLoad ran, and it died at libOVRPlugin.so+0x2ca20 on `ldr x8, [x0]`
// with x0 NULL — a vtable dispatch through state that only exists once VrApi has
// initialised it.
//
// So we serve the soname ourselves. Same shape as the GL and OpenSL gateways:
// a synthetic handle, real implementations where we have them, and a named
// trampoline for everything else so the guest says which of the 466 ovrp_*
// entry points it actually wants rather than dying anonymously.
#ifndef KL_OVRP_H
#define KL_OVRP_H
#include <stdio.h>
#include <stdint.h>

void *kl_ovrp_dlopen(const char *soname);   // NULL if this is not OVRPlugin
int   kl_ovrp_claims(const char *soname);   // the same test, without opening
int   kl_ovrp_is_handle(const void *h);
void *kl_ovrp_sym(const char *name);

// Which ovrp_* the guest resolved and which it called — the M6 work list.
void kl_ovrp_report(FILE *f);

// Which space those poses are in: 0 = eye level, 1 = floor level, 2 = stage,
// as the guest last set with ovrp_SetTrackingOriginType. Beat Saber asks for
// floor level, so y=0 is the floor and a head belongs at standing eye height —
// a frontend that reports an eye-level head into a floor-level world puts the
// camera on the ground and its hands underneath it.
int kl_ovrp_tracking_origin(void);

// The frontend seam, pose in: whoever owns the window tells us where the head
// is, and ovrp_GetNodePoseState reports it back to the guest. Today that is the
// SDL viewer (KL_VIEW=1, kl_view.c) driven by WASD + mouse-look; on visionOS it
// is ARKit's WorldTrackingProvider answering the same call. Without a frontend
// the head stands at eye height above the floor origin (KL_OVRP_EYE_HEIGHT,
// default 1.6 m) and the hands ride a head-relative offset in front of it.
void kl_ovrp_set_head_pose(float px, float py, float pz,
                           float qx, float qy, float qz, float qw);

// The pose the FRONTEND last published — where the head is now, including the
// standing-eye-height default a headless run gets. This is the display side's
// number: the viewer's composite reprojects towards it. It is deliberately not
// the pose the guest is currently seeing, which is pinned per frame below.
void kl_ovrp_get_head_pose(float *px, float *py, float *pz,
                           float *qx, float *qy, float *qz, float *qw);

// Start a guest frame: promote the published poses to the ones the guest will
// see for the whole of it.
//
// **Call this once, at the top of every guest frame, before anything in the
// frame can ask for a pose.** The frontend publishes at display rate on its own
// thread; without this the answer to "where is the head" changes *during* a
// guest frame, and the pose recorded for timewarp is then not the pose the
// picture was drawn from. Reprojection subtracts that record, so the error is
// one guest frame of head rotation applied in the wrong direction — which is
// visible as the image doubling during head turns, and grows exactly as the
// frame rate falls. See the long comment in kl_ovrp.c.
//
// Idempotent in the sense that calling it more often than once per frame is not
// unsafe, only less useful: each call re-pins to the newest published pose.
// KL_OVRP_LATCH=0 disables it and restores the live read.
void kl_ovrp_frame_latch(void);

// M7 — the rest of the pose-in seam: the two Touch controllers. `hand` is
// 0 = left (node 3), 1 = right (node 4). Poses live in the same tracking
// space as the head. Buttons/touches are ovrpButton/ovrpTouch bit values as
// the guest's own OVRPlugin enum defines them (the frontend composes final
// bits); triggers are 0..1, thumbstick -1..1 on both axes.
void kl_ovrp_set_hand_pose(int hand, float px, float py, float pz,
                           float qx, float qy, float qz, float qw);
void kl_ovrp_set_controller_input(int hand, uint32_t buttons, uint32_t touches,
                                  float index_trigger, float hand_trigger,
                                  float stick_x, float stick_y);

// ---------------------------------------------------------------------------
// Timewarp bookkeeping — PLANNING §12.1(3)
//
// Reprojection needs exactly one thing the compositor cannot work out for
// itself: the pose each eye-pair framebuffer was *actually* rendered with, and
// the frustum it was rendered with. We are the one who answered both — the
// guest asked us at ovrp_GetNodePoseState and ovrp_GetNodeFrustum2 — so this is
// bookkeeping, not estimation, which is the whole reason reprojection is cheap
// here.
//
// **Keyed to the swapchain stage, not to a "last pose" global.** Unity does not
// render into one eye texture, it cycles a swapchain: ovrp_SetupEyeTexture2 is
// called per (eye, stage) and ovrp_GetEyeTextureStageCount is ours to answer.
// A pose keyed to anything but the stage reprojects one frame's picture with
// another frame's pose, and that error looks like judder that worsens with
// latency rather than like a bug. We answer 1 stage today, so the ring has one
// live entry — but it is a ring from the start, because retrofitting the key is
// exactly the change that would be got wrong later.
//
// The record is latched at ovrp_BeginFrame (the guest's own "this frame starts
// now") and marked complete at ovrp_EndFrame, so a compositor never samples a
// stage the guest is still drawing into.

typedef struct {
    float    px, py, pz;          // head position, in the current tracking space
    float    qx, qy, qz, qw;      // head orientation
    // The frustum the guest was told to render this frame with, per eye:
    // left, right, top, bottom tangents, all positive. This is Compositor
    // Services' cp_view_get_tangents order, not OVRPlugin's Up/Down/Left/Right
    // — the ovrpFovf transposition happens inside kl_ovrp, at the one place that
    // speaks the guest's ABI.
    float    tangents[2][4];
    uint64_t serial;              // guest frame serial, 1-based; 0 = nothing recorded
    int      stage;               // the swapchain stage this frame drew into
    int      complete;            // has ovrp_EndFrame been seen for this serial?
} kl_ovrp_render_pose;

// What the guest rendered `stage` with. Returns 0 (and leaves *out untouched)
// if nothing has been recorded for that stage yet.
int kl_ovrp_stage_render_pose(int stage, kl_ovrp_render_pose *out);

// The stage of the most recent *completed* guest frame, or -1 before the first.
// This is the stage a compositor should be sampling, and the only stage index
// a caller has any business passing to the call above.
int kl_ovrp_last_complete_stage(void);

// How many swapchain stages we tell the guest it has (KL_OVRP_STAGES). The
// answer to ovrp_GetEyeTextureStageCount, so it bounds every (eye, stage) key
// in the system — a compositor iterating stages must use this rather than its
// own idea of how many there are.
int kl_ovrp_stage_count(void);

// How the pose<->picture association is holding up, readable at any time.
// `dropped` counts guest frames that drew into no eye stage and whose pose was
// therefore not filed at all; `multi` those that drew into several; `cross`
// those drawn by a thread other than the one ending the frame; `disagree` those
// where the guest's own frame index and the observed draw target named
// different stages. All four should be 0. Any argument may be NULL.
void kl_ovrp_association_stats(uint64_t *dropped, uint64_t *multi,
                               uint64_t *cross, uint64_t *disagree);

// The frustum-in seam. By default kl_ovrp reports a coherent symmetric 90°
// frustum, which is what every host run has used. A frontend that knows the
// display's real per-eye field of view can push it here and the guest will
// render that instead — the loop that makes the composite pass a straight
// reprojection with nothing left to stretch.
//
// Tangents are all positive, in cp_view_get_tangents order. `eye` is 0 = left,
// 1 = right. Takes effect on the next frame the guest asks about, and is
// recorded per frame in the render-pose ring, so a frustum that changes
// mid-run stays consistent with the pictures rendered under it.
void kl_ovrp_set_eye_frustum(int eye, float left, float right, float top, float bottom);

// The head->eye offset seam — i.e. the IPD, and the only place the guest can
// learn it from.
//
// libunity's per-frame node loop (libunity+0x9bbff0) asks
// ovrp_GetNodePoseState for node 0 (EyeLeft) and node 1 (EyeRight) as well as
// node 9 (Head), and *that pair is where its stereo separation comes from*:
// there is no ovrp_GetUserIPD in the surface this title imports, so an eye
// pose that equals the head pose is a headset with an IPD of zero. Both eyes
// then render from the same point, every disparity is zero, and the world
// reads as flat and infinitely far away — which presents as "things look too
// big and too far", not as "the stereo is off".
//
// `x, y, z` are metres in the HEAD's own frame (right, up, back), which is
// exactly `cp_view_get_transform`'s translation on visionOS. Defaults to zero
// on both eyes, which is the behaviour every host run so far has had — the
// SDL viewer is monocular, so nothing on the host can tell the difference.
// KL_OVRP_IPD=<metres> overrides both eyes with a symmetric ±IPD/2 and is the
// A/B for "is the separation the compositor pushed the right one".
void kl_ovrp_set_eye_offset(int eye, float x, float y, float z);

// The other half of the same seam: this eye's ORIENTATION relative to the head
// — the cant. On Vision Pro the displays are angled outward, so an eye is not
// merely displaced from the head, it is turned; and the frustum reported for
// that eye (kl_ovrp_set_eye_frustum) is expressed in the turned frame. Pushing
// the tangents without the rotation describes an eye that does not exist, and
// the guest then renders the correct shape of frustum aimed the wrong way.
//
// Identity until a frontend says otherwise, so host runs are unchanged.
// KL_OVRP_EYE_CANT=0 ignores whatever is pushed here.
void kl_ovrp_set_eye_rotation(int eye, float qx, float qy, float qz, float qw);

// The display frequency the guest is told the headset runs at
// (ovrp_GetSystemDisplayFrequency, and the single entry in
// ovrp_GetSystemDisplayAvailableFrequencies). Defaults to the Quest 2's 72 Hz,
// to agree with the device we claim to be everywhere else.
//
// **This must be pushed before the guest boots.** Unity reads it once and
// stores it into its VR timing config; a value that arrives later is a number
// nothing will ever ask for again. Values outside 30..240 are refused as a
// failed measurement rather than passed on, because this one is a divisor.
void kl_ovrp_set_display_frequency(float hz);
float kl_ovrp_display_frequency(void);

#endif
