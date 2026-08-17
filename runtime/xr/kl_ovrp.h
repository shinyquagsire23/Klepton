// The XR front door — libklepton_ovrp, the replacement for Oculus's OVRPlugin.
//
// Settled before any code existed: libOVRPlugin.so links
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

// Which ovrp_* the guest resolved and which it called — the work list.
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

// The frame clock, for a guest that owns its own frame loop — kl_openxr's
// pacer, on the OVRPlugin side of the same question.
//
// `ovrp_WaitToBeginFrame` is where the plugin is specified to block until the
// app should start its next frame; it is this API's `xrWaitFrame`. For every
// Unity guest here that call happens INSIDE a frame the driver already
// initiated, so it must not block and does not — the driver is the clock. A
// NativeActivity guest (RE4) is the other shape: the engine runs its own game
// thread, nothing of ours calls a render function, and this is the only point
// at which the compositor can say "not yet".
//
// So it is installed rather than compiled in, and by the driver that knows it
// has a display AND that the guest owns the loop. NULL is the default and must
// stay so: a host run has no compositor and a pacer there would block forever.
// The callback must return on its own if no pose arrives — a stalled display
// should make the guest render against the last pose, not wedge it.
//
// The latch is taken here too, so the pose the frame is drawn from is pinned at
// the same moment the frame is admitted (see kl_ovrp_frame_latch).
void kl_ovrp_set_frame_pacer(void (*wait)(void));

// The rest of the pose-in seam: the two Touch controllers. `hand` is
// 0 = left (node 3), 1 = right (node 4). Poses live in the same tracking
// space as the head. Buttons/touches are ovrpButton/ovrpTouch bit values as
// the guest's own OVRPlugin enum defines them (the frontend composes final
// bits); triggers are 0..1, thumbstick -1..1 on both axes.
void kl_ovrp_set_hand_pose(int hand, float px, float py, float pz,
                           float qx, float qy, float qz, float qw);

// ...and the same pose WITH its motion. ovrpPoseStatef carries velocity and
// angular velocity beside the pose (kl_ovrp.c documents the layout), libunity
// copies both into the XR node state, and Unity hands them to managed code as
// `XRNodeState.velocity` / `angularVelocity` and `OVRInput.GetLocalController-
// Velocity`. Leaving them zero is not neutral — it is the assertion that a
// controller which is plainly moving is stationary, which is what anything
// doing swing physics or velocity-predicted rendering believes.
//
// Both are in the SAME tracking space as the pose (world, not controller-local
// — the frontend rotates them out of whatever frame its tracker used), linear
// in m/s and angular in rad/s about the world axes, which is OVRPlugin's own
// convention. `kl_ovrp_set_hand_pose` is this with zero motion, and the two
// write through one seqlock update so a reader can never pair this frame's
// pose with last frame's velocity.
void kl_ovrp_set_hand_motion(int hand, float px, float py, float pz,
                             float qx, float qy, float qz, float qw,
                             float vx, float vy, float vz,
                             float avx, float avy, float avz);
void kl_ovrp_set_controller_input(int hand, uint32_t buttons, uint32_t touches,
                                  float index_trigger, float hand_trigger,
                                  float stick_x, float stick_y);

