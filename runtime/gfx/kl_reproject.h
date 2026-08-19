// Timewarp — the composite pass, shared by both compositors.
//
// This is the half that CONSUMES the timewarp bookkeeping; kl_ovrp.c produces
// it (kl_ovrp_stage_render_pose).
//
// The guest renders a flat picture per eye, with a frustum we told it to use,
// about a head pose we told it it had. Two separate things then need
// correcting:
//
//   1. The pose is stale. The guest rendered against the pose predicted at
//      ovrp_BeginFrame and the display shows it one render later. That delta is
//      what "timewarp" names: a rotation, plus a translation deliberately
//      dropped.
//   2. The frustum does not match the display's. The guest renders a
//      Quest-shaped frustum (ovrp_GetNodeFrustum2) and the Vision Pro's per-eye
//      frustum is a different, asymmetric one. A full-screen blit stretches the
//      image to the drawable's field of view, so the world comes out the wrong
//      angular size — which no amount of pose correction fixes.
//
// Both fall out of one pass, which is why this is not a UV-space homography:
// place the guest's picture as a quad at a fixed far distance, sized by the
// tangents it was RENDERED with, in the space its render pose defines; then
// look at that quad from where the head is NOW, through the projection the
// drawable wants. The two corrections are then just the model-view and the
// projection.
//
// Rotation only, deliberately. Positional reprojection needs per-pixel depth;
// without it, translating the viewpoint smears disocclusions and is worse than
// no correction. So both poses drop their translation. The eye offset IS kept —
// it comes from the display side (the drawable's view transform) and is not
// part of the delta. At the quad's distance, treating the render eye as the
// render head costs under a pixel.
//
// One implementation, two compositors: KleptonCompositor.swift (visionOS,
// Compositor Services) and kl_view_mtl.m (the macOS viewer, CAMetalLayer) both
// build uniforms with kl_reproject_build() and compile their shader from
// kl_reproject_msl(). The viewer is also the only place this math can be RUN
// without a device.
#ifndef KL_REPROJECT_H
#define KL_REPROJECT_H

#include <stdint.h>
#include <simd/simd.h>
#include "kl_ovrp.h"

// The distance the guest's picture is placed at, in metres. The quad is
// eye-centred, so this does not affect our own picture at all — it is what the
// SYSTEM's depth-based reprojection is told about our content, and placing it
// far away is what keeps that correction rotational. See kl_reproject_build for
// the full argument, including why a near placement does not fix a frame that
// vanishes. ALVR uses the same 500 m. KL_REPROJECT_DEPTH overrides at runtime.
#define KL_REPROJECT_DEPTH 500.0f

