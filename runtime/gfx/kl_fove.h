// kl_fove — fixed foveated rendering of a Vulkan guest's eye passes through a
// Metal rasterization rate map. kl_fove.m summarises the design; the measured
// facts and the full design are in docs/FOVEATION.md.
//
// KL_FOVEATION=off|low|medium|high (default off). Off means nothing here
// is installed and nothing is swizzled: kl_fove_on() is 0 and every other entry
// point returns at once.
#pragma once
#include <stdint.h>

// 1 when foveation was asked for (and has not been turned off: no rate-map
// support, a refused shader, a clobbered buffer slot).
int  kl_fove_on(void);
// Swizzle MoltenVK's MTLDevice (shader source) and command-buffer classes.
// Call once a Metal device exists, before the guest compiles any shader.
void kl_fove_install(void *mtl_device);
// kl_vulkan: is (w x h) the extent of the texture the guest hands Submit (the
// double-wide eye; 0 before the first Submit)?
int  kl_fove_eye_extent(uint32_t w, uint32_t h);
// kl_vulkan: these MTLTextures are an eye colour/depth pair (depth may be NULL).
// Passes that draw into either get the rate map.
// samples > 1 = an MSAA eye (only registered when kl_fove_msaa()).
void kl_fove_note_eye(void *mtl_colour, void *mtl_depth, uint32_t w, uint32_t h, uint32_t samples);
// 1 when MSAA eyes are foveated too: foveation on and KL_FOVE_MSAA not 0.
// 0 = MSAA eyes stay unfoveated.
int  kl_fove_msaa(void);
// kl_vulkan: an eye target of a guest that renders into the
// runtime's OWN eye images (kl_ovrp / kl_openxr swapchains). `col` is the
// colour the eye pass draws (the swapchain image itself, or the eye-size MSAA
// target it resolves into one); layers 2 = both eyes as the two slices of one
// 2D-array image (multiview: foveated as a layered pass, one map layer per eye,
// each built around that eye's projection centre from kl_ovrp_eye_tangents and, unless
// KL_FOVE_PER_EYE=1, the other eye's in the same rect: kl_fove_curves_env);
// layers 1 = one eye's own image (`eye` 0/1).
void kl_fove_note_eye_array(void *mtl_colour, void *mtl_depth, uint32_t w, uint32_t h, uint32_t samples,
                            uint32_t layers, int eye);
// kl_vulkan: the image behind this texture is going away.
void kl_fove_forget(void *mtl_texture);
// A short status field for a periodic stats line ("" when off).
void kl_fove_perf_field(char *out, unsigned cap);

// The profile maths, public for the unit test (tests/t_fove.c). Fills h[nh]
// (horizontal, samples evenly spaced over the screen width, edge to edge) and
// v[nv] for a double-wide target W x H whose eye e is centred (straight ahead)
// at x = cx[e] (screen px) and both eyes at y = cy. box, ramp are in px,
// floor in (0,1]. Returns 0 on bad input.
int  kl_fove_curves(uint32_t W, uint32_t H, const float cx[2], float cy,
                    float box, float ramp, float floor_, int nh, float *h, int nv, float *v);
// One eye's curves: a W x H one-eye image whose straight-ahead
// point is (cx, cy) screen px.
int  kl_fove_curves1(uint32_t W, uint32_t H, float cx, float cy,
                     float box, float ramp, float floor_, int nh, float *h, int nv, float *v);
// The envelope of two such peaks, (cx[0], cy[0]) and (cx[1], cy[1]): per axis
// the larger of the two profiles. What a one-eye image or a map layer gets by
// default — its own eye's straight-ahead point and the other eye's, placed in
// the same rect, so both eyes' maps are the same relative to their pictures
// (KL_FOVE_PER_EYE=1: the single peak). Returns 0 on bad input.
int  kl_fove_curves_env(uint32_t W, uint32_t H, const float cx[2], const float cy[2],
                        float box, float ramp, float floor_, int nh, float *h, int nv, float *v);
// kl_fove_curves with the envelope: eye 0's half of the double-wide target
// peaks at cx0[0] and cx0[1], eye 1's at cx1[0] and cx1[1].
int  kl_fove_curves_env2(uint32_t W, uint32_t H, const float cx0[2], const float cx1[2], float cy,
                         float box, float ramp, float floor_, int nh, float *h, int nv, float *v);
