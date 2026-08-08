// Timewarp — the composite pass, shared by both compositors.
//
// PLANNING §12.1(3) claimed reprojection "falls out for free if the bookkeeping
// is right". This is the half that consumes that bookkeeping; kl_ovrp.c is the
// half that produces it (kl_ovrp_stage_render_pose).
//
// **What the problem actually is.** The guest renders a flat picture for each
// eye, with a frustum *we* told it to use, about a head pose *we* told it it
// had. By the time that picture reaches the display, the head has moved. Two
// separate things therefore have to be corrected, and they are usually
// confused with each other:
//
//   1. **The pose is stale.** The guest rendered against the pose we predicted
//      at ovrp_BeginFrame; the display shows it one render later. That delta is
//      what "timewarp" names, and it is a rotation (plus a translation we
//      deliberately drop — see below).
//   2. **The frustum does not match the display's.** The guest renders a
//      Quest-shaped frustum (kl_ovrp answers ovrp_GetNodeFrustum2), and the
//      Vision Pro's per-eye frustum is a different, asymmetric one. A
//      full-screen blit of the guest's image stretches it to whatever the
//      drawable's field of view happens to be, which is a *wrong picture* — the
//      world is the wrong angular size — and no amount of pose correction fixes
//      that.
//
// Both are solved by the same pass, and that is the point of doing it this way
// rather than as a UV-space homography: place the guest's picture as a quad at
// a fixed far distance, sized by the tangents it was **rendered** with, in the
// space the pose it was rendered with defines — then look at that quad from
// where the head is **now**, through the projection the drawable actually
// wants. Both corrections are then just the model-view and the projection.
//
// This is the shape ALVR's visionOS client uses for the same job
// (`ALVRClient/Renderer.swift`, `videoFrameVertexShaderCommon` in
// `Shaders.metal`) — a worked, shipping example of reprojecting a
// rendered-elsewhere frame into a Compositor Services drawable. PLANNING §12.8
// records that that code is the user's own and can be relicensed; what is taken
// here is the mechanism, not the streaming architecture around it.
//
// **Rotation only, deliberately.** Positional reprojection needs per-pixel
// depth to be correct; without it, translating the viewpoint smears
// disocclusions and is worse than not correcting at all. So both poses have
// their translation dropped and only the rotation delta is applied. The eye
// offset *is* kept, because it comes from the display side (the drawable's view
// transform) and is not part of the delta. At the quad's distance the residual
// error from treating the render eye as the render head is under a pixel.
//
// **One implementation, two compositors.** KleptonCompositor.swift (visionOS,
// Compositor Services) and kl_view_mtl.m (the macOS viewer, CAMetalLayer) both
// build their uniforms with kl_reproject_build() and compile their shader from
// kl_reproject_msl(). Having the flip, the quad and the delta exist twice is how
// the picture gets debugged twice; on the host the viewer is also the only place
// this math can be *run* before the device is available.
#ifndef KL_REPROJECT_H
#define KL_REPROJECT_H

#include <stdint.h>
#include <simd/simd.h>
#include "kl_ovrp.h"

// The distance the guest's picture is placed at, in metres. Far enough that the
// eye offset and the dropped translation are irrelevant, near enough to stay
// well inside any sane projection. ALVR uses the same 500 m for the same reason.
#define KL_REPROJECT_DEPTH 500.0f

// Matches the MSL struct in kl_reproject_msl(). Bound at buffer(0) for both
// stages: the vertex shader needs the geometry, the fragment shader the slice.
typedef struct {
    simd_float4x4 projection;   // the drawable's projection for this eye, this frame
    simd_float4x4 model_view;   // render-pose space -> display eye space
    simd_float4   tangents;     // what the guest RENDERED with: left, right, top, bottom
    uint32_t      slice;        // array slice of the eye texture this eye owns
    // Metres the quad is placed at. Effectively "far away" on purpose: only
    // ROTATION is corrected (both head poses have their translation dropped),
    // but device_from_view keeps its eye offset, so a near quad would add
    // stereo disparity the guest already baked in. It must still land inside
    // the drawable's depth range — a quad beyond the far plane is clipped and
    // the display is simply black. KL_REPROJECT_DEPTH overrides it.
    float         depth;
    uint32_t      pad[2];
} kl_reproject_uniforms;

// The reprojection pass. Draw as a 4-vertex triangle strip, no vertex buffer.
const char *kl_reproject_msl(void);

// The plain blit the viewer used before any of this existed: a full-screen
// triangle, no uniforms but the slice. Kept because it is the A/B — if the
// reprojected picture is wrong, this is the one known to be right.
const char *kl_reproject_blit_msl(void);

// Build the uniforms for one eye.
//
//   rendered            what the guest drew this image with (kl_ovrp_stage_render_pose)
//   eye                 0 = left, 1 = right — selects which tangents in `rendered`
//   origin_from_device  the head pose NOW, as the display will show it: ARKit's
//                       DeviceAnchor.originFromAnchorTransform, predicted to the
//                       presentation time
//   device_from_view    the eye's offset from the head, i.e. Compositor
//                       Services' cp_view_get_transform. Identity for a
//                       one-eye window.
//   projection          the drawable's projection for this eye
//   slice               which array slice holds this eye's picture
//
// `rendered` may be NULL, which means "no pose was recorded" — the uniforms then
// describe an unreprojected picture rather than an undefined one.
kl_reproject_uniforms kl_reproject_build(const kl_ovrp_render_pose *rendered, int eye,
                                         simd_float4x4 origin_from_device,
                                         simd_float4x4 device_from_view,
                                         simd_float4x4 projection,
                                         uint32_t slice);

// A perspective projection from tangents, all positive, right-handed looking
// down -Z, reverse-Z (near maps to 1). For the viewer, which has no Compositor
// Services to ask, and as the fallback when cp_drawable_compute_projection is
// unavailable.
simd_float4x4 kl_reproject_projection(float left, float right, float top, float bottom,
                                      float near_z);

// The inverse: recover (left, right, top, bottom) tangents from a projection
// matrix, all positive. This is how the drawable's real field of view is read
// back for logging and for kl_ovrp_set_eye_frustum — cp_view_get_tangents was
// deprecated in visionOS 2.0 in favour of a matrix, and the tangents are what
// the guest's OVRPlugin surface speaks.
//
// Independent of the depth convention: only the two rows that carry x and y are
// read, so reverse-Z and an infinite far plane both come out the same.
simd_float4 kl_reproject_tangents(simd_float4x4 projection);

// How far the head turned between the pose the picture was rendered with and
// the pose it is being displayed at, in degrees. The number that says whether
// reprojection is doing anything: ~0 means the guest is keeping up and the pass
// is a blit, a degree or two per frame is ordinary head motion, and a large
// value means frames are being reused.
float kl_reproject_delta_degrees(const kl_ovrp_render_pose *rendered,
                                 simd_float4x4 origin_from_device);

#endif