// Forget the latched KL_REPROJECT_DEPTH so the next build re-reads it. For the
// gate, which places one quad at two distances in one process; nothing in a
// real run changes the depth after it has been chosen.
void kl_reproject_reset_depth(void);

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
    // 0 collapses the quad off-screen instead of drawing it. This exists
    // because the visionOS pass draws BOTH eyes in one encoder now (vertex
    // amplification, see below) and can no longer simply skip one: an eye whose
    // stage has no texture yet has to be expressed in the uniforms rather than
    // by not issuing a draw. kl_reproject_build always sets it.
    uint32_t      visible;
    // 1 = decode the sample from sRGB to linear before it is composited.
    //
    // **This undoes an encode we could not prevent, and it is not a colour
    // preference.** An OpenXR guest that renders into an `SRGB8_ALPHA8`
    // swapchain and brackets that rendering with
    // `glDisable(GL_FRAMEBUFFER_SRGB)` is saying "the values I am writing are
    // ALREADY sRGB code values, store them as they are". A Quest honours that
    // (`EXT_sRGB_write_control`); ANGLE does not expose the extension and ES
    // applies the encode unconditionally to an sRGB attachment, so the stored
    // byte is `encode(V)` where the guest meant `V`. Sampling an sRGB texture
    // decodes once, which hands the composite `V` — the sRGB code value,
    // treated as if it were linear. That is brighter than intended everywhere
    // except pure black and pure white, which is exactly how it looks.
    //
    // One more decode puts it right. Set by kl_reproject_set_srgb_decode(),
    // which kl_glfb drives from what the guest actually asked for — it is off
    // for Beat Saber, whose eye textures are RGBA16F and already linear, and
    // off for any guest that leaves the encode alone.
    uint32_t      srgb_decode;
    // 1 = the picture in this texture has its ORIGIN AT THE TOP LEFT, so the
    // sample's v has to be flipped before it is read.
    //
    // **This is a property of the API the guest rendered with, not of Metal,
    // and it has no error surface.** GL's framebuffer origin is the bottom
    // left, so an eye texture ANGLE filled reads correctly with the mapping
    // this pass was derived against — and every eye texture in this project
    // came from ANGLE until the Vulkan path existed. Vulkan's origin is the TOP
    // left, like Metal's, so a picture a Vulkan guest drew comes out upside
    // down through the same mapping, with everything else about the pipeline
    // working. Measured, on the first run of the Vulkan compositor seam
    //.
    //
    // The authority is kl_glfb_eye_mtl_origin_top_left(eye, stage) — recorded
    // with the texture when it is registered, because two guests can be in one
    // process's lifetime and a global would be a guess. The compositors read it
    // there and pass it in; this file stays linkable against nothing but kl_env
    // (see kl_reproject_set_srgb_decode for why that matters).
    uint32_t      flip_y;
    // Which BLOCK of the grid bound at buffer(1) this eye reads: 0 for a grid
    // with one block, `eye` for a grid with one block per eye. Set by
    // kl_reproject_build from the `grid_per_eye` it is given; the shader turns
    // it into an index using the cell counts it reads out of the grid's own
    // header, so the two cannot disagree about where a block starts.
    //
    // **It exists because the two eyes' render viewports genuinely differ.**
    // The comment this replaced said they could not, on the grounds that Unity
    // derives both from one `renderViewportScale` — true of a title that scales
    // its resolution, and false of one using Oculus SYMMETRIC PROJECTION, where
    // both eyes are rendered with a single union frustum into one widened
    // texture and each eye's own cone is a different SUB-RECT of it (measured
    // on BONELAB: eye 0 at x=0, eye 1 at x=609, both 2271 of 2880 wide).
    // Sampling eye 1 through eye 0's crop shifts its picture
    // by that offset, which is a warped, displaced right eye and nothing else.
    uint32_t      grid_eye;
    // ---- chroma key ---------------------------------------------------------
    //
    // A green-screen matte applied to the guest's picture so the room shows
    // through wherever the guest drew the key colour — the way a flat-screen
    // title is put into passthrough. Ported from VisionOSALVRClient with its
    // maths and its defaults intact, deliberately: the two dials below are
    // numbers a person finds by looking, and keeping them identical means a
    // value found in one client still means the same thing here.
    //
    // **Scalars only, and that is a layout decision.** This struct is duplicated
    // in MSL and the two must agree byte for byte; a float3 is 16-byte aligned
    // there and 4-byte packed here, so the obvious spelling would put the whole
    // frustum silently at the wrong offset. Every field here is a 4-byte scalar,
    // which both compilers lay out the same way, and the key colour is three of
    // them read as `packed_float3` on the other side.
    uint32_t      chroma_on;
    // The HSV distance band, in the units colorclose_hsv measures: below `min`
    // the pixel is fully keyed out, above `max` it is fully kept, and between
    // them it fades. A band rather than a threshold is what stops the matte
    // having a hard jagged edge.
    float         chroma_min, chroma_max;
    float         chroma_key[3];
} kl_reproject_uniforms;

// The blit's uniforms — buffer(0) of its FRAGMENT stage. It carried a bare
// `uint slice` until the flip existed; a second scalar could have gone in
// another buffer slot, but the two are read together on every path that draws
// this pass and one struct is what stops a caller binding half of it.
typedef struct {
    uint32_t slice;    // array slice of the eye texture to sample
    uint32_t flip_y;   // as above — 1 for a picture with a top-left origin
} kl_blit_uniforms;