// ---------------------------------------------------------------------------
// Haptics: the one seam that runs OUT of the guest
//
// Everything above is the frontend answering questions. This is the guest
// giving an order: it queues an amplitude envelope through OVRPlugin's buffered
// haptics API (320 Hz, one byte a sample — kl_ovrp.c documents the whole
// contract and why every field of the descriptor is load-bearing), and a
// frontend that has something to vibrate drains it here.
//
// **It is an envelope follower, and it must be called once per hand per
// frame.** It returns 1 when the hand should be buzzing right now, filling
// `amplitude` (0..1 — the PEAK of the samples that came due since the previous
// call) and `seconds` (how long that window was). The frontend's job is to
// drive the actuator at that level until the next call: this is a LEVEL, not an
// event to schedule, and a caller that plays each one as an independent pulse
// gets a restart per frame.
//
// It is a DRAIN, not a query — a window is reported once. Two callers would
// each see half the envelope.
//
// Not calling it is safe and loses nothing structural: the guest's queue
// retires on the clock either way, so `SamplesAvailable` stays honest on a host
// with no actuator. What is lost is the samples themselves, which is correct —
// there was nothing to feel them.
//
// **Do not reintroduce batching here.** The obvious-looking optimisation — hold
// samples back until enough accumulate to be worth one pulse — was the first
// implementation and it failed on hardware in two ways at once: OVRHapticsOutput
// keeps only ~28 ms queued in low-latency mode, so a 32 ms threshold never
// fired; and the wait let the queue's own drain retire a clip before it was
// ever handed over. Note cuts came out as blips or as nothing while a held
// buzz was fine. kl_ovrp.c has the full account.
//
// ALVR's floor is honoured, as a hold: once a level appears it is reported for
// at least KL_HAPTICS_MIN_MS (32 ms) even if the samples ran out sooner,
// because an actuator cannot act on a 10 ms burst.
//
// Frequency is deliberately not part of this seam. OVRPlugin's is a selector
// between two fixed Touch motor rates, and the second axis a Sense controller
// actually has is CoreHaptics sharpness, which is a different quantity; there
// is no measurement here that would justify mapping one onto the other.
int kl_ovrp_haptics_pull(int hand, float *amplitude, float *seconds);

// ---------------------------------------------------------------------------
// Timewarp bookkeeping
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
    // **The sub-rect of the eye texture the guest actually rendered into**, per
    // eye, as {x, y, w, h} in that texture's own pixels. `w <= 0` means "the
    // whole texture" and is what every path that cannot know answers — 1.28's
    // legacy VRDevice, the null driver, an OpenXR guest — so a compositor's
    // fallback is the picture those guests have always produced.
    //
    // This is Unity's `XRSettings.renderViewportScale` arriving from the other
    // side, and it is NOT a resolution change: the eye texture keeps its size
    // and the guest draws the same frustum into a smaller corner of it, which
    // is how a title changes its render resolution without reallocating a
    // swapchain. Beat Saber does exactly that on entering a map (it carries
    // both `quality.renderViewportScale` and `quality.menuVRResolutionScaleMultiplier`
    // as user settings), and 1.40's XR-SDK display provider asks us for the
    // rect — ovrp_CalculateEyeViewportRect, which is `OculusSystem::
    // SetRenderViewportScale` in libOculusXRPlugin — and then submits it in
    // ovrpLayerSubmit.ViewportRect[eye] at ovrp_EndFrame4.
    //
    // A compositor that ignores it samples the whole texture and shows the
    // picture squeezed into one corner with unwritten texels around it. There
    // is no error anywhere on that path: every call succeeded and the guest
    // rendered exactly what it was told to.
    //
    // **x and y are 0 in every run this can have**, because the rect is the one
    // WE computed: ovrp_CalculateEyeViewportRect anchors it at the origin, as
    // the real plugin does. That matters, because it is the one thing here
    // whose convention is not pinned by a measurement — GL's viewport origin is
    // bottom-left and OVRPlugin's rect appears to be top-down (the display
    // provider flips Pos.y for one headset id), and with y = 0 the two agree.
    // A non-zero origin would have to settle that first.
    int      viewport[2][4];
    // **The eye texture size that rect was measured against**, {w, h}, or 0 when
    // nothing set it. A rect in pixels is meaningless without it, and this is
    // not a theoretical objection — it is a bug that has been seen:
    //
    //   [view] render viewport 0,0 1374x1440 of a 2748x2880 eye texture
    //
    // one line before the same run said 1374x1440 of a **2290x2400** texture.
    // Unity re-creates the eye swapchain at a new size (its own ~1.2x, and again
    // on entering a map) and the record can be one generation behind the texture
    // the compositor is sampling. Cropping the NEW texture by the OLD rect
    // samples too little of it and stretches that across the whole quad: the
    // picture is MAGNIFIED by exactly the ratio of the two generations, with no
    // unwritten texels anywhere to give it away, and the binocular disparity is
    // magnified with it — which is a picture that reads as distorted and
    // cross-eyed rather than as a cropping bug.
    //
    // With this here a compositor uses the FRACTION (rect / viewport_of) and
    // applies it to whatever texture it is actually holding, so a generation
    // skew is harmless instead of a wrong picture: whichever texture the content
    // is in, the guest filled the same fraction of it.
    //
    // Note this cannot be fixed by "read them at the same instant" — there is no
    // instant at which they agree. The rect is filed by the guest's frame thread
    // at ovrp_EndFrame4 and the texture is looked up by the compositor's own
    // thread, and the two describe different frames whenever the guest is
    // decoupled, which is the design.
    int      viewport_of[2];
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