// The straight-ahead point of an eye from its frustum tangents, as a fraction
// of the eye's width from its left edge / of its height from the top.
void kl_fove_centre(const float tan_lrdu[4], float *fx, float *fy);
// The straight-ahead point in screen px of a W x H one-eye image, from the
// fraction (fx, fy) of kl_fove_centre and the guest's render viewport for that
// eye: vp = {x, y, w, h} as kl_ovrp_render_pose.viewport[e], measured against
// a texture of of = {w, h} (kl_ovrp_render_pose.viewport_of). Public for the
// unit test. Returns 1 when the rect moved the point, 0 when the full image was
// used.
// Is a guest render target of w x h an eye-size intermediate for an eye image
// of ew x eh? Equal, or larger by at most KL_FOVE_INTER_SLACK px per axis
// (Unreal rounds its scene target up, e.g. 5088x3236 for a 5088x3234 eye);
// never smaller. Public for the unit test.
#define KL_FOVE_INTER_SLACK 16u
int  kl_fove_inter_extent_ok(uint32_t ew, uint32_t eh, uint32_t w, uint32_t h);
int  kl_fove_eye_px(float fx, float fy, uint32_t W, uint32_t H, const int vp[4], const int of[2],
                    float *cx, float *cy);

// The pending-expand table (kl_fove.m), public for the unit test: entries are
// (command buffer, generation) -> eye colour texture. find returns the slot or
// -1; put returns 0 when the table is full; take removes and returns the texture.
int   kl_fove_pend_find(void *cb, uint64_t gen);
int   kl_fove_pend_put(void *cb, uint64_t gen, void *tex);
void *kl_fove_pend_take(void *cb, uint64_t gen);
int   kl_fove_pend_free_slots(void);
// Exercises the per-command-buffer generation tags with plain objects (buffers
// released without -commit must leave no entry). Returns the failed checks.
int   kl_fove_pend_selftest(void);

// t_fove: kl_fove's MSL patch applied to `src` with the default
// knobs; a malloc'd patched copy, or NULL when the source is left alone.
char *kl_fove_patch_for_test(const char *src);
// t_fove: KL_FOVE_SUBPASS_HELPERS for the next kl_fove_patch_for_test calls
// (1, the default: a helper's subpass read gets the physical position; 0: the
// old behaviour). kf_read_env sets the same knob from the environment.
void  kl_fove_set_subpass_helpers_for_test(int on);

// The guest's OWN eye-size scene target ("intermediate") that reaches an eye
// image — e.g. Batman: Arkham Shadow's render pipeline draws its 2-layer MSAA
// scene into its own target and writes the eye from it; Unreal's SceneColor ->
// tonemap is the same family. KL_FOVE_INTERMEDIATE=0 turns this off
// (default on whenever KL_FOVEATION is).
enum {
    KL_FOVE_VIA_SUBPASS = 1,   // a later subpass of the same render pass writes (or resolves into) the eye image
    KL_FOVE_VIA_XFER    = 2,   // vkCmdCopyImage[2] / vkCmdBlitImage[2] / vkCmdResolveImage[2] into the eye image
    KL_FOVE_VIA_SAMPLED = 3,   // a later pass draws into the eye image (reads the intermediate as a texture)
};
// 1 when foveation is on and KL_FOVE_INTERMEDIATE is not 0.
int  kl_fove_intermediate(void);
// kl_vulkan: a confirmed intermediate. `col`/`dep` = the scene pass's MTLTextures;
// `out` = the eye image a later subpass writes (VIA_SUBPASS only, else NULL).
// VIA_SUBPASS: every encoder of the pass (scene subpasses + the eye subpass)
// gets the same map; the scene stays in physical layout (it is transient: the
// guest's storeOp is DONT_CARE) and the EYE image is expanded. VIA_XFER /
// VIA_SAMPLED: the scene is expanded like an eye, before the next render,
// compute OR blit encoder.
void kl_fove_note_eye_scene(void *mtl_colour, void *mtl_depth, void *mtl_out, uint32_t w, uint32_t h,
                            uint32_t samples, uint32_t layers, int eye, int via);
