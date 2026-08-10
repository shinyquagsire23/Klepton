// The host video decoder: VideoToolbox, behind an interface shaped like the one
// thing the guest actually needs — "here is an access unit, give me back a
// frame".
//
// This is to kl_mediandk.c what kl_audio.c is to kl_opensl.c. That file owns
// the ABI the guest calls (AMediaCodec's handles, its dequeue/queue index
// dance, its error codes); this one owns the host device and knows nothing
// about Android. The split is what makes the decoder testable without a guest
// at all — `make hevc` runs an ffmpeg-generated stream straight through it.
//
// Two things about the shape are load-bearing:
//
//   - The guest hands us Annex-B, because that is what MediaCodec takes, and it
//     puts its parameter sets IN BAND (its AMediaFormat carries no csd-0/csd-1
//     — measured, PLANNING §11.15). VideoToolbox takes neither: it wants a
//     CMVideoFormatDescription built from the parameter sets up front and
//     length-prefixed NALs after. Both conversions live in here.
//   - Output is BGRA, not NV12, and that is a decision rather than a default.
//     The guest samples the frame through a `samplerExternalOES`, whose whole
//     promise on Android is that the YUV->RGB conversion has already happened
//     by the time the shader sees it. Asking VideoToolbox for BGRA keeps that
//     promise on the host side, where a hardware converter does it for free,
//     and leaves us with a single-plane texture that the P5 MTLTexture->EGLImage
//     path in kl_glfb.c already knows how to hand to ANGLE. The alternative —
//     two planes and a conversion in the shader — would mean rewriting the
//     guest's shader rather than merely retargeting its sampler. See kl_egl.c.
#ifndef KL_VTDEC_H
#define KL_VTDEC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <CoreVideo/CoreVideo.h>

typedef struct kl_vtdec kl_vtdec;

// mime is the guest's, e.g. "video/hevc" or "video/avc". NULL if we do not
// serve that codec — the caller reports it, because the guest's own error path
// for "no decoder for this type" is better than anything we could invent.
kl_vtdec *kl_vtdec_create(const char *mime);
void      kl_vtdec_destroy(kl_vtdec *d);

// Feed one access unit, Annex-B, with or without parameter sets in front of it.
// Parameter-set NALs are absorbed rather than submitted: they build the format
// description, and the decompression session is created (or, on a change,
// rebuilt) the first moment a complete set exists. An access unit that is
// nothing but parameter sets is therefore a legitimate no-op, which is exactly
// what a codec-config buffer is.
//
// pts_us is the guest's presentation timestamp in microseconds and comes back
// out unchanged on the matching frame; VideoToolbox is only ever a carrier for
// it. Returns 0 on success, or a negative value if the access unit could not be
// submitted at all (no session yet is NOT an error — see above).
int kl_vtdec_submit(kl_vtdec *d, const uint8_t *annexb, size_t len,
                    int64_t pts_us);

// Take the oldest decoded frame, or NULL if none is ready. The caller owns one
// reference and must CVPixelBufferRelease it.
//
// Frames come out in the order VideoToolbox emits them. For this guest that is
// also presentation order: the stream is a real-time game capture with no
// B-frames, and Steam Link's encoder does not reorder. If a stream ever does,
// the fix is to sort on the pts that is already carried here, not to guess.
CVPixelBufferRef kl_vtdec_pull(kl_vtdec *d, int64_t *pts_us_out);

// Drop everything in flight and everything decoded, and tell VideoToolbox to
// forget its inter-frame state. What AMediaCodec_flush means.
void kl_vtdec_flush(kl_vtdec *d);

// How many frames have been submitted, decoded and dropped since creation, and
// the current frame size. For the fault reporter and for `make hevc`.
void kl_vtdec_stats(const kl_vtdec *d, unsigned *submitted, unsigned *decoded,
                    unsigned *dropped, int *width, int *height);
void kl_vtdec_report(FILE *f);

#endif