// ---------------------------------------------------------------------------
// The OTHER layers — what a compositor has to draw ON TOP of the eye picture
//
// `ovrp_EndFrame4` is handed a LIST, and until 2026-08-15 every reader of it
// walked past anything that was not the eye layer. That is right for the frame
// record — only the eye layer describes the picture being reprojected — and it
// silently dropped everything else, so a guest submitting overlays looked from
// every log here exactly like one submitting none.
//
// Unity's only other layer is a 1x1 nothing renders into, which is why it never
// mattered. **Unreal draws its UI this way**: OculusHMD's FSplash and its
// IStereoLayers quads are how RE4 shows its studio logo, its loading screens
// and its menus, so on that engine "the layer is dropped" means the game's
// entire non-world presentation is invisible while the world composites
// perfectly.
//
// The pose is in the guest's tracking space — the same space the frontend
// publishes the head pose into — so a compositor places it with its own current
// head pose and gets world-locking for free. Unlike the eye quad, which is
// eye-centred and rotation-corrected on purpose (see kl_reproject.h), this one
// keeps its translation: it IS somewhere.
typedef struct {
    int   layer_id;      // as ovrp_SetupLayer handed it out
    int   shape;         // ovrpShape: 0 Quad, 1 Cylinder, 2 Cubemap, 5 Equirect
    int   stage;         // the texture stage the guest named in its submit
    int   tex_w, tex_h;  // the layer's texture size, for the viewport fraction
    int   viewport[2][4];// per-eye {x, y, w, h} within that texture
    float pose[7];       // orientation xyzw then position xyz, tracking space
    float size[2];       // Quad: width and height in METRES
    int   flags;         // ovrpLayerSubmitFlags, as submitted
    int   head_locked;   // the flag above, decoded — see kl_ovrp.c
    // Which eyes see this layer: 0 both, 1 left only, 2 right only. OpenXR's
    // XrCompositionLayerQuad.eyeVisibility, and the reason a guest showing
    // different pixels to each eye submits TWO quads rather than one layer with
    // two views. An OVRPlugin submit has no counterpart and files 0, which is
    // what every layer that ran here before this field was both of.
    int   eye_visibility;
    // 1 when row 0 of the layer's texture is the TOP of the picture — Vulkan
    // and Metal — and 0 for a picture GL rendered, whose origin is the bottom
    // left. The same question kl_glfb_eye_mtl_origin_top_left answers for an
    // eye, with the same polarity, so one convention covers both passes.
    int   origin_top_left;
} kl_ovrp_overlay;

// How many non-eye layers the guest submitted with its most recent frame, and
// what they were. The list is replaced whole at each ovrp_EndFrame4 under the
// frame lock, so a compositor reading it mid-update gets the previous frame's
// list rather than half of two — which for a static splash is the same list.
//
// Returns 0 and leaves *out untouched for an index that is not there.
int kl_ovrp_overlay_count(void);
int kl_ovrp_overlay_get(int i, kl_ovrp_overlay *out);

