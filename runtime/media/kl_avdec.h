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
// Hold the frame still while it is copied out: between these two the decode
// thread cannot write the next frame over the one kl_avdec_frame returned.
void kl_avdec_frame_lock(kl_avdec *d);
void kl_avdec_frame_unlock(kl_avdec *d);

// ---------------------------------------------------------------------------
// Demux-only path, for Unity's NDK video (AMediaExtractor + AMediaCodec).
//
// Unlike kl_avdec above (an all-in-one player for UE4's Java MediaPlayer),
// Unity drives demux and decode as two separate NDK objects: it pulls compressed
// samples from AMediaExtractor and feeds them to AMediaCodec. kl_mediandk's
// AMediaCodec is kl_vtdec, which decodes Annex-B — so this hands the container's
// video back as an Annex-B elementary stream (parameter sets injected on every
// keyframe), which is exactly what that path already absorbs. Reuses the OBB
// byte-range slicing kl_avdec_open uses.
typedef struct kl_avdemux kl_avdemux;

// Open (container, offset, length) — offset/length address a sub-range inside an
// OBB, 0/0 for a plain file. NULL if the file has no video track / cannot open.
kl_avdemux *kl_avdemux_open(const char *container, long long offset, long long size);
void        kl_avdemux_close(kl_avdemux *m);

// Track facts for AMediaExtractor_getTrackFormat. `mime` is "video/avc" or
// "video/hevc" (static string, not owned). Any out-param may be NULL.
int  kl_avdemux_info(kl_avdemux *m, int *w, int *h, const char **mime,
                     long long *duration_us, float *fps);

// The CURRENT sample. read copies it as Annex-B into buf and returns its byte
// length (the full length even if it exceeds cap, matching AMediaExtractor); -1
// at end of stream. time/keyframe describe that same current sample.
long long kl_avdemux_sample_time_us(kl_avdemux *m);
int       kl_avdemux_sample_keyframe(kl_avdemux *m);
long      kl_avdemux_read(kl_avdemux *m, unsigned char *buf, unsigned long cap);

// Move to the next sample; 1 if one is now current, 0 at end of stream.
int  kl_avdemux_advance(kl_avdemux *m);
// Seek so the next current sample is at/after us; 1 on success.
int  kl_avdemux_seek_us(kl_avdemux *m, long long us);

// The clip's SOUND, when it has an audio track: decoded to 16-bit interleaved
// PCM (at most two channels) and served in chunks of 1024 frames, the same
// current-sample shape as the video above. info is 0 for a clip without sound.
int       kl_avdemux_audio_info(kl_avdemux *m, int *rate, int *channels);
long long kl_avdemux_audio_time_us(kl_avdemux *m);     // -1 at end of stream
long      kl_avdemux_audio_read(kl_avdemux *m, unsigned char *buf, unsigned long cap);
int       kl_avdemux_audio_advance(kl_avdemux *m);
int       kl_avdemux_audio_seek_us(kl_avdemux *m, long long us);

#ifdef __cplusplus
}
#endif
#endif
