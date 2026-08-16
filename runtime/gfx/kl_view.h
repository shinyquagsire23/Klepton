// The interactive one-eye viewer (KL_VIEW=1) — the host-side frontend that
// sits on the two seams: pose in through kl_ovrp_set_head_pose, frame out
// through the kl_glfb frame sink. Pure C on SDL3; see kl_view.c for the design.
//
// Built without SDL3 the API still links: kl_view_available() returns 0 and
// kl_view_main() prints a one-line error, so t_boot can fail cleanly.
#ifndef KL_VIEW_H
#define KL_VIEW_H
#include <stdint.h>

int kl_view_available(void);

// The kl_glfb_frame_sink implementation, used by the readback path only
// (KL_VIEW_CPU=1). Runs on the GL thread inside the guest's frame — memcpy and
// go, no SDL calls.
void kl_view_frame_sink(const uint8_t *rgba, int w, int h, void *ctx);

// The SDL loop. Runs on the main thread (macOS requires windowing there) and
// returns when the window closes; `libdir` only decorates the window title.
//
// `hw` selects the frame path: non-zero composites the guest's eye MTLTexture
// straight into the window's CAMetalLayer (kl_view_mtl.m — no readback, no
// copies), zero takes the glReadPixels/upload path and expects the caller to
// have registered kl_view_frame_sink. The caller decides because bringing
// ANGLE up is what answers "is there an MTLDevice", and that must happen
// before the guest thread exists — kl_glfb_init does not arbitrate a race.
int kl_view_main(const char *libdir, int hw);

#endif
