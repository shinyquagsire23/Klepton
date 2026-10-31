# Fixed foveated rendering for Vulkan guests (`runtime/gfx/kl_fove.m`)

Klepton renders a Vulkan guest's eye passes through a Metal
**rasterization rate map** (`MTLRasterizationRateMap`): full shading rate
around each eye's straight-ahead point, a linear ramp, and a floor in the
periphery. The guest is not asked to do anything: Klepton recognises the eye
passes itself, sets the map on the Metal encoders MoltenVK opens for them,
patches the fragment shaders that read `gl_FragCoord`, and expands the
result back to screen layout before anything else reads it.

This document collects the measured facts the design rests on, the design
itself, and the measurements that justify it. The knobs and log lines are
listed in `DEBUG_ENV_VARS.md`, "Fixed foveation". The pure arithmetic is
covered by `tests/t_fove.c` (`make check`); the Metal side by the GPU
self-tests inside the same test when a Metal device with rate-map support is
present.

GLES guests foveate through a different path (`KL_VRR`, ANGLE's rate-map
registry and the compositor); nothing here applies to them.

Contents:

1. Metal rate-map semantics (measured)
2. Why not MoltenVK's own fragment density map support
3. The guest side, measured on Half-Life: Alyx
4. The design
5. The three eye shapes
6. MSAA eyes
7. Ordering: the expand fence (and the compute barrier)
8. Measurements
9. Known gaps
10. Turning it on

---

## 1. Metal rate-map semantics (measured)

Measured on an Apple M4 Pro (macOS 27, Xcode 27 SDK) with a standalone probe
that renders under a map and reads the result back from the GPU. The Metal
API validation layer reported zero errors for every case below. Base map for
1a-1f: screen 512x128, horizontal curve `[1, 0.25]` (full rate at the left
edge falling to quarter rate at the right), vertical `[1]` -> physical
344x128, granularity 32x32.

**1a. `[[position]]` is PHYSICAL.** At physical pixel (304, 10) the fragment
sees `position.xy = (304.5, 10.5)`, while the interpolated screen coordinate
there is 412.09 and `mapPhysicalToScreenCoordinates(304.5)` = 412.10. Any
SPIR-V `gl_FragCoord` read in a mapped pass is therefore wrong unless it is
remapped.

**1b. Viewport, scissor and `renderTargetWidth/Height` are SCREEN (logical)
coordinates.** Viewport x=256 w=256 covers physical columns 218..343 (the
screen->physical mapping predicts 217.7..344); scissor 128..384 covers
120..291 (predicted 120.2..291.7). A double-wide target with two viewports
(`[[viewport_array_index]]` from the instance id, one per eye) and a two-peak
horizontal curve puts the eye boundary at physical column 323 (predicted
323.5) and each eye's centre exactly where `mapScreenToPhysical` puts it, with
no uncovered columns. A screen-size `renderTargetWidth` draws the whole map; a
physical-size one clips it. A guest's per-eye viewports therefore pass through
untouched, and MoltenVK's framebuffer-extent values are already right.

**1c. Attachments may be SCREEN-size; Metal writes the top-left physical
region. LOAD/STORE across passes sharing a map works.** A 512x128 colour
target under the map: columns 0..343 written (exactly the physical extent),
texels beyond untouched. Two consecutive passes with the same map (pass 1
CLEAR + draw the left half, pass 2 LOAD + draw the right half) join at
physical column 218 (predicted 217.7) with nothing lost. Apple documents that
targets "need to be at least" `physicalSize`; bigger is within contract.
The validation layer does NOT check attachment size at all (a target SMALLER
than physical also passed silently), so it will not catch sizing bugs.

**1d. Depth, stencil and MSAA: no restrictions.** Depth32Float, Depth16Unorm
and Depth32Float_Stencil8, each at 1x and 4x MSAA (with colour resolve, and
depth resolve for the depth-only formats): the depth test is honoured at
exactly the predicted physical column in all six. Stencil written in pass 1
and tested in a LOAD pass 2 under the same map: exact. A resolve's output is
in the same physical layout, so a resolved image needs the expand as well.

**1e. Derivatives are per PHYSICAL fragment, so mip LOD rises in the
periphery automatically.** `dfdx(screen x)` = 1.00 at the full-rate edge,
1.34 / 1.84 / 2.75 further out; trilinear LOD = 0.00 / 0.42 / 0.88 / 1.45 =
log2(dfdx). Vulkan's fragment density map does the same (derivatives span the
larger fragment), so a guest written for FDM expects it.

**1f. Both mapping directions exist in MSL, and the expand is cheap.**
`rasterization_rate_map_decoder` with `map_screen_to_physical_coordinates` and
`map_physical_to_screen_coordinates` both compile at run time
(`newLibraryWithSource:`) and agree with the CPU API.

- Expand (a screen-size pass that decodes each pixel to physical and samples
  the physical image with `coord::pixel`, bilinear) against an unmapped
  reference of a smooth pattern: max abs error 0.0002 in the full-rate zone,
  0.021 in a ~0.3-rate zone (real resolution loss, not a mapping error).
- Inverse inside a mapped pass: `map_physical_to_screen(position.xy)` equals
  the interpolated screen coordinate to 0.006 px across the row. **A fragment
  shader recovers Vulkan's `gl_FragCoord` from Metal's `[[position]]` with one
  decoder call.**
- The parameter buffer is 24,624 B whatever the map's size (512x128, 5184x2016
  and 16384x16384 alike).
