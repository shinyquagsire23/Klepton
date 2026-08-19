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

// The refresh rate of the display the viewer's window will open on, in Hz, or
// 0 when SDL cannot say (and in a build without SDL3). Ask this BEFORE the
// guest thread exists: a guest reads the display rate once, early, and paces
// itself and anything on the other end of a wire against it, so a number
// arriving after boot is a number nobody reads.
//
// This is the panel's rate, which is a ceiling and not a promise: what a guest
// actually achieves is its own throughput and can be far below this. The
// viewer's HUD prints both. A guest paced by kl_view_pace_wait below is held to
// the composite and so cannot exceed it either.
float kl_view_display_hz(void);

// ---- the guest's frame clock ------------------------------------------------
//
// For a guest that owns its own frame loop — an OpenXR one, where xrWaitFrame
// is the call that means "my next frame starts here". Install kl_view_pace_wait
// as that runtime's frame pacer and the viewer's composite becomes the guest's
// clock: one tick published per displayed frame, consumed by the guest's wait.
//
// Without it nothing on the host paces such a guest at all. Steam Link's VR
// client then ran its render loop at ~1000 Hz against a 120 Hz window — eight
// submitted frames for every one displayed, every mirror blit paid for each of
// them, and the frame-pacing telemetry it reports to the streaming host
// measured against a clock nobody drives.
//
// The publish/stop halves are the viewer's own; a caller only installs the wait.
void kl_view_pace_wait(void);
void kl_view_pace_publish(void);
void kl_view_pace_stop(void);

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