// ---------------------------------------------------------------------------
// The OVERLAY pass — the layers that are not the eye
//
// A guest can submit more than the eye layer, and until 2026-08-15 every reader
// of `ovrp_EndFrame4`'s list here walked past the rest. Unity's only other
// layer is a 1x1 nothing renders into; **Unreal draws its UI this way**, so on
// RE4 that dropped the studio splash, the loading screens and the menus while
// the world composited perfectly.
//
// It is a separate pass from the reprojection above rather than a mode of it,
// because the two want opposite things. The eye quad is EYE-CENTRED and
// rotation-corrected on purpose: it is a flat picture with no per-pixel depth,
// so it may not be translated, and its distance is chosen for what the display's
// own reprojection needs. An overlay IS somewhere — it has a position in the
// guest's tracking space and a size in metres, and the disparity between the two
// eyes is the entire reason it is a layer instead of pixels the guest drew
// itself. So this one keeps every translation, including the display's eye
// offset.
//
// It also BLENDS. The caller configures that on the pipeline; the fragment
// stage passes the guest's own alpha through, where the eye passes force 1.0
// (see kl_msl_blit for why they must).
typedef struct {
    simd_float4x4 projection;   // the drawable's projection for this eye
    simd_float4x4 model_view;   // quad space -> display eye space, WITH translation
    simd_float2   half_size;    // metres, half the quad's width and height
    // Which part of the layer's texture this eye reads, as a FRACTION.
    // The guest states it in pixels of its own texture; a fraction is what
    // survives the texture and the record being one generation apart, which is
    // the same argument kl_ovrp_render_pose.viewport_of makes at length.
    simd_float4   uv_rect;      // x, y, w, h
    uint32_t      slice;
    // 1 = the layer's picture has its origin at the TOP left, so its v is
    // flipped before it is read — the same meaning and the same polarity as the
    // reprojection pass's field above, read from kl_ovrp_overlay.origin_top_left
    // rather than decided here. It is 1 for a Vulkan guest (RE4's UI quads) and
    // 0 for one that drew through GL.
    uint32_t      flip_y;
    uint32_t      srgb_decode;
    // 0 = draw nothing. Set for a layer whose SHAPE is not implemented, or
    // whose size is absent. A cylinder drawn as a flat rectangle is a wrong
    // picture with no error surface; an absent one is the failure that is
    // already happening and is at least honest.
    uint32_t      visible;
} kl_overlay_uniforms;

// The overlay shader — `kl_ov_v` / `kl_ov_f`, a 4-vertex triangle strip with no
// vertex buffer, buffer(0) an array indexed by [[amplification_id]] exactly as
// the reprojection pass's is.
const char *kl_reproject_overlay_msl(void);

// Build one view's uniforms for one overlay. `origin_from_device` and
// `device_from_view` are the same two matrices kl_reproject_build takes, and
// mean the same things.
//
// A layer this cannot place answers `visible = 0` rather than approximating it.
kl_overlay_uniforms kl_overlay_build(const kl_ovrp_overlay *ov, int eye,
                                     simd_float4x4 origin_from_device,
                                     simd_float4x4 device_from_view,
                                     simd_float4x4 projection);

// Whether the composite should decode its sample from sRGB (above). Pushed
// rather than queried so this file stays linkable on its own — `make reproject`
// builds it against nothing but kl_env — and so both compositors get the same
// answer from one place instead of each deciding.
void kl_reproject_set_srgb_decode(int on);
int  kl_reproject_srgb_decode(void);

// The chroma key, as the panel and the environment set it — see the struct
// field above. `rgb` is the key colour in the same space the composite samples
// in, `dmin`/`dmax` the HSV distance band. `rgb` may be NULL to leave the
// colour alone. Every eye pass built after this call carries the new values, so
// a slider moves the matte while it is being dragged.
void kl_reproject_set_chroma(int on, const float rgb[3], float dmin, float dmax);

// ...and what is in force, so a panel can show the numbers the composite is
// actually using rather than its own idea of them. Any pointer may be NULL.
int  kl_reproject_chroma(float rgb[3], float *dmin, float *dmax);

// The reprojection pass. Draw as a 4-vertex triangle strip, no vertex buffer.
//
// **Buffer(0) is an ARRAY of uniforms, one per amplified view**, indexed by
// `[[amplification_id]]`. A foveated Compositor Services drawable hands out one
// rasterization rate map for the whole layered render pass, and Metal applies a
// rate map's layer N to render-target array index N — which a pass that binds a
// single slice cannot address. So both eyes must be drawn in one pass with
// renderTargetArrayLength = 2 and vertex amplification, which is also what
// Apple's own template and ALVR do. A one-eye caller (the macOS viewer,
// t_reproject) binds a single struct and gets amplification_id 0, unchanged.
//
// `slice` reaches the fragment stage through a flat varying rather than the
// uniform buffer, for the same reason: the fragment shader has no
// amplification_id to index with.
const char *kl_reproject_msl(void);