- An all-1.0 map is an exact identity (physical == screen; decode(1234.5,
  777.25) = (1234.5, 777.25)) at 5184x2016, 8192² and 16384². It is the
  "not foveated" binding for patched shaders.
- Expand cost at 5184x2016 RGBA8: 0.15-0.23 ms of GPU time on the host.

**1g. Granularity, quality floor and physical sizes.** Granularity is 32x32
physical px. The API accepted up to 4096 horizontal samples. Curves are
minimums and are rounded UP: the two-sample `[1, 0.25]` curve actually reaches
~0.36 at the edge (the curve is linear between samples and quantised to
32-px tiles). Quality 0 is accepted and is NOT clamped (the API will go to
~1/80 rate), so the caller must floor the curve itself. Per-eye profiles
mirrored into a two-peak horizontal curve at 5184x2016 (double-wide):

| profile (per eye, equidistant samples) | physical | area / screen | heavy fragment pass vs no map |
|---|---|---|---|
| mild `[.5,1,1,1,.5]` | 4400x1800 | 0.758 | 0.71-0.81x |
| medium `[.25,.5,1,1,1,.5,.25]` | 3608x1496 | 0.516 | 0.45-0.48x |
| strong `[.25,.25,.5,1,1,1,.5,.25,.25]` | 3100x1252 | 0.371 | 0.29-0.36x |
| all 0.25 | 1328x512 | 0.065 | 0.06x |

Fragment cost tracks physical area almost linearly in this synthetic pass.

**1h. Support.** `supportsRasterizationRateMapWithLayerCount:` = YES for 1
and 2 layers, NO for 4 and 8 on the M4 Pro. `MTLRasterizationRate.h` is
identical in the macOS and visionOS SDKs and every rate-map API is available
on visionOS. On the Vision Pro the GPU supports rate maps (Compositor
Services itself hands out per-eye rate maps). The visionOS Simulator is
expected to report no support; Klepton then logs one line and runs
unfoveated.

## 2. Why not MoltenVK's own fragment density map support

