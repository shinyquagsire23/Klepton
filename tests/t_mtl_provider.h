// Host-side stand-in for Compositor Services. See t_mtl_provider.m.
// Diagnostic, host-only: t_boot links it, RUNTIME_SHIP never does.
#ifndef KL_MTL_PROVIDER_H
#define KL_MTL_PROVIDER_H

// Register the eye-texture and layer-texture providers, if KL_GLFB_MTL=1 and
// KL_GLFB=1. The second is what backs a guest whose whole picture is an OpenXR
// quad layer rather than an eye pair.
void kl_mtl_provider_install(void);

// Lit-pixel count of (eye, stage)'s MTLTexture, sampled on a stride. Uses the
// same luma threshold as kl_glfb's capture, so the two numbers are comparable —
// which is the gate: the reference path and the interop path should agree.
unsigned long kl_mtl_count_lit(int eye, int stage, int *out_w, int *out_h);

// The same, for a guest's non-eye LAYER — the only measurement there is for a
// guest that presents its whole frame as a quad, whose eye table stays empty.
unsigned long kl_mtl_count_lit_layer(int layer, int stage, int *out_w, int *out_h);

// Mean of the sum-of-three-channels the last kl_mtl_count_lit() sampled — the
// same quantity kl_glfb prints as "mean luma", for the same reason.
unsigned kl_mtl_mean_luma(void);

// ...and the mean ALPHA over those same samples, 0..255. Trap 33: a guest
// leaves an eye texture's alpha at 0 because that layer is composited opaque,
// and a quad layer that asked for source-alpha blending is then a correct
// picture that composites to nothing. Transparent and black are the same
// display; this is the only number that separates them.
unsigned kl_mtl_mean_alpha(void);

// The eye's MTLTexture as a PNG, tone-mapped as kl_glfb's capture is, so the two
// can be compared as pictures rather than as counts. Returns 1 on success.
int kl_mtl_dump_png(int eye, int stage, const char *path);

// ...and one of a guest's non-eye LAYERS, by (layer, stage) — every image of a
// quad's swapchain can be written out this way, which is what says whether one
// of them has stopped receiving the guest's rendering.
int kl_mtl_dump_png_layer(int layer, int stage, const char *path);

#endif