// ...and the two calls that FILE those records for a guest that does not speak
// OVRPlugin at all.
//
// Both compositors — KleptonCompositor.swift and the macOS viewer — read the
// frame record above and nothing else: with no record, `last_complete` is -1
// forever and every frame composites black however good the picture is. Beat
// Saber files it from ovrp_BeginFrame/ovrp_EndFrame; an OpenXR guest never
// calls either, which is why the Steam Link immersive space was black with a
// perfectly healthy compositor.
//
// The pair is the same two moments, and the OpenXR half is the easier one:
//
//   begin   at xrWaitFrame, the frame latch point. Pins the pose this frame
//           will be drawn with, exactly as ovrp_BeginFrame does.
//   end     at xrEndFrame, with the swapchain image the guest presented.
//
// **The stage is passed in rather than observed**, and that is the difference
// between the two guests. Unity picks its stage privately, so kl_glfb has to
// watch which eye texture it bound as a draw target and klovrp_EndFrame files
// the frame under whatever was seen. An OpenXR guest names
// the image it drew — xrReleaseSwapchainImage, then the projection layer at
// xrEndFrame — so there is nothing to infer and none of that machinery's
// failure modes (`drew into NO eye stage`, `drew into N stages`) can arise.
//
// **...and so is the POSE, which is the other thing OpenXR states outright.**
// `pose7` is {px,py,pz,qx,qy,qz,qw} in the tracking space and `tan8` is the two
// eyes' {left,right,top,bottom} tangents; NULL for either keeps what `begin`
// latched. An OpenXR guest submits both in the projection layer, and taking
// them from there rather than from our latch is what stops a compositor
// timewarping a picture the guest has ALREADY timewarped: a streaming client
// warps the host's frame to the predicted display pose before submitting it,
// says so in the layer, and our correction then correctly comes out near zero.
// Against the latched pose it comes out as the whole frame's head motion,
// applied a second time — which reads as the scene moving further than the head
// did, and as an arc on any rotation, because a head turn translates the eyes.
// A guest that does NOT pre-warp submits the pose it was given, and this is
// then identical to the latch. Strictly better in both cases, so it is not a
// knob.
void kl_ovrp_frame_begin_external(void);
void kl_ovrp_frame_end_external(int stage, const float *pose7, const float *tan8);

// ...and the same for the OTHER layers, which for an OpenXR guest is the whole
// of `xrEndFrame`'s list that is not a projection layer.
//
// The list is replaced whole under the frame lock, exactly as ovrp_EndFrame4's
// is, and n = 0 is the ordinary case rather than an error: a frame that submits
// no quad has none, and leaving the previous frame's list standing would show a
// panel the guest has taken down. Records past the array are DROPPED and named,
// because a layer that silently does not reach the compositor is content the
// user cannot see.
//
// The caller fills every field, including `origin_top_left` and
// `eye_visibility` — this half of the seam knows which API drew the picture and
// which eyes were named, and nothing downstream can recover either.
void kl_ovrp_overlays_external(const kl_ovrp_overlay *v, int n);

// ...and whether the guest's most recent frame carried an EYE PICTURE at all.
//
// A compositor draws the layers the guest submitted THIS frame, and the eye
// pass had no way to be told "not this one": the frame record and the eye
// textures both persist, so once anything had ever filed them the eye was drawn
// for the rest of the run. That is right for every guest that submits a
// projection layer every frame — which is all of them until JKXR, whose engine
// SWITCHES: a projection pair in the world and a single quad in the menu, out
// of the same two swapchains (measured: `[xr] frame shape changed after 8106
// frames: quad(sc0 eyes0) -> proj(2 views: sc0 sc1)`). With the eye latched on
// from the first world frame, its menu came back as a full-field picture
// UNDERNEATH the panel showing the same thing — one screen composited twice.
//
// Defaults to live, so a guest that never files it (every OVRPlugin guest, and
// every OpenXR guest that submits a projection layer every frame) is unchanged.
void kl_ovrp_eye_layer_external(int submitted);
int  kl_ovrp_eye_layer_live(void);

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