- **MoltenVK has no `VK_EXT_fragment_density_map` /
  `VK_KHR_fragment_shading_rate`** (checked at v1.4.2 and on `main`). The open
  upstream issue (#1710) records why a general implementation is hard: Metal
  builds rate maps from host-side curves, while a Vulkan density image can be
  device-local and written by the GPU.
- **SPIRV-Cross has no rate-map `FragCoord` option**, and
  `FragSizeEXT` / `FragInvocationCountEXT` do not translate to MSL at all
  (issue #1015). A shader using `GL_EXT_fragment_invocation_density` would
  fail to compile under MoltenVK whatever route is taken.
- **Building MoltenVK from source is cheap** (about 5 minutes cold for macOS +
  visionOS + simulator, under 1 GB, no patches needed for Xcode 27), so a
  source fork was a real option. It was not needed: every problem a fork
  would solve (choosing the curve, the `gl_FragCoord` remap, the expand) has
  to be solved the same way outside it, and the one part a fork would make
  tidier (the shader patch) can be done exactly on the MSL text MoltenVK
  hands Metal.

So Klepton **imposes the map from outside MoltenVK**: kl_vulkan recognises the
eye passes from their Vulkan identity, kl_fove swizzles the Metal classes
MoltenVK uses (`-renderCommandEncoderWithDescriptor:`,
`-newLibraryWithSource:...`) and does the rest in Metal. The decisive facts
were 1b (viewports and scissors are screen space, so a guest's per-eye
viewports pass through), 1c (screen-size attachments work, so no proxy
textures and no attachment swapping inside MoltenVK's passes) and 1f (the one
wrong thing, `[[position]]`, is fixed exactly by one decoder call, with an
identity map bound wherever nothing is foveated). A guest's own FDM path is
left OFF: Klepton picks the curve itself.

## 3. The guest side, measured on Half-Life: Alyx

Measured on the Mac host boot of Half-Life: Alyx (Source 2, OpenVR,
double-wide eye 5184x2016 at render scale 65), a fixed save, head standing
still. Nothing here changes rendering: the FDM probe strips FDM before
MoltenVK and the census only counts.

### 3.1 Valve's FDM path engages, but its map does not fit

Advertising `VK_EXT_fragment_density_map` (plus the `FRAGMENT_DENSITY_MAP`
format bit on R8G8_UNORM) is enough for Source 2 to enable FDM. Valve then
creates ONE 162x63 R8G8_UNORM map for the 5184x2016 target (32x32 px per
texel, whatever texel size is reported), fills it **once from the CPU**
(`vkCopyMemoryToImage`, `VK_EXT_host_image_copy`), and never updates it. It
chains the FDM features struct at vkCreateDevice with all three flags 0, never
queries the FDM properties, asks for no offset/eye-tracking extension, uses
dynamic rendering (`VkRenderingFragmentDensityMapAttachmentInfoEXT`), and no
shader reads `FragSizeEXT` / `FragInvocationCountEXT`. Exactly two passes per
frame carry the map: the eye clear (0 draws) and the main eye pass (~280
draws).

Valve's map describes ONE centred view (on the Quest each layer is one eye).
In the double-wide layout its full-density box sits across the two eyes'
nasal edges, so it is wrong as shipped. The box is sized by the render
target's height in both axes (square in pixels). Per level, read off the
maps (fraction = shaded fragments / pixels, re-centred per eye):

| level | full-density box | ramp | min density | per-eye fraction |
|---|---|---|---|---|
| low | 1216x1184 | 384 px | 0.333 | 0.511 |
| medium | 1024x992 | 384 px | 0.220 | 0.361 |
| high | 768x800 | 352 px | 0.125 | 0.229 |
| ultra (Valve's default) | 896x864 | hard edge | 0.220 | 0.189 |

The square-mode map is separable as a MIN: d(x, y) = min(fx(x), fy(y))
exactly.

A Metal rate map is a PRODUCT of a horizontal and a vertical curve. The
conservative separable map (never coarser than Valve's on either axis:
h = fx, v = fy) gives physical/screen = 0.635 (low), 0.477 (medium), 0.316
(high). A map matched to Valve's average would push the corners far below
Valve's own floor (0.065 at medium vs 0.22), which is why Klepton uses the
conservative shape with an explicit floor.

Running Valve's FDM path (with FDM stripped before MoltenVK) faulted in 3 of
15 host runs at varying sites in MoltenVK and the allocator, against 0 of 15
runs without it. Klepton's own map does not engage that path, and no fault
was seen with it (§8.3).

### 3.2 Who reads `gl_FragCoord`

In a level, 57 of 62 fragment modules load `gl_FragCoord`, and **279 of the
main pass's 280 draws** use a fragment shader that does. Every use is
"screen position -> 2D texture coordinate":

| pattern | draws/frame (of 280) | what it is |
|---|---|---|
| `uv = FragCoord.xy * k.zz + k.xy; textureLod(t, uv, 0)` | 279 | screen-space texture lookup |
| `floor(FragCoord.xy * s)`, `textureGather` of a low-res depth, compare with `FragCoord.z` | 209 | depth-aware upsample of a low-res buffer |
| `texelFetch(t, ivec2(FragCoord.xy), 0)` | 24 | integer-pixel fetch from a full-res screen texture |

Nothing in the density-mapped passes indexes a light grid or reconstructs a
position from `FragCoord`. So replacing `FragCoord.xy` by
`map_physical_to_screen(position.xy)` at shader entry fixes every use with no
per-shader logic. (`.z` is unaffected by a rate map.)

The eyes are laid out as one viewport over the whole double-wide target, no
viewport index, no layer, no multiview: instanced stereo split by clip
distance. Under a rate map that is fine (1b; clipping happens before
rasterization).

### 3.3 The eye's segments in one frame

| segment | attachments | load/store | carries FDM | draws |
|---|---|---|---|---|
| eye clear | colour 5184x2016 B10G11R11, depth D32S8 (+ resolve) | CLEAR/STORE | yes | 0 |
| kl_vulkan's single-sample resolve copy | eye depth -> copy target | copy | — | — |
| main eye pass | same colour + depth | LOAD/STORE | yes | 280 |
| ...in-pass local-read barrier, 1 draw reads the eye colour as an input attachment | | | (same pass) | 1 |
| bloom chain (15 passes) | | | no | 1 each; the first SAMPLES the eye colour |
| final composite/tonemap 5184x2016 RGBA8 sRGB | | | no | 1; SAMPLES the eye colour |

The eye depth is not read after the main pass; the eye colour is sampled by
the bloom downsample and the composite, both in screen space. **One colour
expand right after the main pass covers every consumer; no depth expand is
needed.** With the hidden-area mask on (`KL_OPENVR_HIDDEN_AREA`), two
per-eye depth-only passes sit between the clear and the main pass and must
use the same map.

## 4. The design

Klepton imposes the map; the guest's own FDM path stays off. Five pieces.

### 4.1 Which passes

kl_vulkan recognises the eye images where their Vulkan identity is plain and
hands their MTLTextures (via `VK_EXT_metal_objects`) to kl_fove. Which images
count is the subject of §5. On the Metal side, every render encoder MoltenVK
opens whose colour attachment 0 (or, with no colour attachment, whose depth
attachment) is a registered eye image is **marked**. In Half-Life: Alyx that
is 3 encoders per frame: the eye clear, the main pass, and the second encoder
MoltenVK opens inside the main pass after the local-read barrier (5 with the
hidden-area mask).

A marked encoder that LOADs the eye is only mapped while an expand of it is
pending on the same Metal command buffer (i.e. the eye is still in physical
layout); otherwise it draws unfoveated and is counted ("eye LOADs left
unfoveated"). Pending state is per Metal command buffer, tagged with a
per-buffer generation so a recycled buffer pointer never matches a stale
entry.

### 4.2 The map

One `MTLRasterizationRateMap` per target shape, screen size = the eye target,
separable. Per axis the profile is Valve's square profile re-centred on each
eye's straight-ahead point: full rate inside a box, a linear ramp, then the
floor. The straight-ahead point comes from the frustum the runtime hands the
guest (an off-axis projection puts it at ~63% of the eye's width from its
outer edge and ~46% from the top on the Vision Pro, not at the image centre).

| level | box (fraction of H) | ramp | floor | physical at 5184x2016 | fragments |
|---|---|---|---|---|---|
| low | 0.603 H (1216 px) | 0.19 H | 0.333 | 3952x1776 | 67.2% |
| medium | 0.508 H (1024 px) | 0.19 H | 0.22 | 3456x1600 | 52.9% |
| high | 0.381 H (768 px) | 0.19 H | 0.125 | 2816x1340 | 36.1% |

(The continuous curves are 64/49/32%; Metal rounds each 32-px tile up.)

**Both eyes' points, not one (the envelope).** On the Vision Pro each eye's
cone is wider outward than inward (60 / 45 degrees), so its straight-ahead
point is 63% across the left eye and 37% across the right. A profile peaked
there alone leaves the outer side of each eye in the ramp and floor: in
Alien: Rogue Incursion (toolchain 565, `high`) the left eye was blurred over
its left half and the right eye over its right half. Doom 3's owner had made
the same complaint about the display's own per-eye curves on the GL path
(f9f514f). Since then every eye image and every map layer is, per axis, the
larger of two profiles: one at its own eye's point, one at the other eye's
point placed in the same rect (its fraction, this eye's viewport). The two
eyes' maps are then the same relative to their pictures and symmetric about
the middle, and both points stay at full rate. A symmetric cone (or Unity's
union cone) makes the two points one, which changes nothing. Measured through
the GPU on Alien's shape (`t_fove`, rate by eighths of the left eye's
picture): one peak 0.16 0.27 0.69 0.99 | 1.00 1.00 1.00 0.76; envelope 0.77
1.00 1.00 1.00 | 1.00 1.00 1.00 0.75, the same for the right eye. It costs
fragments: 49% of the image instead of 39% at `high` there.
`KL_FOVE_PER_EYE=1` brings back the single peak.
`KL_FOVE_BOX`, `KL_FOVE_RAMP` and `KL_FOVE_FLOOR` override the numbers.
Box 0.30 / floor 0.08 at `high` was found invisible through the lenses in
Half-Life: Alyx, and is the setting the targets that bake foveation use.

The map is set on a copy of each marked descriptor in the swizzled
`-renderCommandEncoderWithDescriptor:`, rendering **in place** into the
guest's own screen-size images (1c). Metal snaps the full-rate region's
physical->screen translation to whole pixels, so the fovea is not resampled.

### 4.3 `gl_FragCoord`

MoltenVK hands Metal MSL source through
`-newLibraryWithSource:options:completionHandler:` (and the `:error:`
variant); both are swizzled. Every SPIRV-Cross fragment entry point
(`fragment ... main0(`, or Unreal Engine 5's `main_XXXXXXXX_XXXXXXXX(`, which
SPIRV-Cross keeps unrenamed) that takes `[[position]]` gets one more argument,
`constant rasterization_rate_map_data& kl_fove_rrm [[buffer(N)]]` (N = 20 by
default, `KL_FOVE_BUFFER`), and as its first statement

    gl_FragCoord.xy = rasterization_rate_map_decoder(kl_fove_rrm)
                          .map_physical_to_screen_coordinates(gl_FragCoord.xy, layer);

(z/w untouched). Marked encoders bind the real map's parameter buffer at N;
every other render encoder binds the all-1.0 identity map's (1f). The layer
argument is the shader's `[[render_target_array_index]]`: an existing
argument or stage-in member is reused (a second argument with the same
attribute does not compile), otherwise one is added; Metal answers 0 when the
vertex stage does not write it, which is right for every non-layered pass.

Reads that must stay PHYSICAL keep the original position
(`float4 kl_fove_phys = gl_FragCoord;` is saved before the remap):

- **Subpass inputs.** MoltenVK does not use framebuffer fetch for an input
  attachment here; SPIRV-Cross emulates it as
  `tex.read(uint2(gl_FragCoord.xy))`, adding a `[[position]]` to a shader
  whose SPIR-V never reads FragCoord. That read is the same pixel of an
  attachment that is in physical layout during the pass, so exactly
  `uint2(gl_FragCoord.xy)` is rewritten to `uint2(kl_fove_phys.xy)`. The
  guest's own texelFetch (spelled `uint2(int2(gl_FragCoord.xy))`) is remapped
  as it should be.
- **Subpass inputs read in a helper function.** Unreal 4's mobile renderer
  reads the scene depth (DepthFade, soft-edge translucency) through
  `VulkanSubpassDepthFetch(texture2d_ms_array<float> …, thread float4&
  gl_FragCoord, thread uint& gl_ViewIndex)`, a static inline function ABOVE
  the entry point that does the `.read(uint2(gl_FragCoord.xy), …)` itself.
  A scan of the entry body alone misses it: the helper then gets the remapped
  screen position and reads the physical-layout depth at the wrong texel. In
  Moss that showed as a hard black band in place of the soft shadow in the
  book's fold, with foveation on only (Moss's eye: a 2-layer x4 MSAA scene, a
  depth-read subpass, both under one map). So a helper that takes the
  position as `thread float4& <pos>` and uses it ONLY as
  `<tex>.read(uint2(<pos>.xy)…`, with `<tex>` one of its own texture
  parameters, is passed `kl_fove_phys` at its call sites in the entry body
  instead; a helper that uses the position for anything else (or reads a
  texture it does not take as a parameter) keeps the remapped one.
  Moss's Archive level: 22 of 28 patched fragment entry points have such a
  call (0 were kept physical before). `KL_FOVE_SUBPASS_HELPERS=0` restores the
  old behaviour as an A/B control.
- **Multisampled reads of an eye image** at its own pixel
  (`<ms texture>.read(uint2(int2(gl_FragCoord.xy)), sample)`, used by
  Source 2's soft particles on an MSAA eye): physical, see §6.

Safety nets, each turning foveation off with one line: a `[[position]]`
shader that already uses slot N is refused; a patched shader that fails to
compile is recompiled from the original source; a `setFragmentBuffer*` at slot
N by anyone but kl_fove is counted and, inside a marked encoder, turns
foveation off. In Half-Life: Alyx: 138 libraries, 65 fragment entry points
patched, 4 without `[[position]]` (UI), 0 refused, 0 clobbers. Only when
foveation is on is any source patched. Patched shaders get new Metal
shader-cache keys, so the first launch after enabling foveation recompiles
every fragment shader once.

### 4.4 The expand

After the last marked encoder, lazily at the next non-marked render or compute
encoder on the same Metal command buffer (or at its `-commit`): blit the
physical region of the eye colour into a scratch texture, then one fullscreen
draw back over the whole eye colour with `map_screen_to_physical_coordinates`,
bilinear, each eye's sample clamped to its own physical region so nothing
bleeds across the nasal seam (double-wide), or per slice with that slice's
map layer (layered eyes). Blit encoders do not trigger it for an eye image
(kl_vulkan's single-sample depth-resolve copy sits between the eye clear and
the main pass in the same command buffer); an intermediate that may be
copied into the eye is expanded before a blit too (§5.3). Depth is not
expanded: nothing reads the eye depth after the main pass in any shape seen.

Scratch textures are cached per (pixel format, sample count), up to four
ways per use, and remade only when an entry's size changes. A guest that
expands two intermediates of different formats every frame (Paint the Town
Red: an RG11B10 HDR scene and an RGBA8 sRGB post target) would otherwise
replace each scratch twice a frame; Metal does not return replaced textures
fast enough, and the footprint climbed by ~2.6 GB in ~2 s until the process
was killed. Replaced scratch textures are parked, never freed, because an
in-flight command buffer may still read them.

### 4.5 Instrumentation and the off path

A `[fove]` stats line every 10 s (encoders marked, eye LOADs left unfoveated,
expands, refused, clobbers), and `kl_fove_perf_field` gives the same in short
(level, fragment share, expands, marked, refused, clobbers per second) for a
periodic stats line.

Off (the default): `kl_fove_install` returns at once; nothing is swizzled,
patched or bound; kl_vulkan's foveation entry points are not handed out and
its pass check is one cached `kl_fove_on()` test. Measured against a binary
without the feature, the off path is noise (GPU median 8.30 vs 8.20 ms), with
the same set of log-line prefixes.

## 5. The three eye shapes

### 5.1 OpenVR double-wide

A `vkCmdBeginRendering` pass whose colour attachment 0 and depth are both
exactly the extent of the texture the guest hands `IVRCompositor::Submit`
(NOT `GetRecommendedRenderTargetSize` x 2: the engine rounds its own eye
size), single-sample, and whose colour is not an image the guest has
submitted (the final composite has the same extent and is sometimes drawn
with the eye depth bound; mapping it breaks the picture). One map, a
horizontal curve with one peak per eye.

This shape needs an OpenVR runtime that reports the submitted texture's
extent and both eyes' frusta. There is none in this tree yet: kl_fove keeps
the maths and the map (`kl_fove_curves`, tested), and the recognition answers
"not an eye" until such a runtime reports its geometry
(`kf_double_eye_geometry` in kl_fove.m).

### 5.2 The runtime's own eye images (direct, or MSAA + resolve)

Unity and Unreal Vulkan guests draw their eyes with `vkCmdBeginRenderPass`
into the swapchain kl_ovrp or kl_openxr handed them, usually a 2-layer ARRAY
image (multiview, viewMask 0x3). The eye pass is identified by the images: a
render pass whose colour attachment 0 IS a runtime eye image (**direct**), or
whose resolve attachment 0 is (**MSAA + resolve**: an eye-size MSAA target
resolved into the eye inside the pass), with a depth attachment of the same
extent and sample count.

- A 2-layer eye gets ONE map with two layers, layer l built around eye l's
  projection centre (and, by default, the other eye's: §4.2). It is foveated only as a LAYERED Metal pass
  (`renderTargetArrayLength` 2, MoltenVK's lowering of multiview on Apple
  GPUs); a one-slice pass over the pair draws unfoveated (counted). A one-eye
  image gets a one-layer map for that eye.
- An MSAA pass that resolves in-pass is expanded on the single-sample image it
  resolves into. MoltenVK opens such a pass with storeAction Unknown and the
  resolve texture set, picking the real action at endEncoding; a resolve
  texture with Unknown is treated as resolving. The expand runs per slice.
- **The straight-ahead point** is the frustum's fraction inside the guest's
  render viewport for that eye. The frustum is the guest's own eye-layer FOV
  when it set one up (under symmetric projection Unity renders each eye with
  the union cone into a widened texture), else the one the runtime told it.
  The viewport is read as a FRACTION of the texture it was measured against
  (`kl_ovrp_render_pose.viewport_of`) and applied to the image the map is
  built for: a guest that re-creates its eye images at another size after the
  first frame (Into the Radius, Unity's ~1.2x resize) would otherwise get a
  map centred from the previous generation's rectangle (848 px off in that
  case). A rect that does not fit = the full image.
- **Re-centre on the frame record.** A layered map built before the first
  frame record used the full image for each eye's point; Deadpool renders each
  eye into a ~4012-wide sub-rect of a 5088 texture, so its fovea sat 682 px
  off (a hard clear/blurry seam in both eyes). An ARRAY map's centres are
  recomputed on lookup and the map is rebuilt in place when one moves more
  than 4 px: in place when no pending expand refers to it, else into another
  map slot, the old one kept unchanged for the expands still pending on it
  and recycled once none is (a command buffer that already drew with a map
  keeps it for its remaining passes, so a scene subpass and its eye subpass
  share one). Replaced maps are kept alive 2 s for frames in flight, then
  released. Until toolchain 565 a referenced map was kept as it was, at most
  8 re-centres a run: Alien: Rogue Incursion (dynamic resolution, an expand
  always in flight) kept its 5th map while the viewport went from 79% to 55%
  of the image.
- **Render-area snap** (`KL_FOVE_AREA_SNAP`, default 1). MoltenVK honours
  loadOp CLEAR as a Metal clear only when the render area IS the framebuffer
  extent; otherwise every attachment opens with LOAD and the clear is drawn as
  a quad, and a LOADing eye pass with nothing pending cannot be mapped. Unreal
  quantizes its scene buffer up to a multiple of 4 and renders the
  unquantized height (Pinball FX: area 4608x2935 on a 4608x2936 eye), so an
  eye pass whose area falls a few px short of its framebuffer is widened to
  it. The extra rows/columns lie outside every viewport the guest submits.

### 5.3 The guest's own eye-size intermediate

The scene pass's colour is NOT an eye image but is eye-sized (2 layers /
viewMask 0x3 for a pair, depth of the same extent and samples), and reaches
an eye image later. Batman: Arkham Shadow (a custom Unity render pipeline)
and Unreal's SceneColor -> tonemap are this family. `KL_FOVE_INTERMEDIATE=0`
turns the shape off.

- **Size slack.** The intermediate need not be exactly eye-size: equal or up
  to +16 px per axis, never smaller (`kl_fove_inter_extent_ok`), because
  Unreal rounds its scene target up (5088x3236 for a 5088x3234 eye). The map
  is sized to the intermediate and the expand runs on it (or on what it
  resolves into), so the eye being smaller changes nothing downstream.
- **(a) In-pass (`KL_FOVE_VIA_SUBPASS`).** A later subpass of the SAME render
  pass writes (or resolves into) a runtime eye image (e.g. 3 subpasses:
  2-sample depth, 2-sample colour stored DONT_CARE, the post subpass reads the
  colour as an input attachment and writes the eye). Marked at once: every
  Metal encoder of the pass (scene subpasses and the eye subpass) gets the
  SAME map; the scene stays in physical layout (it is transient: its storeOp
  must be DONT_CARE, the eye attachment must not be LOADed, all subpasses
  share one view mask); subpass inputs read it at the physical position; and
  the EYE image is expanded after the pass. An encoder MoltenVK opens between
  the subpasses does not end the pending entry (only `-commit` does). An eye
  subpass left unmapped while its scene is pending is a broken frame
  (counted as UNMAPPED, logged as `BROKEN FRAME`).
- **(b) Copied or sampled later (`KL_FOVE_VIA_XFER`, `KL_FOVE_VIA_SAMPLED`).**
  The pass's colour is a CANDIDATE, confirmed (sticky until vkDestroyImage)
  by a `vkCmdCopyImage[2]` / `vkCmdBlitImage[2]` / `vkCmdResolveImage[2]` into
  an eye image, or by the next render pass that draws into an eye image (a
  heuristic: that pass is assumed to sample it). A pass into an eye image
  with the eye as its own colour confirms every candidate noted since the
  previous such pass, with or without a depth attachment (Unity binds the
  scene depth, DONT_CARE, to its final blit into the eye), so every hop of a
  post chain (HDR scene -> LDR post target -> eye) is confirmed at once; an
  MSAA pass resolving into the eye, or an in-pass intermediate, only closes
  the window. The confirming frame stays unfoveated; afterwards the
  intermediate is marked like an eye and expanded before the next render,
  compute **or blit** encoder (so its copy into the eye reads screen layout).
- **Transient MSAA scene.** Unreal 4's depth-read subpass pair: subpass 0
  draws the MSAA scene (colour CLEAR/DONT_CARE), subpass 1 LOADs it, reads
  depth as an input and resolves into a single-sample attachment. MoltenVK
  opens subpass 0 as a layered encoder that STORES the samples with no
  resolve, which the MSAA rule (§6) would refuse. When kl_vulkan sees that the
  confirmed intermediate's colour is DONT_CARE and a later subpass resolves it
  into a single-sample attachment (`kl_fove_note_ms_transient`), the layered
  no-resolve encoder is marked and its pending entry HELD (no expand: the
  samples die with the pass), the LOADing resolve encoder is marked with the
  same map, and the single-sample resolve target is what gets expanded. No
  per-sample expand is needed. A layered MSAA store whose samples ARE kept
  still draws unfoveated.
- **Split passes are refused.** MoltenVK splits a multiview render pass
  around every indirect draw: a compute encoder adjusts the instance count,
  then the pass resumes with LOAD. Opening that compute encoder flushes the
  expand mid-pass, so the resumed half draws in screen layout over a depth
  still in physical layout (in Paint the Town Red's combat the view went
  black outside ~30% of the frame, or see-through). So:
  - kl_vulkan refuses an intermediate whose pass records an indirect draw
    (`vkCmdDraw*Indirect*`), including one recorded in a SECONDARY command
    buffer that the pass executes (`vkCmdExecuteCommands`; Unity records its
    scene draws in secondaries). State 3 = refused: never a candidate or
    confirmed again until the image is destroyed.
  - The Metal side refuses too, at the first split it sees (a render encoder
    LOADing an intermediate whose pending expand a compute encoder on the
    same command buffer just flushed), in case the record-time check missed
    it. That frame is already drawn wrong; from the next it is unfoveated.

## 6. MSAA eyes

Foveated unless `KL_FOVE_MSAA=0`. Three cases, by what the pass keeps:

- **Resolved in the pass:** the single-sample resolve target is expanded like
  any single-sample eye (§5.2).
- **Stored with no resolve (one slice only):** the guest's own resolve (in
  Half-Life: Alyx with `+vr_msaa`, a compute pass after the eye passes) reads
  every sample, so the expand must leave a MULTISAMPLED screen-layout image.
  Three steps on the eye's command buffer: (1) resolve the physical region
  into a single-sample physical-size scratch; (2) blit each eye's full-rate
  rectangle (where physical = screen - a whole-pixel offset) with all samples
  into a small multisampled scratch; (3) on the eye, a pixel-rate draw writes
  the bilinear-filtered resolved value to every sample (the periphery), then
  one per-sample draw per eye, scissored to its full-rate rectangle, copies
  the fovea's samples back, so the fovea keeps its MSAA edges sample for
  sample. A LAYERED MSAA pass that stores its samples with no resolve draws
  unfoveated (counted), except the transient case of §5.3.
- **Depth-only clear of a tiled-off MSAA eye:** mapped, and leaves a
  depth-only pending entry so the LOADing passes that follow are mapped.

Multisampled reads of an eye image at its own pixel (`2d_ms` textures read
at `gl_FragCoord`) keep the PHYSICAL position.

## 7. Ordering: the expand fence (and the compute barrier)

**The expand fence** (`KL_FOVE_FENCE`, default 1). MoltenVK (heaps, argument
buffers) does not rely on Metal's hazard tracking to order its passes: every
encoder it opens waits on its own barrier fences and updates them at its end
(~8 waits and 2-3 updates per encoder in Half-Life: Alyx). The expand's
encoders are Klepton's and take no part in that, so the guest's next reader
of the eye could run CONCURRENTLY with the expand. When the reader is the
very next encoder (Source 2's compute MSAA resolve right after the eye
passes) it read the eye while it still held part of the physical layout:
shrunken copies of the scene in the top-left of the left eye, and blocks of
the wrong picture.

Fix: the last encoder of every expand path (single-sample one-slice,
single-sample layered, MSAA) updates a fence, and every render, compute or
blit encoder opened afterwards on the same queue waits on it: in the same
command buffer at once; in a later command buffer once the expand's buffer is
committed (so a wait never precedes its update in submission order). A wait
on a fence whose update has completed costs nothing. The layered
single-sample expand is two encoders: slice 0 updates a second fence, slice 1
waits on it and updates the main one, so the one fence the guest waits on
covers both. Metal fences are per queue: an encoder on another
`MTLCommandQueue` cannot be ordered and is counted as such. `KL_FOVE_FENCE=0`
is the unordered control.

**The compute barrier** (a separate kl_vulkan fix for every Vulkan guest, not
part of foveation and not in this tree). Source 2's MSAA compute resolve records one
dispatch and then barriers whose `srcStageMask` (0x1600) leaves
COMPUTE_SHADER out. By the letter of Vulkan that orders nothing after the
dispatch; a driver that serialises everything hides it, MoltenVK does not,
and later passes read the previous frame's values in whole threadgroup blocks
("vertical rectangles"). A command buffer that recorded a dispatch gets
COMPUTE_SHADER / SHADER_WRITE added to the source scope of its next pipeline
barrier that does not already name COMPUTE_SHADER or ALL_COMMANDS. It showed
up with foveated MSAA eyes, which is why it is listed here.

## 8. Measurements

Host numbers: Mac host boot of Half-Life: Alyx (Apple M4 Pro), render scale
65 (2592x2016 per eye, 5184x2016 double-wide), MSAA 1, head standing still,
3 interleaved runs per level, medians.

### 8.1 Pictures

Full-size eye dumps at a fixed frame, foveated vs unfoveated, share of pixels
differing by more than 8/255 inside the full-density box vs outside (eye 0 /
eye 1):

| comparison | in box | outside |
|---|---|---|
| off vs off (second run) | 0.11% / 0.10% | 0.58% / 0.39% |
| off vs low | 0.04% / 0.03% | 3.8% / 5.2% |
| off vs medium | 0.71% / 0.71% | 6.2% / 8.0% |
| medium vs medium | 0.92% / 0.92% | 0.75% / 0.63% |
| off vs high | 0.35% / 0.36% | 9.4% / 11.9% |
| main menu: off vs medium | 0.21% / 0.22% | 0.17% / 0.16% |

The in-box differences sit entirely on an animated glow whose phase depends
on frame pacing (medium vs medium differs there as much): the fovea is
pixel-exact. The periphery is softer; at `high` the corners show 8-px
stair-steps on edges; lighting, fog, decals, shadows and hands are aligned,
nothing shifted or doubled, no seam at the nasal boundary, no garbage where
the physical region ends, and the hidden-area mask edge lands exactly where
it does unfoveated.

The `gl_FragCoord` patch is shown by the debug picture
`KL_FOVE_FRAGCOORD=grid`: every patched shader paints a 64-px grid of the
position it sees, in foveated passes only. Remapped, the grid is regular in
screen space and continuous across both eyes and the seam; with `gridraw`
(position left physical) the cells stretch toward the periphery and the
eye-1 boundary is displaced. In the tested Half-Life: Alyx scenes, disabling
the patch does not visibly change the game picture (the screen-space textures
those lookups sample are small or gated off there); the patch is required for
correctness in principle and costs nothing measurable.

### 8.2 Performance

Plain `[perf]`, last 20 in-level seconds of each run:

| level | GPU ms/frame | host fps (free-running) | vs off |
|---|---|---|---|
| off | 8.70 | 115.6 | — |
| low | 7.80 | 128.3 | -0.9 ms (-10%) |
| medium | 7.10 | 140.9 | -1.6 ms (-18%) |
| high | 6.30 | 159.0 | -2.4 ms (-28%) |

`KL_PERF_PASSES=1` (exclusive encoder time):

| level | GPU ms | main eye pass | of which vertex | expand (blit + draw) | fragments |
|---|---|---|---|---|---|
| off | 8.50 | 6.21 | 1.01 | — | 100% |
| low | 7.80 | 5.12 | 1.01 | 0.48 | 67.2% |
| medium | 7.00 | 4.38 | 0.99 | 0.41 | 52.9% |
| high | 6.20 | 3.66 | 1.01 | 0.35 | 36.1% |

The main pass's fragment time falls to 79/65/51% for 67/53/36% of the
fragments: less than proportional (per-tile fixed work, attachment
load/store). The expand costs 0.35-0.48 ms (a 22 MB blit plus the draw).
Memory: one scratch texture of physical size x 4 B (28 / 22 / 15 MB at
low/medium/high for this target) plus two 24.6 KB parameter buffers.

On the Vision Pro, `high` with box 0.30 / floor 0.08 shades roughly a
quarter of the eye pass's fragments in the titles that bake it (the
`[perf]` line's `fove` field reads ~22-26%); how much frame time that buys
depends on how GPU-bound the title is at its eye size.

### 8.3 Stability

10 level runs with foveation on (medium x4, low x3, high x3, 45 s each in the
level) plus ~25 other foveated runs (pictures, perf, menu, motion, hidden
area, grid): 0 faults, 0 device losses, 0 refusals, 0 slot clobbers, 0 "eye
LOADs left unfoveated", foveation never turned itself off; 63-65 fragment
entry points patched per run, 3 marked encoders and 1 expand per frame
throughout.

The one failure during development explains a safety net: a patch that
generated invalid MSL left the guest without pipelines and the GPU timed out
(`VK_ERROR_DEVICE_LOST`). A patched shader that fails to compile now falls
back to its original source and turns foveation off.

## 9. Known gaps

- **Expand cost** 0.35-0.48 ms on the host. Expanding straight from a
  physical proxy (no blit) would need proxy attachments inside MoltenVK's
  passes, which this design avoids.
- **Fragment scaling is sub-linear** (65% of the fragment time at 53% of the
  fragments), probably per-tile fixed cost; not investigated.
- **Depth is never expanded.** Nothing in the shapes seen reads the eye depth
  after the eye pass; kl_vulkan's single-sample depth-resolve copies
  therefore copy a physical-layout depth. If depth submission ever matters,
  that copy target needs its own expand.
- **Eye passes split across Metal command buffers** draw the later part
  unfoveated (counted) — correct, just no saving.
- **A layered MSAA pass that stores its samples** (not transient) draws
  unfoveated: the per-sample expand is one-slice only.
- **The sampled-intermediate confirmation is a heuristic** (the next pass into
  the eye is assumed to read the candidate). A guest for which this is wrong
  can turn the shape off with `KL_FOVE_INTERMEDIATE=0` and keep its direct
  eye passes foveated.
- **Every patched shader runs one decoder call per fragment** in every pass
  (identity outside the eye passes); included in the measurements above.
- **First launch after enabling** recompiles every fragment shader once
  (new Metal shader-cache keys).
- **The pending-expand table** is keyed by command buffer and generation and
  is bounded. A Metal command buffer released without `-commit` drops its
  pending expand and leaves no entry (tested; counted as "dropped
  uncommitted"); a full table is counted as "table full".

## 10. Turning it on

`KL_FOVEATION=off|low|medium|high` (default `off`), from the environment
(`visionos/run.sh` forwards every `KL_*` variable into the app it launches). With no rate-map support the runtime logs one line and
runs unfoveated; with no 2-layer support, 2-layer eye pairs stay unfoveated.

Lines that prove engagement (all prefixed `[fove]` or `[vk] [fove]`):

- `ON — ...` with the device and command-buffer class names (they differ per
  GPU and are discovered at run time);
- `EYE PASS (marked) render pass ...`, or `marks pass: ...` for the
  double-wide shape;
- `eye target registered: ...` / `intermediate target registered: ...`;
- `map WxH, N layer(s) ... centre ...` (on the headset the centres are not
  50%/50%: the frusta are asymmetric);
- `first expand: ...`;
- every 10 s, the stats line: encoders marked, eye LOADs left unfoveated,
  expands, refused, clobbers. A near miss is named once with its full
  attachment table: `near miss, NOT marked: ... | why: ...`.

Debug knobs: `KL_FOVE_FRAGCOORD=0|grid|gridraw`, `KL_FOVE_EXPAND=0`,
`KL_FOVE_MSL_DUMP=<dir>`, `KL_FOVE_TRACE_ENC=<n>`, `KL_FOVE_PROBE=1`,
`KL_FOVE_SLOW=<n>`, `KL_FOVE_MSAA_FOVEA=0`. All of them are described in
`DEBUG_ENV_VARS.md`.
