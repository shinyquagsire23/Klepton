// kl_avdec — the host video decoder behind UE4's Java media player.
//
// The split is kl_opensl/kl_audio's and kl_mediandk/kl_vtdec's, one API family
// over: `com.epicgames.ue4.MediaPlayer14` in kl_jni.c is the GUEST's contract
// and knows nothing about how a frame is produced; this is the device, and it
// knows nothing about Java. It is what lets the decode be exercised without a
// guest at all.
//
// Why not kl_vtdec, which already decodes HEVC: that one takes an **Annex-B
// elementary stream** the guest has already demuxed (Steam Link's wire format).
// RE4's cutscenes are H.265 in an **MP4 container** — four of them, `Stored`
// (so uncompressed, a plain byte range) inside `main.203.com.Armature.VR4.obb`,
// the largest being `VR4/Content/Movies/Andy/bio4_opening_h265.mp4`. Demuxing
// MP4 by hand to feed a decoder we already have would be more code than asking
// AVFoundation to do both, and it would be code with no other caller.
//
// Everything the guest can ask for is answered from a real decode or refused;
// nothing here fabricates a duration, a size or a frame. A media file we cannot
// open answers `kl_avdec_open` NULL, which is what the guest's own
// `setDataSource -> false` path is for.
#ifndef KL_AVDEC_H
#define KL_AVDEC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kl_avdec kl_avdec;

// Open a movie. `offset`/`size` name a byte RANGE inside `path` — which is how
// a UE4 guest asks for one, because on Android the movie lives inside the OBB
// and is addressed as (container, offset, length). `size <= 0` means the whole
// file. NULL if it cannot be opened or carries no video track; the reason is
// printed once, by name.
kl_avdec *kl_avdec_open(const char *path, long long offset, long long size);
void      kl_avdec_close(kl_avdec *d);

// Measured off the asset, not guessed. Zero until the asset has been read.
int       kl_avdec_width(kl_avdec *d);
int       kl_avdec_height(kl_avdec *d);
long long kl_avdec_duration_ms(kl_avdec *d);

// The transport. `kl_avdec_play(d, 0)` pauses where it is; position is in
// milliseconds and is the presentation time of the newest frame handed out,
// which is what a guest polling GetCurrentPosition is asking about.
void      kl_avdec_play(kl_avdec *d, int on);
void      kl_avdec_seek(kl_avdec *d, long long ms);
void      kl_avdec_set_looping(kl_avdec *d, int on);
long long kl_avdec_position_ms(kl_avdec *d);
int       kl_avdec_playing(kl_avdec *d);
// True once the stream has run out and is not looping. Latched: a guest polls
// this to decide the movie is over, and a flag that cleared itself would be a
// movie that never ends.
int       kl_avdec_complete(kl_avdec *d);

// The newest decoded frame as **BGRA8**, or NULL if none has been produced yet.
//
// The pointer is owned by the decoder and stays valid until the next call on
// this player from the same thread — the guest hands it straight to the RHI as
// a direct ByteBuffer, so a copy here would be a copy of every frame for
// nothing. `*serial` distinguishes "the same frame again" from a new one
// without comparing pixels, which is what `didResolutionChange` and UE4's own
// frame-update path both actually want to know.
const void *kl_avdec_frame(kl_avdec *d, int *w, int *h, size_t *bytes,
                           unsigned long long *serial);

#ifdef __cplusplus
}
#endif
#endif