// Battery telemetry, as one source of truth. The defaults are the Quest-2
// fiction (95% / not charging) — on the host there is no battery. A visionOS
// frontend that reads the real level off UIDevice pushes it through
// kl_ovrp_set_battery_level, the same seam shape as
// kl_ovrp_set_display_frequency, and BOTH the OVRPlugin query
// (ovrp_GetSystemBatteryLevel2) and the Java BatteryManager answer
// (kl_jni.c) read through the getters, so the two presentations cannot
// disagree. KL_BATTERY_LEVEL / KL_BATTERY_CHARGING still override on the
// host, exactly as they do today.
void kl_ovrp_set_battery_level(int level);     // 0..100, clamped
int  kl_ovrp_battery_level(void);
void kl_ovrp_set_battery_charging(int charging);
int  kl_ovrp_battery_charging(void);
// ...and the third field of the same telemetry, in whole degrees Celsius. There
// is no sensor here, so it is a nominal — but it is read through a getter for
// the reason above: UE4's BatteryReceiver dispatches status, level and
// temperature in one call, so a literal restated there would be a second
// battery.
int  kl_ovrp_battery_temperature(void);

// The predicted display time (seconds) the guest is told the next frame will
// be shown. It drives timewarp — the guest predicts the head pose at the time
// WE name — so a value that does not match when the frame actually presents
// produces a reprojection nobody asked for. There is no display on the host to
// measure, so the default answers the monotonic PRESENT, which is the least-
// wrong no-data value: prediction with zero advance is conservative, whereas 0
// reads as a timestamp far in the past. A visionOS frontend that knows the
// real drawable presentation time pushes it through
// kl_ovrp_set_predicted_display_time (per frame), same seam shape as the
// display frequency and eye texture size. Monotonic, never wall-clock — every
// consumer compares it against other monotonic times, and the XR side already
// defines its clock as CLOCK_MONOTONIC.
void   kl_ovrp_set_predicted_display_time(double t);
double kl_ovrp_predicted_display_time(void);
float kl_ovrp_display_frequency(void);

// The per-eye render target size the guest is told to use
// (ovrp_GetEyeTextureSize). Defaults to the Quest 2's recommended 2290x2400,
// for the same reason everything else here describes a Quest 2.
//
// The display underneath is not a Quest 2 and its drawables are a different
// shape, so a frontend that can measure the real thing should push it — the
// hardcoded pair is a guess that happens to be close, and on visionOS it is
// wrong in both axes. `w`/`h` are in the order ovrp reports them, i.e. the same
// order klovrp_GetEyeTextureSize packs; kl_ovrp.c's SetupEyeTexture2 explains
// the transposition that happens further down.
//
// **Size is not free.** Unity allocates a swapchain of these — stages x eyes,
// RGBA16F — and then applies its OWN resolution scale on top (measured: 1.2x,
// which is where the second generation in a run's logs comes from). A display
// whose logical resolution is much larger than a Quest's therefore multiplies
// straight into hundreds of MiB and into fill cost, so this is clamped
// (KL_OVRP_EYE_MAX) and scalable (KL_OVRP_EYE_SCALE) rather than trusted, and
// what was asked for versus what is being reported is logged once.
//
// Unlike the display frequency this MAY be pushed late: Unity re-creates the
// eye swapchain when the size changes, which a run's logs show it doing three
// times before the menu is up. Pushing before the guest boots is still better —
// it is one fewer rebuild, and each one used to leak a whole generation
//.
void kl_ovrp_set_eye_texture_size(int w, int h);
void kl_ovrp_eye_texture_size(int *w, int *h);