// ---------------------------------------------------------------------------
// The unwarp grid — how a FOVEATED eye texture gets read.
//
// When the guest renders through a rasterization rate map its eye texture is
// stored WARPED: the fovea keeps its texels and the periphery is squeezed, so
// texel (x, y) is no longer screen position (x, y). This pass is the only thing
// that reads the picture, so it undoes that.
//
// Not in the fragment shader: calling `map_screen_to_physical_coordinates` per
// fragment pays a decoder lookup on every pixel of both eyes at display rate.
// The quad is subdivided into a grid instead and each vertex carries a
// PRE-UNWARPED texture coordinate, so the rasterizer's interpolation does the
// rest and the fragment shader still just samples at `in.uv`.
//
// Exact, not an approximation. A Metal rate map is piecewise linear with its
// breakpoints at the layer descriptor's zone boundaries, so a grid whose
// vertices land on those boundaries reproduces the map exactly. Build it at the
// map's zone count, or a multiple, and there is no error to trade off. The
// coordinates are static for a given map, so this is built when the map is
// made, not per frame.
//
// Buffer layout, bound at buffer(1) of the VERTEX stage:
//
//     [0]      = (nx, ny) as floats — the cell counts
//     [base + y*(nx+1) + x] = the texture coordinate for grid vertex (x, y),
//                             where `base` is the drawing view's
//                             kl_reproject_uniforms.grid_base
//
// Self-describing in nx/ny, so the pass needs no extra uniform for the cell
// counts and a caller cannot bind a table that disagrees with the vertex count
// it draws.
//
// One block per eye is allowed, and sometimes required. The cell counts are
// shared — one draw, one mesh — but the COORDINATES need not be: under Oculus
// symmetric projection each eye's picture is a different sub-rect of one
// texture, so each eye needs its own crop. Build with
// kl_reproject_grid_build_eye() into a buffer of kl_reproject_grid_entries_n()
// entries and pass grid_per_eye = 1; a caller with one viewport for both eyes
// builds one block and passes 0.
//
// The zone-aligned exactness is a FULL-VIEWPORT property. Cells uniform in
// screen space land on the map's breakpoints only when they span the whole
// screen; with a smaller render viewport (`vp` below) the cells are finer than
// a zone and no longer coincide, so the unwarp becomes a dense sampling of the
// curve rather than a reproduction of it — sub-texel at this cell count, but
// worth knowing before hunting a soft periphery in a title that scales its
// resolution.
#define KL_REPROJECT_GRID_MAX 64

// Entries (each a simd_float2) a grid buffer needs, header included.
static inline uint32_t kl_reproject_grid_entries(uint32_t nx, uint32_t ny) {
    return 1u + (nx + 1u) * (ny + 1u);
}

// Coordinates in ONE eye's block — the distance from one eye's block to the
// next, and what kl_reproject_build wants as `grid_stride`.
static inline uint32_t kl_reproject_grid_stride(uint32_t nx, uint32_t ny) {
    return (nx + 1u) * (ny + 1u);
}

// ...and the size of a buffer holding `eyes` blocks, header included.
static inline uint32_t kl_reproject_grid_entries_n(uint32_t nx, uint32_t ny,
                                                   uint32_t eyes) {
    if (eyes < 1u) eyes = 1u;
    return 1u + eyes * kl_reproject_grid_stride(nx, ny);
}

// Vertices to draw. Two triangles a cell, six vertices, no index buffer —
// MTLPrimitiveTypeTriangle, not a strip.
static inline uint32_t kl_reproject_grid_vertices(uint32_t nx, uint32_t ny) {
    return 6u * nx * ny;
}

// The 1x1 grid whose corners are (0,0)..(1,1): an unfoveated pass, and exactly
// the picture this file drew before any of this existed. Always bind something —
// there is one code path, and "no foveation" is the identity grid rather than a
// second shader.
void kl_reproject_grid_identity(simd_float2 *out);