// kl_vulkan says whether the confirmed
// intermediate `mtl_colour` is an MSAA scene the guest never stores (storeOp
// DONT_CARE) that a LATER subpass of its pass resolves into a single-sample
// attachment. Then a layered encoder that keeps its samples with no resolve
// (MoltenVK's store between subpass encoders) is mapped and left in physical
// layout — held pending until the resolving encoder, whose single-sample
// resolve target is what gets expanded. Call after kl_fove_note_eye_scene.
void kl_fove_note_ms_transient(void *mtl_colour, int transient);
// The candidate -> confirmed table (keyed by VkImage handle; pure, for t_fove).
// note: an eye-size pass draws into img; returns 1 candidate / 2 confirmed
// (*via = how). confirm: 1 newly confirmed, 0 already confirmed, -1 not a
// candidate. state: 0 unknown / 1 / 2. last: the most recently noted
// candidate (0 none). forget: vkDestroyImage.
int      kl_fove_inter_note(uint64_t img, int *via);
int      kl_fove_inter_confirm(uint64_t img, int via);
int      kl_fove_inter_state(uint64_t img, int *via);
uint64_t kl_fove_inter_last(void);
// A pass reached a runtime eye image. direct 1 (the eye is its
// colour): confirm (via sampled) every candidate noted since the last call —
// the whole post chain, depth attached or not; direct 0 (resolve / in-pass):
// only close the window. Returns newly confirmed, first `cap` into conf[].
int      kl_fove_inter_eye_reached(int direct, uint64_t *conf, int cap);
// An indirect draw inside the image's multiview pass (MoltenVK splits the
// pass around a compute encoder): state 3 = refused, never a
// candidate or confirmed again until forgotten. Returns the previous state.
int      kl_fove_inter_refuse(uint64_t img);
// ...and its Metal colour texture stops being a marked intermediate target.
void     kl_fove_demote(void *col);
// Refuse an intermediate whose pass MoltenVK was seen splitting (the Metal-side
// backstop for the record-time refusal above, which misses an indirect draw
// recorded in a secondary command buffer; sticky until kl_fove_forget).
// 1 if newly refused.
int      kl_fove_split_refuse(void *col);
void     kl_fove_inter_forget(uint64_t img);
void     kl_fove_inter_reset_for_test(void);
// t_fove: the Metal side of an in-pass intermediate on a real
// device — scene encoders + the eye subpass marked with ONE map, nothing
// expanded between them, the eye expanded at commit. Needs KL_FOVEATION set
// before the first kl_fove call. Returns failed checks, -1 when there is no
// Metal device / rate-map support (skipped).
int      kl_fove_inter_gpu_selftest(char *why, unsigned cap);
// t_fove: a layered map built before the first frame record is RE-CENTRED in
// place once the record arrives (asymmetric per-eye render viewports), not
// while a pending expand refers to it, and not for a record that does not move
// the centre (a full-image rect). Runs after
// kl_fove_inter_gpu_selftest (needs its device). Returns failed checks, -1 skipped.
int      kl_fove_recentre_selftest(char *why, unsigned cap);
// t_fove: the expand's scratch textures are cached per pixel format — two
// intermediates of different formats expanded alternately every frame make 4
// textures once, not 4 per frame (replacing them every frame grew the memory
// footprint by gigabytes in seconds). Runs after
// kl_fove_inter_gpu_selftest. Returns failed checks, -1 skipped.
int      kl_fove_scratch_selftest(char *why, unsigned cap);
// t_fove: the layered eye pair of a guest on the Vision Pro's canted cones
// (Alien: Rogue Incursion's shape, a render viewport smaller than the image):
// both layers' maps are the same relative to their eye's rect, symmetric
// about its middle, both straight-ahead points at full rate; with
// KL_FOVE_PER_EYE's single peak the outer side of each eye is the coarse one
// (the bug, reproduced). Then dynamic resolution: the map follows 20 viewport
// changes (the old cap was 8). `rates` (may be NULL) gets, per mode (0 per eye,
// 1 envelope), layer and eighth of the rect, the rate along the middle row
// (2 x 2 x 8 floats); `shots_dir` (may be NULL) gets the four pictures as BMP.
// Returns failed checks, -1 skipped.
int      kl_fove_layers_selftest(char *why, unsigned cap, float *rates, const char *shots_dir);
// The frustum fractions and frame record kf_eye_point reads, for the test
// (on = 0 goes back to kl_ovrp's). vp NULL / of {0,0}: no record.
void     kl_fove_test_eye_record(int on, const float fx[2], const float fy[2], const int vp[2][4], const int of[2]);