// Do we tell the guest the two eyes may be ARRAY LAYERS of one texture rendered
// in a single pass — Unity's Single Pass Instanced / Multiview? This is the one
// answer that picks the guest's whole stereo rendering mode, so it is a function
// rather than a constant per call site: ovrp_GetSystemMultiViewSupported,
// ...Supported2, ovrp_GetEyeTextureArraySupported and ...Supported2 all read it,
// and two of them disagreeing is a guest told it may render single-pass into
// storage that has one layer.
//
// False on the GL gateway always — ANGLE-on-Metal has no multiview here. On the
// Vulkan path it follows KL_OVRP_MULTIVIEW, because MoltenVK implements
// VK_KHR_multiview and kl_vulkan.c can allocate the two-layer image.
int kl_ovrp_multiview(void);

// ---------------------------------------------------------------------------
// The eye view, for a guest that is not OVRPlugin's
//
// Everything above is the OVRPlugin ABI's own seam. This is the same
// information without it: where one eye is, which way it points, and the
// frustum it should render with — the answer klovrp_GetNodePoseState and
// klovrp_GetNodeFrustum2 compose for nodes 0 and 1, exposed so the OpenXR
// runtime (kl_openxr.c, xrLocateViews) can give its guest the identical answer.
//
// Sharing it is the point. The head pose has a per-frame latch here, the eye
// offsets carry the display's real IPD, and the tangents carry its real cant
// and field of view — all of it pushed by one frontend. A second XR API
// computing its own eye poses from the published pose would drift from this one
// by a frame and would miss the cant entirely, and the symptom of that
// (per-eye counter-rotation read as doubling) already cost a session once.
//
// `eye` is 0 = left, 1 = right. The pose is in the current tracking space; the
// tangents are all positive, in cp_view_get_tangents order (left, right, top,
// bottom). Call kl_ovrp_frame_latch() first, exactly as the ovrp path does.
void kl_ovrp_eye_view(int eye, float *px, float *py, float *pz,
                      float *qx, float *qy, float *qz, float *qw,
                      float tangents[4]);

// The head pose as the GUEST sees it — pinned for the duration of its frame by
// kl_ovrp_frame_latch, and therefore NOT the same thing as
// kl_ovrp_get_head_pose above. That one is the frontend's question ("where is
// the head now", which is what a composite reprojects towards); this is the
// guest's ("where was the head when this frame started"). Answering a guest
// with the live value is the doubling bug, and answering a compositor with the
// pinned one makes reprojection a no-op — so the two must stay distinct.
void kl_ovrp_get_guest_head_pose(float *px, float *py, float *pz,
                                 float *qx, float *qy, float *qz, float *qw);

// Standing eye height above the floor origin (KL_OVRP_EYE_HEIGHT), or 0 when
// the guest asked for an eye-level tracking origin. This is the offset between
// OpenXR's STAGE space (floor) and its LOCAL space (where the head started).
float kl_ovrp_eye_height(void);

// ---------------------------------------------------------------------------
// The read side of the pose seam, for the OTHER XR API
//
// Everything above this line is the OVRPlugin ABI answering the OVRPlugin
// guest. These three are the same measurements handed to `kl_openxr.c`, which
// serves an OpenXR guest that never calls a single ovrp_ entry point — and
// they are HERE rather than in a third place for the reason `kl_ovrp_eye_view`
// is: one frontend publishes one sample of one instant, and a second XR API
// deriving its own would drift from the first by a frame.
//
// Read them exactly as the ovrp path does — after `kl_ovrp_frame_latch()`,
// because these return the pose PINNED for the guest's frame and not the live
// one (see `kl_ovrp_get_guest_head_pose` above for why those must stay
// distinct).
//
// Both return 1 when a frontend has actually published for that hand and 0
// when the answer is the head-relative default the ovrp path parks controllers
// at. That distinction is what OpenXR calls `isActive`, and it is the whole
// reason these report it rather than just filling the buffers: an action bound
// to a controller nobody is holding must read inactive, not zero.
//
// Any output pointer may be NULL. `pos`/`vel`/`ang` take 3 floats, `quat` 4
// (x, y, z, w), all in the current tracking space.
int kl_ovrp_hand_motion(int hand, float *pos, float *quat, float *vel, float *ang);