// Fill a grid from a rate map, without this file knowing what a rate map is.
//
// `fn` is the platform's screen->physical conversion — on Apple that is
// `-[MTLRasterizationRateMap mapScreenToPhysicalCoordinates:forLayer:]`, which
// exists on the CPU precisely so this can be precomputed. Both are in PIXELS.
// **NULL is legal and means the identity**, which is what makes the two things
// this grid corrects for one code path instead of two: a foveated eye and a
// partly-used one are the same question — "which texels did the guest write?"
// — asked of the rasterizer and of the viewport.
//
// `vp` is the guest's render viewport in SCREEN pixels, {x, y, w, h}, or NULL
// for the whole screen. It comes from kl_ovrp_render_pose.viewport, i.e. the
// rect the guest submitted with the frame; a title that lowers its render
// resolution does it by shrinking this and NOT by resizing the texture, so a
// grid that ignores it samples unwritten texels and puts the picture in a
// corner. Composes with foveation in the obvious order: the viewport is in
// screen space, so it is applied first and the rate map maps what survives.
//
// `tex_w`/`tex_h` are the eye texture's real dimensions, which the returned
// coordinates are normalised against. They are the SCREEN size, not the
// physical one: the runtime allocates eye textures at screen size and lets
// Metal write the smaller physical region inside them, so that GL, the viewport
// and the scissor all keep agreeing with each other (see kl_glfb.h).
//
// One grid buffer is bound for the whole amplified pass, so two eyes with
// different viewports are served by two BLOCKS in it rather than by two
// buffers: kl_reproject_grid_build_eye writes the header (identical either way)
// and the block for `eye`, and kl_reproject_build points each eye at its own.
// kl_reproject_grid_build is that with eye 0, i.e. the single-block grid every
// caller with one viewport still wants.
typedef void (*kl_reproject_s2p)(void *ctx, float sx, float sy, float *px, float *py);
void kl_reproject_grid_build_eye(simd_float2 *out, uint32_t eye,
                                 uint32_t nx, uint32_t ny,
                                 const float *vp,
                                 float screen_w, float screen_h,
                                 float tex_w, float tex_h,
                                 kl_reproject_s2p fn, void *ctx);
void kl_reproject_grid_build(simd_float2 *out, uint32_t nx, uint32_t ny,
                             const float *vp,
                             float screen_w, float screen_h,
                             float tex_w, float tex_h,
                             kl_reproject_s2p fn, void *ctx);

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
//   flip_y              1 if that picture has a top-left origin — i.e. the guest
//                       rendered it with Vulkan rather than GL. See the
//                       `flip_y` field above; the caller reads it from
//                       kl_glfb_eye_mtl_origin_top_left(eye, stage), which is
//                       where it was recorded with the texture.
//   grid_per_eye        nonzero if the grid bound with this draw carries one
//                       block per eye, so this eye should read its own; **0 for
//                       one shared block**, which is what a caller with a single
//                       viewport (and every caller before per-eye blocks
//                       existed) wants. Stated rather than derived because this
//                       file cannot see the buffer the caller will bind, and an
//                       eye reading another eye's crop has no error surface at
//                       all.
//
// `rendered` may be NULL, which means "no pose was recorded" — the uniforms then
// describe an unreprojected picture rather than an undefined one.
kl_reproject_uniforms kl_reproject_build(const kl_ovrp_render_pose *rendered, int eye,
                                         simd_float4x4 origin_from_device,
                                         simd_float4x4 device_from_view,
                                         simd_float4x4 projection,
                                         uint32_t slice, int flip_y,
                                         int grid_per_eye);

// One projection LAYER's uniforms — the same pass as kl_reproject_build, given
// one layer's own picture, frustum and pose instead of the frame record's.
//
// A guest that submits several projection layers is asking for each to be drawn
// with its own field of view, back to front, and that is all this is: the eye
// quad, once per layer. Nothing about it is new geometry — the rotation
// correction, the eye-centring, the 500 m placement and the sRGB decode are the
// eye pass's, unchanged — which is the argument for doing it this way. A
// flattening has to map one layer's frustum into another's; this maps each
// against the display and never against another layer.
//
// `slice` is the array slice of the layer's texture, as
// kl_glfb_eye_mtl_texture reports it for `pl->slot[eye]`. The flip comes from
// the record rather than from the caller: which way up a layer's picture is
// follows from the API that drew it, and the filer knows which that was.
//
// `visible` is 0 for an eye this layer does not name (slot < 0), which is how a
// one-view layer is expressed — the quad collapses instead of the caller having
// to skip a draw.
kl_reproject_uniforms kl_proj_layer_build(const kl_ovrp_proj_layer *pl, int eye,
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

// The NDC depth (z/w) the quad's centre actually lands at, given built uniforms.
//
// This exists because "is the quad too far to be shown" was argued about for a
// release and settled wrongly (see kl_reproject_build). It is one line of
// arithmetic against the projection the drawable really handed us, so the
// compositor can print it beside the drawable's own depthRange and the question
// stops being a matter of opinion. Reverse-Z, so near maps towards 1 and far
// towards 0; anything in [0,1] is inside the clip range.
float kl_reproject_ndc_depth(const kl_reproject_uniforms *u);

#endif
