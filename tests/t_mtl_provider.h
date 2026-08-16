// Host-side stand-in for Compositor Services. See t_mtl_provider.m.
// Diagnostic, host-only: t_boot links it, RUNTIME_SHIP never does.
#ifndef KL_MTL_PROVIDER_H
#define KL_MTL_PROVIDER_H

// Register the eye-texture provider, if KL_GLFB_MTL=1 and KL_GLFB=1.
void kl_mtl_provider_install(void);

// Lit-pixel count of (eye, stage)'s MTLTexture, sampled on a stride. Uses the
// same luma threshold as kl_glfb's capture, so the two numbers are comparable —
// which is the gate: the reference path and the interop path should agree.
unsigned long kl_mtl_count_lit(int eye, int stage, int *out_w, int *out_h);

// Mean of the sum-of-three-channels the last kl_mtl_count_lit() sampled — the
// same quantity kl_glfb prints as "mean luma", for the same reason.
unsigned kl_mtl_mean_luma(void);

// The eye's MTLTexture as a PNG, tone-mapped as kl_glfb's capture is, so the two
// can be compared as pictures rather than as counts. Returns 1 on success.
int kl_mtl_dump_png(int eye, int stage, const char *path);

#endif