// ...and whether `vel`/`ang` from that call are a measurement at all. A frontend
// that publishes pose only has its motion DERIVED here (successive poses over
// the wall clock), which covers every sample but the first after a gap; those
// report 0 and must be passed on as OpenXR's velocityFlags == 0 rather than as a
// velocity of zero, which asserts the thing is stationary. KL_OVRP_DERIVE_VELOCITY=0
// turns the derivation off, and is the A/B for anything that looks velocity-shaped.
int kl_ovrp_hand_motion_known(int hand);
int kl_ovrp_controller_input(int hand, uint32_t *buttons, uint32_t *touches,
                             float *index_trigger, float *hand_trigger,
                             float *stick_x, float *stick_y);

// `ovrpButton` / `ovrpTouch` RAW bits, as they arrive in the two words above.
//
// Raw, not virtual — the distinction that cost a session once: 0x2000 is
// `PrimaryIndexTrigger` in virtual space and `RThumbstickDown` in raw, so a
// frontend emitting virtual values turned every trigger press into a stick
// flick. The frontends compose these (`kl_view.c`, `KleptonControllers.swift`)
// and `kl_openxr.c` decodes them back into per-path booleans, which is why the
// numbers finally live in a header instead of three times over in comments.
//
// A/B/X/Y are hand-specific in this enum, not per-hand aliases: A and B are the
// RIGHT controller's face buttons and X and Y the LEFT's, and both arrive in
// their own hand's `buttons` word.
enum {
    KL_OVRP_RAW_A                = 0x00000001,
    KL_OVRP_RAW_B                = 0x00000002,
    KL_OVRP_RAW_RTHUMBSTICK      = 0x00000004,
    KL_OVRP_RAW_X                = 0x00000100,
    KL_OVRP_RAW_Y                = 0x00000200,
    KL_OVRP_RAW_LTHUMBSTICK      = 0x00000400,
    KL_OVRP_RAW_RTHUMBSTICK_UP   = 0x00001000,
    KL_OVRP_RAW_RTHUMBSTICK_DOWN = 0x00002000,
    KL_OVRP_RAW_RTHUMBSTICK_LEFT = 0x00004000,
    KL_OVRP_RAW_RTHUMBSTICK_RIGHT= 0x00008000,
    KL_OVRP_RAW_START            = 0x00100000,
    KL_OVRP_RAW_BACK             = 0x00200000,
    KL_OVRP_RAW_RINDEX_TRIGGER   = 0x04000000,
    KL_OVRP_RAW_RHAND_TRIGGER    = 0x08000000,
    KL_OVRP_RAW_LINDEX_TRIGGER   = 0x10000000,
    KL_OVRP_RAW_LHAND_TRIGGER    = 0x20000000,
};

// ...and the OTHER direction, for a guest whose haptics API is not OVRPlugin's.
//
// OpenXR does not have a sample stream. `xrApplyHapticFeedback` carries an
// amplitude and a DURATION and means "buzz at this level until it lapses";
// `xrStopHapticFeedback` ends it early. That is the shape of the legacy
// `ovrp_SetControllerVibration` path rather than the buffered one, so it feeds
// a third source beside those two — deliberately a third, and not a reuse of
// the vibration slot, for the same reason the vibration slot is not folded into
// the ring: two owners of one level erase each other silently, and the symptom
// is haptics that are merely intermittent (kl_ovrp.c documents the day that
// cost). `kl_ovrp_haptics_pull` merges all three by maximum.
//
// `seconds <= 0` is a click of the minimum useful length — OpenXR's
// XR_MIN_HAPTIC_DURATION, which means "as short as the hardware can do" and
// here means the same 32 ms floor the pull already holds every level for.
//
// Frequency is not a parameter, the same decision and for the same reason as
// the rest of this seam: see the note above `kl_ovrp_haptics_pull`.
void kl_ovrp_haptics_apply(int hand, float amplitude, float seconds);
void kl_ovrp_haptics_stop(int hand);

#endif
