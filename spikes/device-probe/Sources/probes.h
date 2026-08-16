#ifndef KLEPTON_PROBES_H
#define KLEPTON_PROBES_H
// Returns a newly-malloc'd UTF-8 report. Caller frees.
// bundle_path: absolute path to the app bundle (for dlopen probes).
char *klepton_run_probes(const char *bundle_path);
// Re-read Darwin TSD slot 5 after graphics frameworks are up.
unsigned long long klepton_tsd_slot(int i);

// ---- P13: ANGLE under AMFI + the Metal interop primitive ----
// Split across the language boundary on purpose: Swift owns Metal and C owns the
// guest, so the texture is allocated on the Swift side exactly
// as Compositor Services will supply it. Returns ANGLE's own MTLDevice — the
// extension requires the texture come from *that* device, not the system default.
void *klepton_angle_init(const char *bundle_path);
// Wrap array slice `slice` of `mtl_texture` as a GL texture and draw into it.
// f16 selects the guest's real eye format (GL_RGBA16F) over RGBA8.
// Returns 0 on success, non-zero at the step that failed.
int klepton_angle_draw(void *mtl_texture, int slice, int f16, int w, int h,
                       float r, float g, float b);
// Swift's own verdicts, appended so P13 reads as one block.
void klepton_angle_note(const char *line);
const char *klepton_angle_log(void);
#endif
